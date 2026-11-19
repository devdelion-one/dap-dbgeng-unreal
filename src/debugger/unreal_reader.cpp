#include "debugger/unreal_reader.h"

namespace dap_dbgeng::debugger
{
namespace
{
constexpr std::size_t kMaxShownItems = 100;
constexpr std::size_t kMaxStringChars = 2048;
constexpr std::uint32_t kNameOffsetBits = 16;
constexpr std::uint32_t kNameOffsetMask = (1u << kNameOffsetBits) - 1;
constexpr std::uint32_t kMaxNameBlocks = 1u << 13;
// cvconst.h (DIA SDK) values, which dbghelp.h doesn't carry.
constexpr DWORD kSymTagData = 7;
constexpr DWORD kSymTagUDT = 11;
constexpr DWORD kSymTagTypedef = 17;
constexpr DWORD kSymTagBaseClass = 18;
constexpr DWORD kDataIsMember = 7;

bool starts_with(const std::string &text, const char *prefix)
{
    return text.rfind(prefix, 0) == 0;
}

std::string trim(std::string text)
{
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string::npos)
    {
        return {};
    }
    const auto last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

// "class TArray<int,TSizedDefaultAllocator<32> >" -> "TArray<int,TSizedDefaultAllocator<32> >".
std::string clean_type(std::string text)
{
    text = trim(std::move(text));
    for (const char *prefix : {"const ", "volatile ", "class ", "struct ", "union ", "enum "})
    {
        while (starts_with(text, prefix))
        {
            text = trim(text.substr(std::strlen(prefix)));
        }
    }
    return text;
}

// The first template argument of "Name<A, B<C>>": "A". Empty when there is none.
std::string first_template_argument(const std::string &type)
{
    const auto open = type.find('<');
    if (open == std::string::npos)
    {
        return {};
    }
    int depth = 0;
    for (std::size_t i = open + 1; i < type.size(); ++i)
    {
        const char ch = type[i];
        if (ch == '<' || ch == '(')
        {
            ++depth;
        }
        else if (ch == '>' || ch == ')')
        {
            if (depth == 0)
            {
                return clean_type(type.substr(open + 1, i - open - 1));
            }
            --depth;
        }
        else if (ch == ',' && depth == 0)
        {
            return clean_type(type.substr(open + 1, i - open - 1));
        }
    }
    return {};
}

bool is_pointer_type(const std::string &type)
{
    return !type.empty() && type.back() == '*';
}

std::string pointee(const std::string &type)
{
    return clean_type(type.substr(0, type.size() - 1));
}

// Built-in C++ types need no module prefix in a cast (and reject one).
bool is_builtin(const std::string &type)
{
    std::string base = type;
    while (!base.empty() && (base.back() == '*' || base.back() == ' '))
    {
        base.pop_back();
    }
    static const std::set<std::string> builtins{"bool",     "char",          "signed char",   "unsigned char",
                                                "wchar_t",  "char16_t",      "char32_t",      "char8_t",
                                                "short",    "unsigned short", "int",          "unsigned int",
                                                "long",     "unsigned long", "__int64",       "unsigned __int64",
                                                "float",    "double",        "long double",   "void",
                                                "int64",    "uint64",        "int32",         "uint32",
                                                "int16",    "uint16",        "int8",          "uint8"};
    return builtins.count(base) != 0;
}

std::string quote(const std::string &text)
{
    std::string out = "\"";
    for (char ch : text)
    {
        if (ch == '"' || ch == '\\')
        {
            out += '\\';
            out += ch;
        }
        else if (ch == '\n')
        {
            out += "\\n";
        }
        else if (ch == '\r')
        {
            out += "\\r";
        }
        else if (ch == '\t')
        {
            out += "\\t";
        }
        else
        {
            out += ch;
        }
    }
    return out + "\"";
}

std::string utf8_from_wide(const std::wstring &wide)
{
    if (wide.empty())
    {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                                          nullptr, nullptr);
    std::string out(static_cast<std::size_t>(bytes > 0 ? bytes : 0), '\0');
    if (bytes > 0)
    {
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), bytes, nullptr,
                            nullptr);
    }
    return out;
}
} // namespace

unreal_reader::unreal_reader(IDebugSymbols3 *symbols, IDebugDataSpaces4 *data, IDebugControl7 *control,
                             IDebugSystemObjects *system_objects, HMODULE dbghelp)
    : symbols_(symbols), data_(data), control_(control), system_objects_(system_objects)
{
    if (dbghelp == nullptr)
    {
        dbghelp = GetModuleHandleW(L"dbghelp.dll");
    }
    if (dbghelp != nullptr)
    {
        get_type_info_ =
            reinterpret_cast<sym_get_type_info_t>(reinterpret_cast<void *>(GetProcAddress(dbghelp, "SymGetTypeInfo")));
    }
}

bool unreal_reader::collect_members(HANDLE process, ULONG64 module_base, ULONG id, ULONG base_offset,
                                    type_layout &out, int depth)
{
    if (depth > 16)
    {
        return false;
    }
    DWORD count = 0;
    if (!get_type_info_(process, module_base, id, TI_GET_CHILDRENCOUNT, &count))
    {
        return false;
    }
    if (count == 0)
    {
        return true;
    }
    std::vector<std::uint8_t> buffer(sizeof(TI_FINDCHILDREN_PARAMS) + count * sizeof(ULONG));
    auto *children = reinterpret_cast<TI_FINDCHILDREN_PARAMS *>(buffer.data());
    children->Count = count;
    children->Start = 0;
    if (!get_type_info_(process, module_base, id, TI_FINDCHILDREN, children))
    {
        return false;
    }
    for (DWORD i = 0; i < count; ++i)
    {
        const ULONG child = children->ChildId[i];
        DWORD tag = 0;
        if (!get_type_info_(process, module_base, child, TI_GET_SYMTAG, &tag))
        {
            continue;
        }
        if (tag == kSymTagBaseClass)
        {
            DWORD is_virtual = 0;
            if (get_type_info_(process, module_base, child, TI_GET_VIRTUALBASECLASS, &is_virtual) && is_virtual != 0)
            {
                continue; // no fixed offset; dbgeng's answer stands for these
            }
            DWORD offset = 0;
            DWORD base_type = 0;
            if (get_type_info_(process, module_base, child, TI_GET_OFFSET, &offset) &&
                get_type_info_(process, module_base, child, TI_GET_TYPEID, &base_type))
            {
                WCHAR *base_name = nullptr;
                if (get_type_info_(process, module_base, base_type, TI_GET_SYMNAME, &base_name) && base_name != nullptr)
                {
                    out.bases.emplace_back(utf8_from_wide(base_name), base_offset + offset);
                    LocalFree(base_name);
                }
                collect_members(process, module_base, base_type, base_offset + offset, out, depth + 1);
            }
            continue;
        }
        if (tag == kSymTagTypedef && depth == 0)
        {
            WCHAR *name = nullptr;
            DWORD type = 0;
            if (get_type_info_(process, module_base, child, TI_GET_SYMNAME, &name) && name != nullptr)
            {
                const std::string alias = utf8_from_wide(name);
                LocalFree(name);
                if (get_type_info_(process, module_base, child, TI_GET_TYPEID, &type))
                {
                    out.typedefs[alias] = type;
                }
            }
            continue;
        }
        if (tag != kSymTagData)
        {
            continue;
        }
        DWORD kind = 0;
        if (!get_type_info_(process, module_base, child, TI_GET_DATAKIND, &kind) || kind != kDataIsMember)
        {
            continue; // static members have no offset
        }
        WCHAR *name = nullptr;
        DWORD offset = 0;
        DWORD type = 0;
        if (!get_type_info_(process, module_base, child, TI_GET_SYMNAME, &name) || name == nullptr)
        {
            continue;
        }
        const std::string member = utf8_from_wide(name);
        LocalFree(name);
        if (!get_type_info_(process, module_base, child, TI_GET_OFFSET, &offset) ||
            !get_type_info_(process, module_base, child, TI_GET_TYPEID, &type))
        {
            continue;
        }
        const field_info info{type, base_offset + offset};
        out.fields.emplace_back(member, info);
        out.by_name[member] = info;
    }
    return true;
}

