#include "debugger/debugger_session.h"

#include "debugger/debugger_session_internal.h"

namespace dap_dbgeng::debugger
{
namespace
{
// Read one symbol-group string field (name / value text / type name) into a
// trimmed std::string; empty when the engine reports nothing.
std::string read_symbol_name(IDebugSymbolGroup2 *group, ULONG index)
{
    std::vector<char> buffer(kSymbolBufferBytes);
    ULONG size = 0;
    if (SUCCEEDED(group->GetSymbolName(index, buffer.data(), static_cast<ULONG>(buffer.size()), &size)) && size != 0)
    {
        return buffer_to_trimmed_string(buffer, size);
    }
    return {};
}

std::string read_symbol_value(IDebugSymbolGroup2 *group, ULONG index)
{
    std::vector<char> buffer(kLocalValueBufferBytes);
    ULONG size = 0;
    if (SUCCEEDED(group->GetSymbolValueText(index, buffer.data(), static_cast<ULONG>(buffer.size()), &size)) &&
        size != 0)
    {
        return buffer_to_trimmed_string(buffer, size);
    }
    return {};
}

std::string read_symbol_type(IDebugSymbolGroup2 *group, ULONG index)
{
    std::vector<char> buffer(kSymbolBufferBytes);
    ULONG size = 0;
    if (SUCCEEDED(group->GetSymbolTypeName(index, buffer.data(), static_cast<ULONG>(buffer.size()), &size)) &&
        size != 0)
    {
        return buffer_to_trimmed_string(buffer, size);
    }
    return {};
}

bool is_plain_number_type(const std::string &type)
{
    static const std::set<std::string> plain{"float", "double", "int", "unsigned int", "short", "unsigned short",
                                             "char", "unsigned char", "signed char", "__int64", "unsigned __int64",
                                             "long", "unsigned long", "bool", "int64", "uint64", "int32", "uint32",
                                             "int16", "uint16", "int8", "uint8"};
    return plain.count(type) != 0;
}

std::string compact_number(const std::string &type, const std::string &value)
{
    if (type != "float" && type != "double")
    {
        return value;
    }
    char *end = nullptr;
    const double number = std::strtod(value.c_str(), &end);
    if (end == value.c_str())
    {
        return value;
    }
    return fmt::format("{:.6g}", number);
}

// Static data members come back with no value ("<Value unavailable error>");
// Visual Studio hides them, and so do we. A small struct of plain numbers
// (FLinearColor, FVector2D, FIntPoint...) reads as "(R=0.2, G=0.8, ...)"
// instead of its type name.
void tidy_tree(std::vector<variable_node> &nodes)
{
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
                               [](const variable_node &n) { return n.type == "<Value unavailable error>"; }),
                nodes.end());
    for (auto &node : nodes)
    {
        tidy_tree(node.children);
        const bool aggregate_text = node.value.rfind("struct ", 0) == 0 || node.value.rfind("class ", 0) == 0;
        if (!aggregate_text || node.children.empty())
        {
            continue;
        }
        std::vector<std::string> parts;
        bool plain = true;
        for (const auto &child : node.children)
        {
            if (is_plain_number_type(child.type) && child.children.empty())
            {
                parts.push_back(child.name + "=" + compact_number(child.type, child.value));
            }
            else if (!(child.type.size() > 0 && child.type.back() == ']'))
            {
                plain = false;
                break;
            }
        }
        if (plain && !parts.empty() && parts.size() <= 6)
        {
            std::string summary = "(";
            for (std::size_t i = 0; i < parts.size(); ++i)
            {
                summary += (i == 0 ? "" : ", ") + parts[i];
            }
            node.value = summary + ")";
        }
    }
}

// A pointer type (trailing '*') is expandable but we do not auto-follow it: that
// risks cycles (linked lists) and unbounded depth. Such nodes stay leaves.
bool type_is_pointer(const std::string &type)
{
    for (auto it = type.rbegin(); it != type.rend(); ++it)
    {
        if (*it == '*')
        {
            return true;
        }
        if (std::isspace(static_cast<unsigned char>(*it)) == 0)
        {
            return false;
        }
    }
    return false;
}

// Hops from a symbol to a top-level (unparented) symbol; top-level == 0.
int symbol_depth(const std::vector<DEBUG_SYMBOL_PARAMETERS> &params, ULONG index)
{
    int depth = 0;
    ULONG parent = params[index].ParentSymbol;
    while (parent != kDebugAnyId && parent < params.size())
    {
        ++depth;
        parent = params[parent].ParentSymbol;
    }
    return depth;
}
} // namespace

// ---------------------------------------------------------------------------
// Stack / vars
// ---------------------------------------------------------------------------
std::vector<stack_frame_info> debugger_session::get_stack_trace(int max_frames)
{
    return get_stack_trace_details(max_frames).frames;
}

int debugger_session::get_stack_trace_fetch_count(int total_frames, int max_frames)
{
    if (total_frames < 0)
    {
        throw std::out_of_range("totalFrames");
    }
    if (max_frames < 0)
    {
        throw std::out_of_range("maxFrames");
    }
    return std::min(total_frames, max_frames);
}

