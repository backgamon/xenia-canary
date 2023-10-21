/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2017 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_THREAD_H_
#define XENIA_CPU_THREAD_H_
#include "xenia/base/cvar.h"
#include "xenia/base/threading.h"

#include <cstdint>
#include "xenia/cpu/ppc/ppc_context.h"
DECLARE_bool(emulate_guest_interrupts_in_software);
namespace xe {
namespace cpu {
class ThreadState;

    // Represents a thread that runs guest code.
class Thread {
 public:
  Thread();
  ~Thread();

  static bool IsInThread();
  static Thread* GetCurrentThread();
  static uint32_t GetCurrentThreadId();
  ThreadState* thread_state() const { return thread_state_; }

  // True if the thread should be paused by the debugger.
  // All threads that can run guest code must be stopped for the debugger to
  // work properly.
  bool can_debugger_suspend() const { return can_debugger_suspend_; }
  void set_can_debugger_suspend(bool value) { can_debugger_suspend_ = value; }

  xe::threading::Thread* thread() { return thread_.get(); }
  const std::string& thread_name() const { return thread_name_; }

 protected:
  thread_local static Thread* current_thread_;

  ThreadState* thread_state_ = nullptr;
  std::unique_ptr<xe::threading::Thread> thread_ = nullptr;

  bool can_debugger_suspend_ = true;
  std::string thread_name_;
};

struct RunnableThread {
  threading::AtomicListEntry list_entry_;

  threading::Fiber* fiber_;
  cpu::ThreadState* thread_state_;
  uint32_t kthread_;
};

class HWThread {
  void ThreadFunc();

  void GuestIPIWorkerThreadFunc();
  bool HandleInterrupts();

  void RunRunnable(RunnableThread* runnable);
  // dpcs?
  void RunIdleProcess();

  static uintptr_t IPIWrapperFunction(void* ud);

 public:
  HWThread(uint32_t cpu_number, cpu::ThreadState* thread_state);
  ~HWThread();

  bool AreInterruptsDisabled();

  bool HasBooted() {
      return ready_;
  }
  volatile bool ready_ = false;
  std::unique_ptr<threading::Thread> os_thread_;

  std::unique_ptr<threading::Fiber> idle_process_fiber_;

  cpu::ThreadState* idle_process_threadstate_;
  uint32_t cpu_number_;

  threading::AtomicListHeader runnable_thread_list_;

  RunnableThread* last_run_thread_ = nullptr;


  threading::AtomicListHeader guest_ipi_list_;

  std::unique_ptr<threading::Thread> guest_ipi_dispatch_worker_;
  std::unique_ptr<threading::Event> guest_ipi_dispatch_event_;
  // set by kernel
  void (*idle_process_function_)(ppc::PPCContext* context) = nullptr;

  void EnqueueRunnableThread(RunnableThread* rth);

  void YieldToScheduler();

  bool TrySendInterruptFromHost(void (*ipi_func)(void*), void* ud);
  //SendGuestIPI is designed to run on a guest thread
  //it ought to be nonblocking, unlike TrySendHostIPI
  bool SendGuestIPI(void (*ipi_func)(void*), void* ud);
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_THREAD_H_
