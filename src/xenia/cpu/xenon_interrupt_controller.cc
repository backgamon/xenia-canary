/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Xenia Canary. All rights reserved. * Released under the BSD
 *license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/xenon_interrupt_controller.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/mmio_handler.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread.h"
namespace xe {
namespace cpu {
XenonInterruptController::XenonInterruptController(HWThread* thread,
                                                   Processor* processor)
    : cpu_number_(thread->cpu_number()),
      owner_(thread),
      processor_(processor) {}

XenonInterruptController::~XenonInterruptController() {}

uint32_t XenonInterruptController::GuestMMIOAddress() const {
  xenia_assert(cpu_number_ < 6);
  return 0x7FFF0000 | (cpu_number_ << 12);
}
static void RaiseMMIOError() {
  xe::FatalError(
      "MMIO for interrupt controller unimplemented; 64-bit reads and "
      "writes unsupported by MMIO subsystem");
}
static uint32_t ReadRegisterStub(void* ppc_context, void* ud, uint32_t addr) {
  RaiseMMIOError();
  return 0;
}

static void WriteRegisterStub(void* ppc_context, void* ud, uint32_t addr,
                              uint32_t value) {
  RaiseMMIOError();
}

void XenonInterruptController::Initialize() {
  memset(data_, 0, sizeof(data_));
  processor_->memory()->AddVirtualMappedRange(GuestMMIOAddress(), 0xFFFF0000,
                                              0xFFFF, this, ReadRegisterStub,
                                              WriteRegisterStub);
}

void XenonInterruptController::SetInterruptSource(uint64_t src) {
  WriteRegisterOffset(0x50, src);
}

void XenonInterruptController::InterruptFunction(void* ud) {
  auto extargs = reinterpret_cast<ExternalInterruptArgs*>(ud);
  auto controller = extargs->controller_;

  controller->SetInterruptSource(extargs->source_);

  controller->owner_->_CallExternalInterruptHandler(
      cpu::ThreadState::GetContext(), controller);
}

void XenonInterruptController::SendExternalInterrupt(
    ExternalInterruptArgs& args) {
  // SetInterruptSource(args.source_);
  while (!owner_->TrySendInterruptFromHost(
      &XenonInterruptController::InterruptFunction, &args)) {
  }
}

void XenonInterruptController::WriteRegisterOffset(uint32_t offset,
                                                   uint64_t value) {
  xenia_assert(offset + 8 <= sizeof(data_));
  *reinterpret_cast<uint64_t*>(reinterpret_cast<char*>(&data_[0]) + offset) =
      value;
}
uint64_t XenonInterruptController::ReadRegisterOffset(uint32_t offset) {
  xenia_assert(offset + 8 <= sizeof(data_));
  return *reinterpret_cast<uint64_t*>(reinterpret_cast<char*>(&data_[0]) +
                                      offset);
}

ppc::PPCInterruptRequest* XenonInterruptController::AllocateInterruptRequest() {
  auto head = free_interrupt_requests_.Pop();
  if (head) {
    return new (head) ppc::PPCInterruptRequest();
  } else {
    return new ppc::PPCInterruptRequest();
  }
}
void XenonInterruptController::FreeInterruptRequest(
    ppc::PPCInterruptRequest* request) {
  // limit the number of available interrupts in the list to a sane value
  // if we hit this number, the guest has probably frozen and isn't processing the interrupts we're sending
  if (free_interrupt_requests_.depth() < 256) {
    free_interrupt_requests_.Push(&request->list_entry_);
  } else {
    delete request;
  }
}
}  // namespace cpu
}  // namespace xe
