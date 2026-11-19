#include "service/dap_server.h"

namespace dap_dbgeng::service
{
void dap_server::handle_variables_request(const protocol::VariablesRequest &request)
{
    const int variables_reference = request.arguments.variables_reference;
    const std::optional<protocol::VariablesArgumentsFilter> filter = request.arguments.filter;
    const auto clamp = [](const std::optional<std::int64_t> &value) -> int {
        return (value.has_value() && *value > 0 && *value <= std::numeric_limits<int>::max()) ? static_cast<int>(*value)
                                                                                              : 0;
    };
    const int start = clamp(request.arguments.start);
    const int count = clamp(request.arguments.count);

    if (variables_reference <= 0)
    {
        send_error_response(request.seq, request.command,
                            "The variables request requires a positive variablesReference.");
        return;
    }

    auto container_it = variables_by_reference_.find(variables_reference);
    if (container_it == variables_by_reference_.end())
    {
        send_error_response(request.seq, request.command,
                            fmt::format("Unknown variablesReference '{}'. Request scopes again for the current stop.",
                                        variables_reference));
        return;
    }

    if (!container_it->second.resolved)
    {
        // Built only now that someone opened it.
        variable_container_context &container = container_it->second;
        frame_context context;
        context.thread_id = container.thread_id;
        context.frame_number = container.frame_number;
        std::vector<debugger::variable_node> nodes;
        try
        {
            debugger::debugger_session &session = require_debugger_session();
            nodes = dispatcher_.invoke([&]() {
                const std::uint32_t original_thread_id = session.get_current_thread_id();
                session.set_current_thread(static_cast<std::uint32_t>(container.thread_id));
                auto result = session.expand_expression(container.frame_number, container.lazy_expression);
                session.set_current_thread(original_thread_id);
                return result;
            });
        }
        catch (const std::exception &exception)
        {
            spdlog::debug("Could not expand '{}': {}", container.lazy_expression, exception.what());
        }
        const std::string evaluate_name = container.lazy_evaluate_name;
        std::vector<protocol::Variable> built;
        built.reserve(nodes.size());
        for (const auto &node : nodes)
        {
            built.push_back(build_variable_tree(node, context, evaluate_name));
        }
        // build_variable_tree may have added entries to the map: look it up again.
        auto again = variables_by_reference_.find(variables_reference);
        again->second.variables = std::move(built);
        again->second.resolved = true;
        container_it = again;
    }

    // Indexed filter on a named container yields an empty array; a null or
    // Named filter returns the container's variables.
    static const std::vector<protocol::Variable> empty;
    const std::vector<protocol::Variable> &variables =
        (filter == protocol::VariablesArgumentsFilter::Indexed) ? empty : container_it->second.variables;

    protocol::VariablesResponse response;
    for (std::size_t index = static_cast<std::size_t>(start); index < variables.size(); ++index)
    {
        if (count > 0 && response.body.variables.size() >= static_cast<std::size_t>(count))
        {
            break;
        }
        response.body.variables.push_back(variables[index]);
    }
    send_response(request.seq, request.command, std::move(response));
}
} // namespace dap_dbgeng::service