stack_trace_details debugger_session::get_stack_trace_details(int max_frames)
{
    throw_if_disposed();
    if (max_frames < 0)
    {
        throw std::out_of_range("maxFrames");
    }

    // Walk the full stack once (capped) so totalFrames reflects the real depth for
    // paging, then resolve each returned frame's name and source via the symbol APIs.
    constexpr ULONG kMaxStackDepth = 1024;
    std::vector<DEBUG_STACK_FRAME_EX> frames(kMaxStackDepth);
    ULONG filled = 0;
    check_hr(control_->GetStackTraceEx(0, 0, 0, frames.data(), static_cast<ULONG>(frames.size()), &filled),
             "Could not enumerate stack trace");
    frames.resize(filled);

    int total_frames = static_cast<int>(filled);
    const int fetch_count = get_stack_trace_fetch_count(total_frames, max_frames);

    std::vector<stack_frame_info> result;
    result.reserve(fetch_count);
    for (int i = 0; i < fetch_count; ++i)
    {
        const DEBUG_STACK_FRAME_EX &frame = frames[i];
        stack_frame_info info;
        info.frame_number = frame.FrameNumber;
        info.instruction_offset = frame.InstructionOffset;
        info.name = try_get_frame_name(frame.InstructionOffset);
        info.source = try_get_frame_source(frame.InstructionOffset);
        result.push_back(std::move(info));
    }
    add_script_frames(frames, result, total_frames);

    return stack_trace_details{std::move(result), total_frames};
}

std::vector<named_value_info> debugger_session::get_locals(std::uint32_t frame_number)
{
    throw_if_disposed();

    // Set the scope to the requested frame, then read locals + arguments from the
    // engine's scope symbol group (the structured path WinDbg's Locals window
    // uses; `dv` text omits variable names in kernel mode).
    execute_command_with_output(fmt::format(".frame {}", frame_number), /*suppress_output_events=*/true);

    IDebugSymbolGroup2 *group = nullptr;
    {
        PDEBUG_SYMBOL_GROUP base_group = nullptr;
        const HRESULT hr = symbols_->GetScopeSymbolGroup(kDebugScopeGroupAll, nullptr, &base_group);
        if (FAILED(hr) || base_group == nullptr)
        {
            return {};
        }
        // GetScopeSymbolGroup returns IDebugSymbolGroup; QI for the 2 variant.
        const HRESULT qi = base_group->QueryInterface(__uuidof(IDebugSymbolGroup2), reinterpret_cast<PVOID *>(&group));
        base_group->Release();
        if (FAILED(qi) || group == nullptr)
        {
            return {};
        }
    }

    std::vector<named_value_info> result;
    ULONG count = 0;
    if (SUCCEEDED(group->GetNumberSymbols(&count)))
    {
        result.reserve(count);
        std::vector<char> buffer(kLocalValueBufferBytes);
        for (ULONG index = 0; index < count; ++index)
        {
            ULONG name_size = 0;
            if (FAILED(group->GetSymbolName(index, buffer.data(), static_cast<ULONG>(buffer.size()), &name_size)) ||
                name_size == 0)
            {
                continue;
            }
            const std::string name = buffer_to_trimmed_string(buffer, name_size);
            if (name.empty())
            {
                continue;
            }

            ULONG value_size = 0;
            std::string value;
            if (SUCCEEDED(
                    group->GetSymbolValueText(index, buffer.data(), static_cast<ULONG>(buffer.size()), &value_size)) &&
                value_size != 0)
            {
                value = buffer_to_trimmed_string(buffer, value_size);
            }

            result.push_back(named_value_info{name, normalize_value_text(value)});
        }
    }

    group->Release();
    return result;
}

