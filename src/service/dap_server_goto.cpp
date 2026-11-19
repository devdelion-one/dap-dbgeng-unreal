#include "service/dap_server.h"

#include "util/dap_argument_reader.h"
#include "util/debugger_session_dap_factory.h"

namespace dap_dbgeng::service
{
namespace
{
namespace reader = util::dap_argument_reader;
namespace factory = util::debugger_session_dap_factory;

constexpr const char *kNotStopped = "Set next statement needs a stopped program.";
} // namespace

// Set next statement, step one: where a line would put the thread. Resolved
// against the top frame of `threadId` (an extension; without it, the current
// thread), within that frame's function only.
void dap_server::handle_goto_targets_request(const protocol::GotoTargetsRequest &request)
{
    const std::optional<std::string> &path = request.arguments.source.path;
    if (!path.has_value() || path->empty() || request.arguments.line <= 0)
    {
        send_error_response(request.seq, request.command, "The gotoTargets request requires a source path and a line.");
        return;
    }
    if (debugger_session_ == nullptr || is_execution_running_.load())
    {
        send_error_response(request.seq, request.command, kNotStopped);
        return;
    }
    const std::optional<int> requested_thread =
        has_current_request_json_ ? reader::try_get_int32(reader::get_arguments(current_request_json_), "threadId")
                                  : std::nullopt;
    const int line = static_cast<int>(request.arguments.line);
    try
    {
        debugger::debugger_session &session = require_debugger_session();
        int thread_id = 0;
        const debugger::goto_target target = dispatcher_.invoke([&]() {
            const std::uint32_t original_thread_id = session.get_current_thread_id();
            try
            {
                if (requested_thread.has_value() && *requested_thread > 0)
                {
                    session.set_current_thread(static_cast<std::uint32_t>(*requested_thread));
                }
                thread_id = static_cast<int>(session.get_current_thread_id());
                debugger::goto_target result = session.resolve_goto_target(*path, line);
                session.set_current_thread(original_thread_id);
                return result;
            }
            catch (...)
            {
                session.set_current_thread(original_thread_id);
                throw;
            }
        });
        const int id = next_goto_target_id_++;
        goto_targets_[id] = goto_target_context{thread_id, target};

        protocol::GotoTarget dap_target;
        dap_target.id = id;
        dap_target.label = fmt::format("line {}", target.line);
        dap_target.line = target.line;
        dap_target.instruction_pointer_reference = fmt::format("0x{:x}", target.address);
        protocol::GotoTargetsResponse response;
        response.body.targets.push_back(std::move(dap_target));
        send_response(request.seq, request.command, std::move(response));
    }
    catch (const std::exception &exception)
    {
        send_error_response(request.seq, request.command,
                            util::debugger_session_dispatcher::unwrap_failure_message(exception, exception.what()));
    }
}

// Step two: move the thread's instruction pointer there, then report a stop
// with reason "goto" (the code in between is skipped or runs again).
void dap_server::handle_goto_request(const protocol::GotoRequest &request)
{
    const int thread_id = request.arguments.thread_id;
    if (!require_positive_thread_id(request.seq, request.command, thread_id))
    {
        return;
    }
    if (debugger_session_ == nullptr || is_execution_running_.load())
    {
        send_error_response(request.seq, request.command, kNotStopped);
        return;
    }
    const auto found = goto_targets_.find(request.arguments.target_id);
    if (found == goto_targets_.end())
    {
        send_error_response(request.seq, request.command,
                            fmt::format("Unknown goto target '{}'. Request gotoTargets again.",
                                        request.arguments.target_id));
        return;
    }
    const goto_target_context target = found->second;
    try
    {
        debugger::debugger_session &session = require_debugger_session();
        dispatcher_.invoke([&]() {
            session.set_current_thread(static_cast<std::uint32_t>(thread_id));
            session.set_instruction_pointer(target.target.address);
        });
    }
    catch (const std::exception &exception)
    {
        send_error_response(request.seq, request.command,
                            util::debugger_session_dispatcher::unwrap_failure_message(exception, exception.what()));
        return;
    }
    send_response(request.seq, request.command, protocol::GotoResponse{});
    send(nlohmann::json(factory::create_stopped_event(protocol::StoppedEventBodyReason::Goto,
                                                      fmt::format("Moved to line {}.", target.target.line), thread_id,
                                                      std::nullopt)));
}
} // namespace dap_dbgeng::service
