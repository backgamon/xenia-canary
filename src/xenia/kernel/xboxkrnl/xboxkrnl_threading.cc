/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include <algorithm>
#include <vector>
#include "xenia/base/atomic.h"
#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xmutant.h"
#include "xenia/kernel/xsemaphore.h"
#include "xenia/kernel/xthread.h"
#include "xenia/kernel/xtimer.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

// r13 + 0x100: pointer to thread local state
// Thread local state:
//   0x058: kernel time
//   0x14C: thread id
//   0x150: if >0 then error states don't get set
//   0x160: last error

// GetCurrentThreadId:
// lwz       r11, 0x100(r13)
// lwz       r3, 0x14C(r11)

// RtlGetLastError:
// lwz r11, 0x150(r13)
// if (r11 == 0) {
//   lwz r11, 0x100(r13)
//   stw r3, 0x160(r11)
// }

// RtlSetLastError:
// lwz r11, 0x150(r13)
// if (r11 == 0) {
//   lwz r11, 0x100(r13)
//   stw r3, 0x160(r11)
// }

// RtlSetLastNTError:
// r3 = RtlNtStatusToDosError(r3)
// lwz r11, 0x150(r13)
// if (r11 == 0) {
//   lwz r11, 0x100(r13)
//   stw r3, 0x160(r11)
// }

template <typename T>
object_ref<T> LookupNamedObject(KernelState* kernel_state,
                                uint32_t obj_attributes_ptr) {
  // If the name exists and its type matches, we can return that (ref+1)
  // with a success of NAME_EXISTS.
  // If the name exists and its type doesn't match, we do NAME_COLLISION.
  // Otherwise, we add like normal.
  if (!obj_attributes_ptr) {
    return nullptr;
  }
  auto obj_attributes =
      kernel_state->memory()->TranslateVirtual<X_OBJECT_ATTRIBUTES*>(
          obj_attributes_ptr);
  assert_true(obj_attributes->name_ptr != 0);
  auto name = util::TranslateAnsiStringAddress(kernel_state->memory(),
                                               obj_attributes->name_ptr);
  if (!name.empty()) {
    X_HANDLE handle = X_INVALID_HANDLE_VALUE;
    X_RESULT result =
        kernel_state->object_table()->GetObjectByName(name, &handle);
    if (XSUCCEEDED(result)) {
      // Found something! It's been retained, so return.
      auto obj = kernel_state->object_table()->LookupObject<T>(handle);
      if (obj) {
        // The caller will do as it likes.
        obj->ReleaseHandle();
        return obj;
      }
    }
  }
  return nullptr;
}

uint32_t ExCreateThread(xe::be<uint32_t>* handle_ptr, uint32_t stack_size,
                        xe::be<uint32_t>* thread_id_ptr,
                        uint32_t xapi_thread_startup, uint32_t start_address,
                        uint32_t start_context, uint32_t creation_flags) {
  // Invalid Link
  // http://jafile.com/uploads/scoop/main.cpp.txt
  // DWORD
  // LPHANDLE Handle,
  // DWORD    StackSize,
  // LPDWORD  ThreadId,
  // LPVOID   XapiThreadStartup, ?? often 0
  // LPVOID   StartAddress,
  // LPVOID   StartContext,
  // DWORD    CreationFlags // 0x80?

  auto kernel_state_var = kernel_state();
  // xenia_assert((creation_flags & 2) == 0);  // creating system thread?
  if (creation_flags & 2) {
    XELOGE("Guest is creating a system thread!");
  }

  uint32_t thread_process = (creation_flags & 2)
                                ? kernel_state_var->GetSystemProcess()
                                : kernel_state_var->GetTitleProcess();
  X_KPROCESS* target_process =
      kernel_state_var->memory()->TranslateVirtual<X_KPROCESS*>(thread_process);
  // Inherit default stack size
  uint32_t actual_stack_size = stack_size;

  if (actual_stack_size == 0) {
    actual_stack_size = target_process->kernel_stack_size;
  }

  // Stack must be aligned to 16kb pages
  actual_stack_size =
      std::max((uint32_t)0x4000, ((actual_stack_size + 0xFFF) & 0xFFFFF000));

  auto thread = object_ref<XThread>(new XThread(
      kernel_state(), actual_stack_size, xapi_thread_startup, start_address,
      start_context, creation_flags, true, false, thread_process));

  X_STATUS result = thread->Create();
  if (XFAILED(result)) {
    // Failed!
    XELOGE("Thread creation failed: {:08X}", result);
    return result;
  }

  if (XSUCCEEDED(result)) {
    if (handle_ptr) {
      if (creation_flags & 0x80) {
        *handle_ptr = thread->guest_object();
      } else {
        *handle_ptr = thread->handle();
      }
    }
    if (thread_id_ptr) {
      *thread_id_ptr = thread->thread_id();
    }
  }
  return result;
}

dword_result_t ExCreateThread_entry(lpdword_t handle_ptr, dword_t stack_size,
                                    lpdword_t thread_id_ptr,
                                    dword_t xapi_thread_startup,
                                    lpvoid_t start_address,
                                    lpvoid_t start_context,
                                    dword_t creation_flags) {
  return ExCreateThread(handle_ptr, stack_size, thread_id_ptr,
                        xapi_thread_startup, start_address, start_context,
                        creation_flags);
}
DECLARE_XBOXKRNL_EXPORT1(ExCreateThread, kThreading, kImplemented);

uint32_t ExTerminateThread(uint32_t exit_code) {
  XThread* thread = XThread::GetCurrentThread();

  // NOTE: this kills us right now. We won't return from it.
  return thread->Exit(exit_code);
}

dword_result_t ExTerminateThread_entry(dword_t exit_code) {
  return ExTerminateThread(exit_code);
}
DECLARE_XBOXKRNL_EXPORT1(ExTerminateThread, kThreading, kImplemented);

uint32_t NtResumeThread(uint32_t handle, uint32_t* suspend_count_ptr) {
  X_RESULT result = X_STATUS_INVALID_HANDLE;
  uint32_t suspend_count = 0;

  auto thread = kernel_state()->object_table()->LookupObject<XThread>(handle);

  if (thread) {
    if (thread->type() == XObject::Type::Thread) {
      // result = thread->Resume(&suspend_count);
      result = X_STATUS_SUCCESS;
      suspend_count = xeKeResumeThread(cpu::ThreadState::GetContext(),
                                       thread->guest_object<X_KTHREAD>());
    } else {
      return X_STATUS_OBJECT_TYPE_MISMATCH;
    }
  } else {
    return X_STATUS_INVALID_HANDLE;
  }
  if (suspend_count_ptr) {
    *suspend_count_ptr = suspend_count;
  }

  return result;
}