std::vector<variable_node> debugger_session::get_locals_tree(std::uint32_t frame_number)
{
    throw_if_disposed();
    const auto t_frame_start = std::chrono::steady_clock::now();

    // Same scope symbol group get_locals reads, but here we expand aggregate
    // members in place so structs/classes become a tree. The group is only live
    // for this call; we snapshot the whole (bounded) tree before releasing it, so
    // later variables requests never need the group again.
    execute_command_with_output(fmt::format(".frame {}", frame_number), /*suppress_output_events=*/true);

    IDebugSymbolGroup2 *group = nullptr;
    {
        PDEBUG_SYMBOL_GROUP base_group = nullptr;
        const HRESULT hr = symbols_->GetScopeSymbolGroup(kDebugScopeGroupAll, nullptr, &base_group);
        if (FAILED(hr) || base_group == nullptr)
        {
            return {};
        }
        const HRESULT qi = base_group->QueryInterface(__uuidof(IDebugSymbolGroup2), reinterpret_cast<PVOID *>(&group));
        base_group->Release();
        if (FAILED(qi) || group == nullptr)
        {
            return {};
        }
    }

    // Phase 1: expand value aggregates, bounded by depth and a total-node budget.
    // ExpandSymbol inserts children right after the parent and shifts every later
    // index, so we re-query parameters from scratch after each expansion and act
    // on one symbol per scan. The EXPANDED flag stops us re-expanding a node;
    // pointers are skipped (no auto-follow). The loop terminates because each
    // successful expansion sets a flag, shrinking the eligible set.
    // One depth level per pass: read the parameters once, then expand that
    // level's aggregates from the last index to the first. Expanding a symbol
    // only shifts the indices after it, so the indices still to visit stay
    // valid. Re-reading everything after each single expansion made a large
    // frame (UEditorEngine::Tick) take over 20 s.
    for (int level = 0; level < kMaxExpansionDepth; ++level)
    {
        ULONG count = 0;
        if (FAILED(group->GetNumberSymbols(&count)) || count == 0 || count >= kMaxExpandedNodes)
        {
            break;
        }
        std::vector<DEBUG_SYMBOL_PARAMETERS> params(count);
        if (FAILED(group->GetSymbolParameters(0, count, params.data())))
        {
            break;
        }

        bool expanded_any = false;
        ULONG budget_left = static_cast<ULONG>(kMaxExpandedNodes) - count;
        for (ULONG i = count; i-- > 0;)
        {
            const DEBUG_SYMBOL_PARAMETERS &p = params[i];
            if ((p.Flags & kDebugSymbolExpanded) != 0 || p.SubElements == 0 || symbol_depth(params, i) != level)
            {
                continue;
            }
            if (p.SubElements > budget_left || type_is_pointer(read_symbol_type(group, i)))
            {
                continue;
            }
            if (SUCCEEDED(group->ExpandSymbol(i, TRUE)))
            {
                expanded_any = true;
                budget_left -= p.SubElements;
            }
        }
        (void)expanded_any;
    }

    std::size_t node_count = 0;
    // Phase 2: read the now-stable flat list and rebuild the tree via ParentSymbol.
    std::vector<variable_node> roots;
    ULONG count = 0;
    if (SUCCEEDED(group->GetNumberSymbols(&count)) && count != 0)
    {
        std::vector<DEBUG_SYMBOL_PARAMETERS> params(count);
        if (SUCCEEDED(group->GetSymbolParameters(0, count, params.data())))
        {
            std::vector<variable_node> nodes(count);
            for (ULONG i = 0; i < count; ++i)
            {
                nodes[i].name = read_symbol_name(group, i);
                nodes[i].value = normalize_value_text(read_symbol_value(group, i));
                nodes[i].type = read_symbol_type(group, i);
                // The fields inside a value already shown as an object pointer
                // (DebugPtr, Handle...) are dropped; don't read them.
                const ULONG parent = params[i].ParentSymbol;
                const bool inside_object_pointer =
                    parent != kDebugAnyId && parent < i && !nodes[parent].lazy_expression.empty() &&
                    !nodes[parent].reader_replaces;
                if (!inside_object_pointer)
                {
                    apply_unreal_view(group, i, nodes[i], /*summarize=*/false);
                }
                ++node_count;
                // Expandable but not inlined (depth/budget cap or pointer): leave
                // children empty so the caller can still flag it if it wants.
                nodes[i].is_expandable = params[i].SubElements != 0 && (params[i].Flags & kDebugSymbolExpanded) == 0;
                // Address + size feed memoryReference and data breakpoints;
                // enregistered symbols have neither (left 0).
                ULONG64 offset = 0;
                if (SUCCEEDED(group->GetSymbolOffset(i, &offset)))
                {
                    nodes[i].address = offset;
                }
                ULONG size = 0;
                if (SUCCEEDED(group->GetSymbolSize(i, &size)))
                {
                    nodes[i].size = size;
                }
            }

            // Children always have a higher index than their parent, so linking
            // high-to-low moves a fully built child subtree into its parent before
            // the parent itself is moved. Inserting at the front preserves order.
            for (ULONG idx = count; idx-- > 0;)
            {
                const ULONG parent = params[idx].ParentSymbol;
                if (parent != kDebugAnyId && parent < count)
                {
                    nodes[parent].children.insert(nodes[parent].children.begin(), std::move(nodes[idx]));
                }
            }
            for (ULONG i = 0; i < count; ++i)
            {
                if (params[i].ParentSymbol == kDebugAnyId)
                {
                    roots.push_back(std::move(nodes[i]));
                }
            }
        }
    }

    group->Release();
    for (auto &root : roots)
    {
        expand_reader_children(root);
    }
    tidy_tree(roots);
    spdlog::debug("locals tree: {} nodes in {} ms", node_count,
                  std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                        t_frame_start)
                      .count());
    return roots;
}

void debugger_session::apply_unreal_view(IDebugSymbolGroup2 *group, ULONG index, variable_node &node, bool summarize)
{
    if (unreal_reader_ == nullptr)
    {
        return;
    }
    DEBUG_SYMBOL_ENTRY entry{};
    if (FAILED(group->GetSymbolEntryInformation(index, &entry)))
    {
        return;
    }
    ULONG64 address = 0;
    if (FAILED(group->GetSymbolOffset(index, &address)))
    {
        address = 0;
    }
    const auto view =
        unreal_reader_->view(node.type, entry.ModuleBase, entry.TypeId, address, node.value, /*with_children=*/false);
    if (!view.has_value())
    {
        const bool aggregate_text = node.value.rfind("struct ", 0) == 0 || node.value.rfind("class ", 0) == 0;
        if (summarize && aggregate_text && address != 0)
        {
            if (const auto summary = unreal_reader_->summary(entry.ModuleBase, entry.TypeId, address))
            {
                node.value = *summary;
            }
        }
        return;
    }
    node.value = view->display;
    if (!view->expand_expression.empty())
    {
        node.lazy_expression = view->expand_expression;
    }
    if (view->replaces_children && view->lazy_children)
    {
        // Read only when opened (a map's items).
        if (address != 0)
        {
            node.lazy_expression =
                unreal_reader::reader_expression(entry.ModuleBase, entry.TypeId, address, node.type);
        }
        return;
    }
    if (view->replaces_children)
    {
        node.reader_replaces = true;
        node.reader_children = view->children;
        if (address != 0)
        {
            node.lazy_expression = unreal_reader::reader_expression(entry.ModuleBase, entry.TypeId, address, node.type);
        }
    }
}

