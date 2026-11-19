#include "debugger/debugger_session.h"

#include "debugger/debugger_session_internal.h"

namespace dap_dbgeng::debugger
{
// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------
void debugger_session::continue_()
{
    expect_break_in_.store(false);
    // Resuming invalidates the captured exception; exceptionInfo must never
    // describe a previous stop.
    last_exception_.reset();
    execute_debugger_command("g", "Could not continue execution");
}

void debugger_session::step_into()
{
    expect_break_in_.store(false);
    last_exception_.reset();
    execute_debugger_command("t", "Could not step into");
}

void debugger_session::step_over()
{
    expect_break_in_.store(false);
    last_exception_.reset();
    execute_debugger_command("p", "Could not step over");
}

void debugger_session::step_out()
{
    expect_break_in_.store(false);
    last_exception_.reset();
    execute_debugger_command("gu", "Could not step out");
}

void debugger_session::enable_instruction_stepping()
{
    throw_if_disposed();
    execute_debugger_command("l-t", "Could not enable instruction stepping");
}

void debugger_session::interrupt()
{
    throw_if_disposed();
    // The one cross-thread-safe engine call: request an active interrupt (the
    // programmatic equivalent of Ctrl+Break). Works for user-mode and kernel.
    expect_break_in_.store(true);
    check_hr(control_->SetInterrupt(DEBUG_INTERRUPT_ACTIVE), "Could not interrupt target");
}

void debugger_session::wait_for_event(std::uint32_t timeout)
{
    throw_if_disposed();
    check_hr(control_->WaitForEvent(kWaitDefaultFlags, timeout), "WaitForEvent failed");
}
bool debugger_session::is_halted()
{
    throw_if_disposed();
    ULONG status = 0;
    check_hr(control_->GetExecutionStatus(&status), "Could not read the execution status");
    return status == DEBUG_STATUS_BREAK;
}
} // namespace dap_dbgeng::debugger
