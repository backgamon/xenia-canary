/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/audio_system.h"

#include "xenia/apu/apu_flags.h"
#include "xenia/apu/audio_driver.h"
#include "xenia/apu/xma_decoder.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/base/ring_buffer.h"
#include "xenia/base/string_buffer.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/emulator.h"
// As with normal Microsoft, there are like twelve different ways to access
// the audio APIs. Early games use XMA*() methods almost exclusively to touch
// decoders. Later games use XAudio*() and direct memory writes to the XMA
// structures (as opposed to the XMA* calls), meaning that we have to support
// both.
//
// For ease of implementation, most audio related processing is handled in
// AudioSystem, and the functions here call off to it.
// The XMA*() functions just manipulate the audio system in the guest context
// and let the normal AudioSystem handling take it, to prevent duplicate
// implementations. They can be found in xboxkrnl_audio_xma.cc

DEFINE_uint32(apu_max_queued_frames, 64,
              "Allows changing max buffered audio frames to reduce audio "
              "delay. Minimum is 16.",
              "APU");

#define AUDIOSYSTEM_NOWAIT_FOR_CALLBACK 1
namespace xe {
namespace apu {
struct GuestMessage {
  threading::AtomicListEntry list_entry;
  uint32_t client_callback_;
  uint32_t client_callback_arg_;
};
AudioSystem::AudioSystem(cpu::Processor* processor)
    : memory_(processor->memory()),
      processor_(processor),
      worker_running_(false) {
  std::memset(clients_, 0, sizeof(clients_));
  queued_frames_ = std::max(cvars::apu_max_queued_frames, (uint32_t)16);

  for (size_t i = 0; i < kMaximumClientCount; ++i) {
    client_semaphores_[i] = xe::threading::Semaphore::Create(0, queued_frames_);
    wait_handles_[i] = client_semaphores_[i].get();
  }
  shutdown_event_ = xe::threading::Event::CreateAutoResetEvent(false);
  assert_not_null(shutdown_event_);
  wait_handles_[kMaximumClientCount] = shutdown_event_.get();

  xma_decoder_ = std::make_unique<xe::apu::XmaDecoder>(processor_);

  resume_event_ = xe::threading::Event::CreateAutoResetEvent(false);
  signal_event_ = xe::threading::Event::CreateAutoResetEvent(false);
  assert_not_null(resume_event_);
}

AudioSystem::~AudioSystem() {
  if (xma_decoder_) {
    xma_decoder_->Shutdown();
  }
}

X_STATUS AudioSystem::Setup(kernel::KernelState* kernel_state) {
  X_STATUS result = xma_decoder_->Setup(kernel_state);
  if (result) {
    return result;
  }
  kernel_state_ = kernel_state;
  worker_running_ = true;

  threading::Thread::CreationParameters crparams{};
  worker_thread_ = threading::Thread::Create(
      crparams, std::bind(&AudioSystem::WorkerThreadMain, this));
  Emulator::Get()->RegisterGuestHardwareBlockThread(worker_thread_.get());
  // As we run audio callbacks the debugger must be able to suspend us.
  worker_thread_->set_name("Audio Worker");
  worker_thread_->set_affinity_mask(0b11000000);

  
  return X_STATUS_SUCCESS;
}
void AudioSystem::StartGuestWorkerThread(kernel::KernelState* kernel) {
  xenia_assert(!guest_thread_);
  auto context = cpu::ThreadState::GetContext();
  guest_thread_ =
      kernel::object_ref<kernel::XHostThread>(new kernel::XHostThread(
          kernel, 65536U, 0x10000083u,
          [this]() {
            std::vector<GuestMessage*> messages_rev{};
            messages_rev.reserve(128);
            auto context = cpu::ThreadState::GetContext();
            while (true) {
              context->CheckInterrupt();
              auto callbacks = guest_worker_messages_.Flush();

              if (!callbacks) {
               //auto status = kernel::xboxkrnl::xeNtYieldExecution(
               //     context);
                context->CheckInterrupt();
                //if (status == X_STATUS_NO_YIELD_PERFORMED) {
                  int64_t wait_time = -10000 * 1; //1 ms
                  kernel::xboxkrnl::xeKeDelayExecutionThread(context, 1, true,
                                                             &wait_time);
               // }
                continue;
              }
              kernel::xboxkrnl::xeKeEnterCriticalRegion(context);
              while (callbacks) {
                messages_rev.push_back((GuestMessage*)callbacks);
                callbacks = callbacks->next_;
                context->CheckInterrupt();
              }
              std::reverse(messages_rev.begin(), messages_rev.end());

              for (auto&& order : messages_rev) {
                uint64_t args[] = {order->client_callback_arg_};
                auto kpcr = kernel::GetKPCR(context);

                auto current_irql = kpcr->current_irql;
                
                
                xenia_assert(current_irql == kernel::IRQL_PASSIVE);
                this->processor()->Execute(context->thread_state(),
                                           order->client_callback_, args,
                                           countof(args));
                delete order;
                context->CheckInterrupt();
#if AUDIOSYSTEM_NOWAIT_FOR_CALLBACK == 0
                signal_event_->Set();
#endif
              }
              messages_rev.clear();
              kernel::xboxkrnl::xeKeLeaveCriticalRegion(context);

            }
            return true;
          },
          kernel->GetSystemProcess()));
  guest_thread_->Create();
  kernel::xboxkrnl::xeKeSetPriorityThread(
      context, guest_thread_->guest_object<kernel::X_KTHREAD>(), 25);
  kernel::xboxkrnl::xeKeResumeThread(
      context, guest_thread_->guest_object<kernel::X_KTHREAD>());
}
void AudioSystem::WorkerThreadMain() {
  // Initialize driver and ringbuffer.
  Initialize();

  // Main run loop.
  while (worker_running_) {
    // These handles signify the number of submitted samples. Once we reach
    // 64 samples, we wait until our audio backend releases a semaphore
    // (signaling a sample has finished playing)
    auto result =
        xe::threading::WaitAny(wait_handles_, xe::countof(wait_handles_), true);
    if (result.first == xe::threading::WaitResult::kFailed) {
      // TODO: Assert?
      continue;
    }

    if (result.first == threading::WaitResult::kSuccess &&
        result.second == kMaximumClientCount) {
      // Shutdown event signaled.
      if (paused_) {
        pause_fence_.Signal();
        threading::Wait(resume_event_.get(), false);
      }

      continue;
    }

    // Number of clients pumped
    bool pumped = false;
    if (result.first == xe::threading::WaitResult::kSuccess) {
      auto index = result.second;
      auto global_lock = global_critical_region_.Acquire();
      uint32_t client_callback = clients_[index].callback;
      uint32_t client_callback_arg = clients_[index].wrapped_callback_arg;
      global_lock.unlock();
      client_callback_arg_in_ = client_callback_arg;
      client_callback_in_ = client_callback;

      auto msg = new GuestMessage();
      msg->client_callback_ = client_callback_in_;
      msg->client_callback_arg_ = client_callback_arg_in_;
      guest_worker_messages_.Push(&msg->list_entry);
#if AUDIOSYSTEM_NOWAIT_FOR_CALLBACK == 0
      threading::Wait(signal_event_.get(), false);
#endif
      pumped = true;
    }

    if (!worker_running_) {
      break;
    }

    if (!pumped) {
      SCOPE_profile_cpu_i("apu", "Sleep");
      xe::threading::Sleep(std::chrono::milliseconds(500));
    }
  }
  worker_running_ = false;

  // TODO(benvanik): call module API to kill?
}

int AudioSystem::FindFreeClient() {
  for (int i = 0; i < kMaximumClientCount; i++) {
    auto& client = clients_[i];
    if (!client.in_use) {
      return i;
    }
  }

  return -1;
}

void AudioSystem::Initialize() {}

void AudioSystem::Shutdown() {
  worker_running_ = false;
  shutdown_event_->Set();
  if (worker_thread_) {
    threading::Wait(worker_thread_.get(), false);
    Emulator::Get()->UnregisterGuestHardwareBlockThread(worker_thread_.get());
    worker_thread_.reset();
  }
}

X_STATUS AudioSystem::RegisterClient(uint32_t callback, uint32_t callback_arg,
                                     size_t* out_index) {
  auto global_lock = global_critical_region_.Acquire();

  auto index = FindFreeClient();
  assert_true(index >= 0);

  auto client_semaphore = client_semaphores_[index].get();
  auto ret = client_semaphore->Release(queued_frames_, nullptr);
  assert_true(ret);

  AudioDriver* driver;
  auto result = CreateDriver(index, client_semaphore, &driver);
  if (XFAILED(result)) {
    return result;
  }
  assert_not_null(driver);

  uint32_t ptr = memory()->SystemHeapAlloc(0x4);
  xe::store_and_swap<uint32_t>(memory()->TranslateVirtual(ptr), callback_arg);

  clients_[index] = {driver, callback, callback_arg, ptr, true};

  if (out_index) {
    *out_index = index;
  }

  return X_STATUS_SUCCESS;
}

void AudioSystem::SubmitFrame(size_t index, uint32_t samples_ptr) {
  SCOPE_profile_cpu_f("apu");

  auto global_lock = global_critical_region_.Acquire();
  assert_true(index < kMaximumClientCount);
  assert_true(clients_[index].driver != NULL);
  (clients_[index].driver)->SubmitFrame(samples_ptr);
}

void AudioSystem::UnregisterClient(size_t index) {
  SCOPE_profile_cpu_f("apu");

  auto global_lock = global_critical_region_.Acquire();
  assert_true(index < kMaximumClientCount);
  DestroyDriver(clients_[index].driver);
  memory()->SystemHeapFree(clients_[index].wrapped_callback_arg);
  clients_[index] = {0};

  // Drain the semaphore of its count.
  auto client_semaphore = client_semaphores_[index].get();
  xe::threading::WaitResult wait_result;
  do {
    wait_result = xe::threading::Wait(client_semaphore, false,
                                      std::chrono::milliseconds(0));
  } while (wait_result == xe::threading::WaitResult::kSuccess);
  assert_true(wait_result == xe::threading::WaitResult::kTimeout);
}

bool AudioSystem::Save(ByteStream* stream) {
  stream->Write(kAudioSaveSignature);

  // Count the number of used clients first.
  // Any gaps should be handled gracefully.
  uint32_t used_clients = 0;
  for (int i = 0; i < kMaximumClientCount; i++) {
    if (clients_[i].in_use) {
      used_clients++;
    }
  }

  stream->Write(used_clients);
  for (uint32_t i = 0; i < kMaximumClientCount; i++) {
    auto& client = clients_[i];
    if (!client.in_use) {
      continue;
    }

    stream->Write(i);
    stream->Write(client.callback);
    stream->Write(client.callback_arg);
    stream->Write(client.wrapped_callback_arg);
  }

  return true;
}

bool AudioSystem::Restore(ByteStream* stream) {
  if (stream->Read<uint32_t>() != kAudioSaveSignature) {
    XELOGE("AudioSystem::Restore - Invalid magic value!");
    return false;
  }

  uint32_t num_clients = stream->Read<uint32_t>();
  for (uint32_t i = 0; i < num_clients; i++) {
    auto id = stream->Read<uint32_t>();
    assert_true(id < kMaximumClientCount);

    auto& client = clients_[id];

    // Reset the semaphore and recreate the driver ourselves.
    if (client.driver) {
      UnregisterClient(id);
    }

    client.callback = stream->Read<uint32_t>();
    client.callback_arg = stream->Read<uint32_t>();
    client.wrapped_callback_arg = stream->Read<uint32_t>();

    client.in_use = true;

    auto client_semaphore = client_semaphores_[id].get();
    auto ret = client_semaphore->Release(queued_frames_, nullptr);
    assert_true(ret);

    AudioDriver* driver = nullptr;
    auto status = CreateDriver(id, client_semaphore, &driver);
    if (XFAILED(status)) {
      XELOGE(
          "AudioSystem::Restore - "
          "Call to CreateDriver "
          "failed with status "
          "{:08X}",
          status);
      return false;
    }

    assert_not_null(driver);
    client.driver = driver;
  }

  return true;
}

void AudioSystem::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  // Kind of a hack, but it works.
  shutdown_event_->Set();
  pause_fence_.Wait();

  xma_decoder_->Pause();
}

void AudioSystem::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;

  resume_event_->Set();

  xma_decoder_->Resume();
}

}  // namespace apu
}  // namespace xe