variable_node debugger_session::node_from_reader_child(const unreal_child &child)
{
    variable_node node;
    node.name = child.name;
    node.type = child.type;
    node.address = child.address;
    if (const auto plain = unreal_reader_->plain_value(child.type, child.address))
    {
        node.value = *plain;
        return node;
    }
    const auto view =
        unreal_reader_->view(child.type, child.module_base, child.type_id, child.address, "", /*with_children=*/false);
    if (view.has_value())
    {
        node.value = view->display;
        if (!view->expand_expression.empty())
        {
            node.lazy_expression = view->expand_expression;
        }
        else if (view->replaces_children)
        {
            node.lazy_expression = child.expression;
        }
    }
    else if (!child.type.empty() && child.type.back() == '*')
    {
        // A plain pointer item: its address, opening onto what it points to.
        node.value = unreal_reader_->brief(child.module_base, child.type, child.type_id, child.address);
        const auto pointer = unreal_reader_->pointer_at(child.address);
        if (pointer && *pointer != 0)
        {
            node.lazy_expression = unreal_reader_->pointee_expression(child.module_base, child.type, *pointer);
        }
    }
    else
    {
        node.value = unreal_reader_->summary(child.module_base, child.type_id, child.address).value_or("{...}");
        node.lazy_expression = child.expression;
    }
    node.is_expandable = !node.lazy_expression.empty();
    return node;
}

void debugger_session::expand_reader_children(variable_node &node)
{
    if (node.reader_replaces)
    {
        std::vector<variable_node> raw = std::move(node.children);
        node.children.clear();
        for (const auto &child : node.reader_children)
        {
            node.children.push_back(node_from_reader_child(child));
        }
        node.reader_children.clear();
        node.reader_replaces = false;
        node.lazy_expression.clear();
        if (!raw.empty())
        {
            variable_node raw_view;
            raw_view.name = "[Raw View]";
            raw_view.type = node.type;
            raw_view.children = std::move(raw);
            node.children.push_back(std::move(raw_view));
        }
        return;
    }
    if (!node.lazy_expression.empty())
    {
        // An object pointer opens onto the object; its raw handle fields go.
        node.children.clear();
        node.is_expandable = true;
        return;
    }
    for (auto &child : node.children)
    {
        expand_reader_children(child);
    }
}