const unreal_reader::type_layout *unreal_reader::layout(ULONG64 module_base, ULONG id)
{
    if (get_type_info_ == nullptr || system_objects_ == nullptr || layouts_agree_ == false)
    {
        return nullptr;
    }
    const auto key = std::make_pair(module_base, id);
    const auto cached = layouts_.find(key);
    if (cached != layouts_.end())
    {
        return cached->second.get();
    }
    std::unique_ptr<type_layout> result;
    ULONG64 handle = 0;
    DWORD tag = 0;
    if (SUCCEEDED(system_objects_->GetCurrentProcessHandle(&handle)) && handle != 0 &&
        get_type_info_(reinterpret_cast<HANDLE>(handle), module_base, id, TI_GET_SYMTAG, &tag) && tag == kSymTagUDT)
    {
        auto built = std::make_unique<type_layout>();
        if (collect_members(reinterpret_cast<HANDLE>(handle), module_base, id, 0, *built, 0))
        {
            result = std::move(built);
        }
    }
    if (result && !layouts_agree_.has_value() && !result->fields.empty())
    {
        // The first real answer is compared with dbgeng's own.
        const auto &[name, mine] = result->fields.back();
        ULONG engine_type = 0;
        ULONG engine_offset = 0;
        layouts_agree_ = SUCCEEDED(symbols_->GetFieldTypeAndOffset(module_base, id, name.c_str(), &engine_type,
                                                                   &engine_offset)) &&
                         engine_type == mine.type_id && engine_offset == mine.offset;
        if (!*layouts_agree_)
        {
            spdlog::warn("unreal_reader: dbghelp's type layout differs from dbgeng's ({}: {}+{} vs {}+{}); "
                         "using dbgeng's queries",
                         name, mine.type_id, mine.offset, engine_type, engine_offset);
            return nullptr;
        }
    }
    const type_layout *pointer = result.get();
    layouts_.emplace(key, std::move(result));
    return pointer;
}

void unreal_reader::forget_modules()
{
    module_names_.clear();
    name_blocks_.clear();
    name_layouts_.clear();
    core_module_.reset();
    object_array_.reset();
    text_histories_.clear();
    dynamic_types_.clear();
    layouts_.clear();
}

ULONG64 unreal_reader::module_with_type(ULONG64 module_base, const char *type, const char *probe_field)
{
    // A module may know the type only as a forward declaration, with no
    // fields; it counts only if the field we need is there.
    const auto described = [&](ULONG64 base) {
        const auto id = type_id(base, type);
        return id && field(base, *id, probe_field).has_value();
    };
    if (described(module_base))
    {
        return module_base;
    }
    if (!core_module_.has_value())
    {
        core_module_ = 0;
        // Editor and modular builds keep Core in its own DLL; a monolithic game
        // has it in the executable (module index 0).
        for (const char *name : {"UnrealEditor_Core", "UnrealGame_Core", "UE4Editor_Core"})
        {
            ULONG64 base = 0;
            if (SUCCEEDED(symbols_->GetModuleByModuleName(name, 0, nullptr, &base)) && base != 0)
            {
                core_module_ = base;
                break;
            }
        }
        if (*core_module_ == 0)
        {
            ULONG64 base = 0;
            if (SUCCEEDED(symbols_->GetModuleByIndex(0, &base)))
            {
                core_module_ = base;
            }
        }
    }
    return *core_module_ != 0 && described(*core_module_) ? *core_module_ : module_base;
}

std::optional<ULONG> unreal_reader::nested_type(ULONG64 module_base, ULONG id, const char *name)
{
    if (const type_layout *members = layout(module_base, id))
    {
        const auto found = members->typedefs.find(name);
        if (found != members->typedefs.end())
        {
            return found->second;
        }
    }
    return std::nullopt;
}

std::optional<unreal_reader::field_info> unreal_reader::field(ULONG64 module_base, ULONG type_id, const char *name)
{
    const auto key = std::make_tuple(module_base, type_id, std::string(name));
    const auto cached = fields_.find(key);
    if (cached != fields_.end())
    {
        return cached->second;
    }
    std::optional<field_info> result;
    if (const type_layout *members = layout(module_base, type_id))
    {
        const auto found = members->by_name.find(name);
        if (found != members->by_name.end())
        {
            result = found->second;
        }
    }
    else
    {
        field_info info;
        if (SUCCEEDED(symbols_->GetFieldTypeAndOffset(module_base, type_id, name, &info.type_id, &info.offset)))
        {
            result = info;
        }
    }
    fields_[key] = result;
    return result;
}

std::optional<ULONG> unreal_reader::type_id(ULONG64 module_base, const std::string &name)
{
    const auto key = std::make_pair(module_base, name);
    const auto cached = type_ids_.find(key);
    if (cached != type_ids_.end())
    {
        return cached->second;
    }
    std::optional<ULONG> result;
    ULONG id = 0;
    if (SUCCEEDED(symbols_->GetTypeId(module_base, name.c_str(), &id)))
    {
        result = id;
    }
    type_ids_[key] = result;
    return result;
}

std::string unreal_reader::type_name(ULONG64 module_base, ULONG id)
{
    const auto key = std::make_pair(module_base, id);
    const auto cached = type_names_.find(key);
    if (cached != type_names_.end())
    {
        return cached->second;
    }
    char buffer[1024] = {};
    ULONG size = 0;
    std::string name;
    if (SUCCEEDED(symbols_->GetTypeName(module_base, id, buffer, sizeof(buffer), &size)))
    {
        name = clean_type(buffer);
    }
    type_names_[key] = name;
    return name;
}

ULONG unreal_reader::type_size(ULONG64 module_base, ULONG id)
{
    const auto key = std::make_pair(module_base, id);
    const auto cached = type_sizes_.find(key);
    if (cached != type_sizes_.end())
    {
        return cached->second;
    }
    ULONG size = 0;
    if (FAILED(symbols_->GetTypeSize(module_base, id, &size)))
    {
        size = 0;
    }
    type_sizes_[key] = size;
    return size;
}

std::string unreal_reader::module_name(ULONG64 module_base)
{
    const auto it = module_names_.find(module_base);
    if (it != module_names_.end())
    {
        return it->second;
    }
    char buffer[512] = {};
    ULONG size = 0;
    std::string name;
    if (SUCCEEDED(symbols_->GetModuleNameString(DEBUG_MODNAME_MODULE, DEBUG_ANY_ID, module_base, buffer,
                                                sizeof(buffer), &size)))
    {
        name = buffer;
    }
    module_names_[module_base] = name;
    return name;
}

template <class T> std::optional<T> unreal_reader::read(ULONG64 address)
{
    T value{};
    ULONG done = 0;
    if (address == 0 || FAILED(data_->ReadVirtual(address, &value, sizeof(T), &done)) || done != sizeof(T))
    {
        return std::nullopt;
    }
    return value;
}

std::optional<std::string> unreal_reader::read_text(ULONG64 address, std::size_t count, bool wide)
{
    if (count == 0)
    {
        return std::string();
    }
    count = std::min(count, kMaxStringChars);
    ULONG done = 0;
    if (wide)
    {
        std::wstring text(count, L'\0');
        const ULONG bytes = static_cast<ULONG>(count * sizeof(wchar_t));
        if (FAILED(data_->ReadVirtual(address, text.data(), bytes, &done)) || done != bytes)
        {
            return std::nullopt;
        }
        return utf8_from_wide(text);
    }
    std::string text(count, '\0');
    if (FAILED(data_->ReadVirtual(address, text.data(), static_cast<ULONG>(count), &done)) || done != count)
    {
        return std::nullopt;
    }
    return text;
}

void unreal_reader::calibrate_stride(name_layout &layout, ULONG64 blocks, std::uint32_t comparison_index)
{
    const std::uint32_t block = comparison_index >> kNameOffsetBits;
    const std::uint32_t offset = comparison_index & kNameOffsetMask;
    if (offset == 0 || block >= kMaxNameBlocks)
    {
        return;
    }
    const auto block_address = read<ULONG64>(blocks + static_cast<ULONG64>(block) * sizeof(ULONG64));
    if (!block_address || *block_address == 0)
    {
        return;
    }
    for (const ULONG stride : {2u, 4u, 8u, 16u})
    {
        const ULONG64 entry = *block_address + static_cast<ULONG64>(stride) * offset;
        if (layout.has_comparison_id)
        {
            // An entry stores its comparison id first; the right stride lands
            // on the very id we came from.
            if (read<std::uint32_t>(entry).value_or(0) == comparison_index)
            {
                layout.stride = stride;
                return;
            }
            continue;
        }
        const auto header = read<std::uint16_t>(entry + layout.header);
        if (!header)
        {
            continue;
        }
        const std::size_t length = layout.long_length ? (*header >> 1) : (*header >> 6);
        if (length == 0 || length > 1024 || (*header & 1) != 0)
        {
            continue;
        }
        const auto text = read_text(entry + layout.chars, std::min<std::size_t>(length, 64), false);
        if (text && std::all_of(text->begin(), text->end(),
                                [](char ch) { return ch >= 0x20 && ch < 0x7f; }))
        {
            layout.stride = stride;
            return;
        }
    }
}

