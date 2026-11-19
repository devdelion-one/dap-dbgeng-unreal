#include "service/dap_server.h"

#include "util/debugger_session_dap_factory.h"

namespace dap_dbgeng::service
{
namespace factory = util::debugger_session_dap_factory;

// ---- Callback-driven event queueing ----------------------------------------

void dap_server::queue_thread_event(protocol::ThreadEventBodyReason reason)
{
    const std::optional<int> thread_id = try_get_current_thread_id();
    if (!thread_id.has_value())
    {
        return;
    }

    nlohmann::json message = nlohmann::json(factory::create_thread_event(reason, *thread_id));
    if (is_execution_running_.load() || has_pending_stopped_event())
    {
        std::lock_guard<std::mutex> lock(pending_thread_events_gate_);
        pending_thread_events_.push_back(std::move(message));
        return;
    }

    queue_session_event(std::move(message));
}

void dap_server::queue_session_termination(int exit_code)
{
    is_execution_running_.store(false);
    session_stop_or_exit_observed_.store(true);
    flush_pending_thread_events();
    queue_session_event(nlohmann::json(factory::create_exited_event(exit_code)));
    queue_session_event(nlohmann::json(protocol::TerminatedEvent{}));
    dispose_debugger_session();
    should_exit_ = true;
}

void dap_server::queue_session_ended()
{
    is_execution_running_.store(false);
    session_stop_or_exit_observed_.store(true);
    if (debugger_session_ == nullptr)
    {
        return;
    }
    flush_pending_thread_events();
    queue_session_event(nlohmann::json(protocol::TerminatedEvent{}));
    dispose_debugger_session();
    should_exit_ = true;
}

void dap_server::flush_pending_thread_events()
{
    std::deque<nlohmann::json> events;
    {
        std::lock_guard<std::mutex> lock(pending_thread_events_gate_);
        if (pending_thread_events_.empty())
        {
            return;
        }
        events.swap(pending_thread_events_);
    }
    for (auto &message : events)
    {
        queue_session_event(std::move(message));
    }
}

void dap_server::queue_session_event(nlohmann::json message)
{
    send(std::move(message));
}

// ---- Wait-for-session-event loop -------------------------------------------

void dap_server::start_waiting_for_session_event(debugger::debugger_session &session)
{
    std::unique_lock<std::mutex> lock(session_event_wait_gate_);
    if (wait_loop_active_ && !session_stop_or_exit_observed_.load())
    {
        return;
    }
    if (wait_loop_active_)
    {
        // The previous loop saw its stop and is finishing (restoring source-line
        // stepping after a step, say). A resume that lands in that window must
        // still get a loop, or nothing calls WaitForEvent and the target never runs.
        lock.unlock();
        if (wait_for_session_event_thread_.joinable())
        {
            wait_for_session_event_thread_.join();
        }
        lock.lock();
    }
    if (wait_for_session_event_thread_.joinable())
    {
        wait_for_session_event_thread_.join();
    }
    session_stop_or_exit_observed_.store(false);
    wait_loop_active_ = true;
    debugger::debugger_session *raw = &session;
    wait_for_session_event_thread_ = std::thread([this, raw]() { wait_for_session_event_loop(raw); });
}

void dap_server::wait_for_session_event_loop(debugger::debugger_session *session)
{
    int failures_in_a_row = 0;
    while (debugger_session_.get() == session && !session_stop_or_exit_observed_.load())
    {
        try
        {
            dispatcher_.invoke([session]() {
                session->wait_for_event();
                return 0;
            });
            failures_in_a_row = 0;
            if (session_stop_or_exit_observed_.load())
            {
                restore_source_line_stepping_if_needed(*session);
            }
            else if (debugger_session_.get() == session)
            {
                // The wait returned without a stop anyone reported: let the
                // target run on rather than leave it halted unnoticed.
                dispatcher_.invoke([session]() {
                    if (session->is_halted())
                    {
                        spdlog::debug("A wait ended halted with no stop to report; resuming.");
                        session->continue_();
                    }
                    return 0;
                });
            }
        }
        catch (const std::exception &exception)
        {
            if (debugger_session_.get() != session)
            {
                break;
            }
            // One failed wait (a deferred breakpoint that can't be resolved when a
            // module loads, say) doesn't end the session; only repeated failures do.
            spdlog::warn("Waiting for a debugger session event failed ({} in a row): {}", failures_in_a_row + 1,
                         exception.what());
            if (++failures_in_a_row >= 8)
            {
                spdlog::error("Giving up waiting for debugger session events: {}", exception.what());
                break;
            }
            // A failed wait can leave the target halted with no stop reported (a
            // breakpoint on a line with no code fails when its module loads).
            // Nobody is told about that halt, so let the target run on.
            if (!session_stop_or_exit_observed_.load())
            {
                try
                {
                    dispatcher_.invoke([session]() {
                        if (session->is_halted())
                        {
                            spdlog::warn("The target was left halted after a failed wait; resuming it.");
                            session->continue_();
                        }
                        return 0;
                    });
                }
                catch (const std::exception &resume_error)
                {
                    spdlog::warn("Could not resume after a failed wait: {}", resume_error.what());
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    // If the engine reported session-end inside its own callback, dispose_debugger_session
    // parked the engine here rather than tearing it down re-entrantly. Now that
    // wait_for_event has unwound, destroy it on the dispatcher thread (off the
    // callback stack) so the COM teardown is safe.
    drain_pending_disposed_session();

    {
        std::lock_guard<std::mutex> lock(session_event_wait_gate_);
        wait_loop_active_ = false;
    }
    restore_source_line_stepping_after_stop_ = false;
}

void dap_server::drain_pending_disposed_session()
{
    std::unique_ptr<debugger::debugger_session> owned = std::move(pending_disposed_session_);
    pending_disposed_session_ = nullptr;
    if (owned == nullptr)
    {
        return;
    }

    try
    {
        dispatcher_.invoke([&]() {
            owned.reset();
            return 0;
        });
    }
    catch (const std::exception &)
    {
        // Best effort: the engine may already be gone.
    }
}

void dap_server::join_wait_loop()
{
    std::thread thread_to_join;
    {
        std::lock_guard<std::mutex> lock(session_event_wait_gate_);
        thread_to_join = std::move(wait_for_session_event_thread_);
    }
    if (thread_to_join.joinable())
    {
        thread_to_join.join();
    }

    // Safety net: if a re-entrant teardown parked a session but the wait loop did
    // not drain it (e.g. it had already exited), destroy it now off the callback.
    drain_pending_disposed_session();
}
} // namespace dap_dbgeng::service