std::vector<variable_node> debugger_session::expand_expression(std::uint32_t frame_number,
                                                               const std::string &expression_text)
{
    throw_if_disposed();
    const bool raw = expression_text.rfind("raw:", 0) == 0;
    const std::string expression = raw ? expression_text.substr(4) : expression_text;
    if (const auto target = unreal_reader::parse_reader_expression(expression))
    {
        // A value the reader opens itself, no engine expression involved; its
        // raw members are an ordinary typed expression, parsed only if opened.
        const std::string typed = unreal_reader_ != nullptr
                                      ? unreal_reader_->typed_expression(target->module_base, target->type,
                                                                         target->address)
                                      : std::string();
        if (raw || unreal_reader_ == nullptr)
        {
            // The container's own fields, from its layout: no expression for
            // dbgeng to parse.
            if (unreal_reader_ != nullptr)
            {
                if (const auto fields = unreal_reader_->members(target->module_base, target->type_id, target->address))
                {
                    std::vector<variable_node> out;
                    for (const auto &child : *fields)
                    {
                        out.push_back(node_from_reader_child(child));
                    }
                    return out;
                }
            }
            return expand_expression(frame_number, "raw:" + typed);
        }
        const auto view = unreal_reader_->view(target->type, target->module_base, target->type_id, target->address, "");
        std::vector<variable_node> out;
        if (view.has_value() && view->replaces_children)
        {
            for (const auto &child : view->children)
            {
                out.push_back(node_from_reader_child(child));
            }
            variable_node raw_view;
            raw_view.name = "[Raw View]";
            raw_view.type = target->type;
            raw_view.lazy_expression = "raw:" + expression;
            raw_view.is_expandable = true;
            out.push_back(std::move(raw_view));
            return out;
        }
        if (view.has_value() && !view->expand_expression.empty())
        {
            return expand_expression(frame_number, view->expand_expression);
        }
        return expand_expression(frame_number, typed);
    }

    // `*(Module!Type *)0x...` needs no frame: an empty group is far cheaper than
    // the frame's whole scope group.
    const bool absolute = expression.rfind("*(", 0) == 0 && expression.find(")0x") != std::string::npos;
    ULONG index = DEBUG_ANY_ID;
    IDebugSymbolGroup2 *group = nullptr;
    if (absolute)
    {
        if (FAILED(symbols_->CreateSymbolGroup2(&group)) || group == nullptr)
        {
            throw std::runtime_error("Could not create a symbol group.");
        }
        const HRESULT added = group->AddSymbol(expression.c_str(), &index);
        if (FAILED(added) || index == DEBUG_ANY_ID)
        {
            group->Release();
            throw std::runtime_error(fmt::format("Could not resolve '{}'.", expression));
        }
    }
    else
    {
        group = open_scope_symbol_group(frame_number, expression, index);
    }
    struct group_releaser
    {
        IDebugSymbolGroup2 *g;
        ~group_releaser()
        {
            g->Release();
        }
    } releaser{group};

    std::vector<variable_node> out;
    if (!raw && unreal_reader_ != nullptr)
    {
        variable_node self;
        self.value = normalize_value_text(read_symbol_value(group, index));
        self.type = read_symbol_type(group, index);
        DEBUG_SYMBOL_ENTRY entry{};
        ULONG64 address = 0;
        group->GetSymbolOffset(index, &address);
        if (SUCCEEDED(group->GetSymbolEntryInformation(index, &entry)))
        {
            const auto view = unreal_reader_->view(self.type, entry.ModuleBase, entry.TypeId, address, self.value);
            if (view.has_value() && view->replaces_children)
            {
                for (const auto &child : view->children)
                {
                    out.push_back(node_from_reader_child(child));
                }
                variable_node raw_view;
                raw_view.name = "[Raw View]";
                raw_view.type = self.type;
                raw_view.lazy_expression = "raw:" + expression;
                raw_view.is_expandable = true;
                out.push_back(std::move(raw_view));
                return out;
            }
            if (view.has_value() && !view->expand_expression.empty() && view->expand_expression != expression)
            {
                return expand_expression(frame_number, view->expand_expression);
            }
        }
    }

    DEBUG_SYMBOL_PARAMETERS params{};
    if (FAILED(group->GetSymbolParameters(index, 1, &params)) || params.SubElements == 0 ||
        FAILED(group->ExpandSymbol(index, TRUE)))
    {
        return out;
    }
    ULONG count = 0;
    if (FAILED(group->GetNumberSymbols(&count)) || count <= index + 1)
    {
        return out;
    }
    std::vector<DEBUG_SYMBOL_PARAMETERS> children(count);
    if (FAILED(group->GetSymbolParameters(0, count, children.data())))
    {
        return out;
    }
    for (ULONG i = index + 1; i < count; ++i)
    {
        if (children[i].ParentSymbol != index)
        {
            continue;
        }
        variable_node child;
        child.name = read_symbol_name(group, i);
        child.type = read_symbol_type(group, i);
        if (child.type == "<Value unavailable error>")
        {
            continue; // a static member
        }
        child.value = normalize_value_text(read_symbol_value(group, i));
        ULONG64 child_address = 0;
        if (SUCCEEDED(group->GetSymbolOffset(i, &child_address)))
        {
            child.address = child_address;
        }
        apply_unreal_view(group, i, child);
        child.reader_children.clear();
        child.reader_replaces = false;
        // A vtable's slots (__vfptr) are function pointers, and dbgeng takes
        // seconds to fail on their array type: not worth opening.
        if (child.lazy_expression.empty() && children[i].SubElements != 0 && child_address != 0 &&
            !type_is_pointer(child.type) && child.name != "__vfptr" && unreal_reader_ != nullptr)
        {
            DEBUG_SYMBOL_ENTRY entry{};
            if (SUCCEEDED(group->GetSymbolEntryInformation(i, &entry)))
            {
                child.lazy_expression = unreal_reader_->typed_expression(entry.ModuleBase, child.type, child_address);
            }
        }
        child.is_expandable = !child.lazy_expression.empty();
        out.push_back(std::move(child));
    }
    return out;
}

std::optional<std::string> debugger_session::hover_expression(std::uint32_t frame_number,
                                                              const std::string &expression)
{
    throw_if_disposed();
    // The first name in "A", "A.B" or "A->B".
    std::size_t end = 0;
    while (end < expression.size() &&
           (std::isalnum(static_cast<unsigned char>(expression[end])) != 0 || expression[end] == '_'))
    {
        ++end;
    }
    if (end == 0 || std::isdigit(static_cast<unsigned char>(expression[0])) != 0)
    {
        return std::nullopt;
    }
    const std::string first = expression.substr(0, end);
    const std::string rest = expression.substr(end);
    if (!rest.empty() && rest.rfind(".", 0) != 0 && rest.rfind("->", 0) != 0 && rest.rfind("[", 0) != 0)
    {
        return std::nullopt;
    }

    execute_command_with_output(fmt::format(".frame {}", frame_number), /*suppress_output_events=*/true);
    PDEBUG_SYMBOL_GROUP base_group = nullptr;
    if (FAILED(symbols_->GetScopeSymbolGroup(kDebugScopeGroupAll, nullptr, &base_group)) || base_group == nullptr)
    {
        return std::nullopt;
    }
    IDebugSymbolGroup2 *group = nullptr;
    const HRESULT qi = base_group->QueryInterface(__uuidof(IDebugSymbolGroup2), reinterpret_cast<PVOID *>(&group));
    base_group->Release();
    if (FAILED(qi) || group == nullptr)
    {
        return std::nullopt;
    }
    bool is_local = false;
    bool has_this = false;
    ULONG count = 0;
    if (SUCCEEDED(group->GetNumberSymbols(&count)) && count != 0)
    {
        // The frame's scope group is shared and may still be expanded from the
        // locals tree: only its top-level symbols are locals.
        std::vector<DEBUG_SYMBOL_PARAMETERS> params(count);
        if (SUCCEEDED(group->GetSymbolParameters(0, count, params.data())))
        {
            for (ULONG i = 0; i < count; ++i)
            {
                if (params[i].ParentSymbol != kDebugAnyId)
                {
                    continue;
                }
                const std::string name = read_symbol_name(group, i);
                is_local = is_local || name == first;
                has_this = has_this || name == "this";
            }
        }
    }
    group->Release();
    if (is_local)
    {
        return expression;
    }
    if (has_this)
    {
        return "this->" + expression;
    }
    return std::nullopt;
}