dword_result_t NtResumeThread_entry(dword_t handle,
                                    lpdword_t suspend_count_ptr) {
  uint32_t suspend_count =
      suspend_count_ptr ? static_cast<uint32_t>(*suspend_count_ptr) : 0u;

  const X_RESULT result =
      NtResumeThread(handle, suspend_count_ptr ? &suspend_count : nullptr);

  if (suspend_count_ptr) {
    *suspend_count_ptr = suspend_count;
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(NtResumeThread, kThreading, kImplemented);

dword_result_t KeResumeThread_entry(pointer_t<X_KTHREAD> thread_ptr,
                                    const ppc_context_t& context) {
  return xeKeResumeThread(context, thread_ptr);
}
DECLARE_XBOXKRNL_EXPORT1(KeResumeThread, kThreading, kImplemented);

dword_result_t NtSuspendThread_entry(dword_t handle,
                                     lpdword_t suspend_count_ptr,
                                     const ppc_context_t& context) {
  X_RESULT result = X_STATUS_SUCCESS;
  uint32_t suspend_count = 0;

  auto thread = kernel_state()->object_table()->LookupObject<XThread>(handle);
  if (thread) {
    if (thread->type() == XObject::Type::Thread) {
      auto current_pcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);

      if (current_pcr->prcb_data.current_thread == thread->guest_object() ||
          !thread->guest_object<X_KTHREAD>()->terminated) {
        suspend_count =
            xeKeSuspendThread(context, thread->guest_object<X_KTHREAD>());
      } else {
        return X_STATUS_THREAD_IS_TERMINATING;
      }
    } else {
      return X_STATUS_OBJECT_TYPE_MISMATCH;
    }
  } else {
    return X_STATUS_INVALID_HANDLE;
  }

  if (suspend_count_ptr) {
    *suspend_count_ptr = suspend_count;
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(NtSuspendThread, kThreading, kImplemented);

dword_result_t KeSuspendThread_entry(pointer_t<X_KTHREAD> kthread,
                                     const ppc_context_t& context) {
  return xeKeSuspendThread(context, kthread);
}
DECLARE_XBOXKRNL_EXPORT1(KeSuspendThread, kThreading, kImplemented);

void KeSetCurrentStackPointers_entry(lpvoid_t stack_ptr,
                                     pointer_t<X_KTHREAD> thread,
                                     lpvoid_t stack_alloc_base,
                                     lpvoid_t stack_base, lpvoid_t stack_limit,
                                     const ppc_context_t& context) {
  auto current_thread = XThread::GetCurrentThread();

  auto pcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
  // also supposed to load msr mask, and the current msr with that, and store
  thread->stack_alloc_base = stack_alloc_base.value();
  thread->stack_base = stack_base.value();
  thread->stack_limit = stack_limit.value();
  pcr->stack_base_ptr = stack_base.guest_address();
  pcr->stack_end_ptr = stack_limit.guest_address();
  context->r[1] = stack_ptr.guest_address();

  // If a fiber is set, and the thread matches, reenter to avoid issues with
  // host stack overflowing.
  if (thread->fiber_ptr &&
      current_thread->guest_object() == thread.guest_address()) {
    context->processor->backend()->PrepareForReentry(context.value());
    current_thread->Reenter(static_cast<uint32_t>(context->lr));
  }
}
DECLARE_XBOXKRNL_EXPORT2(KeSetCurrentStackPointers, kThreading, kImplemented,
                         kHighFrequency);

dword_result_t KeSetAffinityThread_entry(lpvoid_t thread_ptr, dword_t affinity,
                                         lpdword_t previous_affinity_ptr) {
  // The Xbox 360, according to disassembly of KeSetAffinityThread, unlike
  // Windows NT, stores the previous affinity via the pointer provided as an
  // argument, not in the return value - the return value is used for the
  // result.
  if (!affinity) {
    return X_STATUS_INVALID_PARAMETER;
  }
  auto thread = XObject::GetNativeObject<XThread>(kernel_state(), thread_ptr);
  if (thread) {
    if (previous_affinity_ptr) {
      *previous_affinity_ptr = uint32_t(1) << thread->active_cpu();
    }
    thread->SetAffinity(affinity);
  }
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(KeSetAffinityThread, kThreading, kImplemented);

dword_result_t KeQueryBasePriorityThread_entry(lpvoid_t thread_ptr) {
  int32_t priority = 0;

  auto thread = XObject::GetNativeObject<XThread>(kernel_state(), thread_ptr);
  if (thread) {
    priority = thread->QueryPriority();
  }

  return priority;
}
DECLARE_XBOXKRNL_EXPORT1(KeQueryBasePriorityThread, kThreading, kImplemented);

dword_result_t KeSetBasePriorityThread_entry(lpvoid_t thread_ptr,
                                             dword_t increment) {
  int32_t prev_priority = 0;
  auto thread = XObject::GetNativeObject<XThread>(kernel_state(), thread_ptr);

  if (thread) {
    prev_priority = thread->QueryPriority();
    thread->SetPriority(increment);
  }

  return prev_priority;
}
DECLARE_XBOXKRNL_EXPORT1(KeSetBasePriorityThread, kThreading, kImplemented);

dword_result_t KeSetDisableBoostThread_entry(pointer_t<X_KTHREAD> thread_ptr,
                                             dword_t disabled) {
  // supposed to acquire dispatcher lock + a prcb lock, all just to exchange
  // this char there is no other special behavior going on in this function,
  // just acquiring locks to do this exchange
  auto old_boost_disabled =
      reinterpret_cast<std::atomic_uint8_t*>(&thread_ptr->boost_disabled)
          ->exchange(static_cast<uint8_t>(disabled));

  return old_boost_disabled;
}
DECLARE_XBOXKRNL_EXPORT1(KeSetDisableBoostThread, kThreading, kImplemented);

uint32_t xeKeGetCurrentProcessType(cpu::ppc::PPCContext* context) {
  auto pcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);

  if (!pcr->prcb_data.dpc_active)
    return context->TranslateVirtual(pcr->prcb_data.current_thread)
        ->process_type;
  return pcr->processtype_value_in_dpc;
}
void xeKeSetCurrentProcessType(uint32_t type, cpu::ppc::PPCContext* context) {
  auto pcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
  if (pcr->prcb_data.dpc_active) {
    pcr->processtype_value_in_dpc = type;
  }
}

dword_result_t KeGetCurrentProcessType_entry(const ppc_context_t& context) {
  return xeKeGetCurrentProcessType(context);
}
DECLARE_XBOXKRNL_EXPORT2(KeGetCurrentProcessType, kThreading, kImplemented,
                         kHighFrequency);

void KeSetCurrentProcessType_entry(dword_t type, const ppc_context_t& context) {
  xeKeSetCurrentProcessType(type, context);
}
DECLARE_XBOXKRNL_EXPORT1(KeSetCurrentProcessType, kThreading, kImplemented);

dword_result_t KeQueryPerformanceFrequency_entry() {
  uint64_t result = Clock::guest_tick_frequency();
  return static_cast<uint32_t>(result);
}
DECLARE_XBOXKRNL_EXPORT2(KeQueryPerformanceFrequency, kThreading, kImplemented,
                         kHighFrequency);

uint32_t KeDelayExecutionThread(uint32_t processor_mode, uint32_t alertable,
                                uint64_t* interval_ptr,
                                cpu::ppc::PPCContext* ctx) {
  XThread* thread = XThread::GetCurrentThread();

  if (alertable) {
    X_STATUS stat = xeProcessUserApcs(ctx);
    if (stat == X_STATUS_USER_APC) {
      return stat;
    }
  }
  X_STATUS result = thread->Delay(processor_mode, alertable, *interval_ptr);

  if (result == X_STATUS_USER_APC) {
    result = xeProcessUserApcs(ctx);
    if (result == X_STATUS_USER_APC) {
      return result;
    }
  }

  return result;
}

dword_result_t KeDelayExecutionThread_entry(dword_t processor_mode,
                                            dword_t alertable,
                                            lpqword_t interval_ptr,
                                            const ppc_context_t& context) {
  uint64_t interval = interval_ptr ? static_cast<uint64_t>(*interval_ptr) : 0u;
  return KeDelayExecutionThread(processor_mode, alertable,
                                interval_ptr ? &interval : nullptr, context);
}
DECLARE_XBOXKRNL_EXPORT3(KeDelayExecutionThread, kThreading, kImplemented,
                         kBlocking, kHighFrequency);

dword_result_t NtYieldExecution_entry(const ppc_context_t& context) {
  return xeNtYieldExecution(context);
}
DECLARE_XBOXKRNL_EXPORT2(NtYieldExecution, kThreading, kImplemented,
                         kHighFrequency);

void KeQuerySystemTime_entry(lpqword_t time_ptr, const ppc_context_t& ctx) {
  uint32_t ts_bundle = ctx->kernel_state->GetKeTimestampBundle();

  *time_ptr =
      ctx->TranslateVirtual<X_TIME_STAMP_BUNDLE*>(ts_bundle)->system_time;
}
DECLARE_XBOXKRNL_EXPORT1(KeQuerySystemTime, kThreading, kImplemented);

// https://msdn.microsoft.com/en-us/library/ms686801
dword_result_t KeTlsAlloc_entry() {
  uint32_t slot = kernel_state()->AllocateTLS();
  XThread::GetCurrentThread()->SetTLSValue(slot, 0);

  return slot;
}
DECLARE_XBOXKRNL_EXPORT1(KeTlsAlloc, kThreading, kImplemented);

// https://msdn.microsoft.com/en-us/library/ms686804
dword_result_t KeTlsFree_entry(dword_t tls_index) {
  if (tls_index == X_TLS_OUT_OF_INDEXES) {
    return 0;
  }

  kernel_state()->FreeTLS(tls_index);
  return 1;
}
DECLARE_XBOXKRNL_EXPORT1(KeTlsFree, kThreading, kImplemented);

// https://msdn.microsoft.com/en-us/library/ms686812
dword_result_t KeTlsGetValue_entry(dword_t tls_index) {
  // xboxkrnl doesn't actually have an error branch - it always succeeds, even
  // if it overflows the TLS.
  uint32_t value = 0;
  if (XThread::GetCurrentThread()->GetTLSValue(tls_index, &value)) {
    return value;
  }

  return 0;
}
DECLARE_XBOXKRNL_EXPORT2(KeTlsGetValue, kThreading, kImplemented,
                         kHighFrequency);

// https://msdn.microsoft.com/en-us/library/ms686818
dword_result_t KeTlsSetValue_entry(dword_t tls_index, dword_t tls_value) {
  // xboxkrnl doesn't actually have an error branch - it always succeeds, even
  // if it overflows the TLS.
  if (XThread::GetCurrentThread()->SetTLSValue(tls_index, tls_value)) {
    return 1;
  }

  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(KeTlsSetValue, kThreading, kImplemented);

void KeInitializeEvent_entry(pointer_t<X_KEVENT> event_ptr, dword_t event_type,
                             dword_t initial_state,
                             const ppc_context_t& context) {
  event_ptr.Zero();
  event_ptr->header.type = event_type;
  event_ptr->header.signal_state = (uint32_t)initial_state;
  util::XeInitializeListHead(&event_ptr->header.wait_list, context);
  auto ev =
      XObject::GetNativeObject<XEvent>(kernel_state(), event_ptr, event_type);
  if (!ev) {
    assert_always();
    return;
  }
}
DECLARE_XBOXKRNL_EXPORT1(KeInitializeEvent, kThreading, kImplemented);

dword_result_t KeSetEvent_entry(pointer_t<X_KEVENT> event_ptr,
                                dword_t increment, dword_t wait,
                                const ppc_context_t& context) {
  return xeKeSetEvent(context, event_ptr, increment, wait);
}
DECLARE_XBOXKRNL_EXPORT2(KeSetEvent, kThreading, kImplemented, kHighFrequency);

dword_result_t KePulseEvent_entry(pointer_t<X_KEVENT> event_ptr,
                                  dword_t increment, dword_t wait) {
  xenia_assert(false);
  return 0;
}
DECLARE_XBOXKRNL_EXPORT2(KePulseEvent, kThreading, kImplemented,
                         kHighFrequency);

dword_result_t KeResetEvent_entry(pointer_t<X_KEVENT> event_ptr,
                                  const ppc_context_t& context) {
  return xeKeResetEvent(context, event_ptr);
}
DECLARE_XBOXKRNL_EXPORT1(KeResetEvent, kThreading, kImplemented);

dword_result_t NtCreateEvent_entry(
    lpdword_t handle_ptr, pointer_t<X_OBJECT_ATTRIBUTES> obj_attributes_ptr,
    dword_t event_type, dword_t initial_state) {
  // Check for an existing timer with the same name.
  auto existing_object =
      LookupNamedObject<XEvent>(kernel_state(), obj_attributes_ptr);
  if (existing_object) {
    if (existing_object->type() == XObject::Type::Event) {
      if (handle_ptr) {
        existing_object->RetainHandle();
        *handle_ptr = existing_object->handle();
      }
      return X_STATUS_OBJECT_NAME_EXISTS;
    } else {
      return X_STATUS_INVALID_HANDLE;
    }
  }

  auto ev = object_ref<XEvent>(new XEvent(kernel_state()));
  ev->Initialize(!event_type, !!initial_state);

  // obj_attributes may have a name inside of it, if != NULL.
  if (obj_attributes_ptr) {
    ev->SetAttributes(obj_attributes_ptr);
  }

  if (handle_ptr) {
    *handle_ptr = ev->handle();
  }
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtCreateEvent, kThreading, kImplemented);

uint32_t xeNtSetEvent(uint32_t handle, xe::be<uint32_t>* previous_state_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto ev = kernel_state()->object_table()->LookupObject<XEvent>(handle);
  if (ev) {
    // d3 ros does this
    if (ev->type() != XObject::Type::Event) {
      return X_STATUS_OBJECT_TYPE_MISMATCH;
    }
    int32_t was_signalled = xeKeSetEvent(cpu::ThreadState::GetContext(),
                                         ev->guest_object<X_KEVENT>(), 1, 0);
    if (previous_state_ptr) {
      *previous_state_ptr = static_cast<uint32_t>(was_signalled);
    }
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

dword_result_t NtSetEvent_entry(dword_t handle, lpdword_t previous_state_ptr) {
  return xeNtSetEvent(handle, previous_state_ptr);
}
DECLARE_XBOXKRNL_EXPORT2(NtSetEvent, kThreading, kImplemented, kHighFrequency);

dword_result_t NtPulseEvent_entry(dword_t handle,
                                  lpdword_t previous_state_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  xenia_assert(false);

  return result;
}
DECLARE_XBOXKRNL_EXPORT2(NtPulseEvent, kThreading, kImplemented,
                         kHighFrequency);
dword_result_t NtQueryEvent_entry(dword_t handle, lpdword_t out_struc) {
  X_STATUS result = X_STATUS_SUCCESS;

  xenia_assert(false);

  return result;
}
DECLARE_XBOXKRNL_EXPORT2(NtQueryEvent, kThreading, kImplemented,
                         kHighFrequency);
uint32_t xeNtClearEvent(uint32_t handle) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto ev = kernel_state()->object_table()->LookupObject<XEvent>(handle);
  if (ev) {
    ev->Reset();
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

dword_result_t NtClearEvent_entry(dword_t handle) {
  return xeNtClearEvent(handle);
}
DECLARE_XBOXKRNL_EXPORT2(NtClearEvent, kThreading, kImplemented,
                         kHighFrequency);

void xeKeInitializeSemaphore(X_KSEMAPHORE* semaphore, int count, int limit) {
  semaphore->header.type = 5;  // SemaphoreObject
  semaphore->header.signal_state = (uint32_t)count;
  semaphore->limit = (uint32_t)limit;
  util::XeInitializeListHead(

      &semaphore->header.wait_list, kernel_memory());

  auto sem = XObject::GetNativeObject<XSemaphore>(kernel_state(), semaphore,
                                                  5 /* SemaphoreObject */);
  if (!sem) {
    assert_always();
    return;
  }
}
// https://msdn.microsoft.com/en-us/library/windows/hardware/ff552150(v=vs.85).aspx
void KeInitializeSemaphore_entry(pointer_t<X_KSEMAPHORE> semaphore_ptr,
                                 dword_t count, dword_t limit) {
  xeKeInitializeSemaphore(semaphore_ptr, count, limit);
}
DECLARE_XBOXKRNL_EXPORT1(KeInitializeSemaphore, kThreading, kImplemented);

uint32_t xeKeReleaseSemaphore(X_KSEMAPHORE* semaphore_ptr, uint32_t increment,
                              uint32_t adjustment, uint32_t wait) {
  auto sem =
      XObject::GetNativeObject<XSemaphore>(kernel_state(), semaphore_ptr);
  if (!sem) {
    assert_always();
    return 0;
  }

  // TODO(benvanik): increment thread priority?
  // TODO(benvanik): wait?

  return sem->ReleaseSemaphore(adjustment);
}

dword_result_t KeReleaseSemaphore_entry(pointer_t<X_KSEMAPHORE> semaphore_ptr,
                                        dword_t increment, dword_t adjustment,
                                        dword_t wait) {
  return xeKeReleaseSemaphore(semaphore_ptr, increment, adjustment, wait);
}
DECLARE_XBOXKRNL_EXPORT1(KeReleaseSemaphore, kThreading, kImplemented);

dword_result_t NtCreateSemaphore_entry(lpdword_t handle_ptr,
                                       lpvoid_t obj_attributes_ptr,
                                       dword_t count, dword_t limit) {
  // Check for an existing semaphore with the same name.
  auto existing_object =
      LookupNamedObject<XSemaphore>(kernel_state(), obj_attributes_ptr);
  if (existing_object) {
    if (existing_object->type() == XObject::Type::Semaphore) {
      if (handle_ptr) {
        existing_object->RetainHandle();
        *handle_ptr = existing_object->handle();
      }
      return X_STATUS_OBJECT_NAME_EXISTS;
    } else {
      return X_STATUS_INVALID_HANDLE;
    }
  }

  auto sem = object_ref<XSemaphore>(new XSemaphore(kernel_state()));
  if (!sem->Initialize((int32_t)count, (int32_t)limit)) {
    if (handle_ptr) {
      *handle_ptr = 0;
    }
    sem->ReleaseHandle();
    return X_STATUS_INVALID_PARAMETER;
  }

  // obj_attributes may have a name inside of it, if != NULL.
  if (obj_attributes_ptr) {
    sem->SetAttributes(obj_attributes_ptr);
  }

  if (handle_ptr) {
    *handle_ptr = sem->handle();
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtCreateSemaphore, kThreading, kImplemented);

dword_result_t NtReleaseSemaphore_entry(dword_t sem_handle,
                                        dword_t release_count,
                                        lpdword_t previous_count_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;
  int32_t previous_count = 0;

  auto sem =
      kernel_state()->object_table()->LookupObject<XSemaphore>(sem_handle);
  if (sem) {
    previous_count = sem->ReleaseSemaphore((int32_t)release_count);
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }
  if (previous_count_ptr) {
    *previous_count_ptr = (uint32_t)previous_count;
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT2(NtReleaseSemaphore, kThreading, kImplemented,
                         kHighFrequency);

dword_result_t NtCreateMutant_entry(
    lpdword_t handle_out, pointer_t<X_OBJECT_ATTRIBUTES> obj_attributes,
    dword_t initial_owner) {
  // Check for an existing timer with the same name.
  auto existing_object = LookupNamedObject<XMutant>(
      kernel_state(), obj_attributes.guest_address());
  if (existing_object) {
    if (existing_object->type() == XObject::Type::Mutant) {
      if (handle_out) {
        existing_object->RetainHandle();
        *handle_out = existing_object->handle();
      }
      return X_STATUS_OBJECT_NAME_EXISTS;
    } else {
      return X_STATUS_INVALID_HANDLE;
    }
  }

  auto mutant = object_ref<XMutant>(new XMutant(kernel_state()));
  mutant->Initialize(initial_owner ? true : false, obj_attributes);

  // obj_attributes may have a name inside of it, if != NULL.
  if (obj_attributes) {
    mutant->SetAttributes(obj_attributes);
  }

  if (handle_out) {
    *handle_out = mutant->handle();
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtCreateMutant, kThreading, kImplemented);

dword_result_t NtReleaseMutant_entry(dword_t mutant_handle,
                                     lpdword_t previous_count,
                                     const ppc_context_t& context) {
  auto kernel = context->kernel_state;
  uint32_t object = 0;
  uint32_t ref_res = kernel->ReferenceObjectByHandle(
      context, mutant_handle,
      kernel->GetKernelGuestGlobals() +
          offsetof(KernelGuestGlobals, ExMutantObjectType),
      &object);
  if ((int)ref_res < 0) {
    return ref_res;
  }

  X_KMUTANT* mutant = context->TranslateVirtual<X_KMUTANT*>(object);

  auto prev_count = xeKeReleaseMutant(context, mutant, 1, 0, 0);

  kernel->DereferenceObject(context, object);

  if (previous_count) {
    *previous_count = prev_count;
  }
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(NtReleaseMutant, kThreading, kImplemented);

void xeKeInitializeMutant(X_KMUTANT* mutant, bool initially_owned,
                          xe::cpu::ppc::PPCContext* context) {
  mutant->header.type = 2;

  if (initially_owned) {
    auto v4 = context->TranslateVirtual(
        context->TranslateVirtualGPR<X_KPCR*>(context->r[13])
            ->prcb_data.current_thread);
    mutant->header.signal_state = 0;
    mutant->owner = context->HostToGuestVirtual(v4);
    auto old_irql = kernel_state()->LockDispatcher(context);

    util::XeInsertHeadList(v4->mutants_list.blink_ptr, &mutant->unk_list,
                           context);

    kernel_state()->UnlockDispatcher(context, old_irql);

  } else {
    mutant->owner = 0;
    mutant->header.signal_state = 1;
  }
  util::XeInitializeListHead(&mutant->header.wait_list, context);
  mutant->abandoned = 0;
}
void KeInitializeMutant_entry(pointer_t<X_KMUTANT> mutant,
                              dword_t initially_owned,
                              const ppc_context_t& context) {
  xeKeInitializeMutant(mutant, static_cast<unsigned char>(initially_owned),
                       context);
}

DECLARE_XBOXKRNL_EXPORT1(KeInitializeMutant, kThreading, kImplemented);

void xeKeInitializeTimerEx(X_KTIMER* timer, uint32_t type, uint32_t proctype,
                           PPCContext* context) {
  xenia_assert(proctype < 3);
  xenia_assert(type == 0 || type == 1);
  // other fields are unmodified, they must carry through multiple calls of
  // initialize
  timer->header.process_type = proctype;
  timer->header.inserted = 0;
  timer->header.type = type + 8;
  timer->header.signal_state = 0;
  timer->table_bucket_entry.blink_ptr = 0U;
  timer->table_bucket_entry.flink_ptr = 0U;
  // todo: should initialize wait list in header
  util::XeInitializeListHead(&timer->header.wait_list, context);
  timer->due_time = 0;
  timer->period = 0;
}

void KeInitializeTimerEx_entry(pointer_t<X_KTIMER> timer, dword_t type,
                               dword_t proctype, const ppc_context_t& context) {
  xeKeInitializeTimerEx(timer, type, proctype & 0xFF, context);
}
DECLARE_XBOXKRNL_EXPORT1(KeInitializeTimerEx, kThreading, kImplemented);

dword_result_t NtCreateTimer_entry(lpdword_t handle_ptr,
                                   lpvoid_t obj_attributes_ptr,
                                   dword_t timer_type) {
  // timer_type = NotificationTimer (0) or SynchronizationTimer (1)

  // Check for an existing timer with the same name.
  auto existing_object =
      LookupNamedObject<XTimer>(kernel_state(), obj_attributes_ptr);
  if (existing_object) {
    if (existing_object->type() == XObject::Type::Timer) {
      if (handle_ptr) {
        existing_object->RetainHandle();
        *handle_ptr = existing_object->handle();
      }
      return X_STATUS_OBJECT_NAME_EXISTS;
    } else {
      return X_STATUS_INVALID_HANDLE;
    }
  }

  auto timer = object_ref<XTimer>(new XTimer(kernel_state()));
  timer->Initialize(timer_type);

  // obj_attributes may have a name inside of it, if != NULL.
  if (obj_attributes_ptr) {
    timer->SetAttributes(obj_attributes_ptr);
  }

  if (handle_ptr) {
    *handle_ptr = timer->handle();
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(NtCreateTimer, kThreading, kImplemented);

dword_result_t NtSetTimerEx_entry(dword_t timer_handle, lpqword_t due_time_ptr,
                                  lpvoid_t routine_ptr /*PTIMERAPCROUTINE*/,
                                  dword_t unk_one, lpvoid_t routine_arg,
                                  dword_t resume, dword_t period_ms,
                                  dword_t unk_zero) {
  assert_true(unk_one == 1);
  assert_true(unk_zero == 0);

  uint64_t due_time = *due_time_ptr;

  X_STATUS result = X_STATUS_SUCCESS;

  auto timer =
      kernel_state()->object_table()->LookupObject<XTimer>(timer_handle);
  if (timer) {
    result =
        timer->SetTimer(due_time, period_ms, routine_ptr.guest_address(),
                        routine_arg.guest_address(), resume ? true : false);
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(NtSetTimerEx, kThreading, kImplemented);

dword_result_t NtCancelTimer_entry(dword_t timer_handle,
                                   lpdword_t current_state_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto timer =
      kernel_state()->object_table()->LookupObject<XTimer>(timer_handle);
  if (timer) {
    result = timer->Cancel();
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }
  if (current_state_ptr) {
    *current_state_ptr = 0;
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT1(NtCancelTimer, kThreading, kImplemented);

uint32_t xeKeWaitForSingleObject(void* object_ptr, uint32_t wait_reason,
                                 uint32_t processor_mode, uint32_t alertable,
                                 uint64_t* timeout_ptr) {
  return xeKeWaitForSingleObject(
      cpu::ThreadState::GetContext(), (X_DISPATCH_HEADER*)object_ptr,
      wait_reason, processor_mode, alertable, (int64_t*)timeout_ptr);
}

dword_result_t KeWaitForSingleObject_entry(lpvoid_t object_ptr,
                                           dword_t wait_reason,
                                           dword_t processor_mode,
                                           dword_t alertable,
                                           lpqword_t timeout_ptr) {
  uint64_t timeout = timeout_ptr ? static_cast<uint64_t>(*timeout_ptr) : 0u;
  return xeKeWaitForSingleObject(object_ptr, wait_reason, processor_mode,
                                 alertable, timeout_ptr ? &timeout : nullptr);
}
DECLARE_XBOXKRNL_EXPORT3(KeWaitForSingleObject, kThreading, kImplemented,
                         kBlocking, kHighFrequency);

uint32_t NtWaitForSingleObjectEx(uint32_t object_handle, uint32_t wait_mode,
                                 uint32_t alertable, uint64_t* timeout_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  auto object =
      kernel_state()->object_table()->LookupObject<XObject>(object_handle);
  if (object) {
    uint64_t timeout = timeout_ptr ? static_cast<uint64_t>(*timeout_ptr) : 0u;

    return xeKeWaitForSingleObject(object->guest_object<X_DISPATCH_HEADER>(), 3,
                                   wait_mode, alertable, &timeout);

  } else {
    result = X_STATUS_INVALID_HANDLE;
  }

  return result;
}

dword_result_t NtWaitForSingleObjectEx_entry(dword_t object_handle,
                                             dword_t wait_mode,
                                             dword_t alertable,
                                             lpqword_t timeout_ptr) {
  uint64_t timeout = timeout_ptr ? static_cast<uint64_t>(*timeout_ptr) : 0u;
  return NtWaitForSingleObjectEx(object_handle, wait_mode, alertable,
                                 timeout_ptr ? &timeout : nullptr);
}
DECLARE_XBOXKRNL_EXPORT3(NtWaitForSingleObjectEx, kThreading, kImplemented,
                         kBlocking, kHighFrequency);

dword_result_t KeWaitForMultipleObjects_entry(
    dword_t count, lpdword_t objects_ptr, dword_t wait_type,
    dword_t wait_reason, dword_t processor_mode, dword_t alertable,
    lpqword_t timeout_ptr, lpvoid_t wait_block_array_ptr,
    const ppc_context_t& context) {
  assert_true(wait_type <= 1);

  assert_true(count <= 64);
  uint32_t old_irql = kernel_state()->LockDispatcher(context);
  object_ref<XObject> objects[64];
  {
    auto crit = global_critical_region::AcquireDirect();
    for (uint32_t n = 0; n < count; n++) {
      auto object_ptr = kernel_memory()->TranslateVirtual(objects_ptr[n]);
      auto object_ref = XObject::GetNativeObject<XObject>(kernel_state(),
                                                          object_ptr, -1, true);
      if (!object_ref) {
        kernel_state()->UnlockDispatcher(context, old_irql);
        return X_STATUS_INVALID_PARAMETER;
      }

      objects[n] = std::move(object_ref);
    }
  }
  uint64_t timeout = timeout_ptr ? static_cast<uint64_t>(*timeout_ptr) : 0u;
  X_STATUS result = XObject::WaitMultiple(
      uint32_t(count), reinterpret_cast<XObject**>(&objects[0]), wait_type,
      wait_reason, processor_mode, alertable, timeout_ptr ? &timeout : nullptr,
      context);
  kernel_state()->UnlockDispatcher(context, old_irql);
  if (alertable) {
    if (result == X_STATUS_USER_APC) {
      result = xeProcessUserApcs(nullptr);
    }
  }
  return result;
}
DECLARE_XBOXKRNL_EXPORT3(KeWaitForMultipleObjects, kThreading, kImplemented,
                         kBlocking, kHighFrequency);

uint32_t xeNtWaitForMultipleObjectsEx(uint32_t count, xe::be<uint32_t>* handles,
                                      uint32_t wait_type, uint32_t wait_mode,
                                      uint32_t alertable, uint64_t* timeout_ptr,
                                      cpu::ppc::PPCContext* context) {
  assert_true(wait_type <= 1);

  assert_true(count <= 64);
  object_ref<XObject> objects[64];

  uint32_t old_irql = kernel_state()->LockDispatcher(context);
  /*
        Reserving to squash the constant reallocations, in a benchmark of one
     particular game over a period of five minutes roughly 11% of CPU time was
     spent inside a helper function to Windows' heap allocation function. 7% of
     that time was traced back to here

         edit: actually switched to fixed size array, as there can never be more
     than 64 events specified
  */
  {
    auto crit = global_critical_region::AcquireDirect();
    for (uint32_t n = 0; n < count; n++) {
      uint32_t object_handle = handles[n];
      auto object = kernel_state()->object_table()->LookupObject<XObject>(
          object_handle, true);
      if (!object) {
        kernel_state()->UnlockDispatcher(context, old_irql);
        return X_STATUS_INVALID_PARAMETER;
      }
      objects[n] = std::move(object);
    }
  }

  auto result = XObject::WaitMultiple(
      count, reinterpret_cast<XObject**>(&objects[0]), wait_type, 6, wait_mode,
      alertable, timeout_ptr, context);
  kernel_state()->UnlockDispatcher(context, old_irql);

  if (alertable) {
    if (result == X_STATUS_USER_APC) {
      result = xeProcessUserApcs(nullptr);
    }
  }
  return result;
}

dword_result_t NtWaitForMultipleObjectsEx_entry(
    dword_t count, lpdword_t handles, dword_t wait_type, dword_t wait_mode,
    dword_t alertable, lpqword_t timeout_ptr, const ppc_context_t& context) {
  uint64_t timeout = timeout_ptr ? static_cast<uint64_t>(*timeout_ptr) : 0u;
  if (!count || count > 64 || (wait_type != 1 && wait_type)) {
    return X_STATUS_INVALID_PARAMETER;
  }
  return xeNtWaitForMultipleObjectsEx(
      count, handles, wait_type, wait_mode, alertable,
      timeout_ptr ? &timeout : nullptr, context);
}
DECLARE_XBOXKRNL_EXPORT3(NtWaitForMultipleObjectsEx, kThreading, kImplemented,
                         kBlocking, kHighFrequency);

dword_result_t NtSignalAndWaitForSingleObjectEx_entry(
    dword_t signal_handle, dword_t wait_handle, dword_t alertable, dword_t r6,
    lpqword_t timeout_ptr, const ppc_context_t& context) {
  X_STATUS result = X_STATUS_SUCCESS;

  uint32_t old_irql = context->kernel_state->LockDispatcher(context);

  auto signal_object = kernel_state()->object_table()->LookupObject<XObject>(
      signal_handle, true);
  auto wait_object =
      kernel_state()->object_table()->LookupObject<XObject>(wait_handle, true);
  if (signal_object && wait_object) {
    uint64_t timeout = timeout_ptr ? static_cast<uint64_t>(*timeout_ptr) : 0u;
    result = XObject::SignalAndWait(signal_object.get(), wait_object.get(), 3,
                                    1, alertable,
                                    timeout_ptr ? &timeout : nullptr, context);
  } else {
    result = X_STATUS_INVALID_HANDLE;
  }
  context->kernel_state->UnlockDispatcher(context, old_irql);
  if (alertable) {
    if (result == X_STATUS_USER_APC) {
      result = xeProcessUserApcs(nullptr);
    }
  }
  return result;
}
DECLARE_XBOXKRNL_EXPORT3(NtSignalAndWaitForSingleObjectEx, kThreading,
                         kImplemented, kBlocking, kHighFrequency);

static void PrefetchForCAS(const void* value) { swcache::PrefetchW(value); }

uint32_t xeKeKfAcquireSpinLock(PPCContext* ctx, X_KSPINLOCK* lock,
                               bool change_irql) {
  auto old_irql = change_irql ? xeKfRaiseIrql(ctx, 2) : 0;

  PrefetchForCAS(lock);
  assert_true(lock->pcr_of_owner != static_cast<uint32_t>(ctx->r[13]));
  // Lock.
  while (!xe::atomic_cas(0, xe::byte_swap(static_cast<uint32_t>(ctx->r[13])),
                         &lock->pcr_of_owner.value)) {
  }

  return old_irql;
}

dword_result_t KfAcquireSpinLock_entry(pointer_t<X_KSPINLOCK> lock_ptr,
                                       const ppc_context_t& context) {
  return xeKeKfAcquireSpinLock(context, lock_ptr, true);
}
DECLARE_XBOXKRNL_EXPORT3(KfAcquireSpinLock, kThreading, kImplemented, kBlocking,
                         kHighFrequency);

void xeKeKfReleaseSpinLock(PPCContext* ctx, X_KSPINLOCK* lock,
                           uint32_t old_irql, bool change_irql) {
  assert_true(lock->pcr_of_owner == static_cast<uint32_t>(ctx->r[13]));
  // Unlock.
  lock->pcr_of_owner.value = 0;

  if (change_irql) {
    // Unlock.
    if (old_irql >= 2) {
      return;
    }

    // Restore IRQL.
    xeKfLowerIrql(ctx, old_irql);
  }
}

void KfReleaseSpinLock_entry(pointer_t<X_KSPINLOCK> lock_ptr, dword_t old_irql,
                             const ppc_context_t& ppc_ctx) {
  xeKeKfReleaseSpinLock(ppc_ctx, lock_ptr, old_irql, true);
}

DECLARE_XBOXKRNL_EXPORT2(KfReleaseSpinLock, kThreading, kImplemented,
                         kHighFrequency);
// todo: this is not accurate
void KeAcquireSpinLockAtRaisedIrql_entry(pointer_t<X_KSPINLOCK> lock_ptr,
                                         const ppc_context_t& ppc_ctx) {
  xeKeKfAcquireSpinLock(ppc_ctx, lock_ptr, false);
}
DECLARE_XBOXKRNL_EXPORT3(KeAcquireSpinLockAtRaisedIrql, kThreading,
                         kImplemented, kBlocking, kHighFrequency);

dword_result_t KeTryToAcquireSpinLockAtRaisedIrql_entry(
    pointer_t<X_KSPINLOCK> lock_ptr, const ppc_context_t& ppc_ctx) {
  // Lock.
  auto lock = reinterpret_cast<uint32_t*>(lock_ptr.host_address());
  assert_true(lock_ptr->pcr_of_owner != static_cast<uint32_t>(ppc_ctx->r[13]));
  PrefetchForCAS(lock);
  if (!ppc_ctx->processor->GuestAtomicCAS32(
          ppc_ctx, 0, static_cast<uint32_t>(ppc_ctx->r[13]),
          lock_ptr.guest_address())) {
    return 0;
  }
  return 1;
}
DECLARE_XBOXKRNL_EXPORT4(KeTryToAcquireSpinLockAtRaisedIrql, kThreading,
                         kImplemented, kBlocking, kHighFrequency, kSketchy);

void KeReleaseSpinLockFromRaisedIrql_entry(pointer_t<X_KSPINLOCK> lock_ptr,
                                           const ppc_context_t& ppc_ctx) {
  xeKeKfReleaseSpinLock(ppc_ctx, lock_ptr, 0, false);
}

DECLARE_XBOXKRNL_EXPORT2(KeReleaseSpinLockFromRaisedIrql, kThreading,
                         kImplemented, kHighFrequency);

void xeKeEnterCriticalRegion(PPCContext* context) {
  GetKThread()->apc_disable_count--;
}

void KeEnterCriticalRegion_entry(const ppc_context_t& context) {
  xeKeEnterCriticalRegion(context);
}
DECLARE_XBOXKRNL_EXPORT2(KeEnterCriticalRegion, kThreading, kImplemented,
                         kHighFrequency);

void xeKeLeaveCriticalRegion(PPCContext* context) {
  auto enable_count = ++GetKThread(context)->apc_disable_count;
  if (!enable_count) {
    if (!GetKThread(context)->apc_lists[0].empty(context)) {
      // kernel apc list not empty
      GetKThread(context)->running_kernel_apcs = 1;
      GetKPCR(context)->apc_software_interrupt_state = 1;

      // not very confident in this
      if (1 <= GetKPCR(context)->current_irql) {
        xeDispatchProcedureCallInterrupt(
            GetKPCR(context)->current_irql,
            GetKPCR(context)->software_interrupt_state, context);
      }
    }
  }
}
void KeLeaveCriticalRegion_entry(const ppc_context_t& context) {
  xeKeLeaveCriticalRegion(context);
}
DECLARE_XBOXKRNL_EXPORT2(KeLeaveCriticalRegion, kThreading, kImplemented,
                         kHighFrequency);

dword_result_t KeRaiseIrqlToDpcLevel_entry(const ppc_context_t& ctx) {
  auto pcr = ctx.GetPCR();
  uint32_t old_irql = pcr->current_irql;

  if (old_irql > 2) {
    XELOGE("KeRaiseIrqlToDpcLevel - old_irql > 2");
  }

  // oddly, this does not set the current interrupt priority
  pcr->current_irql = 2;

  return old_irql;
}
DECLARE_XBOXKRNL_EXPORT2(KeRaiseIrqlToDpcLevel, kThreading, kImplemented,
                         kHighFrequency);
void xeKfLowerIrql(PPCContext* ctx, unsigned char new_irql) {
  X_KPCR* kpcr = GetKPCR(ctx);

  if (new_irql > kpcr->current_irql) {
    XELOGE("KfLowerIrql : new_irql > kpcr->current_irql!");
  }
  kpcr->current_irql = new_irql;

  kernel_state()->SetCurrentInterruptPriority(ctx, kpcr, new_irql);

  if (new_irql < 2) {
    // the called function does a ton of other stuff including changing the
    // irql and interrupt_related

    uint16_t swint = GetKPCR(ctx)->software_interrupt_state;
    if (new_irql < swint) {
      xeDispatchProcedureCallInterrupt(new_irql, swint, ctx);
    }
  }
}
// irql is supposed to be per thread afaik...
void KfLowerIrql_entry(dword_t new_irql, const ppc_context_t& ctx) {
  xeKfLowerIrql(ctx, static_cast<unsigned char>(new_irql));
}
DECLARE_XBOXKRNL_EXPORT2(KfLowerIrql, kThreading, kImplemented, kHighFrequency);

unsigned char xeKfRaiseIrql(PPCContext* ctx, unsigned char new_irql) {
  X_KPCR* v1 = ctx->TranslateVirtualGPR<X_KPCR*>(ctx->r[13]);

  uint32_t old_irql = v1->current_irql;
  v1->current_irql = new_irql;

  if (old_irql > (unsigned int)new_irql) {
    XELOGE("KfRaiseIrql - old_irql > new_irql!");
  }
  kernel_state()->SetCurrentInterruptPriority(ctx, v1, new_irql);
  return old_irql;
}
// used by aurora's nova plugin
// like the other irql related functions, writes to an unknown mmio range (
// 0x7FFF ). The range is indexed by the low 16 bits of the KPCR's pointer (so
// r13)
dword_result_t KfRaiseIrql_entry(dword_t new_irql, const ppc_context_t& ctx) {
  return xeKfRaiseIrql(ctx, new_irql);
}

DECLARE_XBOXKRNL_EXPORT2(KfRaiseIrql, kThreading, kImplemented, kHighFrequency);

void AddTimer(PPCContext* context, X_KTIMER* timer) {
  auto& timers = context
                     ->TranslateVirtual<KernelGuestGlobals*>(
                         context->kernel_state->GetKernelGuestGlobals())
                     ->running_timers;

  timers.InsertHead(timer, context);
}

int XeInsertTimerToList(PPCContext* context,
                        X_TIME_STAMP_BUNDLE* KeTimeStampBundle, X_KTIMER* timer,
                        int64_t time) {
  int v3 = timer->period;

  timer->header.inserted = 1;
  timer->header.absolute = 0;
  if (!v3) {
    timer->header.signal_state = 0;
  }

  // todo: logic here is not so simple

  if (time < 0) {
    // i dont actually know where it sets due_time
    timer->due_time = KeTimeStampBundle->system_time - time;
    AddTimer(context, timer);
    return 1;

  } else {
    // huh?
    if (static_cast<int64_t>(KeTimeStampBundle->system_time - time) >= 0) {
      // already done!
      timer->header.signal_state = 1;
      timer->header.inserted = 0;
      return 0;
    }

    timer->header.absolute = 1;
    timer->due_time = time;
    AddTimer(context, timer);
    return 1;
  }
}

struct queued_timer_dpc_t {
  XDPC* dpc;
  uint32_t routine;
  uint32_t context;
};
void xeHandleTimers(PPCContext* context, uint32_t timer_related) {
  xenia_assert(GetKPCR(context)->current_irql == IRQL_DISPATCH);

  auto kernel = context->kernel_state;
  std::vector<X_KTIMER*> expired_timers;
  expired_timers.reserve(32);
  auto globals = context->TranslateVirtual<KernelGuestGlobals*>(
      kernel->GetKernelGuestGlobals());

  kernel->LockDispatcherAtIrql(context);
  uint64_t current_interrupt_time = globals->KeTimestampBundle.interrupt_time;

  for (auto&& timer : globals->running_timers.IterateForward(context)) {
    if (timer.due_time > current_interrupt_time) {
      expired_timers.push_back(&timer);
    }
  }

  for (auto&& timer_to_remove : expired_timers) {
    util::XeRemoveEntryList(&timer_to_remove->table_bucket_entry, context);
  }

  // make sure we run timers in order of their expiration. i think this ordering
  // is guaranteed by the kernel
  std::sort(expired_timers.begin(), expired_timers.end(),
            [](X_KTIMER* a, X_KTIMER* b) { return a->due_time < b->due_time; });

  // 360 xboxkrnl uses an array of 16 of these, and loops collecting dpcs
  // until the array is filled, then executes them, then checks for more timers
  // and so on until all timers have been handled
  // this does alter the ordering somewhat, but i doubt anything relies on that
  std::vector<queued_timer_dpc_t> qdpcs{};
  qdpcs.reserve(expired_timers.size());
  uint64_t current_systemtime = globals->KeTimestampBundle.system_time;

  for (auto&& timer : expired_timers) {
    timer->header.inserted = 0;
    timer->table_bucket_entry.Zero();

    if (context->TranslateVirtual<X_LIST_ENTRY*>(
            timer->header.wait_list.flink_ptr) != &timer->header.wait_list) {
      xeDispatchSignalStateChange(context, &timer->header, 0);
    }

    if (timer->period) {
      XeInsertTimerToList(context, &globals->KeTimestampBundle, timer,
                          -10000LL * timer->period);
    }

    auto dpc = context->TranslateVirtual(timer->dpc);
    // check if null
    if (dpc != (XDPC*)context->virtual_membase) {
      if (dpc->desired_cpu_number ||
          dpc->desired_cpu_number ==
              GetKPCR(context)->prcb_data.current_cpu + 1) {
        // dpc is intended for us to execute
        // save all data, i guess other dpcs might modify it?
        qdpcs.push_back(queued_timer_dpc_t{dpc, dpc->routine, dpc->context});
      } else {
        xeKeInsertQueueDpc(dpc, static_cast<uint32_t>(current_systemtime),
                           static_cast<uint32_t>(current_systemtime >> 32),
                           context);
      }
    }
  }
  kernel->UnlockDispatcher(context, IRQL_DISPATCH);

  for (auto&& queued_dpc : qdpcs) {
    uint64_t dpc_args[] = {context->HostToGuestVirtual(queued_dpc.dpc),
                           queued_dpc.context,
                           static_cast<uint32_t>(current_systemtime),
                           static_cast<uint32_t>(current_systemtime >> 32)};

    context->processor->Execute(context->thread_state, queued_dpc.routine,
                                dpc_args, countof(dpc_args));
  }
}
static void set_msr_interrupt_bits(PPCContext* context, uint32_t value) {
  // todo: implement!
}

void xeExecuteDPCList2(
    PPCContext* context, uint32_t timer_unk,
    util::X_TYPED_LIST<XDPC, offsetof(XDPC, list_entry)>& dpc_list,
    uint32_t zero_register) {
  xenia_assert(GetKPCR(context)->current_irql == IRQL_DISPATCH);

  do {
    // they only check if this value is nonzero. they probably
    // just use r1 because its a readily available nonzero register
    GetKPCR(context)->prcb_data.dpc_active =
        static_cast<uint32_t>(context->r[1]);
    uint32_t tmp_msr_mask = 0xFDFFD7FF;
    GetKPCR(context)->msr_mask = tmp_msr_mask;
    context->msr &= tmp_msr_mask;

    if (timer_unk) {
      uint32_t v4 = GetKPCR(context)->timer_related;
      GetKPCR(context)->timer_pending = zero_register;
      set_msr_interrupt_bits(context, 0xFFFF8000);
      xeHandleTimers(context, v4);
      set_msr_interrupt_bits(context, zero_register);
    }
    X_KSPINLOCK* spin = &GetKPCR(context)->prcb_data.dpc_lock;
    while (true) {
      xboxkrnl::xeKeKfAcquireSpinLock(context, spin, false);

      if (dpc_list.empty(context)) {
        xboxkrnl::xeKeKfReleaseSpinLock(context, spin, 0, false);
        break;
      }
      auto dpc = dpc_list.HeadObject(context);
      util::XeRemoveEntryList(&dpc->list_entry, context);
      auto routine = dpc->routine;
      auto ctx = dpc->context;
      auto arg1 = dpc->arg1;
      auto arg2 = dpc->arg2;
      dpc->selected_cpu_number = zero_register;

      xboxkrnl::xeKeKfReleaseSpinLock(context, spin, 0, false);

      set_msr_interrupt_bits(context, 0xFFFF8000);
      uint64_t dpc_args[] = {context->HostToGuestVirtual(dpc), ctx, arg1, arg2};
      context->processor->Execute(context->thread_state, routine, dpc_args, 4);

      // make sure the routine didnt change the irql
      xenia_assert(GetKPCR(context)->current_irql == IRQL_DISPATCH);

      set_msr_interrupt_bits(context, zero_register);

      if (dpc_list.empty(context)) {
        break;
      }
    }
    GetKPCR(context)->prcb_data.dpc_active = zero_register;
    GetKPCR(context)->prcb_data.dpc_related_40 = zero_register;

    // this doesnt make much sense to me, so its likely an issue with the
    // decompilation
    timer_unk = dpc_list.flink_ptr;

  } while (!dpc_list.empty(context));
}

void xeDispatchProcedureCallInterrupt(unsigned int new_irql,
                                      unsigned int software_interrupt_mask,
                                      cpu::ppc::PPCContext* context) {
  if (new_irql < software_interrupt_mask) {
    uint64_t saved_msr = context->msr;

    if (software_interrupt_mask >> 8) {
      GetKPCR(context)->current_irql =
          static_cast<unsigned char>(software_interrupt_mask >> 8);
      uint32_t sw_state;
      do {
        context->msr |= 0x8000ULL;
        xeHandleDPCsAndThreadSwapping(context);
        context->msr &= ~(0x8000ULL);
        sw_state = GetKPCR(context)->software_interrupt_state;
      } while (sw_state >> 8);
      GetKPCR(context)->current_irql = sw_state | new_irql;
      if (sw_state <= new_irql) {
        context->msr = saved_msr;
        return;
      }
    } else {
      if (software_interrupt_mask <= new_irql) {
        return;
      }
      GetKPCR(context)->current_irql = software_interrupt_mask;
    }

    do {
      context->msr |= 0x8000ULL;
      xenia_assert(offsetof(X_KPCR, apc_software_interrupt_state) == 0x9);
      GetKPCR(context)->apc_software_interrupt_state = new_irql;

      xeProcessKernelApcs(context);

      context->msr &= ~0x8000ULL;
    } while (GetKPCR(context)->apc_software_interrupt_state);
    GetKPCR(context)->current_irql = new_irql;

    context->msr = saved_msr;
  }
}

uint32_t xeNtQueueApcThread(uint32_t thread_handle, uint32_t apc_routine,
                            uint32_t apc_routine_context, uint32_t arg1,
                            uint32_t arg2, cpu::ppc::PPCContext* context) {
  auto kernelstate = context->kernel_state;
  auto memory = kernelstate->memory();
  auto thread =
      kernelstate->object_table()->LookupObject<XThread>(thread_handle);

  if (!thread) {
    XELOGE("NtQueueApcThread: Incorrect thread handle! Might cause crash");
    return X_STATUS_INVALID_HANDLE;
  }

  uint32_t apc_ptr = memory->SystemHeapAlloc(XAPC::kSize);
  if (!apc_ptr) {
    return X_STATUS_NO_MEMORY;
  }
  XAPC* apc = context->TranslateVirtual<XAPC*>(apc_ptr);
  xeKeInitializeApc(apc, thread->guest_object(), XAPC::kDummyKernelRoutine, 0,
                    apc_routine, 1 /*user apc mode*/, apc_routine_context);

  if (!xeKeInsertQueueApc(apc, arg1, arg2, 0, context)) {
    memory->SystemHeapFree(apc_ptr);
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}
dword_result_t NtQueueApcThread_entry(dword_t thread_handle,
                                      lpvoid_t apc_routine,
                                      lpvoid_t apc_routine_context,
                                      lpvoid_t arg1, lpvoid_t arg2,
                                      const ppc_context_t& context) {
  return xeNtQueueApcThread(thread_handle, apc_routine, apc_routine_context,
                            arg1, arg2, context);
}
/*
    todo: Kernel Apc queue logic is very different! this does not process things
   in the proper order! it also does not set irql right, and does not set some
   kthread/kpcr vars that need setting
*/
template <X_STATUS alert_status_res, uint32_t which_queue>
X_STATUS xeProcessApcQueue(PPCContext* ctx) {
  if (!ctx) {
    ctx = cpu::ThreadState::Get()->context();
  }
  X_STATUS alert_status = X_STATUS_SUCCESS;
  auto kpcr = ctx->TranslateVirtualGPR<X_KPCR*>(ctx->r[13]);

  auto current_thread = ctx->TranslateVirtual(kpcr->prcb_data.current_thread);

  uint32_t unlocked_irql =
      xeKeKfAcquireSpinLock(ctx, &current_thread->apc_lock);

  auto& user_apc_queue = current_thread->apc_lists[which_queue];

  // use guest stack for temporaries
  uint32_t old_stack_pointer = static_cast<uint32_t>(ctx->r[1]);

  uint32_t scratch_address = old_stack_pointer - 16;
  ctx->r[1] = old_stack_pointer - 32;

  while (!user_apc_queue.empty(ctx)) {
    uint32_t apc_ptr = user_apc_queue.flink_ptr;
    
    if (!apc_ptr) {
      XELOGE("Null link in apc queue!!");
      user_apc_queue.Initialize(ctx);
      break;
    }

    XAPC* apc = user_apc_queue.ListEntryObject(
        ctx->TranslateVirtual<X_LIST_ENTRY*>(apc_ptr));

    uint8_t* scratch_ptr = ctx->TranslateVirtual(scratch_address);
    xe::store_and_swap<uint32_t>(scratch_ptr + 0, apc->normal_routine);
    xe::store_and_swap<uint32_t>(scratch_ptr + 4, apc->normal_context);
    xe::store_and_swap<uint32_t>(scratch_ptr + 8, apc->arg1);
    xe::store_and_swap<uint32_t>(scratch_ptr + 12, apc->arg2);
    util::XeRemoveEntryList(&apc->list_entry, ctx);
    apc->enqueued = 0;

    xeKeKfReleaseSpinLock(ctx, &current_thread->apc_lock, unlocked_irql);
    alert_status = alert_status_res;
    if (apc->kernel_routine != XAPC::kDummyKernelRoutine) {
      uint64_t kernel_args[] = {
          apc_ptr,
          scratch_address + 0,
          scratch_address + 4,
          scratch_address + 8,
          scratch_address + 12,
      };
      ctx->processor->Execute(ctx->thread_state, apc->kernel_routine,
                              kernel_args, xe::countof(kernel_args));
    } else {
      ctx->kernel_state->memory()->SystemHeapFree(apc_ptr);
    }

    uint32_t normal_routine = xe::load_and_swap<uint32_t>(scratch_ptr + 0);
    uint32_t normal_context = xe::load_and_swap<uint32_t>(scratch_ptr + 4);
    uint32_t arg1 = xe::load_and_swap<uint32_t>(scratch_ptr + 8);
    uint32_t arg2 = xe::load_and_swap<uint32_t>(scratch_ptr + 12);

    if (normal_routine) {
      uint64_t normal_args[] = {normal_context, arg1, arg2};
      ctx->processor->Execute(ctx->thread_state, normal_routine, normal_args,
                              xe::countof(normal_args));
    }

    unlocked_irql = xeKeKfAcquireSpinLock(ctx, &current_thread->apc_lock);
  }

  ctx->r[1] = old_stack_pointer;

  xeKeKfReleaseSpinLock(ctx, &current_thread->apc_lock, unlocked_irql);
  return alert_status;
}

X_STATUS xeProcessUserApcs(PPCContext* ctx) {
  return xeProcessApcQueue<X_STATUS_USER_APC, 1>(ctx);
}

X_STATUS xeProcessKernelApcs(PPCContext* ctx) {
  return xeProcessApcQueue<X_STATUS_USER_APC, 0>(ctx);
}

static void YankApcList(PPCContext* ctx, X_KTHREAD* current_thread,
                        unsigned apc_mode, bool rundown) {
  uint32_t unlocked_irql =
      xeKeKfAcquireSpinLock(ctx, &current_thread->apc_lock);

  XAPC* result = nullptr;
  auto& user_apc_queue = current_thread->apc_lists[apc_mode];

  if (user_apc_queue.empty(ctx)) {
    result = nullptr;
  } else {
    result = user_apc_queue.HeadObject(ctx);
    for (auto&& entry : user_apc_queue.IterateForward(ctx)) {
      entry.enqueued = 0;
    }
    util::XeRemoveEntryList(&user_apc_queue, ctx);
  }

  xeKeKfReleaseSpinLock(ctx, &current_thread->apc_lock, unlocked_irql);

  if (rundown && result) {
    XAPC* current_entry = result;
    while (true) {
      XAPC* this_entry = current_entry;
      uint32_t next_entry = this_entry->list_entry.flink_ptr;

      if (this_entry->rundown_routine) {
        uint64_t args[] = {ctx->HostToGuestVirtual(this_entry)};
        kernel_state()->processor()->Execute(ctx->thread_state,
                                             this_entry->rundown_routine, args,
                                             xe::countof(args));
      } else {
        ctx->kernel_state->memory()->SystemHeapFree(
            ctx->HostToGuestVirtual(this_entry));
      }

      if (next_entry == 0) {
        break;
      }
      current_entry = user_apc_queue.ListEntryObject(
          ctx->TranslateVirtual<X_LIST_ENTRY*>(next_entry));
      if (current_entry == result) {
        break;
      }
    }
  }
}

void xeRundownApcs(cpu::ppc::PPCContext* ctx) {
  auto kpcr = ctx->TranslateVirtualGPR<X_KPCR*>(ctx->r[13]);

  auto current_thread = ctx->TranslateVirtual(kpcr->prcb_data.current_thread);
  YankApcList(ctx, current_thread, 1, true);
  YankApcList(ctx, current_thread, 0, false);
}
DECLARE_XBOXKRNL_EXPORT1(NtQueueApcThread, kThreading, kImplemented);
void xeKeInitializeApc(XAPC* apc, uint32_t thread_ptr, uint32_t kernel_routine,
                       uint32_t rundown_routine, uint32_t normal_routine,
                       uint32_t apc_mode, uint32_t normal_context) {
  apc->thread_ptr = thread_ptr;
  apc->kernel_routine = kernel_routine;
  apc->rundown_routine = rundown_routine;
  apc->normal_routine = normal_routine;
  apc->type = 18;
  if (normal_routine) {
    apc->apc_mode = apc_mode;
    apc->normal_context = normal_context;
  } else {
    apc->apc_mode = 0;
    apc->normal_context = 0;
  }
  apc->enqueued = 0;
}
void KeInitializeApc_entry(pointer_t<XAPC> apc, lpvoid_t thread_ptr,
                           lpvoid_t kernel_routine, lpvoid_t rundown_routine,
                           lpvoid_t normal_routine, dword_t processor_mode,
                           lpvoid_t normal_context) {
  xeKeInitializeApc(apc, thread_ptr, kernel_routine, rundown_routine,
                    normal_routine, processor_mode, normal_context);
}
DECLARE_XBOXKRNL_EXPORT1(KeInitializeApc, kThreading, kImplemented);

uint32_t xeKeInsertQueueApc(XAPC* apc, uint32_t arg1, uint32_t arg2,
                            uint32_t priority_increment,
                            cpu::ppc::PPCContext* context) {
  uint32_t thread_guest_pointer = apc->thread_ptr;
  if (!thread_guest_pointer) {
    return 0;
  }
  auto target_thread = context->TranslateVirtual<X_KTHREAD*>(apc->thread_ptr);
  auto old_irql = xeKeKfAcquireSpinLock(context, &target_thread->apc_lock);
  uint32_t result;
  if (!target_thread->may_queue_apcs || apc->enqueued) {
    result = 0;
  } else {
    apc->arg1 = arg1;
    apc->arg2 = arg2;
    apc->enqueued = 1;
    xeKeInsertQueueApcHelper(context, apc, priority_increment);

    result = 1;
  }
  xeDispatcherSpinlockUnlock(context, &target_thread->apc_lock, old_irql);
  return result;
}

// i doubt this is correctly implemented
static void SendRunKernelApcIPI(void* ud) {
  xeProcessKernelApcs(cpu::ThreadState::GetContext());
}

void xeKeInsertQueueApcHelper(cpu::ppc::PPCContext* context, XAPC* apc,
                              int priority_increment) {
  auto apc_thread = context->TranslateVirtual(apc->thread_ptr);
  auto apc_mode = apc->apc_mode;
  auto& which_list = apc_thread->apc_lists[apc->apc_mode];

  if (apc->normal_routine) {
    which_list.InsertTail(apc, context);
  } else {
    XAPC* insertion_pos = nullptr;
    for (auto&& sub_apc : which_list.IterateForward(context)) {
      insertion_pos = &sub_apc;
      if (sub_apc.normal_routine) {
        break;
      }
    }
    if (!insertion_pos) {
      which_list.InsertHead(apc, context);
    } else {
      util::XeInsertHeadList(insertion_pos->list_entry.blink_ptr,
                             &apc->list_entry, context);
    }
  }

  context->kernel_state->LockDispatcherAtIrql(context);
  {
    X_STATUS wait_status;
    auto target_thread_state = apc_thread->thread_state;
    if (apc_mode) {
      if (target_thread_state == 5 && apc_thread->processor_mode == 1 &&
          apc_thread->alerted) {
        wait_status = X_STATUS_USER_APC;
        apc_thread->unk_8A = 1;
        goto LABEL_25;
      }
    } else {
      apc_thread->running_kernel_apcs = 1;
      if (target_thread_state == 2) {
        auto thread_processor = apc_thread->current_cpu;
        if (thread_processor == GetKPCR(context)->prcb_data.current_cpu) {
          GetKPCR(context)->apc_software_interrupt_state = 1;
        } else {
          // THIS IS DEFINITELY BADLY IMPLEMENTED!
          context->processor->GetCPUThread(thread_processor)
              ->SendGuestIPI(SendRunKernelApcIPI, nullptr);
        }
        goto LABEL_26;
      }
      if (target_thread_state == 5 && !apc_thread->unk_A4 &&
          (!apc->normal_routine ||
           !apc_thread->apc_disable_count && !apc_thread->unk_88)) {
        wait_status = X_STATUS_KERNEL_APC;
      LABEL_25:
        xeEnqueueThreadPostWait(context, apc_thread, wait_status,
                                priority_increment);
        goto LABEL_26;
      }
    }
  LABEL_26:;
  }

  context->kernel_state->UnlockDispatcherAtIrql(context);
}
dword_result_t KeInsertQueueApc_entry(pointer_t<XAPC> apc, lpvoid_t arg1,
                                      lpvoid_t arg2, dword_t priority_increment,
                                      const ppc_context_t& context) {
  return xeKeInsertQueueApc(apc, arg1, arg2, priority_increment, context);
}
DECLARE_XBOXKRNL_EXPORT1(KeInsertQueueApc, kThreading, kImplemented);

dword_result_t KeRemoveQueueApc_entry(pointer_t<XAPC> apc,
                                      const ppc_context_t& context) {
  bool result = false;

  uint32_t thread_guest_pointer = apc->thread_ptr;
  if (!thread_guest_pointer) {
    return 0;
  }
  auto target_thread = context->TranslateVirtual<X_KTHREAD*>(apc->thread_ptr);
  auto old_irql = xeKeKfAcquireSpinLock(context, &target_thread->apc_lock);

  if (apc->enqueued) {
    result = true;
    apc->enqueued = 0;
    util::XeRemoveEntryList(&apc->list_entry, context);
    // todo: this is incomplete, there is more logic here in actual kernel
  }
  xeKeKfReleaseSpinLock(context, &target_thread->apc_lock, old_irql);

  return result ? 1 : 0;
}
DECLARE_XBOXKRNL_EXPORT1(KeRemoveQueueApc, kThreading, kImplemented);

dword_result_t KiApcNormalRoutineNop_entry(dword_t unk0 /* output? */,
                                           dword_t unk1 /* 0x13 */) {
  return 0;
}
DECLARE_XBOXKRNL_EXPORT1(KiApcNormalRoutineNop, kThreading, kStub);

void KeInitializeDpc_entry(pointer_t<XDPC> dpc, lpvoid_t routine,
                           lpvoid_t context) {
  dpc->Initialize(routine, context);
}
DECLARE_XBOXKRNL_EXPORT2(KeInitializeDpc, kThreading, kImplemented, kSketchy);

static void DPCIPIFunction(void* ud) {
  xeHandleDPCsAndThreadSwapping(cpu::ThreadState::Get()->context());
}
uint32_t xeKeInsertQueueDpc(XDPC* dpc, uint32_t arg1, uint32_t arg2,
                            PPCContext* ctx) {
  bool result = false;

  auto old_irql = xeKfRaiseIrql(ctx, 124);

  X_KPRCB* target_prcb;
  auto inserted_cpunum = dpc->desired_cpu_number;

  if (dpc->desired_cpu_number) {
    target_prcb =
        &ctx->kernel_state->KPCRPageForCpuNumber(dpc->desired_cpu_number - 1)
             ->pcr.prcb_data;
  } else {
    target_prcb = &GetKPCR(ctx)->prcb_data;
    inserted_cpunum = target_prcb->current_cpu + 1;
  }

  xboxkrnl::xeKeKfAcquireSpinLock(ctx, &target_prcb->dpc_lock, false);
  bool send_interrupt = false;
  if (dpc->selected_cpu_number == 0) {
    result = true;
    dpc->selected_cpu_number = inserted_cpunum;
    dpc->arg1 = arg1;
    dpc->arg2 = arg2;
    util::XeInsertTailList(&target_prcb->queued_dpcs_list_head,
                           &dpc->list_entry, ctx);
    if (!target_prcb->dpc_active && !target_prcb->dpc_related_40) {
      send_interrupt = true;
      target_prcb->dpc_related_40 = 1;
    }
  }
  xboxkrnl::xeKeKfReleaseSpinLock(ctx, &target_prcb->dpc_lock, 0, false);

  if (send_interrupt) {
    if (target_prcb == &GetKPCR(ctx)->prcb_data) {
      GetKPCR(ctx)->generic_software_interrupt = 2;
    } else {
      uint32_t cpunum = inserted_cpunum - 1;

      // ctx->kernel_state->SendIPI(1 << (inserted_cpunum - 1), 2);
      // kernel sends an ipi here. i havent been able to figure out what the
      // args to it mean, but presumably the IPI just triggers running the dpc
      // list on the
      ctx->processor->GetCPUThread(cpunum)->SendGuestIPI(DPCIPIFunction,
                                                         nullptr);
    }
  }

  xboxkrnl::xeKfLowerIrql(ctx, old_irql);
  return result;
}

dword_result_t KeInsertQueueDpc_entry(pointer_t<XDPC> dpc, dword_t arg1,
                                      dword_t arg2, const ppc_context_t& ctx) {
  return xeKeInsertQueueDpc(dpc, arg1, arg2, ctx);
}
DECLARE_XBOXKRNL_EXPORT2(KeInsertQueueDpc, kThreading, kStub, kSketchy);

uint32_t xeKeRemoveQueueDpc(XDPC* dpc, PPCContext* ctx) {
  bool result = false;

  auto old_irql = xeKfRaiseIrql(ctx, 124);
  auto selected_cpu_number = dpc->selected_cpu_number;
  if (selected_cpu_number) {
    // need to hold the dpc lock, find the pcr it belongs to
    auto targeted_pcr =
        &ctx->kernel_state->KPCRPageForCpuNumber(selected_cpu_number - 1)->pcr;

    xboxkrnl::xeKeKfAcquireSpinLock(ctx, &targeted_pcr->prcb_data.dpc_lock,
                                    false);
    {
      if (dpc->selected_cpu_number) {
        util::XeRemoveEntryList(&dpc->list_entry, ctx);
        dpc->selected_cpu_number = 0;
      }
    }
    xboxkrnl::xeKeKfReleaseSpinLock(ctx, &targeted_pcr->prcb_data.dpc_lock,
                                    false);
  }

  xeKfLowerIrql(ctx, old_irql);
  return selected_cpu_number != 0;
}

dword_result_t KeRemoveQueueDpc_entry(pointer_t<XDPC> dpc,
                                      const ppc_context_t& ctx) {
  return xeKeRemoveQueueDpc(dpc, ctx);
}
DECLARE_XBOXKRNL_EXPORT1(KeRemoveQueueDpc, kThreading, kImplemented);

// https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/blob/51e4dfcaacfdbd1a9692272931a436371492f72d/import/OpenXDK/include/xboxkrnl/xboxkrnl.h#L1372
struct X_ERWLOCK {
  be<int32_t> lock_count;              // 0x0
  be<uint32_t> writers_waiting_count;  // 0x4
  be<uint32_t> readers_waiting_count;  // 0x8
  be<uint32_t> readers_entry_count;    // 0xC
  X_KEVENT writer_event;               // 0x10
  X_KSEMAPHORE reader_semaphore;       // 0x20
  X_KSPINLOCK spin_lock;               // 0x34
};
static_assert_size(X_ERWLOCK, 0x38);

void ExInitializeReadWriteLock_entry(pointer_t<X_ERWLOCK> lock_ptr,
                                     const ppc_context_t& context) {
  lock_ptr->lock_count = -1;
  lock_ptr->writers_waiting_count = 0;
  lock_ptr->readers_waiting_count = 0;
  lock_ptr->readers_entry_count = 0;
  KeInitializeEvent_entry(&lock_ptr->writer_event, 1, 0, context);
  KeInitializeSemaphore_entry(&lock_ptr->reader_semaphore, 0, 0x7FFFFFFF);
  lock_ptr->spin_lock.pcr_of_owner = 0;
}
DECLARE_XBOXKRNL_EXPORT1(ExInitializeReadWriteLock, kThreading, kImplemented);

void ExAcquireReadWriteLockExclusive_entry(pointer_t<X_ERWLOCK> lock_ptr,
                                           const ppc_context_t& ppc_context) {
  auto old_irql = xeKeKfAcquireSpinLock(ppc_context, &lock_ptr->spin_lock);

  int32_t lock_count = ++lock_ptr->lock_count;
  if (!lock_count) {
    xeKeKfReleaseSpinLock(ppc_context, &lock_ptr->spin_lock, old_irql);
    return;
  }

  lock_ptr->writers_waiting_count++;

  xeKeKfReleaseSpinLock(ppc_context, &lock_ptr->spin_lock, old_irql);
  xeKeWaitForSingleObject(&lock_ptr->writer_event, 7, 0, 0, nullptr);
}
DECLARE_XBOXKRNL_EXPORT2(ExAcquireReadWriteLockExclusive, kThreading,
                         kImplemented, kBlocking);

dword_result_t ExTryToAcquireReadWriteLockExclusive_entry(
    pointer_t<X_ERWLOCK> lock_ptr, const ppc_context_t& ppc_context) {
  auto old_irql = xeKeKfAcquireSpinLock(ppc_context, &lock_ptr->spin_lock);

  uint32_t result;
  if (lock_ptr->lock_count < 0) {
    lock_ptr->lock_count = 0;
    result = 1;
  } else {
    result = 0;
  }

  xeKeKfReleaseSpinLock(ppc_context, &lock_ptr->spin_lock, old_irql);
  return result;
}
DECLARE_XBOXKRNL_EXPORT1(ExTryToAcquireReadWriteLockExclusive, kThreading,
                         kImplemented);

void ExAcquireReadWriteLockShared_entry(pointer_t<X_ERWLOCK> lock_ptr,
                                        const ppc_context_t& ppc_context) {
  auto old_irql = xeKeKfAcquireSpinLock(ppc_context, &lock_ptr->spin_lock);

  int32_t lock_count = ++lock_ptr->lock_count;
  if (!lock_count ||
      (lock_ptr->readers_entry_count && !lock_ptr->writers_waiting_count)) {
    lock_ptr->readers_entry_count++;
    xeKeKfReleaseSpinLock(ppc_context, &lock_ptr->spin_lock, old_irql);
    return;
  }

  lock_ptr->readers_waiting_count++;

  xeKeKfReleaseSpinLock(ppc_context, &lock_ptr->spin_lock, old_irql);
  xeKeWaitForSingleObject(&lock_ptr->reader_semaphore, 7, 0, 0, nullptr);
}
DECLARE_XBOXKRNL_EXPORT2(ExAcquireReadWriteLockShared, kThreading, kImplemented,
                         kBlocking);

dword_result_t ExTryToAcquireReadWriteLockShared_entry(
    pointer_t<X_ERWLOCK> lock_ptr, const ppc_context_t& ppc_context) {
  auto old_irql = xeKeKfAcquireSpinLock(ppc_context, &lock_ptr->spin_lock);

  uint32_t result;
  if (lock_ptr->lock_count < 0 ||
      (lock_ptr->readers_entry_count && !lock_ptr->writers_waiting_count)) {
    lock_ptr->lock_count++;
    lock_ptr->readers_entry_count++;
    result = 1;
  } else {
    result = 0;
  }

  xeKeKfReleaseSpinLock(ppc_context, &lock_ptr->spin_lock, old_irql);
  return result;
}
DECLARE_XBOXKRNL_EXPORT1(ExTryToAcquireReadWriteLockShared, kThreading,
                         kImplemented);

void ExReleaseReadWriteLock_entry(pointer_t<X_ERWLOCK> lock_ptr,
                                  const ppc_context_t& ppc_context) {
  auto old_irql = xeKeKfAcquireSpinLock(ppc_context, &lock_ptr->spin_lock);

  int32_t lock_count = --lock_ptr->lock_count;

  if (lock_count < 0) {
    lock_ptr->readers_entry_count = 0;
    xeKeKfReleaseSpinLock(ppc_context, &lock_ptr->spin_lock, old_irql);
    return;
  }

  if (!lock_ptr->readers_entry_count) {
    auto readers_waiting_count = lock_ptr->readers_waiting_count;
    if (readers_waiting_count) {
      lock_ptr->readers_waiting_count = 0;
      lock_ptr->readers_entry_count = readers_waiting_count;
      xeKeKfReleaseSpinLock(ppc_context, &lock_ptr->spin_lock, old_irql);
      xeKeReleaseSemaphore(&lock_ptr->reader_semaphore, 1,
                           readers_waiting_count, 0);
      return;
    }
  }

  auto readers_entry_count = --lock_ptr->readers_entry_count;
  if (readers_entry_count) {
    xeKeKfReleaseSpinLock(ppc_context, &lock_ptr->spin_lock, old_irql);
    return;
  }

  lock_ptr->writers_waiting_count--;
  xeKeKfReleaseSpinLock(ppc_context, &lock_ptr->spin_lock, old_irql);
  xeKeSetEvent(ppc_context, &lock_ptr->writer_event, 1, 0);
}
DECLARE_XBOXKRNL_EXPORT1(ExReleaseReadWriteLock, kThreading, kImplemented);

// NOTE: This function is very commonly inlined, and probably won't be called!
pointer_result_t InterlockedPushEntrySList_entry(
    pointer_t<X_SLIST_HEADER> plist_ptr, pointer_t<X_SINGLE_LIST_ENTRY> entry) {
  assert_not_null(plist_ptr);
  assert_not_null(entry);

  alignas(8) X_SLIST_HEADER old_hdr = *plist_ptr;
  alignas(8) X_SLIST_HEADER new_hdr = {{0}, 0, 0};
  uint32_t old_head = 0;
  do {
    old_hdr = *plist_ptr;
    new_hdr.depth = old_hdr.depth + 1;
    new_hdr.sequence = old_hdr.sequence + 1;

    old_head = old_hdr.next.next;
    entry->next = old_hdr.next.next;
    new_hdr.next.next = entry.guest_address();
  } while (
      !xe::atomic_cas(*(uint64_t*)(&old_hdr), *(uint64_t*)(&new_hdr),
                      reinterpret_cast<uint64_t*>(plist_ptr.host_address())));

  return old_head;
}
DECLARE_XBOXKRNL_EXPORT2(InterlockedPushEntrySList, kThreading, kImplemented,
                         kHighFrequency);

pointer_result_t InterlockedPopEntrySList_entry(
    pointer_t<X_SLIST_HEADER> plist_ptr) {
  assert_not_null(plist_ptr);

  uint32_t popped = 0;
  alignas(8) X_SLIST_HEADER old_hdr = {{0}, 0, 0};
  alignas(8) X_SLIST_HEADER new_hdr = {{0}, 0, 0};
  do {
    old_hdr = *plist_ptr;
    auto next = kernel_memory()->TranslateVirtual<X_SINGLE_LIST_ENTRY*>(
        old_hdr.next.next);
    if (!old_hdr.next.next) {
      return 0;
    }
    popped = old_hdr.next.next;

    new_hdr.depth = old_hdr.depth - 1;
    new_hdr.next.next = next->next;
    new_hdr.sequence = old_hdr.sequence;
  } while (
      !xe::atomic_cas(*(uint64_t*)(&old_hdr), *(uint64_t*)(&new_hdr),
                      reinterpret_cast<uint64_t*>(plist_ptr.host_address())));

  return popped;
}
DECLARE_XBOXKRNL_EXPORT2(InterlockedPopEntrySList, kThreading, kImplemented,
                         kHighFrequency);

pointer_result_t InterlockedFlushSList_entry(
    pointer_t<X_SLIST_HEADER> plist_ptr) {
  assert_not_null(plist_ptr);

  alignas(8) X_SLIST_HEADER old_hdr = *plist_ptr;
  alignas(8) X_SLIST_HEADER new_hdr = {{0}, 0, 0};
  uint32_t first = 0;
  do {
    old_hdr = *plist_ptr;
    first = old_hdr.next.next;
    new_hdr.next.next = 0;
    new_hdr.depth = 0;
    new_hdr.sequence = 0;
  } while (
      !xe::atomic_cas(*(uint64_t*)(&old_hdr), *(uint64_t*)(&new_hdr),
                      reinterpret_cast<uint64_t*>(plist_ptr.host_address())));

  return first;
}
DECLARE_XBOXKRNL_EXPORT1(InterlockedFlushSList, kThreading, kImplemented);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Threading);