std::optional<std::string> unreal_reader::name_from_index(ULONG64 module_base, std::uint32_t index,
                                                          std::uint32_t comparison_index)
{
    const auto cached = names_.find(index);
    if (cached != names_.end())
    {
        return cached->second;
    }

    // Every Unreal module keeps its own pointer to the global name blocks
    // (ModuleBoilerplate.h), so the module the value lives in always has one.
    auto blocks_it = name_blocks_.find(module_base);
    if (blocks_it == name_blocks_.end())
    {
        ULONG64 blocks = 0;
        const std::string symbol = module_name(module_base) + "!GNameBlocksDebug";
        ULONG64 symbol_address = 0;
        if (SUCCEEDED(symbols_->GetOffsetByName(symbol.c_str(), &symbol_address)))
        {
            blocks = read<ULONG64>(symbol_address).value_or(0);
        }
        blocks_it = name_blocks_.emplace(module_base, blocks).first;
    }
    if (blocks_it->second == 0)
    {
        return std::nullopt;
    }

    auto layout_it = name_layouts_.find(module_base);
    if (layout_it == name_layouts_.end())
    {
        name_layout layout;
        const ULONG64 typed = module_with_type(module_base, "FNameEntry", "Header");
        if (const auto entry = type_id(typed, "FNameEntry"))
        {
            if (const auto header = field(typed, *entry, "Header"))
            {
                layout.header = header->offset;
                layout.chars = header->offset + 2;
                // The length bits differ by build: editor builds keep the case
                // (Len : 15), the rest store a probe hash (Len : 10).
                layout.long_length = !field(typed, header->type_id, "LowercaseProbeHash").has_value();
            }
            if (const auto chars = field(typed, *entry, "AnsiName"))
            {
                layout.chars = chars->offset;
            }
            layout.has_comparison_id = field(typed, *entry, "ComparisonId").has_value();
        }
        else
        {
            spdlog::debug("unreal_reader: no FNameEntry type found; name layout guessed");
        }
        layout_it = name_layouts_.emplace(module_base, layout).first;
    }
    name_layout &layout = layout_it->second;
    if (layout.stride == 0)
    {
        calibrate_stride(layout, blocks_it->second, comparison_index);
        if (layout.stride == 0)
        {
            return std::nullopt;
        }
    }

    const std::uint32_t block = index >> kNameOffsetBits;
    const std::uint32_t offset = index & kNameOffsetMask;
    if (block >= kMaxNameBlocks)
    {
        return std::nullopt;
    }
    const auto block_address = read<ULONG64>(blocks_it->second + static_cast<ULONG64>(block) * sizeof(ULONG64));
    if (!block_address || *block_address == 0)
    {
        return std::nullopt;
    }
    const ULONG64 entry = *block_address + static_cast<ULONG64>(layout.stride) * offset;
    const auto header = read<std::uint16_t>(entry + layout.header);
    if (!header)
    {
        return std::nullopt;
    }
    const bool wide = (*header & 1) != 0;
    const std::size_t length = layout.long_length ? (*header >> 1) : (*header >> 6);
    if (length > 1024)
    {
        return std::nullopt;
    }
    auto text = read_text(entry + layout.chars, length, wide);
    if (!text || text->empty() ||
        !std::all_of(text->begin(), text->end(), [](char ch) { return static_cast<unsigned char>(ch) >= 0x20; }))
    {
        return std::nullopt;
    }
    names_[index] = *text;
    return text;
}

std::optional<std::string> unreal_reader::read_name(ULONG64 module_base, ULONG id, ULONG64 address)
{
    auto index_field = field(module_base, id, "DisplayIndex");
    if (!index_field)
    {
        index_field = field(module_base, id, "ComparisonIndex");
    }
    if (!index_field)
    {
        return std::nullopt;
    }
    ULONG value_offset = 0;
    if (const auto value = field(module_base, index_field->type_id, "Value"))
    {
        value_offset = value->offset;
    }
    const auto index = read<std::uint32_t>(address + index_field->offset + value_offset);
    if (!index)
    {
        return std::nullopt;
    }
    std::uint32_t comparison = *index;
    if (const auto comparison_field = field(module_base, id, "ComparisonIndex"))
    {
        comparison = read<std::uint32_t>(address + comparison_field->offset + value_offset).value_or(*index);
    }
    auto name = name_from_index(module_base, *index, comparison);
    if (!name)
    {
        return std::nullopt;
    }
    if (const auto number_field = field(module_base, id, "Number"))
    {
        const auto number = read<std::uint32_t>(address + number_field->offset);
        if (number && *number > 0)
        {
            *name += "_" + std::to_string(*number - 1);
        }
    }
    return name;
}

std::optional<unreal_reader::array_info> unreal_reader::read_array(ULONG64 module_base, ULONG id, ULONG64 address,
                                                                   const std::string &array_type_name)
{
    const auto num_field = field(module_base, id, "ArrayNum");
    const auto allocator = field(module_base, id, "AllocatorInstance");
    if (!num_field || !allocator)
    {
        return std::nullopt;
    }
    array_info info;
    info.num = read<std::int32_t>(address + num_field->offset).value_or(-1);
    if (info.num < 0)
    {
        return std::nullopt;
    }
    info.element = first_template_argument(array_type_name);

    info.data = allocator_data(module_base, allocator->type_id, address + allocator->offset);
    return info;
}

ULONG64 unreal_reader::allocator_data(ULONG64 module_base, ULONG allocator_type, ULONG64 allocator_address)
{
    if (const auto data = field(module_base, allocator_type, "Data"))
    {
        return read<ULONG64>(allocator_address + data->offset).value_or(0);
    }
    ULONG64 data = 0;
    if (const auto secondary = field(module_base, allocator_type, "SecondaryData"))
    {
        // Inline allocators keep the first items in place and spill to the heap.
        if (const auto secondary_data = field(module_base, secondary->type_id, "Data"))
        {
            data = read<ULONG64>(allocator_address + secondary->offset + secondary_data->offset).value_or(0);
        }
    }
    if (data == 0)
    {
        if (const auto inline_data = field(module_base, allocator_type, "InlineData"))
        {
            data = allocator_address + inline_data->offset;
        }
    }
    return data;
}

std::optional<std::string> unreal_reader::read_string(ULONG64 module_base, ULONG id, ULONG64 address)
{
    const auto data = field(module_base, id, "Data");
    if (!data)
    {
        return std::nullopt;
    }
    const auto array = read_array(module_base, data->type_id, address + data->offset, type_name(module_base, data->type_id));
    if (!array)
    {
        return std::nullopt;
    }
    if (array->num <= 1 || array->data == 0)
    {
        return std::string();
    }
    const bool wide = array->element != "char" && array->element != "ANSICHAR" && array->element != "UTF8CHAR";
    return read_text(array->data, static_cast<std::size_t>(array->num - 1), wide);
}