IDebugSymbolGroup2 *debugger_session::open_scope_symbol_group(std::uint32_t frame_number, const std::string &expression,
                                                              ULONG &symbol_index)
{
    // Scope to the frame, then add the target expression as a symbol in the
    // scope group - the engine's own frame-scoped resolver (the same path
    // WinDbg's Locals editor uses). No expression-command string.
    execute_command_with_output(fmt::format(".frame {}", frame_number), /*suppress_output_events=*/true);

    IDebugSymbolGroup2 *group = nullptr;
    {
        PDEBUG_SYMBOL_GROUP base_group = nullptr;
        const HRESULT hr = symbols_->GetScopeSymbolGroup(kDebugScopeGroupAll, nullptr, &base_group);
        if (FAILED(hr) || base_group == nullptr)
        {
            throw std::runtime_error("Could not open the scope symbol group.");
        }
        const HRESULT qi = base_group->QueryInterface(__uuidof(IDebugSymbolGroup2), reinterpret_cast<PVOID *>(&group));
        base_group->Release();
        if (FAILED(qi) || group == nullptr)
        {
            throw std::runtime_error("Could not access the scope symbol group.");
        }
    }

    symbol_index = DEBUG_ANY_ID;
    if (FAILED(group->AddSymbol(expression.c_str(), &symbol_index)) || symbol_index == DEBUG_ANY_ID)
    {
        group->Release();
        throw std::runtime_error(fmt::format("Could not resolve '{}' in the current frame.", expression));
    }
    return group;
}

namespace
{
// Builds the result node for a scope-group symbol resolved by AddSymbol.
variable_node create_symbol_node(IDebugSymbolGroup2 *group, ULONG index, const std::string &expression)
{
    variable_node node;
    // Display name is the trailing path segment (after the last '.' or ']').
    const std::size_t separator = expression.find_last_of(".]");
    node.name = separator == std::string::npos ? expression : expression.substr(separator + 1);
    node.value = normalize_value_text(read_symbol_value(group, index));
    node.type = read_symbol_type(group, index);
    DEBUG_SYMBOL_PARAMETERS params{};
    if (SUCCEEDED(group->GetSymbolParameters(index, 1, &params)))
    {
        node.is_expandable = params.SubElements != 0;
    }
    return node;
}
} // namespace

variable_node debugger_session::set_local_value(std::uint32_t frame_number, const std::string &expression,
                                                const std::string &value)
{
    throw_if_disposed();

    ULONG index = DEBUG_ANY_ID;
    IDebugSymbolGroup2 *group = open_scope_symbol_group(frame_number, expression, index);
    struct group_releaser
    {
        IDebugSymbolGroup2 *g;
        ~group_releaser()
        {
            g->Release();
        }
    } releaser{group};

    // Write the value with the engine's own type-aware text assignment.
    check_hr(group->WriteSymbol(index, value.c_str()), fmt::format("Could not assign '{}' to '{}'", value, expression));
    return create_symbol_node(group, index, expression);
}

variable_node debugger_session::get_local_value(std::uint32_t frame_number, const std::string &expression)
{
    throw_if_disposed();

    ULONG index = DEBUG_ANY_ID;
    IDebugSymbolGroup2 *group = open_scope_symbol_group(frame_number, expression, index);
    struct group_releaser
    {
        IDebugSymbolGroup2 *g;
        ~group_releaser()
        {
            g->Release();
        }
    } releaser{group};

    // AddSymbol accepts unresolved expressions and reads them back as an
    // engine error banner with no type; only a typed symbol is a real value.
    variable_node node = create_symbol_node(group, index, expression);
    if (node.type.empty())
    {
        throw std::runtime_error(fmt::format("Could not resolve '{}' in the current frame.", expression));
    }
    apply_unreal_view(group, index, node);
    node.reader_children.clear();
    node.reader_replaces = false;
    if (node.lazy_expression.empty())
    {
        DEBUG_SYMBOL_PARAMETERS params{};
        if (SUCCEEDED(group->GetSymbolParameters(index, 1, &params)) && params.SubElements != 0)
        {
            node.lazy_expression = expression;
        }
    }
    return node;
}

