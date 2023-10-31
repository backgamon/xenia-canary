/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/kernel_state.h"

#include <string>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/hid/input_system.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_memory.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_module.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_ob.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xmodule.h"
#include "xenia/kernel/xnotifylistener.h"
#include "xenia/kernel/xobject.h"
#include "xenia/kernel/xthread.h"

DEFINE_bool(apply_title_update, true, "Apply title updates.", "Kernel");

namespace xe {
namespace kernel {
struct DispatchQueueEntry : public threading::AtomicListEntry {
  std::function<void()> function;
  DispatchQueueEntry(std::function<void()> fn)
      : threading::AtomicListEntry(), function(std::move(fn)) {}
};
constexpr uint32_t kDeferredOverlappedDelayMillis = 100;

// This is a global object initialized with the XboxkrnlModule.
// It references the current kernel state object that all kernel methods should
// be using to stash their variables.
KernelState* shared_kernel_state_ = nullptr;

KernelState* kernel_state() { return shared_kernel_state_; }

KernelState::KernelState(Emulator* emulator)
    : emulator_(emulator),
      memory_(emulator->memory()),
      dispatch_thread_running_(false),
      kernel_trampoline_group_(emulator->processor()->backend()) {
  assert_null(shared_kernel_state_);
  shared_kernel_state_ = this;
  processor_ = emulator->processor();
  file_system_ = emulator->file_system();

  app_manager_ = std::make_unique<xam::AppManager>();
  achievement_manager_ = std::make_unique<AchievementManager>();
  user_profiles_.emplace(0, std::make_unique<xam::UserProfile>(0));

  BootKernel();

  auto content_root = emulator_->content_root();
  if (!content_root.empty()) {
    content_root = std::filesystem::absolute(content_root);
  }
  content_manager_ = std::make_unique<xam::ContentManager>(this, content_root);

  // Hardcoded maximum of 2048 TLS slots.
  tls_bitmap_.Resize(2048);

  auto hc_loc_heap = memory_->LookupHeap(strange_hardcoded_page_);
  bool fixed_alloc_worked = hc_loc_heap->AllocFixed(
      strange_hardcoded_page_, 65536, 0,
      kMemoryAllocationCommit | kMemoryAllocationReserve,
      kMemoryProtectRead | kMemoryProtectWrite);

  xenia_assert(fixed_alloc_worked);

  xam::AppManager::RegisterApps(this, app_manager_.get());
}

KernelState::~KernelState() {
  SetExecutableModule(nullptr);

  if (dispatch_thread_running_) {
    dispatch_thread_running_ = false;
    dispatch_thread_->Wait(0, 0, 0, nullptr);
  }

  executable_module_.reset();
  user_modules_.clear();
  kernel_modules_.clear();

  // Delete all objects.
  object_table_.Reset();

  // Shutdown apps.
  app_manager_.reset();

  assert_true(shared_kernel_state_ == this);
  shared_kernel_state_ = nullptr;
}

KernelState* KernelState::shared() { return shared_kernel_state_; }

uint32_t KernelState::title_id() const {
  assert_not_null(executable_module_);

  xex2_opt_execution_info* exec_info = 0;
  executable_module_->GetOptHeader(XEX_HEADER_EXECUTION_INFO, &exec_info);

  if (exec_info) {
    return exec_info->title_id;
  }

  return 0;
}

util::XdbfGameData KernelState::title_xdbf() const {
  return module_xdbf(executable_module_);
}

util::XdbfGameData KernelState::module_xdbf(
    object_ref<UserModule> exec_module) const {
  assert_not_null(exec_module);

  uint32_t resource_data = 0;
  uint32_t resource_size = 0;
  if (XSUCCEEDED(exec_module->GetSection(
          fmt::format("{:08X}", exec_module->title_id()).c_str(),
          &resource_data, &resource_size))) {
    util::XdbfGameData db(memory()->TranslateVirtual(resource_data),
                          resource_size);
    return db;
  }
  return util::XdbfGameData(nullptr, resource_size);
}

uint32_t KernelState::AllocateTLS() { return uint32_t(tls_bitmap_.Acquire()); }

void KernelState::FreeTLS(uint32_t slot) {
  const std::vector<object_ref<XThread>> threads =
      object_table()->GetObjectsByType<XThread>();

  for (const object_ref<XThread>& thread : threads) {
    if (thread->is_guest_thread()) {
      thread->SetTLSValue(slot, 0);
    }
  }
  tls_bitmap_.Release(slot);
}

void KernelState::RegisterTitleTerminateNotification(uint32_t routine,
                                                     uint32_t priority) {
  TerminateNotification notify;
  notify.guest_routine = routine;
  notify.priority = priority;

  terminate_notifications_.push_back(notify);
}

void KernelState::RemoveTitleTerminateNotification(uint32_t routine) {
  for (auto it = terminate_notifications_.begin();
       it != terminate_notifications_.end(); it++) {
    if (it->guest_routine == routine) {
      terminate_notifications_.erase(it);
      break;
    }
  }
}

void KernelState::RegisterModule(XModule* module) {}

void KernelState::UnregisterModule(XModule* module) {}

bool KernelState::RegisterUserModule(object_ref<UserModule> module) {
  auto lock = global_critical_region_.Acquire();

  for (auto user_module : user_modules_) {
    if (user_module->path() == module->path()) {
      // Already loaded.
      return false;
    }
  }

  user_modules_.push_back(module);
  return true;
}

void KernelState::UnregisterUserModule(UserModule* module) {
  auto lock = global_critical_region_.Acquire();

  for (auto it = user_modules_.begin(); it != user_modules_.end(); it++) {
    if ((*it)->path() == module->path()) {
      user_modules_.erase(it);
      return;
    }
  }
}

bool KernelState::IsKernelModule(const std::string_view name) {
  if (name.empty()) {
    // Executing module isn't a kernel module.
    return false;
  }
  // NOTE: no global lock required as the kernel module list is static.
  for (auto kernel_module : kernel_modules_) {
    if (kernel_module->Matches(name)) {
      return true;
    }
  }
  return false;
}

object_ref<KernelModule> KernelState::GetKernelModule(
    const std::string_view name) {
  assert_true(IsKernelModule(name));

  for (auto kernel_module : kernel_modules_) {
    if (kernel_module->Matches(name)) {
      return retain_object(kernel_module.get());
    }
  }

  return nullptr;
}

object_ref<XModule> KernelState::GetModule(const std::string_view name,
                                           bool user_only) {
  if (name.empty()) {
    // NULL name = self.
    // TODO(benvanik): lookup module from caller address.
    return GetExecutableModule();
  } else if (xe::utf8::equal_case(name, "kernel32.dll")) {
    // Some games request this, for some reason. wtf.
    return nullptr;
  }

  auto global_lock = global_critical_region_.Acquire();

  if (!user_only) {
    for (auto kernel_module : kernel_modules_) {
      if (kernel_module->Matches(name)) {
        return retain_object(kernel_module.get());
      }
    }
  }

  auto path(name);

  // Resolve the path to an absolute path.
  auto entry = file_system_->ResolvePath(name);
  if (entry) {
    path = entry->absolute_path();
  }

  for (auto user_module : user_modules_) {
    if (user_module->Matches(path)) {
      return retain_object(user_module.get());
    }
  }
  return nullptr;
}
struct LaunchInterrupt {
  object_ref<UserModule>* module;
  XThread* thread;
};

void KernelState::LaunchModuleInterrupt(void* ud) {
  LaunchInterrupt* launch = reinterpret_cast<LaunchInterrupt*>(ud);
  auto kernel = kernel_state();
  kernel->SetExecutableModule(*launch->module);
  kernel->CreateDispatchThread();
  launch->thread =
      new XThread(kernel_state(), (*launch->module)->stack_size(), 0,
                  (*launch->module)->entry_point(), 0, 0x1000100, true, true);

  launch->thread->set_name("Main XThread");

  X_STATUS result = launch->thread->Create();
  if (XFAILED(result)) {
    XELOGE("Could not create launch thread: {:08X}", result);

    delete launch->thread;
    launch->thread = nullptr;
    return;
  }

  // Waits for a debugger client, if desired.
  // kernel->emulator()->processor()->PreLaunch();

  // Resume the thread now.
  // If the debugger has requested a suspend this will just decrement the
  // suspend count without resuming it until the debugger wants.
  // launch->thread->Resume();
}

object_ref<XThread> KernelState::LaunchModule(object_ref<UserModule> module) {
  if (!module->is_executable()) {
    return nullptr;
  }
#if 0
  SetExecutableModule(module);
  XELOGI("KernelState: Launching module...");

  // Create a thread to run in.
  // We start suspended so we can run the debugger prep.
  auto thread = object_ref<XThread>(
      new XThread(kernel_state(), module->stack_size(), 0,
                  module->entry_point(), 0, X_CREATE_SUSPENDED, true, true));

  // We know this is the 'main thread'.
  thread->set_name("Main XThread");

  X_STATUS result = thread->Create();
  if (XFAILED(result)) {
    XELOGE("Could not create launch thread: {:08X}", result);
    return nullptr;
  }

  // Waits for a debugger client, if desired.
  emulator()->processor()->PreLaunch();

  // Resume the thread now.
  // If the debugger has requested a suspend this will just decrement the
  // suspend count without resuming it until the debugger wants.
  thread->Resume();

  return thread;
#else
  // this is pretty bad
  LaunchInterrupt li;
  li.module = &module;
  li.thread = nullptr;
  while (!processor()->GetCPUThread(0)->TrySendInterruptFromHost(
      LaunchModuleInterrupt, &li, true)) {
    threading::NanoSleep(10000);
  }
  if (li.thread) {
    return object_ref<XThread>(li.thread);
  } else {
    return nullptr;
  }

#endif
}

object_ref<UserModule> KernelState::GetExecutableModule() {
  if (!executable_module_) {
    return nullptr;
  }
  return executable_module_;
}

void KernelState::SetExecutableModule(object_ref<UserModule> module) {
  if (module.get() == executable_module_.get()) {
    return;
  }
  executable_module_ = std::move(module);
  if (!executable_module_) {
    return;
  }

  auto title_process =
      memory_->TranslateVirtual<X_KPROCESS*>(GetTitleProcess());

  InitializeProcess(title_process, X_PROCTYPE_TITLE, 10, 13, 17);

  xex2_opt_tls_info* tls_header = nullptr;
  executable_module_->GetOptHeader(XEX_HEADER_TLS_INFO, &tls_header);
  if (tls_header) {
    title_process->tls_static_data_address = tls_header->raw_data_address;
    title_process->tls_data_size = tls_header->data_size;
    title_process->tls_raw_data_size = tls_header->raw_data_size;
    title_process->tls_slot_size = tls_header->slot_count * 4;
    SetProcessTLSVars(title_process, tls_header->slot_count,
                      tls_header->data_size, tls_header->raw_data_address);
  }

  uint32_t kernel_stacksize = 0;

  executable_module_->GetOptHeader(XEX_HEADER_DEFAULT_STACK_SIZE,
                                   &kernel_stacksize);
  if (kernel_stacksize) {
    kernel_stacksize = (kernel_stacksize + 4095) & 0xFFFFF000;
    if (kernel_stacksize < 0x4000) {
      kernel_stacksize = 0x4000;
    }
    title_process->kernel_stack_size = kernel_stacksize;
  }

  // Setup the kernel's XexExecutableModuleHandle field.
  auto export_entry = processor()->export_resolver()->GetExportByOrdinal(
      "xboxkrnl.exe", ordinals::XexExecutableModuleHandle);
  if (export_entry) {
    assert_not_zero(export_entry->variable_ptr);
    auto variable_ptr = memory()->TranslateVirtual<xe::be<uint32_t>*>(
        export_entry->variable_ptr);
    *variable_ptr = executable_module_->hmodule_ptr();
  }

  // Setup the kernel's ExLoadedImageName field
  export_entry = processor()->export_resolver()->GetExportByOrdinal(
      "xboxkrnl.exe", ordinals::ExLoadedImageName);

  if (export_entry) {
    char* variable_ptr =
        memory()->TranslateVirtual<char*>(export_entry->variable_ptr);
    xe::string_util::copy_truncating(
        variable_ptr, executable_module_->path(),
        xboxkrnl::XboxkrnlModule::kExLoadedImageNameSize);
  }
}
void KernelState::CreateDispatchThread() {
  // Spin up deferred dispatch worker.
  if (!dispatch_thread_running_) {
    dispatch_thread_running_ = true;
    dispatch_thread_ = object_ref<XHostThread>(
        new XHostThread(this, 128 * 1024, XE_FLAG_AFFINITY_CPU2, [this]() {
          // As we run guest callbacks the debugger must be able to suspend us.
          auto context = cpu::ThreadState::GetContext();
          while (dispatch_thread_running_) {
            context->CheckInterrupt();
            DispatchQueueEntry* entry =
                reinterpret_cast<DispatchQueueEntry*>(dispatch_queue_.Pop());

            if (!entry) {
              // xboxkrnl::xeNtYieldExecution(context);
              int64_t interval = -100000;  // 10 ms
              xboxkrnl::xeKeDelayExecutionThread(context, 0, false, &interval);
              continue;
            } else {
              entry->function();
              delete entry;
            }
          }
          return 0;
        }));  // don't think an equivalent exists on real hw
    dispatch_thread_->set_name("Kernel Dispatch");
    dispatch_thread_->Create();
  }
}

void KernelState::LoadKernelModule(object_ref<KernelModule> kernel_module) {
  auto global_lock = global_critical_region_.Acquire();
  kernel_modules_.push_back(std::move(kernel_module));
}

object_ref<UserModule> KernelState::LoadUserModule(
    const std::string_view raw_name, bool call_entry) {
  // Some games try to load relative to launch module, others specify full path.
  auto name = xe::utf8::find_name_from_guest_path(raw_name);
  std::string path(raw_name);
  if (name == raw_name) {
    assert_not_null(executable_module_);
    path = xe::utf8::join_guest_paths(
        xe::utf8::find_base_guest_path(executable_module_->path()), name);
  }

  object_ref<UserModule> module;
  {
    auto global_lock = global_critical_region_.Acquire();

    // See if we've already loaded it
    for (auto& existing_module : user_modules_) {
      if (existing_module->path() == path) {
        return existing_module;
      }
    }

    global_lock.unlock();

    // Module wasn't loaded, so load it.
    module = object_ref<UserModule>(new UserModule(this));
    X_STATUS status = module->LoadFromFile(path);
    if (XFAILED(status)) {
      object_table()->ReleaseHandle(module->handle());
      return nullptr;
    }

    global_lock.lock();

    // Putting into the listing automatically retains.
    user_modules_.push_back(module);
  }
  return module;
}

X_RESULT KernelState::FinishLoadingUserModule(
    const object_ref<UserModule> module, bool call_entry) {
  // TODO(Gliniak): Apply custom patches here
  X_RESULT result = module->LoadContinue();
  if (XFAILED(result)) {
    return result;
  }
  module->Dump();
  emulator_->patcher()->ApplyPatchesForTitle(memory_, module->title_id(),
                                             module->hash());
  emulator_->on_patch_apply();
  if (module->xex_module()) {
    module->xex_module()->Precompile();
  }

  if (module->is_dll_module() && module->entry_point() && call_entry) {
    // Call DllMain(DLL_PROCESS_ATTACH):
    // https://msdn.microsoft.com/en-us/library/windows/desktop/ms682583%28v=vs.85%29.aspx
    uint64_t args[] = {
        module->handle(),
        1,  // DLL_PROCESS_ATTACH
        0,  // 0 because always dynamic
    };
    auto thread_state = XThread::GetCurrentThread()->thread_state();
    processor()->Execute(thread_state, module->entry_point(), args,
                         xe::countof(args));
  }
  return result;
}

X_RESULT KernelState::ApplyTitleUpdate(const object_ref<UserModule> module) {
  X_RESULT result = X_STATUS_SUCCESS;
  if (!cvars::apply_title_update) {
    return result;
  }

  std::vector<xam::XCONTENT_AGGREGATE_DATA> tu_list =
      content_manager()->ListContent(1, xe::XContentType::kInstaller,
                                     module->title_id());

  if (tu_list.empty()) {
    return result;
  }

  uint32_t disc_number = -1;
  if (module->is_multi_disc_title()) {
    disc_number = module->disc_number();
  }
  // TODO(Gliniak): Support for selecting from multiple TUs
  const xam::XCONTENT_AGGREGATE_DATA& title_update = tu_list.front();
  X_RESULT open_status =
      content_manager()->OpenContent("UPDATE", title_update, disc_number);

  // Use the corresponding patch for the launch module
  std::filesystem::path patch_xexp = fmt::format("{0}.xexp", module->name());

  std::string resolved_path = "";
  file_system()->FindSymbolicLink("UPDATE:", resolved_path);
  xe::vfs::Entry* patch_entry = kernel_state()->file_system()->ResolvePath(
      resolved_path + patch_xexp.generic_string());

  if (patch_entry) {
    const std::string patch_path = patch_entry->absolute_path();
    XELOGI("Loading XEX patch from {}", patch_path);
    auto patch_module = object_ref<UserModule>(new UserModule(this));

    result = patch_module->LoadFromFile(patch_path);
    if (result != X_STATUS_SUCCESS) {
      XELOGE("Failed to load XEX patch, code: {}", result);
      return X_STATUS_UNSUCCESSFUL;
    }

    result = patch_module->xex_module()->ApplyPatch(module->xex_module());
    if (result != X_STATUS_SUCCESS) {
      XELOGE("Failed to apply XEX patch, code: {}", result);
      return X_STATUS_UNSUCCESSFUL;
    }
  }
  return result;
}

void KernelState::UnloadUserModule(const object_ref<UserModule>& module,
                                   bool call_entry) {
  auto global_lock = global_critical_region_.Acquire();

  if (module->is_dll_module() && module->entry_point() && call_entry) {
    // Call DllMain(DLL_PROCESS_DETACH):
    // https://msdn.microsoft.com/en-us/library/windows/desktop/ms682583%28v=vs.85%29.aspx
    uint64_t args[] = {
        module->handle(),
        0,  // DLL_PROCESS_DETACH
        0,  // 0 for now, assume XexUnloadImage is like FreeLibrary
    };
    auto thread_state = XThread::GetCurrentThread()->thread_state();
    processor()->Execute(thread_state, module->entry_point(), args,
                         xe::countof(args));
  }

  auto iter = std::find_if(
      user_modules_.begin(), user_modules_.end(),
      [&module](const auto& e) { return e->path() == module->path(); });
  assert_true(iter != user_modules_.end());  // Unloading an unregistered module
                                             // is probably really bad
  user_modules_.erase(iter);

  // Ensure this module was not somehow registered twice
  assert_true(std::find_if(user_modules_.begin(), user_modules_.end(),
                           [&module](const auto& e) {
                             return e->path() == module->path();
                           }) == user_modules_.end());

  object_table()->ReleaseHandleInLock(module->handle());
}

void KernelState::TerminateTitle() {
  XELOGD("KernelState::TerminateTitle");
  auto global_lock = global_critical_region_.Acquire();

  // Call terminate routines.
  // TODO(benvanik): these might take arguments.
  // FIXME: Calling these will send some threads into kernel code and they'll
  // hold the lock when terminated! Do we need to wait for all threads to exit?
  /*
  if (from_guest_thread) {
    for (auto routine : terminate_notifications_) {
      auto thread_state = XThread::GetCurrentThread()->thread_state();
      processor()->Execute(thread_state, routine.guest_routine);
    }
  }
  terminate_notifications_.clear();
  */

  // Kill all guest threads.
  for (auto it = threads_by_id_.begin(); it != threads_by_id_.end();) {
    if (!XThread::IsInThread(it->second) && it->second->is_guest_thread()) {
      auto thread = it->second;

      if (thread->is_running()) {
        // Need to step the thread to a safe point (returns it to guest code
        // so it's guaranteed to not be holding any locks / in host kernel
        // code / etc). Can't do that properly if we have the lock.
        if (!emulator_->is_paused()) {
          thread->thread()->Suspend();
        }

        global_lock.unlock();
        processor_->StepToGuestSafePoint(thread->thread_id());
        thread->Terminate(0);
        global_lock.lock();
      }

      // Erase it from the thread list.
      it = threads_by_id_.erase(it);
    } else {
      ++it;
    }
  }

  // Third: Unload all user modules (including the executable).
  for (size_t i = 0; i < user_modules_.size(); i++) {
    X_STATUS status = user_modules_[i]->Unload();
    assert_true(XSUCCEEDED(status));

    object_table_.RemoveHandle(user_modules_[i]->handle());
  }
  user_modules_.clear();

  // Release all objects in the object table.
  object_table_.PurgeAllObjects();

  // Unregister all notify listeners.
  notify_listeners_.clear();

  // Clear the TLS map.
  tls_bitmap_.Reset();

  // Unset the executable module.
  executable_module_ = nullptr;

  if (XThread::IsInThread()) {
    threads_by_id_.erase(XThread::GetCurrentThread()->thread_id());

    // Now commit suicide (using Terminate, because we can't call into guest
    // code anymore).
    global_lock.unlock();
    XThread::GetCurrentThread()->Terminate(0);
  }
}

void KernelState::RegisterThread(XThread* thread) {
  auto global_lock = global_critical_region_.Acquire();
  threads_by_id_[thread->thread_id()] = thread;
}

void KernelState::UnregisterThread(XThread* thread) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = threads_by_id_.find(thread->thread_id());
  if (it != threads_by_id_.end()) {
    threads_by_id_.erase(it);
  }
}

void KernelState::OnThreadExecute(XThread* thread) {
  auto global_lock = global_critical_region_.Acquire();

  // Must be called on executing thread.
  assert_true(XThread::GetCurrentThread() == thread);

  // Call DllMain(DLL_THREAD_ATTACH) for each user module:
  // https://msdn.microsoft.com/en-us/library/windows/desktop/ms682583%28v=vs.85%29.aspx
  auto thread_state = thread->thread_state();
  for (auto user_module : user_modules_) {
    if (user_module->is_dll_module() && user_module->entry_point()) {
      uint64_t args[] = {
          user_module->handle(),
          2,  // DLL_THREAD_ATTACH
          0,  // 0 because always dynamic
      };
      processor()->Execute(thread_state, user_module->entry_point(), args,
                           xe::countof(args));
    }
  }
}

void KernelState::OnThreadExit(XThread* thread) {
  auto global_lock = global_critical_region_.Acquire();

  // Must be called on executing thread.
  assert_true(XThread::GetCurrentThread() == thread);

  // Call DllMain(DLL_THREAD_DETACH) for each user module:
  // https://msdn.microsoft.com/en-us/library/windows/desktop/ms682583%28v=vs.85%29.aspx
  auto thread_state = thread->thread_state();
  for (auto user_module : user_modules_) {
    if (user_module->is_dll_module() && user_module->entry_point()) {
      uint64_t args[] = {
          user_module->handle(),
          3,  // DLL_THREAD_DETACH
          0,  // 0 because always dynamic
      };
      processor()->Execute(thread_state, user_module->entry_point(), args,
                           xe::countof(args));
    }
  }

  emulator()->processor()->OnThreadExit(thread->thread_id());
}

object_ref<XThread> KernelState::GetThreadByID(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  XThread* thread = nullptr;
  auto it = threads_by_id_.find(thread_id);
  if (it != threads_by_id_.end()) {
    thread = it->second;
  }
  return retain_object(thread);
}

void KernelState::RegisterNotifyListener(XNotifyListener* listener) {
  auto global_lock = global_critical_region_.Acquire();
  notify_listeners_.push_back(retain_object(listener));

  // Games seem to expect a few notifications on startup, only for the first
  // listener.
  // https://cs.rin.ru/forum/viewtopic.php?f=38&t=60668&hilit=resident+evil+5&start=375
  if (!has_notified_startup_ && listener->mask() & 0x00000001) {
    has_notified_startup_ = true;
    // XN_SYS_UI (on, off)
    listener->EnqueueNotification(0x00000009, 1);
    listener->EnqueueNotification(0x00000009, 0);
    // XN_SYS_SIGNINCHANGED x2
    listener->EnqueueNotification(0x0000000A, 1);
    listener->EnqueueNotification(0x0000000A, 1);
  }
}

void KernelState::UnregisterNotifyListener(XNotifyListener* listener) {
  auto global_lock = global_critical_region_.Acquire();
  for (auto it = notify_listeners_.begin(); it != notify_listeners_.end();
       ++it) {
    if ((*it).get() == listener) {
      notify_listeners_.erase(it);
      break;
    }
  }
}

void KernelState::BroadcastNotification(XNotificationID id, uint32_t data) {
  auto global_lock = global_critical_region_.Acquire();
  for (const auto& notify_listener : notify_listeners_) {
    notify_listener->EnqueueNotification(id, data);
  }
}

void KernelState::CompleteOverlapped(uint32_t overlapped_ptr, X_RESULT result) {
  CompleteOverlappedEx(overlapped_ptr, result, result, 0);
}

void KernelState::CompleteOverlappedEx(uint32_t overlapped_ptr, X_RESULT result,
                                       uint32_t extended_error,
                                       uint32_t length) {
  auto ptr = memory()->TranslateVirtual(overlapped_ptr);
  XOverlappedSetResult(ptr, result);
  XOverlappedSetExtendedError(ptr, extended_error);
  XOverlappedSetLength(ptr, length);
  X_HANDLE event_handle = XOverlappedGetEvent(ptr);
  if (event_handle) {
    auto ev = object_table()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    if (ev) {
      ev->Set(0, false);
    }
  }
  if (XOverlappedGetCompletionRoutine(ptr)) {
    X_HANDLE thread_handle = XOverlappedGetContext(ptr);
    auto thread = object_table()->LookupObject<XThread>(thread_handle);
    if (thread) {
      // Queue APC on the thread that requested the overlapped operation.
      uint32_t routine = XOverlappedGetCompletionRoutine(ptr);
      thread->EnqueueApc(routine, result, length, overlapped_ptr);
    }
  }
}

void KernelState::CompleteOverlappedImmediate(uint32_t overlapped_ptr,
                                              X_RESULT result) {
  // TODO(gibbed): there are games that check 'length' of overlapped as
  // an indication of success. WTF?
  // Setting length to -1 when not success seems to be helping.
  uint32_t length = !result ? 0 : 0xFFFFFFFF;
  CompleteOverlappedImmediateEx(overlapped_ptr, result, result, length);
}

void KernelState::CompleteOverlappedImmediateEx(uint32_t overlapped_ptr,
                                                X_RESULT result,
                                                uint32_t extended_error,
                                                uint32_t length) {
  auto ptr = memory()->TranslateVirtual(overlapped_ptr);
  XOverlappedSetContext(ptr, XThread::GetCurrentThreadHandle());
  CompleteOverlappedEx(overlapped_ptr, result, extended_error, length);
}

void KernelState::CompleteOverlappedDeferred(
    std::function<void()> completion_callback, uint32_t overlapped_ptr,
    X_RESULT result, std::function<void()> pre_callback,
    std::function<void()> post_callback) {
  CompleteOverlappedDeferredEx(std::move(completion_callback), overlapped_ptr,
                               result, result, 0, pre_callback, post_callback);
}

void KernelState::CompleteOverlappedDeferredEx(
    std::function<void()> completion_callback, uint32_t overlapped_ptr,
    X_RESULT result, uint32_t extended_error, uint32_t length,
    std::function<void()> pre_callback, std::function<void()> post_callback) {
  CompleteOverlappedDeferredEx(
      [completion_callback, result, extended_error, length](
          uint32_t& cb_extended_error, uint32_t& cb_length) -> X_RESULT {
        completion_callback();
        cb_extended_error = extended_error;
        cb_length = length;
        return result;
      },
      overlapped_ptr, pre_callback, post_callback);
}

void KernelState::CompleteOverlappedDeferred(
    std::function<X_RESULT()> completion_callback, uint32_t overlapped_ptr,
    std::function<void()> pre_callback, std::function<void()> post_callback) {
  CompleteOverlappedDeferredEx(
      [completion_callback](uint32_t& extended_error,
                            uint32_t& length) -> X_RESULT {
        auto result = completion_callback();
        extended_error = static_cast<uint32_t>(result);
        length = 0;
        return result;
      },
      overlapped_ptr, pre_callback, post_callback);
}

void KernelState::CompleteOverlappedDeferredEx(
    std::function<X_RESULT(uint32_t&, uint32_t&)> completion_callback,
    uint32_t overlapped_ptr, std::function<void()> pre_callback,
    std::function<void()> post_callback) {
  auto ptr = memory()->TranslateVirtual(overlapped_ptr);
  XOverlappedSetResult(ptr, X_ERROR_IO_PENDING);
  XOverlappedSetContext(ptr, XThread::GetCurrentThreadHandle());
  X_HANDLE event_handle = XOverlappedGetEvent(ptr);
  if (event_handle) {
    auto ev = object_table()->LookupObject<XObject>(event_handle);

    assert_not_null(ev);
    if (ev && ev->type() == XObject::Type::Event) {
      ev.get<XEvent>()->Reset();
    }
  }

  DispatchQueueEntry* new_entry =
      new DispatchQueueEntry([this, completion_callback, overlapped_ptr,
                              pre_callback, post_callback]() {
        auto context = cpu::ThreadState::GetContext();
        context->CheckInterrupt();
        if (pre_callback) {
          pre_callback();
        }
        context->CheckInterrupt();
        uint32_t extended_error, length;
        auto result = completion_callback(extended_error, length);
        context->CheckInterrupt();
        CompleteOverlappedEx(overlapped_ptr, result, extended_error, length);
        context->CheckInterrupt();
        if (post_callback) {
          post_callback();
        }
      });
  dispatch_queue_.Push(new_entry);
}

bool KernelState::Save(ByteStream* stream) {
  XELOGD("Serializing the kernel...");
  stream->Write(kKernelSaveSignature);

  // Save the object table
  object_table_.Save(stream);

  // Write the TLS allocation bitmap
  auto tls_bitmap = tls_bitmap_.data();
  stream->Write(uint32_t(tls_bitmap.size()));
  for (size_t i = 0; i < tls_bitmap.size(); i++) {
    stream->Write<uint64_t>(tls_bitmap[i]);
  }

  // We save XThreads absolutely first, as they will execute code upon save
  // (which could modify the kernel state)
  auto threads = object_table_.GetObjectsByType<XThread>();
  uint32_t* num_threads_ptr =
      reinterpret_cast<uint32_t*>(stream->data() + stream->offset());
  stream->Write(static_cast<uint32_t>(threads.size()));

  size_t num_threads = threads.size();
  XELOGD("Serializing {} threads...", threads.size());
  for (auto thread : threads) {
    if (!thread->is_guest_thread()) {
      // Don't save host threads. They can be reconstructed on startup.
      num_threads--;
      continue;
    }

    if (!thread->Save(stream)) {
      XELOGD("Failed to save thread \"{}\"", thread->name());
      num_threads--;
    }
  }

  *num_threads_ptr = static_cast<uint32_t>(num_threads);

  // Save all other objects
  auto objects = object_table_.GetAllObjects();
  uint32_t* num_objects_ptr =
      reinterpret_cast<uint32_t*>(stream->data() + stream->offset());
  stream->Write(static_cast<uint32_t>(objects.size()));

  size_t num_objects = objects.size();
  XELOGD("Serializing {} objects...", num_objects);
  for (auto object : objects) {
    auto prev_offset = stream->offset();

    if (object->is_host_object() || object->type() == XObject::Type::Thread) {
      // Don't save host objects or save XThreads again
      num_objects--;
      continue;
    }

    stream->Write<uint32_t>(static_cast<uint32_t>(object->type()));
    if (!object->Save(stream)) {
      XELOGD("Did not save object of type {}", object->type());
      assert_always();

      // Revert backwards and overwrite if a save failed.
      stream->set_offset(prev_offset);
      num_objects--;
    }
  }

  *num_objects_ptr = static_cast<uint32_t>(num_objects);
  return true;
}

// length of a guest timer tick is normally 1 millisecond
void KernelState::SystemClockInterrupt() {
  // todo: set interrupt priority, irql
  auto context = cpu::ThreadState::Get()->context();

  auto kpcr = GetKPCR(context);

  auto cpu_num = GetPCRCpuNum(kpcr);

  // only cpu 0 updates timestamp bundle + timers
  if (cpu_num == 0) {
    X_TIME_STAMP_BUNDLE* lpKeTimeStampBundle =
        memory_->TranslateVirtual<X_TIME_STAMP_BUNDLE*>(GetKeTimestampBundle());
    // uint32_t uptime_ms = Clock::QueryGuestUptimeMillis();
    // uint64_t time_imprecise = static_cast<uint64_t>(uptime_ms) * 1000000ULL;

    uint64_t time_imprecise = (lpKeTimeStampBundle->interrupt_time += 10000ULL);
    lpKeTimeStampBundle->system_time += 10000ULL;
    lpKeTimeStampBundle->tick_count += 1;

    /*
      check timers!
    */

    auto globals =
        context->TranslateVirtual<KernelGuestGlobals*>(GetKernelGuestGlobals());
    context->kernel_state->LockDispatcherAtIrql(context);
    for (auto& timer : globals->running_timers.IterateForward(context)) {
      if (timer.due_time <= time_imprecise) {
        kpcr->timer_pending = 2;  // actual clock interrupt does a lot more
        kpcr->generic_software_interrupt = 2;
        break;
      }
    }
    context->kernel_state->UnlockDispatcherAtIrql(context);
  }

  auto current_thread = kpcr->prcb_data.current_thread.xlat();
  auto idle_thread = kpcr->prcb_data.idle_thread.xlat();
  if (idle_thread != current_thread) {
    auto v16 = current_thread->unk_B4 - 3;
    current_thread->unk_B4 = v16;
    if (v16 <= 0) {
      kpcr->timeslice_ended = 2;
      kpcr->generic_software_interrupt = 2;
    }
  }
  GenericExternalInterruptEpilog(context);
}
void KernelState::GenericExternalInterruptEpilog(
    cpu::ppc::PPCContext* context) {
  auto kpcr = GetKPCR(context);
  uint32_t r3 = kpcr->current_irql;
  uint32_t r4 = kpcr->software_interrupt_state;
  if (r3 < r4) {
    xboxkrnl::xeDispatchProcedureCallInterrupt(r3, r4, context);
  }
}

uint32_t KernelState::GetKeTimestampBundle() {
  return this->GetKernelGuestGlobals() +
         offsetof(KernelGuestGlobals, KeTimestampBundle);
}

bool KernelState::Restore(ByteStream* stream) {
  // Check the magic value.
  if (stream->Read<uint32_t>() != kKernelSaveSignature) {
    return false;
  }

  // Restore the object table
  object_table_.Restore(stream);

  // Read the TLS allocation bitmap
  auto num_bitmap_entries = stream->Read<uint32_t>();
  auto& tls_bitmap = tls_bitmap_.data();
  tls_bitmap.resize(num_bitmap_entries);
  for (uint32_t i = 0; i < num_bitmap_entries; i++) {
    tls_bitmap[i] = stream->Read<uint64_t>();
  }

  uint32_t num_threads = stream->Read<uint32_t>();
  XELOGD("Loading {} threads...", num_threads);
  for (uint32_t i = 0; i < num_threads; i++) {
    auto thread = XObject::Restore(this, XObject::Type::Thread, stream);
    if (!thread) {
      // Can't continue the restore or we risk misalignment.
      assert_always();
      return false;
    }
  }

  uint32_t num_objects = stream->Read<uint32_t>();
  XELOGD("Loading {} objects...", num_objects);
  for (uint32_t i = 0; i < num_objects; i++) {
    uint32_t type = stream->Read<uint32_t>();

    auto obj = XObject::Restore(this, XObject::Type(type), stream);
    if (!obj) {
      // Can't continue the restore or we risk misalignment.
      assert_always();
      return false;
    }
  }

  return true;
}

uint8_t KernelState::GetConnectedUsers() const {
  auto input_sys = emulator_->input_system();

  auto lock = input_sys->lock();

  return input_sys->GetConnectedSlots();
}
// todo: definitely need to do more to pretend to be in a dpc
void KernelState::BeginDPCImpersonation(cpu::ppc::PPCContext* context,
                                        DPCImpersonationScope& scope) {
  auto kpcr = GetKPCR(context);
  xenia_assert(kpcr->prcb_data.dpc_active == 0);
  scope.previous_irql_ = kpcr->current_irql;

  kpcr->current_irql = 2;
  kpcr->prcb_data.dpc_active = 1;
}
void KernelState::EndDPCImpersonation(cpu::ppc::PPCContext* context,
                                      DPCImpersonationScope& end_scope) {
  auto kpcr = GetKPCR(context);
  xenia_assert(kpcr->prcb_data.dpc_active == 1);
  kpcr->current_irql = end_scope.previous_irql_;
  kpcr->prcb_data.dpc_active = 0;
}

struct IPIParams {
  cpu::Processor* processor_;
  uint32_t source_;
  uint32_t interrupt_callback_data_;
  uint32_t interrupt_callback_;
};
void KernelState::GraphicsInterruptDPC(PPCContext* context) {
  uint32_t callback = static_cast<uint32_t>(context->r[5]);
  uint64_t callback_data[] = {context->r[4], context->r[6]};
  auto kpcr = GetKPCR(context);
  xenia_assert(kpcr->processtype_value_in_dpc == X_PROCTYPE_IDLE);
  xenia_assert(kpcr->prcb_data.dpc_active != 0);
  xenia_assert(context->msr == 0x9030);
  xenia_assert(context->kernel_state->GetPCRCpuNum(kpcr) == 2);
  if (callback) {
    xboxkrnl::xeKeSetCurrentProcessType(X_PROCTYPE_TITLE, context);
    context->processor->Execute(context->thread_state(), callback,
                                callback_data, countof(callback_data));
    xenia_assert(GetKPCR(context)->prcb_data.dpc_active != 0);
    xboxkrnl::xeKeSetCurrentProcessType(X_PROCTYPE_IDLE, context);
  }
}

void KernelState::CPInterruptIPI(void* ud) {
  IPIParams* params = reinterpret_cast<IPIParams*>(ud);
  auto current_ts = cpu::ThreadState::Get();
  auto current_context = current_ts->context();
  auto kernel_state = current_context->kernel_state;

  auto pcr =
      current_context->TranslateVirtualGPR<X_KPCR*>(current_context->r[13]);
  auto kthread =
      current_context->TranslateVirtual(pcr->prcb_data.current_thread);

  auto guest_globals = kernel_state->GetKernelGuestGlobals(current_context);

  // in real xboxkrnl, it passes 0 for both args to the dpc,
  // but its more convenient for us to pass the interrupt
  guest_globals->graphics_interrupt_dpc.context = params->source_;
  xboxkrnl::xeKeInsertQueueDpc(
      &guest_globals->graphics_interrupt_dpc, params->interrupt_callback_,
      params->interrupt_callback_data_, current_context);

  delete params;

  // this causes all games to freeze!
  //  it is an external interrupt though, but i guess we need to wait until
  //  it exits an interrupt context
  // GenericExternalInterruptEpilog(current_context);
}

void KernelState::EmulateCPInterruptDPC(uint32_t interrupt_callback,
                                        uint32_t interrupt_callback_data,
                                        uint32_t source, uint32_t cpu) {
  if (!interrupt_callback) {
    return;
  }

  // auto thread = kernel::XThread::GetCurrentThread();
  // assert_not_null(thread);

  // Pick a CPU, if needed. We're going to guess 2. Because.
  if (cpu == 0xFFFFFFFF) {
    cpu = 2;
  }
  // thread->SetActiveCpu(cpu);

  /*
    in reality, our interrupt is a callback that is called in a dpc which is
    scheduled by the actual interrupt

    we need to impersonate a dpc
  */

  IPIParams* params = new IPIParams();
  params->processor_ = processor();
  params->source_ = source;
  params->interrupt_callback_ = interrupt_callback;
  params->interrupt_callback_data_ = interrupt_callback_data;
  auto hwthread = processor_->GetCPUThread(cpu);
  while (!hwthread->TrySendInterruptFromHost(CPInterruptIPI, params)) {
  }
}

X_KSPINLOCK* KernelState::GetDispatcherLock(cpu::ppc::PPCContext* context) {
  return &context
              ->TranslateVirtual<KernelGuestGlobals*>(GetKernelGuestGlobals())
              ->dispatcher_lock;
}

uint32_t KernelState::LockDispatcher(cpu::ppc::PPCContext* context) {
  return xboxkrnl::xeKeKfAcquireSpinLock(context, GetDispatcherLock(context),
                                         true);
}

void KernelState::UnlockDispatcher(cpu::ppc::PPCContext* context,
                                   uint32_t irql) {
  xboxkrnl::xeKeKfReleaseSpinLock(context, GetDispatcherLock(context), irql,
                                  true);
}

void KernelState::LockDispatcherAtIrql(cpu::ppc::PPCContext* context) {
  xboxkrnl::xeKeKfAcquireSpinLock(context, GetDispatcherLock(context), false);
}

void KernelState::UnlockDispatcherAtIrql(cpu::ppc::PPCContext* context) {
  xboxkrnl::xeKeKfReleaseSpinLock(context, GetDispatcherLock(context), 0,
                                  false);
}

uint32_t KernelState::ReferenceObjectByHandle(cpu::ppc::PPCContext* context,
                                              uint32_t handle,
                                              uint32_t guest_object_type,
                                              uint32_t* object_out) {
  return xboxkrnl::xeObReferenceObjectByHandle(handle, guest_object_type,
                                               object_out);
}
void KernelState::DereferenceObject(cpu::ppc::PPCContext* context,
                                    uint32_t object) {
  xboxkrnl::xeObDereferenceObject(context, object);
}

void KernelState::AssertDispatcherLocked(cpu::ppc::PPCContext* context) {
  xenia_assert(
      context->TranslateVirtual<KernelGuestGlobals*>(GetKernelGuestGlobals())
          ->dispatcher_lock.pcr_of_owner ==
      static_cast<uint32_t>(context->r[13]));
}

void KernelState::UpdateUsedUserProfiles() {
  const uint8_t used_slots_bitmask = GetConnectedUsers();

  for (uint32_t i = 1; i < 4; i++) {
    bool is_used = used_slots_bitmask & (1 << i);

    if (IsUserSignedIn(i) && !is_used) {
      user_profiles_.erase(i);
      BroadcastNotification(0x12, 0);
    }

    if (!IsUserSignedIn(i) && is_used) {
      user_profiles_.emplace(i, std::make_unique<xam::UserProfile>(i));
      BroadcastNotification(0x12, 0);
    }
  }
}

uint32_t KernelState::AllocateInternalHandle(void* ud) {
  std::unique_lock lock{this->internal_handle_table_mutex_};

  uint32_t new_id = current_internal_handle_++;

  while (internal_handles_.count(new_id) == 1) {
    new_id = current_internal_handle_++;
  }
  internal_handles_[new_id] = ud;
  return new_id;
}
void* KernelState::_FreeInternalHandle(uint32_t id) {
  std::unique_lock lock{this->internal_handle_table_mutex_};
  auto iter = internal_handles_.find(id);
  xenia_assert(iter != internal_handles_.end());
  auto result = iter->second;
  internal_handles_.erase(iter);
  return result;
}
X_KPCR_PAGE* KernelState::KPCRPageForCpuNumber(uint32_t i) {
  return memory()->TranslateVirtual<X_KPCR_PAGE*>(processor()->GetPCRForCPU(i));
}

X_STATUS KernelState::ContextSwitch(PPCContext* context, X_KTHREAD* guest) {
  // todo: disable interrupts here!
  // this is incomplete
  auto pre_swap = [this, context, guest]() {
    auto kpcr = GetKPCR();

    guest->thread_state = 2;
    auto stkbase = guest->stack_base;
    auto stklim = guest->stack_limit;
    // it sets r1 to this? i dont think we need, because we have different
    // contexts
    auto kstask = guest->stack_kernel;

    auto thrd_tls = guest->tls_address;

    uint64_t old_msr = context->msr;
    context->DisableEI();

    kpcr->stack_base_ptr = stkbase;
    kpcr->stack_end_ptr = stklim;
    kpcr->tls_ptr = thrd_tls;

    guest->unk_90 += 1;
    xenia_assert(kpcr->prcb_data.enqueued_processor_threads_lock.pcr_of_owner ==
                 context->HostToGuestVirtual(kpcr));
    context->msr = old_msr;
    kpcr->prcb_data.enqueued_processor_threads_lock.pcr_of_owner = 0;
    kpcr->apc_software_interrupt_state =
        guest->deferred_apc_software_interrupt_state;
  };
  X_HANDLE host_handle;

  xenia_assert(GetKPCR(context)->prcb_data.current_thread.xlat() == guest);

  auto old_kpcr = GetKPCR(context);

  if (!object_table()->HostHandleForGuestObject(
          context->HostToGuestVirtual(guest), host_handle)) {
    xenia_assert(GetKPCR(context)->prcb_data.idle_thread.xlat() == guest);
    // if theres no host object for this guest thread, its definitely the idle
    // thread for this processor
    xenia_assert(guest->process_type == X_PROCTYPE_IDLE &&
                 guest->process_type_dup == X_PROCTYPE_IDLE &&
                 guest->process == GetIdleProcess());
    auto prcb = context->TranslateVirtual(guest->a_prcb_ptr);

    xenia_assert(prcb == &GetKPCR(context)->prcb_data);

    auto hw_thread = processor()->GetCPUThread(prcb->current_cpu);
    pre_swap();
    XThread::SetCurrentThread(nullptr);
    GetKPCR(context)->prcb_data.current_thread = guest;
    hw_thread->YieldToScheduler();
  } else {
    auto xthrd = object_table()->LookupObject<XThread>(host_handle).release();
    pre_swap();

    xthrd->thread_state()->context()->r[13] = context->r[13];

    xthrd->SwitchToDirect();
  }
  if (GetKPCR(context) != old_kpcr) {
    XELOGE("Thread was switched from one HW thread to another.");
  }
  // XThread::SetCurrentThread(saved_currthread);

  // r31 after the swap = our thread

  X_KTHREAD* thread_to_load_from = GetKThread(context);
  xenia_assert(thread_to_load_from != guest);
  auto r3 = thread_to_load_from->unk_A4;
  auto wait_result = thread_to_load_from->wait_result;
  GetKPCR(context)->current_irql = r3;
  auto intstate = GetKPCR(context)->software_interrupt_state;
  if (r3 < intstate) {
    xboxkrnl::xeDispatchProcedureCallInterrupt(r3, intstate, context);
  }
  return wait_result;
}
cpu::XenonInterruptController* KernelState::InterruptControllerFromPCR(
    cpu::ppc::PPCContext* context, X_KPCR* pcr) {
  uint32_t cpunum = kernel_state()->GetPCRCpuNum(pcr);
  auto hwthread = processor()->GetCPUThread(cpunum);
  return hwthread->interrupt_controller();
}

void KernelState::SetCurrentInterruptPriority(cpu::ppc::PPCContext* context,
                                              X_KPCR* pcr, uint32_t priority) {
  auto ic = kernel_state()->InterruptControllerFromPCR(context, pcr);
  ic->WriteRegisterOffset(8, static_cast<uint64_t>(priority));
  uint64_t ack = ic->ReadRegisterOffset(8);
}
uint32_t KernelState::GetKernelTickCount() {
  return memory()
      ->TranslateVirtual<X_TIME_STAMP_BUNDLE*>(GetKeTimestampBundle())
      ->tick_count;
}
uint64_t KernelState::GetKernelSystemTime() {
  return memory()
      ->TranslateVirtual<X_TIME_STAMP_BUNDLE*>(GetKeTimestampBundle())
      ->system_time;
}
uint64_t KernelState::GetKernelInterruptTime() {
  return memory()
      ->TranslateVirtual<X_TIME_STAMP_BUNDLE*>(GetKeTimestampBundle())
      ->interrupt_time;
}
void KernelState::KernelIdleProcessFunction(cpu::ppc::PPCContext* context) {
  context->kernel_state = kernel_state();
  auto kpcr = GetKPCR(context);
  auto kthread = GetKThread(context);
  while (true) {
    kpcr->prcb_data.running_idle_thread = kpcr->prcb_data.idle_thread;

    while (!kpcr->generic_software_interrupt) {
      xenia_assert(context->ExternalInterruptsEnabled());

      xenia_assert(GetKThread(context) == kthread);
      xenia_assert(kpcr->current_irql == IRQL_DISPATCH);
      context->CheckInterrupt();
      _mm_pause();
    }

    /*
      it doesnt call this function in normal kernel, but the code just looks to
      be it inlined
      pass true so that the function does not reinstert the idle thread into the
      ready list
    */
    xboxkrnl::xeHandleDPCsAndThreadSwapping(context, true);
  }
}

void KernelState::KernelDecrementerInterrupt(void* ud) {
  auto context = cpu::ThreadState::GetContext();
  auto kpcr = GetKPCR(context);
  uint32_t r5 = kpcr->unk_19;
  uint32_t r3 = kpcr->current_irql;
  uint32_t r6 = 0x7FFFFFFF;
  uint32_t r7 = 2;
  auto cpu = context->processor->GetCPUThread(
      context->kernel_state->GetPCRCpuNum(kpcr));
  cpu->SetDecrementerTicks(r6);
  if (r5 == 0) {
    return;
  }
  kpcr->generic_software_interrupt = r7;
  kpcr->unk_1B = r7;
  kpcr->timeslice_ended = r7;
  uint32_t r4 = kpcr->software_interrupt_state;
  if (r3 < 2 && r3 < r4) {
    xboxkrnl::xeDispatchProcedureCallInterrupt(r3, r4, context);
  }
}

KernelGuestGlobals* KernelState::GetKernelGuestGlobals(
    cpu::ppc::PPCContext* context) {
  return context->TranslateVirtual<KernelGuestGlobals*>(
      GetKernelGuestGlobals());
}
}  // namespace kernel
}  // namespace xe
