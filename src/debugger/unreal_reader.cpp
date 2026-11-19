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

unreal_reader::unreal_reader(IDebugSymbols3 *symbols, IDebugDataSpaces4 *data, IDebugControl7 *control)
    : symbols_(symbols), data_(data), control_(control)
{
}

void unreal_reader::forget_modules()
{
    module_names_.clear();
    name_blocks_.clear();
    name_layouts_.clear();
    core_module_.reset();
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

std::optional<unreal_reader::field_info> unreal_reader::field(ULONG64 module_base, ULONG type_id, const char *name)
{
    const auto key = std::make_tuple(module_base, type_id, std::string(name));
    const auto cached = fields_.find(key);
    if (cached != fields_.end())
    {
        return cached->second;
    }
    std::optional<field_info> result;
    field_info info;
    if (SUCCEEDED(symbols_->GetFieldTypeAndOffset(module_base, type_id, name, &info.type_id, &info.offset)))
    {
        result = info;
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
    spdlog::debug("unreal_reader: name {:#x}: block {} at {:#x}, entry {:#x}, layout h{} c{} s{} long{}, header {:#x}, len {}",
                  index, block, *block_address, entry, layout.header, layout.chars, layout.stride, layout.long_length,
                  *header, length);
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

    const ULONG64 allocator_address = address + allocator->offset;
    if (const auto data = field(module_base, allocator->type_id, "Data"))
    {
        info.data = read<ULONG64>(allocator_address + data->offset).value_or(0);
    }
    else if (const auto secondary = field(module_base, allocator->type_id, "SecondaryData"))
    {
        // Inline allocators keep the first items in place and spill to the heap.
        if (const auto secondary_data = field(module_base, secondary->type_id, "Data"))
        {
            info.data = read<ULONG64>(allocator_address + secondary->offset + secondary_data->offset).value_or(0);
        }
        if (info.data == 0)
        {
            if (const auto inline_data = field(module_base, allocator->type_id, "InlineData"))
            {
                info.data = allocator_address + inline_data->offset;
            }
        }
    }
    return info;
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
        spdlog::debug("unreal_reader: could not read the name of object 0x{:x} in {} (blocks 0x{:x})", object,
                      module_name(module_base), name_blocks_.count(module_base) ? name_blocks_[module_base] : 0);
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

std::optional<std::int32_t> unreal_reader::set_count(ULONG64 module_base, ULONG id, ULONG64 address)
{
    if (const auto pairs = field(module_base, id, "Pairs"))
    {
        return set_count(module_base, pairs->type_id, address + pairs->offset);
    }
    const auto elements = field(module_base, id, "Elements");
    if (!elements)
    {
        return std::nullopt;
    }
    const auto data = field(module_base, elements->type_id, "Data");
    const auto free = field(module_base, elements->type_id, "NumFreeIndices");
    if (!data || !free)
    {
        return std::nullopt;
    }
    const auto num_field = field(module_base, data->type_id, "ArrayNum");
    if (!num_field)
    {
        return std::nullopt;
    }
    const ULONG64 elements_address = address + elements->offset;
    const auto num = read<std::int32_t>(elements_address + data->offset + num_field->offset);
    const auto free_count = read<std::int32_t>(elements_address + free->offset);
    if (!num || !free_count || *num < *free_count)
    {
        return std::nullopt;
    }
    return *num - *free_count;
}

std::optional<unreal_view> unreal_reader::view(const std::string &raw_type, ULONG64 module_base, ULONG id,
                                               ULONG64 address, const std::string &value_text)
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
        const auto element_id = array->element.empty() ? std::nullopt : type_id(module_base, array->element);
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
            child.expression = typed_expression(module_base, array->element, item);
            result.children.push_back(std::move(child));
        }
        return result;
    }
    if (starts_with(type, "TMap<") || starts_with(type, "TSet<"))
    {
        const auto count = set_count(module_base, id, address);
        if (count)
        {
            return unreal_view{*count == 0 ? std::string("Empty") : fmt::format("Num={}", *count)};
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
    if (layout_it == summary_layouts_.end())
    {
        // Worked out once per type: which fields make the summary.
        static const std::set<std::string> plain{"float", "double", "bool", "int", "int32", "long", "unsigned int",
                                                 "uint32", "unsigned long", "__int64", "int64", "unsigned __int64",
                                                 "uint64", "short", "int16", "unsigned short", "uint16",
                                                 "unsigned char", "uint8", "char", "signed char", "int8"};
        std::optional<std::vector<summary_field>> layout = std::vector<summary_field>{};
        for (ULONG index = 0; index < 32 && layout.has_value(); ++index)
        {
            char name[256] = {};
            ULONG size = 0;
            if (FAILED(symbols_->GetFieldName(module_base, id, index, name, sizeof(name), &size)))
            {
                break;
            }
            const auto info = field(module_base, id, name);
            if (!info)
            {
                continue; // static members have no offset
            }
            const std::string field_type = type_name(module_base, info->type_id);
            if (!field_type.empty() && field_type.back() == ']')
            {
                continue; // a fixed array view over the same fields (FLinearColor::RGBA)
            }
            if (plain.count(field_type) == 0 || layout->size() >= 6)
            {
                layout.reset();
                break;
            }
            layout->push_back(summary_field{name, info->offset, field_type});
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