std::pair<std::uint64_t, std::uint32_t> debugger_session::get_symbol_address(std::uint32_t frame_number,
                                                                             const std::string &expression)
{
    throw_if_disposed();

    ULONG index = DEBUG_ANY_ID;
    IDebugSymbolGroup2 *group = open_scope_symbol_group(frame_number, expression, index);
    struct group_releaser
    {
        IDebugSymbolGroup2 *g;
        ~group_releaser()
        {
            g->Release();
        }
    } releaser{group};

    ULONG64 offset = 0;
    if (FAILED(group->GetSymbolOffset(index, &offset)) || offset == 0)
    {
        throw std::runtime_error(fmt::format("'{}' has no memory address (it may be enregistered).", expression));
    }
    ULONG size = 0;
    if (FAILED(group->GetSymbolSize(index, &size)) || size == 0)
    {
        throw std::runtime_error(fmt::format("Could not determine the size of '{}'.", expression));
    }
    return {offset, size};
}

std::vector<named_value_info> debugger_session::get_registers(std::uint32_t frame_number)
{
    throw_if_disposed();

    // Point the register source at the requested frame, then read the register set
    // structurally (the equivalent of `.frame N; r`). A frame that cannot be scoped
    // has no register view to report.
    if (FAILED(symbols_->SetScopeFrameByIndex(frame_number)))
    {
        return {};
    }

    ULONG count = 0;
    if (registers_ == nullptr || FAILED(registers_->GetNumberRegisters(&count)) || count == 0)
    {
        return {};
    }

    // Read each register individually: a batch read fails atomically if any single
    // register (e.g. an absent AVX register) can't be read. Skip sub-registers
    // (al/ax/eax alias rax) and non-integer registers so the set mirrors the
    // general-purpose register view rather than every alias and vector lane.
    std::vector<named_value_info> result;
    std::vector<char> name_buffer(kRegisterNameBufferBytes);
    for (ULONG index = 0; index < count; ++index)
    {
        ULONG name_size = 0;
        DEBUG_REGISTER_DESCRIPTION description{};
        if (FAILED(registers_->GetDescription(index, name_buffer.data(), static_cast<ULONG>(name_buffer.size()),
                                              &name_size, &description)))
        {
            continue;
        }
        if ((description.Flags & DEBUG_REGISTER_SUB_REGISTER) != 0)
        {
            continue;
        }
        if (description.Type != DEBUG_VALUE_INT8 && description.Type != DEBUG_VALUE_INT16 &&
            description.Type != DEBUG_VALUE_INT32 && description.Type != DEBUG_VALUE_INT64)
        {
            continue;
        }

        DEBUG_VALUE value{};
        if (FAILED(registers_->GetValues2(DEBUG_REGSRC_FRAME, 1, &index, 0, &value)) &&
            FAILED(registers_->GetValues(1, &index, 0, &value)))
        {
            continue;
        }

        const std::string name = buffer_to_trimmed_string(name_buffer, name_size);
        if (!name.empty())
        {
            result.push_back(named_value_info{name, format_register_value(value)});
        }
    }
    return result;
}

std::optional<source_location> debugger_session::try_get_frame_source(std::uint64_t instruction_offset)
{
    throw_if_disposed();

    std::vector<char> buffer(kSymbolBufferBytes);
    ULONG line = 0;
    ULONG file_size = 0;
    ULONG64 displacement = 0;
    const HRESULT hr = symbols_->GetLineByOffset(instruction_offset, &line, buffer.data(),
                                                 static_cast<ULONG>(buffer.size()), &file_size, &displacement);
    if (FAILED(hr) || line == 0 || file_size == 0)
    {
        return std::nullopt;
    }

    std::size_t length = std::min(static_cast<std::size_t>(file_size), buffer.size());
    if (length > 0 && buffer[length - 1] == '\0')
    {
        --length;
    }
    if (length == 0)
    {
        return std::nullopt;
    }

    std::string path(buffer.data(), length);
    if (trim_both(path).empty())
    {
        return std::nullopt;
    }
    return source_location{path, static_cast<int>(line)};
}

// ---------------------------------------------------------------------------
// Memory / disassembly
// ---------------------------------------------------------------------------
std::optional<std::string> debugger_session::try_get_frame_name(std::uint64_t instruction_offset)
{
    std::vector<char> buffer(kSymbolBufferBytes);
    ULONG name_size = 0;
    ULONG64 displacement = 0;
    const HRESULT hr = symbols_->GetNameByOffset(instruction_offset, buffer.data(), static_cast<ULONG>(buffer.size()),
                                                 &name_size, &displacement);
    if (FAILED(hr) || name_size == 0)
    {
        return std::nullopt;
    }
    std::size_t length = std::min(static_cast<std::size_t>(name_size), buffer.size());
    if (length > 0 && buffer[length - 1] == '\0')
    {
        --length;
    }
    std::string name = trim_both(std::string(buffer.data(), length));
    if (name.empty())
    {
        return std::nullopt;
    }
    return displacement == 0 ? name : fmt::format("{}+0x{:X}", name, displacement);
}