std::optional<ULONG64> unreal_reader::object_pointer(ULONG64 module_base, ULONG id, ULONG64 address, int depth)
{
    if (depth > 4)
    {
        return std::nullopt;
    }
    const std::string name = type_name(module_base, id);
    if (is_pointer_type(name))
    {
        return read<ULONG64>(address);
    }
    if (const auto debug_ptr = field(module_base, id, "DebugPtr"))
    {
        return read<ULONG64>(address + debug_ptr->offset);
    }
    if (const auto object_ptr = field(module_base, id, "ObjectPtr"))
    {
        return object_pointer(module_base, object_ptr->type_id, address + object_ptr->offset, depth + 1);
    }
    if (const auto handle = field(module_base, id, "Handle"))
    {
        const auto value = read<ULONG64>(address + handle->offset);
        // A set low bit is a packed reference that hasn't been resolved yet.
        if (value && (*value & 1) == 0)
        {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<std::string> unreal_reader::object_label(ULONG64 module_base, ULONG64 object)
{
    if (object == 0)
    {
        return std::string("nullptr");
    }
    module_base = module_with_type(module_base, "UObjectBase", "NamePrivate");
    const auto base = type_id(module_base, "UObjectBase");
    if (!base)
    {
        spdlog::debug("unreal_reader: no UObjectBase in {}", module_name(module_base));
        return std::nullopt;
    }
    const auto name_field = field(module_base, *base, "NamePrivate");
    if (!name_field)
    {
        return std::nullopt;
    }
    // UE 5.6+ wraps the name with the object's hash index.
    ULONG name_type = name_field->type_id;
    ULONG name_offset = name_field->offset;
    if (type_name(module_base, name_type) != "FName")
    {
        if (const auto inner = field(module_base, name_type, "Name"))
        {
            name_type = inner->type_id;
            name_offset += inner->offset;
        }
    }
    const auto name = read_name(module_base, name_type, object + name_offset);
    if (!name)
    {
        return std::nullopt;
    }
    std::string label = *name;
    if (const auto class_field = field(module_base, *base, "ClassPrivate"))
    {
        const auto class_object = object_pointer(module_base, class_field->type_id, object + class_field->offset);
        if (class_object && *class_object != 0)
        {
            if (const auto class_name = read_name(module_base, name_type, *class_object + name_offset))
            {
                label += " (" + *class_name + ")";
            }
        }
    }
    return label;
}

std::optional<std::string> unreal_reader::soft_path(ULONG64 module_base, ULONG id, ULONG64 address, int depth)
{
    if (depth > 4)
    {
        return std::nullopt;
    }
    const std::string name = type_name(module_base, id);
    if (name == "FSoftObjectPath")
    {
        std::string package;
        std::string asset;
        if (const auto asset_path = field(module_base, id, "AssetPath"))
        {
            const ULONG64 path_address = address + asset_path->offset;
            if (const auto package_name = field(module_base, asset_path->type_id, "PackageName"))
            {
                package = read_name(module_base, package_name->type_id, path_address + package_name->offset).value_or("");
            }
            if (const auto asset_name = field(module_base, asset_path->type_id, "AssetName"))
            {
                asset = read_name(module_base, asset_name->type_id, path_address + asset_name->offset).value_or("");
            }
        }
        if ((package.empty() || package == "None") && (asset.empty() || asset == "None"))
        {
            return std::string("null");
        }
        std::string text = package;
        if (!asset.empty() && asset != "None")
        {
            text += "." + asset;
        }
        if (const auto sub_path = field(module_base, id, "SubPathString"))
        {
            if (type_name(module_base, sub_path->type_id) == "FString")
            {
                const auto sub = read_string(module_base, sub_path->type_id, address + sub_path->offset);
                if (sub && !sub->empty())
                {
                    text += ":" + *sub;
                }
            }
        }
        return text;
    }
    for (const char *inner : {"SoftObjectPtr", "ObjectID", "SoftObjectPath"})
    {
        if (const auto next = field(module_base, id, inner))
        {
            return soft_path(module_base, next->type_id, address + next->offset, depth + 1);
        }
    }
    return std::nullopt;
}

std::optional<ULONG64> unreal_reader::find_module(std::initializer_list<const char *> names)
{
    for (const char *name : names)
    {
        ULONG64 base = 0;
        if (SUCCEEDED(symbols_->GetModuleByModuleName(name, 0, nullptr, &base)) && base != 0)
        {
            return base;
        }
    }
    // A monolithic game has everything in the executable (module index 0).
    ULONG64 base = 0;
    if (SUCCEEDED(symbols_->GetModuleByIndex(0, &base)) && base != 0)
    {
        return base;
    }
    return std::nullopt;
}

std::optional<unreal_reader::dynamic_class> unreal_reader::dynamic_type(ULONG64 object,
                                                                         const std::string &raw_static_type)
{
    const auto vtable = read<ULONG64>(object);
    if (!vtable || *vtable == 0)
    {
        return std::nullopt;
    }
    const std::string static_type = clean_type(raw_static_type);
    const auto key = std::make_pair(*vtable, static_type);
    const auto known = dynamic_types_.find(key);
    if (known != dynamic_types_.end())
    {
        return known->second;
    }
    std::optional<dynamic_class> found;
    char name[1024] = {};
    ULONG size = 0;
    ULONG64 displacement = 0;
    // The symbol names the class but not which of its vtables this is (a
    // base's sits further into the object): the pointer's own type, found
    // among the class's bases, says how far.
    constexpr std::string_view kTail = "::`vftable'";
    if (SUCCEEDED(symbols_->GetNameByOffset(*vtable, name, sizeof(name), &size, &displacement)) && displacement == 0)
    {
        const std::string symbol = name;
        const auto bang = symbol.find('!');
        if (bang != std::string::npos && symbol.size() > kTail.size() + bang + 1 &&
            symbol.compare(symbol.size() - kTail.size(), kTail.size(), kTail) == 0)
        {
            ULONG64 base = 0;
            const std::string class_name = symbol.substr(bang + 1, symbol.size() - kTail.size() - bang - 1);
            if (SUCCEEDED(symbols_->GetModuleByModuleName(symbol.substr(0, bang).c_str(), 0, nullptr, &base)) &&
                base != 0)
            {
                if (class_name == static_type || static_type.empty())
                {
                    found = dynamic_class{base, class_name, 0};
                }
                else if (const auto id = type_id(base, class_name))
                {
                    if (const type_layout *members = layout(base, *id))
                    {
                        for (const auto &[base_name, offset] : members->bases)
                        {
                            if (base_name == static_type)
                            {
                                found = dynamic_class{base, class_name, offset};
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
    dynamic_types_.emplace(key, found);
    return found;
}

std::optional<unreal_reader::set_contents> unreal_reader::read_set(ULONG64 module_base, ULONG id, ULONG64 address,
                                                                   bool with_items)
{
    const auto elements = field(module_base, id, "Elements");
    if (!elements)
    {
        return std::nullopt;
    }
    const ULONG64 elements_address = address + elements->offset;
    set_contents result;

    // TCompactSet: the elements themselves, one after another.
    if (const auto num_elements = field(module_base, id, "NumElements"))
    {
        const auto num = read<std::int32_t>(address + num_elements->offset);
        if (!num || *num < 0)
        {
            return std::nullopt;
        }
        result.num = *num;
        if (!with_items || result.num == 0)
        {
            return result;
        }
        result.element = first_template_argument(type_name(module_base, id));
        result.element_id = nested_type(module_base, id, "ElementType");
        if (!result.element_id && !result.element.empty())
        {
            result.element_id = type_id(module_base, result.element);
        }
        const ULONG stride = result.element_id ? type_size(module_base, *result.element_id) : 0;
        const ULONG64 data = allocator_data(module_base, elements->type_id, elements_address);
        if (stride == 0 || data == 0)
        {
            return result;
        }
        const std::size_t shown = std::min<std::size_t>(static_cast<std::size_t>(result.num), kMaxShownItems);
        for (std::size_t i = 0; i < shown; ++i)
        {
            result.items.push_back(data + static_cast<ULONG64>(i) * stride);
        }
        return result;
    }

    // TSparseSet (the default TSet): a TSparseArray of TSparseSetElement<T>,
    // whose allocated slots are the set bits of AllocationFlags.
    const auto data = field(module_base, elements->type_id, "Data");
    const auto free = field(module_base, elements->type_id, "NumFreeIndices");
    if (!data || !free)
    {
        return std::nullopt;
    }
    const auto num_field = field(module_base, data->type_id, "ArrayNum");
    const auto data_allocator = field(module_base, data->type_id, "AllocatorInstance");
    if (!num_field || !data_allocator)
    {
        return std::nullopt;
    }
    const auto slots = read<std::int32_t>(elements_address + data->offset + num_field->offset);
    const auto free_count = read<std::int32_t>(elements_address + free->offset);
    if (!slots || !free_count || *free_count < 0 || *slots < *free_count)
    {
        return std::nullopt;
    }
    result.num = *slots - *free_count;
    if (!with_items || result.num == 0)
    {
        return result;
    }

    // "TSparseArray<TSparseSetElement<T>,...>": the element and its Value.
    // The set's own typedefs name its element; by name only as a last resort.
    const std::string set_element = first_template_argument(type_name(module_base, elements->type_id));
    ULONG value_offset = 0;
    result.element = starts_with(set_element, "TSparseSetElement<") ? first_template_argument(set_element) : set_element;
    if (const auto stored = nested_type(module_base, id, "SetElementType"))
    {
        if (const auto value = field(module_base, *stored, "Value"))
        {
            value_offset = value->offset;
            result.element_id = value->type_id;
        }
    }
    if (!result.element_id)
    {
        result.element_id = nested_type(module_base, id, "ElementType");
    }
    if (!result.element_id && starts_with(set_element, "TSparseSetElement<"))
    {
        if (const auto stored = type_id(module_base, set_element))
        {
            if (const auto value = field(module_base, *stored, "Value"))
            {
                value_offset = value->offset;
                result.element_id = value->type_id;
            }
        }
    }
    if (!result.element_id && !result.element.empty())
    {
        result.element_id = type_id(module_base, result.element);
    }
    // A slot is TSparseArrayElementOrFreeListLink<TAlignedBytes<Size,Align>>:
    // a union of the element's bytes and two int32 free-list links.
    ULONG stride = 0;
    const std::string link = first_template_argument(type_name(module_base, data->type_id));
    const auto bytes_at = link.find("TAlignedBytes<");
    if (bytes_at != std::string::npos)
    {
        unsigned long size = 0;
        unsigned long align = 0;
        if (sscanf_s(link.c_str() + bytes_at, "TAlignedBytes<%lu,%lu>", &size, &align) == 2 && size > 0 &&
            align > 0)
        {
            const unsigned long unit = std::max(align, 4ul);
            stride = static_cast<ULONG>((std::max(size, 8ul) + unit - 1) / unit * unit);
        }
    }
    if (stride == 0)
    {
        if (const auto link_id = nested_type(module_base, elements->type_id, "FElementOrFreeListLink"))
        {
            stride = type_size(module_base, *link_id);
        }
    }
    if (stride == 0 && !link.empty())
    {
        if (const auto link_id = type_id(module_base, link))
        {
            stride = type_size(module_base, *link_id);
        }
    }
    if (!result.element_id || stride == 0)
    {
        return result;
    }
    const ULONG64 slot_data = allocator_data(module_base, data_allocator->type_id,
                                             elements_address + data->offset + data_allocator->offset);
    const auto flags = field(module_base, elements->type_id, "AllocationFlags");
    if (slot_data == 0 || !flags)
    {
        return result;
    }
    const auto flags_allocator = field(module_base, flags->type_id, "AllocatorInstance");
    if (!flags_allocator)
    {
        return result;
    }
    const ULONG64 bits = allocator_data(module_base, flags_allocator->type_id,
                                        elements_address + flags->offset + flags_allocator->offset);
    if (bits == 0)
    {
        return result;
    }
    constexpr std::size_t kWordsPerRead = 256;
    std::vector<std::uint32_t> words;
    std::size_t words_start = 0;
    const std::size_t want = std::min<std::size_t>(static_cast<std::size_t>(result.num), kMaxShownItems);
    for (std::size_t slot = 0; slot < static_cast<std::size_t>(*slots) && result.items.size() < want; ++slot)
    {
        const std::size_t word = slot / 32;
        if (word < words_start || word >= words_start + words.size())
        {
            words_start = word;
            const std::size_t total_words = (static_cast<std::size_t>(*slots) + 31) / 32;
            words.assign(std::min(kWordsPerRead, total_words - word), 0);
            ULONG done = 0;
            const ULONG bytes = static_cast<ULONG>(words.size() * sizeof(std::uint32_t));
            if (FAILED(data_->ReadVirtual(bits + word * sizeof(std::uint32_t), words.data(), bytes, &done)) ||
                done != bytes)
            {
                break;
            }
        }
        if (((words[word - words_start] >> (slot % 32)) & 1u) != 0)
        {
            result.items.push_back(slot_data + static_cast<ULONG64>(slot) * stride + value_offset);
        }
    }
    return result;
}

std::string unreal_reader::brief(ULONG64 module_base, const std::string &type, ULONG id, ULONG64 address)
{
    if (const auto plain = plain_value(type, address))
    {
        return *plain;
    }
    if (const auto shown = view(type, module_base, id, address, "", /*with_children=*/false))
    {
        return shown->display;
    }
    if (is_pointer_type(type))
    {
        const auto pointer = read<ULONG64>(address);
        return !pointer ? std::string("?") : *pointer == 0 ? std::string("nullptr") : fmt::format("0x{:x}", *pointer);
    }
    return summary(module_base, id, address).value_or("{...}");
}

std::optional<ULONG64> unreal_reader::weak_object(ULONG64 module_base, ULONG id, ULONG64 address, bool &stale)
{
    stale = false;
    if (const auto inner = field(module_base, id, "WeakPtr"))
    {
        return weak_object(module_base, inner->type_id, address + inner->offset, stale);
    }
    const auto index_field = field(module_base, id, "ObjectIndex");
    const auto serial_field = field(module_base, id, "ObjectSerialNumber");
    if (!index_field || !serial_field)
    {
        return std::nullopt;
    }
    const auto index = read<std::int32_t>(address + index_field->offset);
    const auto serial = read<std::int32_t>(address + serial_field->offset);
    if (!index || !serial)
    {
        return std::nullopt;
    }
    if (*serial < 1)
    {
        return 0;
    }

    if (!object_array_.has_value())
    {
        object_array_ = object_array{};
        // GUObjectArray lives in CoreUObject; FUObjectArray::ObjObjects is
        // the chunked table of FUObjectItems.
        const auto core = find_module({"UnrealEditor_CoreUObject", "UnrealGame_CoreUObject", "UE4Editor_CoreUObject"});
        ULONG64 symbol = 0;
        const auto array_type = core ? type_id(*core, "FUObjectArray") : std::nullopt;
        const auto item_type = core ? type_id(*core, "FUObjectItem") : std::nullopt;
        if (core && array_type && item_type &&
            SUCCEEDED(symbols_->GetOffsetByName((module_name(*core) + "!GUObjectArray").c_str(), &symbol)))
        {
            const auto table = field(*core, *array_type, "ObjObjects");
            const auto objects = table ? field(*core, table->type_id, "Objects") : std::nullopt;
            const auto num_elements = table ? field(*core, table->type_id, "NumElements") : std::nullopt;
            const auto object = field(*core, *item_type, "Object");
            const auto serial_number = field(*core, *item_type, "SerialNumber");
            // A packed item (UE_PACK_FUOBJECT_ITEM) splits the pointer; not read.
            if (objects && num_elements && object && serial_number &&
                is_pointer_type(type_name(*core, object->type_id)))
            {
                object_array_->ok = true;
                object_array_->objects = symbol + table->offset + objects->offset;
                object_array_->num_elements = symbol + table->offset + num_elements->offset;
                object_array_->item_size = type_size(*core, *item_type);
                object_array_->object_offset = object->offset;
                object_array_->serial_offset = serial_number->offset;
            }
        }
        if (!object_array_->ok)
        {
            spdlog::warn("unreal_reader: GUObjectArray not found; weak object pointers are not read");
        }
    }
    if (!object_array_->ok || object_array_->item_size == 0)
    {
        return std::nullopt;
    }
    const auto count = read<std::int32_t>(object_array_->num_elements);
    if (!count || *index < 0 || *index >= *count)
    {
        stale = true;
        return 0;
    }
    // FChunkedFixedUObjectArray::NumElementsPerChunk (an enum, not in the PDB).
    constexpr std::uint32_t kObjectsPerChunk = 64 * 1024;
    const auto table = read<ULONG64>(object_array_->objects);
    const auto chunk = table ? read<ULONG64>(*table + (static_cast<std::uint32_t>(*index) / kObjectsPerChunk) * 8)
                             : std::nullopt;
    if (!chunk || *chunk == 0)
    {
        return std::nullopt;
    }
    const ULONG64 item =
        *chunk + static_cast<ULONG64>(static_cast<std::uint32_t>(*index) % kObjectsPerChunk) * object_array_->item_size;
    const auto item_serial = read<std::int32_t>(item + object_array_->serial_offset);
    const auto object = read<ULONG64>(item + object_array_->object_offset);
    if (!item_serial || !object)
    {
        return std::nullopt;
    }
    if (*item_serial != *serial || *object == 0)
    {
        stale = true;
        return 0;
    }
    return object;
}

// TDisplayStringPtrBase -> DisplayStringPtr (TRefCountPtr) -> Reference ->
// FRefCountedDisplayString::DisplayString.
std::optional<std::string> unreal_reader::display_string(ULONG64 module_base, ULONG id, ULONG64 address)
{
    const auto holder = field(module_base, id, "DisplayStringPtr");
    const auto reference = holder ? field(module_base, holder->type_id, "Reference") : std::nullopt;
    if (!reference)
    {
        return std::nullopt;
    }
    const auto pointer = read<ULONG64>(address + holder->offset + reference->offset);
    if (!pointer || *pointer == 0)
    {
        return std::nullopt;
    }
    const auto counted = type_id(module_base, "FRefCountedDisplayString");
    const auto text = counted ? field(module_base, *counted, "DisplayString") : std::nullopt;
    if (!text)
    {
        return std::nullopt;
    }
    return read_string(module_base, text->type_id, *pointer + text->offset);
}

// Unreal.natvis: FText shows its history's localized string, else its source
// string (FTextHistory_Base), or the generated display string.
std::optional<std::string> unreal_reader::read_text_value(ULONG64 module_base, ULONG id, ULONG64 address)
{
    const auto data = field(module_base, id, "TextData");
    const auto reference = data ? field(module_base, data->type_id, "Reference") : std::nullopt;
    if (!reference)
    {
        return std::nullopt;
    }
    const auto history = read<ULONG64>(address + data->offset + reference->offset);
    if (!history)
    {
        return std::nullopt;
    }
    if (*history == 0)
    {
        return std::string();
    }
    // The history's class comes from its vtable's symbol
    // ("UnrealEditor_Core!FTextHistory_Base::`vftable'"); the classes are
    // Core's own, described only in Core's PDB.
    const auto vtable = read<ULONG64>(*history);
    if (!vtable)
    {
        return std::nullopt;
    }
    auto known = text_histories_.find(*vtable);
    if (known == text_histories_.end())
    {
        std::optional<std::pair<ULONG64, ULONG>> found;
        char name[512] = {};
        ULONG size = 0;
        ULONG64 displacement = 0;
        if (SUCCEEDED(symbols_->GetNameByOffset(*vtable, name, sizeof(name), &size, &displacement)) &&
            displacement == 0)
        {
            const std::string symbol = name;
            const auto bang = symbol.find('!');
            const auto tail = symbol.rfind("::`vftable'");
            if (bang != std::string::npos && tail != std::string::npos && tail > bang)
            {
                const std::string module = symbol.substr(0, bang);
                const std::string history_class = symbol.substr(bang + 1, tail - bang - 1);
                ULONG64 base = 0;
                if (SUCCEEDED(symbols_->GetModuleByModuleName(module.c_str(), 0, nullptr, &base)) && base != 0)
                {
                    if (const auto history_type = type_id(base, history_class))
                    {
                        found = std::make_pair(base, *history_type);
                    }
                }
            }
        }
        known = text_histories_.emplace(*vtable, found).first;
    }
    if (!known->second)
    {
        return std::nullopt;
    }
    const auto [base, history_type] = *known->second;
    if (const auto localized = field(base, history_type, "LocalizedString"))
    {
        if (auto text = display_string(base, localized->type_id, *history + localized->offset))
        {
            return text;
        }
    }
    if (const auto shown = field(base, history_type, "DisplayString"))
    {
        if (type_name(base, shown->type_id) == "FString")
        {
            return read_string(base, shown->type_id, *history + shown->offset);
        }
    }
    if (const auto source = field(base, history_type, "SourceString"))
    {
        return read_string(base, source->type_id, *history + source->offset);
    }
    // A string table entry: its reference data's cached display string.
    if (const auto table = field(base, history_type, "StringTableReferenceData"))
    {
        const auto object = field(base, table->type_id, "Object");
        const auto data_type = type_id(base, "FTextHistory_StringTableEntry::FStringTableReferenceData");
        const auto pointer = object ? read<ULONG64>(*history + table->offset + object->offset) : std::nullopt;
        if (pointer && *pointer != 0 && data_type)
        {
            if (const auto cached = field(base, *data_type, "DisplayString"))
            {
                if (auto text = display_string(base, cached->type_id, *pointer + cached->offset))
                {
                    return text;
                }
            }
        }
        return std::string("<string table entry>");
    }
    return std::nullopt;
}

std::optional<unreal_view> unreal_reader::view(const std::string &raw_type, ULONG64 module_base, ULONG id,
                                               ULONG64 address, const std::string &value_text, bool with_children)
{
    const std::string type = clean_type(raw_type);
    if (module_base == 0 || (address == 0 && !is_pointer_type(type)))
    {
        return std::nullopt;
    }
    const std::string module = module_name(module_base);

    if (type == "FName")
    {
        const auto name = read_name(module_base, id, address);
        if (name)
        {
            return unreal_view{quote(*name)};
        }
        return std::nullopt;
    }
    if (type == "UObjectBase::FNameAndObjectHashIndex")
    {
        // UE 5.6+ keeps an object's name with its hash index.
        if (const auto inner = field(module_base, id, "Name"))
        {
            if (const auto name = read_name(module_base, inner->type_id, address + inner->offset))
            {
                return unreal_view{quote(*name)};
            }
        }
        return std::nullopt;
    }
    if (type == "FString")
    {
        const auto text = read_string(module_base, id, address);
        if (text)
        {
            return unreal_view{quote(*text)};
        }
        return std::nullopt;
    }
    if (type == "FGuid")
    {
        const auto a = field(module_base, id, "A");
        const auto b = field(module_base, id, "B");
        const auto c = field(module_base, id, "C");
        const auto d = field(module_base, id, "D");
        if (!a || !b || !c || !d)
        {
            return std::nullopt;
        }
        const auto va = read<std::uint32_t>(address + a->offset);
        const auto vb = read<std::uint32_t>(address + b->offset);
        const auto vc = read<std::uint32_t>(address + c->offset);
        const auto vd = read<std::uint32_t>(address + d->offset);
        if (!va || !vb || !vc || !vd)
        {
            return std::nullopt;
        }
        return unreal_view{fmt::format("{{{:08X}-{:08X}-{:08X}-{:08X}}}", *va, *vb, *vc, *vd)};
    }
    if (type == "FGameplayTag")
    {
        if (const auto tag = field(module_base, id, "TagName"))
        {
            const auto name = read_name(module_base, tag->type_id, address + tag->offset);
            if (name)
            {
                return unreal_view{*name};
            }
        }
        return std::nullopt;
    }
    if (type == "FSoftObjectPath" || type == "FSoftObjectPtr" || starts_with(type, "TSoftObjectPtr<") ||
        starts_with(type, "TSoftClassPtr<"))
    {
        const auto path = soft_path(module_base, id, address);
        if (path)
        {
            return unreal_view{*path};
        }
        return std::nullopt;
    }
    if (starts_with(type, "TArray<"))
    {
        const auto array = read_array(module_base, id, address, type);
        if (!array)
        {
            return std::nullopt;
        }
        unreal_view result;
        result.replaces_children = true;
        if (array->num == 0)
        {
            result.display = "Empty";
            return result;
        }
        result.display = array->num > static_cast<std::int32_t>(kMaxShownItems)
                             ? fmt::format("Num={} (the first {} shown)", array->num, kMaxShownItems)
                             : fmt::format("Num={}", array->num);
        auto element_id = nested_type(module_base, id, "ElementType");
        if (!element_id && !array->element.empty())
        {
            element_id = type_id(module_base, array->element);
        }
        const ULONG element_size = element_id ? type_size(module_base, *element_id) : 0;
        if (array->data == 0 || element_size == 0)
        {
            result.replaces_children = false;
            return result;
        }
        const std::size_t shown = std::min<std::size_t>(static_cast<std::size_t>(array->num), kMaxShownItems);
        for (std::size_t i = 0; i < shown; ++i)
        {
            const ULONG64 item = array->data + static_cast<ULONG64>(i) * element_size;
            unreal_child child;
            child.name = fmt::format("[{}]", i);
            child.type = array->element;
            child.module_base = module_base;
            child.type_id = *element_id;
            child.address = item;
            child.expression = reader_expression(module_base, *element_id, item, array->element);
            result.children.push_back(std::move(child));
        }
        return result;
    }
    if (starts_with(type, "TMap<") || starts_with(type, "TMultiMap<") || starts_with(type, "TSet<") ||
        starts_with(type, "TSparseSet<") || starts_with(type, "TCompactSet<"))
    {
        const bool is_map = starts_with(type, "TMap<") || starts_with(type, "TMultiMap<");
        ULONG set_type = id;
        ULONG64 set_address = address;
        if (is_map)
        {
            const auto pairs = field(module_base, id, "Pairs");
            if (!pairs)
            {
                return std::nullopt;
            }
            set_type = pairs->type_id;
            set_address = address + pairs->offset;
        }
        const auto set = read_set(module_base, set_type, set_address, with_children);
        if (!set)
        {
            return std::nullopt;
        }
        unreal_view result;
        if (set->num == 0)
        {
            result.display = "Empty";
            return result;
        }
        result.display = set->num > static_cast<std::int32_t>(kMaxShownItems)
                             ? fmt::format("Num={} (the first {} shown)", set->num, kMaxShownItems)
                             : fmt::format("Num={}", set->num);
        result.replaces_children = true;
        result.lazy_children = true;
        if (!with_children)
        {
            return result;
        }
        if (!set->element_id || set->items.empty())
        {
            result.replaces_children = false;
            result.lazy_children = false;
            return result;
        }
        // A map's element is TTuple<Key, Value> (TPair): "[Key] = Value".
        const auto key = is_map ? field(module_base, *set->element_id, "Key") : std::nullopt;
        const auto value = is_map ? field(module_base, *set->element_id, "Value") : std::nullopt;
        if (is_map && (!key || !value))
        {
            result.replaces_children = false;
            result.lazy_children = false;
            return result;
        }
        const std::string key_type = is_map ? type_name(module_base, key->type_id) : std::string();
        const std::string value_type = is_map ? type_name(module_base, value->type_id) : set->element;
        for (std::size_t i = 0; i < set->items.size(); ++i)
        {
            const ULONG64 item = set->items[i];
            unreal_child child;
            child.module_base = module_base;
            child.type = value_type;
            if (is_map)
            {
                std::string key_text = brief(module_base, key_type, key->type_id, item + key->offset);
                if (key_type == "FName" && key_text.size() >= 2 && key_text.front() == '"' && key_text.back() == '"')
                {
                    key_text = key_text.substr(1, key_text.size() - 2);
                }
                if (key_text.size() > 80)
                {
                    key_text = key_text.substr(0, 77) + "...";
                }
                child.name = "[" + key_text + "]";
                child.type_id = value->type_id;
                child.address = item + value->offset;
            }
            else
            {
                child.name = fmt::format("[{}]", i);
                child.type_id = *set->element_id;
                child.address = item;
            }
            child.expression = reader_expression(module_base, child.type_id, child.address, child.type);
            result.children.push_back(std::move(child));
        }
        return result;
    }
    if (type == "FWeakObjectPtr" || starts_with(type, "TWeakObjectPtr<"))
    {
        bool stale = false;
        const auto object = weak_object(module_base, id, address, stale);
        if (!object)
        {
            return std::nullopt;
        }
        if (stale)
        {
            return unreal_view{"stale"};
        }
        const auto label = object_label(module_base, *object);
        if (!label)
        {
            return std::nullopt;
        }
        unreal_view result{*label};
        const std::string target = type == "FWeakObjectPtr" ? std::string("UObject") : first_template_argument(type);
        if (*object != 0 && !target.empty())
        {
            result.expand_expression = typed_expression(module_base, target, *object);
        }
        return result;
    }
    if (starts_with(type, "TSharedPtr<") || starts_with(type, "TSharedRef<") || starts_with(type, "TUniquePtr<"))
    {
        auto pointer_field = field(module_base, id, "Object");
        if (!pointer_field)
        {
            pointer_field = field(module_base, id, "Ptr");
        }
        if (!pointer_field)
        {
            return std::nullopt;
        }
        const auto object = read<ULONG64>(address + pointer_field->offset);
        if (!object)
        {
            return std::nullopt;
        }
        if (*object == 0)
        {
            return unreal_view{"nullptr"};
        }
        const std::string target = first_template_argument(type);
        unreal_view result;
        // An object pointer reads as the object; a type the reader knows as
        // its text; anything else as its address and type.
        const std::string pointer_type = type_name(module_base, pointer_field->type_id);
        auto shown = view(pointer_type, module_base, pointer_field->type_id, address + pointer_field->offset, "",
                          /*with_children=*/false);
        if (!shown && (target == "FString" || target == "FName" || target == "FText" ||
                       starts_with(target, "TArray<") || starts_with(target, "TMap<") || starts_with(target, "TSet<")))
        {
            if (const auto target_id = type_id(module_base, target))
            {
                shown = view(target, module_base, *target_id, *object, "", /*with_children=*/false);
            }
        }
        const auto real = shown ? std::nullopt : dynamic_type(*object, target);
        result.display = shown ? shown->display : fmt::format("0x{:x} ({})", *object, real ? real->name : target);
        if (shown && !shown->expand_expression.empty())
        {
            result.expand_expression = shown->expand_expression;
        }
        else if (real)
        {
            result.expand_expression = typed_expression(real->module_base, real->name, *object - real->offset);
        }
        else if (!target.empty() && !is_builtin(target))
        {
            result.expand_expression = typed_expression(module_base, target, *object);
        }
        return result;
    }
    if (starts_with(type, "TOptional<"))
    {
        const auto value = field(module_base, id, "TypedValue");
        if (!value)
        {
            return std::nullopt;
        }
        // bIsSet is a bool, or empty for a type with an intrusive unset state
        // (then the value's own text says whether it is set).
        if (const auto is_set = field(module_base, id, "bIsSet"))
        {
            if (type_name(module_base, is_set->type_id) == "bool")
            {
                const auto set = read<std::uint8_t>(address + is_set->offset);
                if (!set)
                {
                    return std::nullopt;
                }
                if (*set == 0)
                {
                    return unreal_view{"unset"};
                }
            }
        }
        const ULONG64 value_address = address + value->offset;
        const std::string value_type = type_name(module_base, value->type_id);
        unreal_view result;
        if (const auto plain = plain_value(value_type, value_address))
        {
            result.display = *plain;
            return result;
        }
        const auto shown = view(value_type, module_base, value->type_id, value_address, "", /*with_children=*/false);
        if (shown)
        {
            result.display = shown->display;
            result.expand_expression = shown->expand_expression;
        }
        else
        {
            result.display = summary(module_base, value->type_id, value_address).value_or("{...}");
        }
        if (result.expand_expression.empty() && !value_type.empty() && !is_pointer_type(value_type) &&
            !is_builtin(value_type))
        {
            result.expand_expression = typed_expression(module_base, value_type, value_address);
        }
        return result;
    }
    if (type == "FText")
    {
        const auto text = read_text_value(module_base, id, address);
        if (text)
        {
            return unreal_view{quote(*text)};
        }
        return std::nullopt;
    }
    if (starts_with(type, "TObjectPtr<") || starts_with(type, "ObjectPtr_Private::TNonAccessTrackedObjectPtr<") ||
        starts_with(type, "TNonAccessTrackedObjectPtr<"))
    {
        const auto object = object_pointer(module_base, id, address);
        if (!object)
        {
            return std::nullopt;
        }
        const auto label = object_label(module_base, *object);
        if (!label)
        {
            return std::nullopt;
        }
        unreal_view result{*label};
        const std::string target = first_template_argument(type);
        if (*object != 0 && !target.empty())
        {
            result.expand_expression = typed_expression(module_base, target, *object);
        }
        return result;
    }
    if (is_pointer_type(type))
    {
        const std::string target = pointee(type);
        if (target.empty() || is_builtin(target) || is_pointer_type(target))
        {
            return std::nullopt;
        }
        // Looking the pointee type up by name costs up to a third of a second
        // per type. Unreal's object classes are named U.../A...; the object's
        // name and class are then read and must both make sense.
        const std::string bare = target.substr(target.find_last_of(':') == std::string::npos ? 0 : target.find_last_of(':') + 1);
        if (bare.size() < 2 || (bare[0] != 'U' && bare[0] != 'A') ||
            std::isupper(static_cast<unsigned char>(bare[1])) == 0)
        {
            return std::nullopt;
        }
        std::optional<ULONG64> object = address != 0 ? read<ULONG64>(address) : std::nullopt;
        if (!object)
        {
            // "0x00000139`32ec2800 class UPIX_DeveloperSettings *"
            std::string hex = value_text.substr(0, value_text.find(' '));
            hex.erase(std::remove(hex.begin(), hex.end(), '`'), hex.end());
            if (starts_with(hex, "0x"))
            {
                hex = hex.substr(2);
            }
            char *end = nullptr;
            const unsigned long long parsed = std::strtoull(hex.c_str(), &end, 16);
            if (hex.empty() || end == nullptr || *end != 0)
            {
                return std::nullopt;
            }
            object = parsed;
        }
        const auto label = object_label(module_base, *object);
        if (label && (*object == 0 || label->find(" (") != std::string::npos))
        {
            unreal_view result{*label};
            if (*object != 0)
            {
                result.expand_expression = typed_expression(module_base, target, *object);
            }
            return result;
        }
    }
    return std::nullopt;
}

std::string unreal_reader::reader_expression(ULONG64 module_base, ULONG id, ULONG64 address, const std::string &type)
{
    return fmt::format("reader:{:x}:{:x}:{:x}:{}", module_base, id, address, clean_type(type));
}

std::optional<unreal_reader::reader_target> unreal_reader::parse_reader_expression(const std::string &expression)
{
    if (!starts_with(expression, "reader:"))
    {
        return std::nullopt;
    }
    reader_target target;
    std::size_t at = 7;
    const auto number = [&](ULONG64 &value) {
        const auto end = expression.find(':', at);
        if (end == std::string::npos)
        {
            return false;
        }
        value = std::strtoull(expression.substr(at, end - at).c_str(), nullptr, 16);
        at = end + 1;
        return true;
    };
    ULONG64 id = 0;
    if (!number(target.module_base) || !number(id) || !number(target.address))
    {
        return std::nullopt;
    }
    target.type_id = static_cast<ULONG>(id);
    target.type = expression.substr(at);
    return target;
}

std::optional<std::vector<unreal_child>> unreal_reader::members(ULONG64 module_base, ULONG id, ULONG64 address)
{
    const type_layout *known = layout(module_base, id);
    if (known == nullptr || known->fields.empty())
    {
        return std::nullopt;
    }
    std::vector<unreal_child> children;
    for (const auto &[name, info] : known->fields)
    {
        unreal_child child;
        child.name = name;
        child.type = type_name(module_base, info.type_id);
        child.module_base = module_base;
        child.type_id = info.type_id;
        child.address = address + info.offset;
        child.expression = reader_expression(module_base, info.type_id, child.address, child.type);
        children.push_back(std::move(child));
    }
    return children;
}

std::string unreal_reader::pointee_expression(ULONG64 module_base, const std::string &pointer_type, ULONG64 pointer)
{
    const std::string type = clean_type(pointer_type);
    if (!is_pointer_type(type))
    {
        return {};
    }
    const std::string target = pointee(type);
    if (target.empty() || target == "void" || is_builtin(target) || is_pointer_type(target))
    {
        return {};
    }
    if (const auto real = dynamic_type(pointer, target))
    {
        return typed_expression(real->module_base, real->name, pointer - real->offset);
    }
    return typed_expression(module_base, target, pointer);
}

std::string unreal_reader::typed_expression(ULONG64 module_base, const std::string &raw_type, ULONG64 address)
{
    const std::string type = clean_type(raw_type);
    const std::string qualified = is_builtin(type) ? type : module_name(module_base) + "!" + type;
    return fmt::format("*({} *)0x{:x}", qualified, address);
}

std::optional<std::string> unreal_reader::plain_value(const std::string &raw_type, ULONG64 address)
{
    const std::string type = clean_type(raw_type);
    if (type == "float")
    {
        if (const auto v = read<float>(address))
        {
            return fmt::format("{:.6g}", *v);
        }
    }
    else if (type == "double")
    {
        if (const auto v = read<double>(address))
        {
            return fmt::format("{:.6g}", *v);
        }
    }
    else if (type == "bool")
    {
        if (const auto v = read<std::uint8_t>(address))
        {
            return std::string(*v != 0 ? "true" : "false");
        }
    }
    else if (type == "int" || type == "int32" || type == "long")
    {
        if (const auto v = read<std::int32_t>(address))
        {
            return std::to_string(*v);
        }
    }
    else if (type == "unsigned int" || type == "uint32" || type == "unsigned long")
    {
        if (const auto v = read<std::uint32_t>(address))
        {
            return std::to_string(*v);
        }
    }
    else if (type == "__int64" || type == "int64")
    {
        if (const auto v = read<std::int64_t>(address))
        {
            return std::to_string(*v);
        }
    }
    else if (type == "unsigned __int64" || type == "uint64")
    {
        if (const auto v = read<std::uint64_t>(address))
        {
            return std::to_string(*v);
        }
    }
    else if (type == "short" || type == "int16")
    {
        if (const auto v = read<std::int16_t>(address))
        {
            return std::to_string(*v);
        }
    }
    else if (type == "unsigned short" || type == "uint16")
    {
        if (const auto v = read<std::uint16_t>(address))
        {
            return std::to_string(*v);
        }
    }
    else if (type == "unsigned char" || type == "uint8")
    {
        if (const auto v = read<std::uint8_t>(address))
        {
            return std::to_string(*v);
        }
    }
    else if (type == "char" || type == "signed char" || type == "int8")
    {
        if (const auto v = read<std::int8_t>(address))
        {
            return std::to_string(*v);
        }
    }
    return std::nullopt;
}

std::optional<std::string> unreal_reader::summary(ULONG64 module_base, ULONG id, ULONG64 address)
{
    const auto key = std::make_pair(module_base, id);
    auto layout_it = summary_layouts_.find(key);
    if (layout_it == summary_layouts_.end() && is_pointer_type(type_name(module_base, id)))
    {
        layout_it = summary_layouts_.emplace(key, std::nullopt).first;
    }
    if (layout_it == summary_layouts_.end())
    {
        // Worked out once per type: which fields make the summary.
        static const std::set<std::string> plain{"float", "double", "bool", "int", "int32", "long", "unsigned int",
                                                 "uint32", "unsigned long", "__int64", "int64", "unsigned __int64",
                                                 "uint64", "short", "int16", "unsigned short", "uint16",
                                                 "unsigned char", "uint8", "char", "signed char", "int8"};
        std::optional<std::vector<summary_field>> layout = std::vector<summary_field>{};
        std::vector<std::pair<std::string, field_info>> members;
        if (const type_layout *known = this->layout(module_base, id))
        {
            members = known->fields;
        }
        else if (get_type_info_ != nullptr && layouts_agree_.value_or(false))
        {
            // dbghelp answers for structs; this isn't one (an enum, say).
        }
        else
        {
            // Each GetFieldName costs up to half a second on a math type with
            // many static members, so the walk ends once the fields read
            // cover the whole type.
            const ULONG type_bytes = type_size(module_base, id);
            ULONG covered = 0;
            for (ULONG index = 0; index < 32 && (type_bytes == 0 || covered < type_bytes); ++index)
            {
                char name[256] = {};
                ULONG size = 0;
                if (FAILED(symbols_->GetFieldName(module_base, id, index, name, sizeof(name), &size)))
                {
                    break;
                }
                if (const auto info = field(module_base, id, name))
                {
                    members.emplace_back(name, *info);
                    covered = std::max(covered, info->offset + type_size(module_base, info->type_id));
                    const std::string field_type = type_name(module_base, info->type_id);
                    if (plain.count(field_type) == 0 && (field_type.empty() || field_type.back() != ']'))
                    {
                        break; // not a summary; the rest needn't be read
                    }
                }
            }
        }
        for (const auto &[name, info] : members)
        {
            const std::string field_type = type_name(module_base, info.type_id);
            if (!field_type.empty() && field_type.back() == ']')
            {
                continue; // a fixed array view over the same fields (FLinearColor::RGBA)
            }
            if (plain.count(field_type) == 0 || layout->size() >= 6)
            {
                layout.reset();
                break;
            }
            layout->push_back(summary_field{name, info.offset, field_type});
        }
        if (layout.has_value() && layout->empty())
        {
            layout.reset();
        }
        layout_it = summary_layouts_.emplace(key, std::move(layout)).first;
    }
    if (!layout_it->second.has_value())
    {
        return std::nullopt;
    }
    std::vector<std::string> parts;
    for (const auto &f : *layout_it->second)
    {
        const auto value = plain_value(f.type, address + f.offset);
        if (!value)
        {
            return std::nullopt;
        }
        parts.push_back(f.name + "=" + *value);
    }
    std::string text = "(";
    for (std::size_t i = 0; i < parts.size(); ++i)
    {
        text += (i == 0 ? "" : ", ") + parts[i];
    }
    return text + ")";
}
} // namespace dap_dbgeng::debugger
