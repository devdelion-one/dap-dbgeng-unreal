#include "debugger/debugger_session.h"

#include "debugger/debugger_session_internal.h"

namespace dap_dbgeng::debugger
{
namespace
{
// As line breakpoints: a line with no code moves to the next one with code.
constexpr int kGotoSnapRange = 40;
// Blueprint frames get frame numbers of their own, above any real stack depth.
constexpr std::uint32_t kScriptFrameNumberBase = 0x10000;
// "BP_Grunt · ReceiveAnyDamage".
constexpr const char *kScriptFrameSeparator = " \xC2\xB7 ";

std::string base_name(const std::string &path)
{
    const auto slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}
} // namespace

void debugger_session::add_script_frames(const std::vector<DEBUG_STACK_FRAME_EX> &native,
                                         std::vector<stack_frame_info> &frames, int &total_frames)
{
    if (unreal_reader_ == nullptr || is_kernel_ || native.empty())
    {
        return;
    }
    ULONG64 teb = 0;
    if (FAILED(system_objects_->GetCurrentThreadTeb(&teb)) || teb == 0)
    {
        return;
    }
    const std::vector<unreal_reader::script_frame> script = unreal_reader_->script_stack(teb);
    if (script.empty())
    {
        return;
    }

    // An FFrame is a local of the engine function that made it, so it lies
    // between that frame's stack pointer and its caller's.
    const auto owner_of = [&](ULONG64 address) -> std::optional<std::size_t> {
        for (std::size_t k = 0; k < native.size(); ++k)
        {
            const ULONG64 low = native[k].StackOffset;
            const ULONG64 high = k + 1 < native.size() ? native[k + 1].StackOffset : low + 0x100000;
            if (address >= low && address < high)
            {
                return k;
            }
        }
        return std::nullopt;
    };
    // Outermost first: an FFrame not on the native stack sits with the one
    // outside it, above it.
    std::vector<std::size_t> positions(script.size(), native.size());
    for (std::size_t i = 0; i < script.size(); ++i)
    {
        if (const auto owner = owner_of(script[i].frame))
        {
            positions[i] = *owner;
        }
        else if (i > 0)
        {
            positions[i] = positions[i - 1];
        }
    }

    std::vector<stack_frame_info> merged;
    merged.reserve(frames.size() + script.size());
    int added = 0;
    const auto add_at = [&](std::size_t position) {
        for (std::size_t i = script.size(); i-- > 0;)
        {
            if (positions[i] != position || script[i].native)
            {
                continue;
            }
            stack_frame_info info;
            info.frame_number = kScriptFrameNumberBase + static_cast<std::uint32_t>(i);
            info.native_frame = position < native.size() ? native[position].FrameNumber : 0;
            info.name = (script[i].owner.empty() ? std::string("Blueprint") : script[i].owner) + kScriptFrameSeparator +
                        script[i].function_name;
            info.script_frame = script[i].frame;
            info.script_object = script[i].object_name;
            merged.push_back(std::move(info));
            ++added;
        }
    };
    for (const auto &frame : frames)
    {
        add_at(frame.frame_number);
        merged.push_back(frame);
    }
    if (frames.size() == native.size())
    {
        add_at(native.size());
    }
    if (added == 0)
    {
        return;
    }
    // Frames past the fetched window still count toward the total.
    for (std::size_t i = 0; i < script.size(); ++i)
    {
        if (!script[i].native && positions[i] >= frames.size() && positions[i] < native.size())
        {
            ++total_frames;
        }
    }
    total_frames += added;
    frames = std::move(merged);
}

std::vector<variable_node> debugger_session::get_script_locals(std::uint64_t script_frame)
{
    throw_if_disposed();
    if (unreal_reader_ == nullptr)
    {
        return {};
    }
    std::vector<variable_node> nodes = unreal_reader_->script_locals(script_frame);
    for (auto &node : nodes)
    {
        expand_reader_children(node);
    }
    return nodes;
}

std::uint64_t debugger_session::current_instruction_offset()
{
    ULONG64 offset = 0;
    check_hr(registers_->GetInstructionOffset2(DEBUG_REGSRC_DEBUGGEE, &offset),
             "Could not read the instruction pointer");
    return offset;
}

std::optional<std::pair<std::string, std::uint64_t>> debugger_session::function_at(std::uint64_t address)
{
    std::vector<char> buffer(kSymbolBufferBytes);
    ULONG name_size = 0;
    ULONG64 displacement = 0;
    if (FAILED(symbols_->GetNameByOffset(address, buffer.data(), static_cast<ULONG>(buffer.size()), &name_size,
                                         &displacement)) ||
        name_size == 0)
    {
        return std::nullopt;
    }
    std::string name = buffer_to_trimmed_string(buffer, name_size);
    std::uint64_t start = 0;
    if (name.empty() || !try_get_symbol_start(address, start))
    {
        return std::nullopt;
    }
    return std::make_pair(std::move(name), start);
}

goto_target debugger_session::resolve_goto_target(const std::string &file_path, int line)
{
    throw_if_disposed();
    const std::uint64_t here = current_instruction_offset();
    const auto function = function_at(here);
    if (!function)
    {
        throw std::runtime_error("Set next statement needs symbols for the current function.");
    }
    ULONG index = 0;
    ULONG64 base = 0;
    check_hr(symbols_->GetModuleByOffset(here, 0, &index, &base), "Could not find the current module");
    std::vector<char> module_buffer(kSymbolBufferBytes);
    ULONG module_size = 0;
    check_hr(symbols_->GetModuleNames(index, base, nullptr, 0, nullptr, module_buffer.data(),
                                      static_cast<ULONG>(module_buffer.size()), &module_size, nullptr, 0, nullptr),
             "Could not read the current module's name");
    const std::string module = buffer_to_trimmed_string(module_buffer, module_size);
    const std::string file = base_name(file_path);

    for (int candidate = line; candidate < line + kGotoSnapRange; ++candidate)
    {
        DEBUG_VALUE value{};
        const std::string expression = fmt::format("`{}!{}:{}`", module, file, candidate);
        if (FAILED(control_->Evaluate(expression.c_str(), DEBUG_VALUE_INT64, &value, nullptr)) || value.I64 == 0)
        {
            continue;
        }
        const auto there = function_at(value.I64);
        if (!there || there->second != function->second)
        {
            throw std::runtime_error(fmt::format(
                "Line {} is outside {}. Set next statement moves only within the current function.", line,
                function->first.substr(0, function->first.find('+'))));
        }
        if (value.I64 == function->second)
        {
            throw std::runtime_error(fmt::format(
                "Line {} is where {} starts: moving there would run its entry code again.", line,
                function->first.substr(0, function->first.find('+'))));
        }
        ULONG actual = static_cast<ULONG>(candidate);
        ULONG64 displacement = 0;
        if (FAILED(symbols_->GetLineByOffset(value.I64, &actual, nullptr, 0, nullptr, &displacement)))
        {
            actual = static_cast<ULONG>(candidate);
        }
        return goto_target{value.I64, static_cast<int>(actual)};
    }
    throw std::runtime_error(
        fmt::format("No code on line {} or the {} lines after it.", line, kGotoSnapRange - 1));
}

void debugger_session::set_instruction_pointer(std::uint64_t address)
{
    throw_if_disposed();
    const auto here = function_at(current_instruction_offset());
    const auto there = function_at(address);
    if (!here || !there || here->second != there->second)
    {
        throw std::runtime_error("Set next statement moves only within the current function.");
    }
    ULONG index = 0;
    check_hr(registers_->GetIndexByName("rip", &index), "Could not find the instruction pointer register");
    DEBUG_VALUE value{};
    value.Type = DEBUG_VALUE_INT64;
    value.I64 = address;
    check_hr(registers_->SetValues2(DEBUG_REGSRC_DEBUGGEE, 1, &index, 0, &value),
             "Could not move the instruction pointer");
    symbols_->ResetScope();
}
} // namespace dap_dbgeng::debugger