std::string debugger_session::describe_code_break(ULONG64 address)
{
    // An Unreal ensure() prints "Ensure condition failed: Expr [File:F]
    // [Line: N]" to the debugger, then breaks. The break itself sits in a tiny
    // lambda the linker folds with every other one, so its symbol and line
    // name some unrelated ensure; the message names the real one.
    std::string message;
    {
        std::lock_guard<std::mutex> lock(output_capture_sync_);
        if (!last_ensure_message_.empty() &&
            std::chrono::steady_clock::now() - last_ensure_message_time_ < std::chrono::seconds(30))
        {
            message = last_ensure_message_;
        }
        last_ensure_message_.clear();
    }
    std::string where = try_get_frame_name(address).value_or(fmt::format("0x{:x}", address));
    bool ensure = !message.empty();

    // Without the message: the call just before the break is
    // ExecCheckImplInternal (ensure) or EnsureFailed (ensureMsgf).
    if (!ensure)
    {
        std::array<unsigned char, 48> code{};
        ULONG done = 0;
        if (SUCCEEDED(data_spaces_->ReadVirtual(address - code.size(), code.data(), static_cast<ULONG>(code.size()),
                                                &done)) &&
            done == code.size())
        {
            const ULONG64 start = address - code.size();
            for (std::size_t at = 0; at + 5 <= code.size() && !ensure; ++at)
            {
                std::optional<ULONG64> target;
                if (code[at] == 0xE8)
                {
                    std::int32_t relative = 0;
                    std::memcpy(&relative, &code[at + 1], sizeof(relative));
                    target = start + at + 5 + static_cast<ULONG64>(static_cast<std::int64_t>(relative));
                }
                else if (code[at] == 0xFF && at + 6 <= code.size() && code[at + 1] == 0x15)
                {
                    // call qword ptr [rip+disp]: an import from Core's DLL.
                    std::int32_t relative = 0;
                    std::memcpy(&relative, &code[at + 2], sizeof(relative));
                    ULONG64 slot = start + at + 6 + static_cast<ULONG64>(static_cast<std::int64_t>(relative));
                    ULONG64 pointer = 0;
                    if (SUCCEEDED(data_spaces_->ReadVirtual(slot, &pointer, sizeof(pointer), &done)) &&
                        done == sizeof(pointer))
                    {
                        target = pointer;
                    }
                }
                if (target)
                {
                    const std::string name = try_get_frame_name(*target).value_or("");
                    ensure = name.find("ExecCheckImplInternal") != std::string::npos ||
                             name.find("EnsureFailed") != std::string::npos;
                }
            }
        }
    }
    std::string place;
    if (const auto source = try_get_frame_source(address))
    {
        place = fmt::format(" ({}:{})", std::filesystem::path(source->path).filename().string(), source->line);
    }
    if (!message.empty())
    {
        // "0 [File:D:\...\UnrealEngine.cpp] [Line: 11355]" -> "0 (UnrealEngine.cpp:11355)".
        std::string text = message.substr(std::strlen("Ensure condition failed: "));
        const auto file_at = text.find(" [File:");
        const auto line_at = text.find("] [Line: ");
        if (file_at != std::string::npos && line_at != std::string::npos && line_at > file_at)
        {
            const std::string file = text.substr(file_at + 7, line_at - file_at - 7);
            const auto line_end = text.find(']', line_at + 9);
            const std::string line = text.substr(line_at + 9, line_end == std::string::npos ? std::string::npos
                                                                                           : line_end - line_at - 9);
            std::string rest = line_end == std::string::npos ? std::string() : trim_both(text.substr(line_end + 1));
            text = fmt::format("{} ({}:{})", trim_both(text.substr(0, file_at)),
                               std::filesystem::path(file).filename().string(), trim_both(line));
            if (!rest.empty())
            {
                text += " " + rest.substr(0, 200);
            }
        }
        return fmt::format("Paused on an ensure() failure: {}.", text);
    }
    if (ensure)
    {
        return fmt::format("Paused on an ensure() failure in {}{}.", where, place);
    }
    return fmt::format("Paused on a __debugbreak in {}{}.", where, place);
}

bool debugger_session::try_get_symbol_start(std::uint64_t instruction_offset, std::uint64_t &symbol_start)
{
    symbol_start = 0;

    std::vector<char> buffer(kSymbolBufferBytes);
    ULONG name_size = 0;
    ULONG64 displacement = 0;
    const HRESULT hr = symbols_->GetNameByOffset(instruction_offset, buffer.data(), static_cast<ULONG>(buffer.size()),
                                                 &name_size, &displacement);
    if (FAILED(hr) || name_size == 0)
    {
        return false;
    }
    std::size_t length = std::min(static_cast<std::size_t>(name_size), buffer.size());
    if (length > 0 && buffer[length - 1] == '\0')
    {
        --length;
    }
    const std::string name = trim_both(std::string(buffer.data(), length));
    if (name.empty())
    {
        return false;
    }

    std::uint64_t effective_displacement = displacement;
    if (effective_displacement == 0)
    {
        std::uint64_t inline_displacement = 0;
        if (try_get_inline_symbol_displacement(name, inline_displacement))
        {
            effective_displacement = inline_displacement;
        }
    }

    if (effective_displacement > instruction_offset)
    {
        return false; // would underflow
    }
    symbol_start = instruction_offset - effective_displacement;
    return true;
}
} // namespace dap_dbgeng::debugger
