#include "debugger/unreal_reader.h"

// Blueprint frames: every FFrame the engine's script interpreter makes
// becomes the thread's top tracking frame (CoreUObject's thread_local
// GTopTrackingStackFrame, ScriptCore.cpp) and links the one before it
// (FFrame::PreviousTrackingFrame), in every build, Shipping included.

namespace dap_dbgeng::debugger
{
namespace
{
constexpr int kMaxScriptFrames = 1024;
constexpr int kMaxScriptProperties = 400;
constexpr std::int32_t kMaxScriptArrayItems = 100;
// FUNC_Native (ObjectMacros.h): a C++ function ProcessEvent runs, not script.
constexpr std::uint32_t kFunctionNative = 0x00000400;
// x64 TEB: ThreadLocalStoragePointer, the array of the thread's static TLS
// blocks, one per module, indexed by the module's _tls_index (PE TLS).
constexpr ULONG64 kTebThreadLocalStoragePointer = 0x58;
constexpr ULONG64 kMaxTlsBlock = 0x100000;

bool ends_with(const std::string &text, const char *suffix)
{
    const std::size_t length = std::strlen(suffix);
    return text.size() >= length && text.compare(text.size() - length, length, suffix) == 0;
}

// The C++ type of a numeric property, as the reader's plain_value reads it.
const char *numeric_type(const std::string &property_class)
{
    static const std::map<std::string, const char *> types = {
        {"IntProperty", "int32"},     {"Int64Property", "int64"},   {"Int16Property", "int16"},
        {"Int8Property", "int8"},     {"UInt64Property", "uint64"}, {"UInt32Property", "uint32"},
        {"UInt16Property", "uint16"}, {"ByteProperty", "uint8"},    {"FloatProperty", "float"},
        {"DoubleProperty", "double"}};
    const auto found = types.find(property_class);
    return found == types.end() ? nullptr : found->second;
}

// Properties whose memory is one of the engine's own C++ types, which the
// reader already knows how to show.
const char *engine_value_type(const std::string &property_class)
{
    static const std::map<std::string, const char *> types = {
        {"NameProperty", "FName"},
        {"StrProperty", "FString"},
        {"TextProperty", "FText"},
        {"WeakObjectProperty", "FWeakObjectPtr"},
        {"SoftObjectProperty", "FSoftObjectPtr"},
        {"SoftClassProperty", "FSoftObjectPtr"},
        {"LazyObjectProperty", "FLazyObjectPtr"}};
    const auto found = types.find(property_class);
    return found == types.end() ? nullptr : found->second;
}

bool is_object_property(const std::string &property_class)
{
    return property_class == "ObjectProperty" || property_class == "ClassProperty" ||
           property_class == "ObjectPtrProperty" || property_class == "ClassPtrProperty" ||
           property_class == "InterfaceProperty";
}
} // namespace

std::optional<ULONG64> unreal_reader::script_module()
{
    if (script_module_.has_value())
    {
        return *script_module_;
    }
    // Not kept when missing: CoreUObject may not have loaded yet.
    if (const auto found = find_module({"UnrealEditor_CoreUObject", "UnrealGame_CoreUObject", "UE4Editor_CoreUObject"}))
    {
        if (type_id(*found, "FFrame").has_value())
        {
            script_module_ = *found;
            return *found;
        }
    }
    return std::nullopt;
}

std::optional<ULONG64> unreal_reader::top_frame_address(ULONG64 module_base, ULONG64 teb)
{
    // A thread's static TLS block: TEB.ThreadLocalStoragePointer[_tls_index].
    const auto block_of = [&](std::uint32_t index) -> std::optional<ULONG64> {
        const auto blocks = read<ULONG64>(teb + kTebThreadLocalStoragePointer);
        if (!blocks || *blocks == 0)
        {
            return std::nullopt;
        }
        return read<ULONG64>(*blocks + static_cast<ULONG64>(index) * sizeof(ULONG64));
    };
    if (!top_frame_tls_.has_value())
    {
        // dbgeng resolves a thread_local for the current thread; its offset in
        // that thread's block is the same in every thread's.
        std::optional<std::pair<std::uint32_t, ULONG64>> found;
        const std::string module = module_name(module_base);
        ULONG64 index_address = 0;
        ULONG64 variable = 0;
        if (SUCCEEDED(symbols_->GetOffsetByName((module + "!_tls_index").c_str(), &index_address)) &&
            SUCCEEDED(symbols_->GetOffsetByName((module + "!GTopTrackingStackFrame").c_str(), &variable)))
        {
            const auto index = read<std::uint32_t>(index_address);
            const auto block = index ? block_of(*index) : std::nullopt;
            if (block && *block != 0 && variable >= *block && variable - *block < kMaxTlsBlock)
            {
                found = std::make_pair(*index, variable - *block);
            }
        }
        if (!found)
        {
            spdlog::debug("unreal_reader: no GTopTrackingStackFrame in {}", module);
        }
        top_frame_tls_ = found;
    }
    if (!top_frame_tls_->has_value())
    {
        return std::nullopt;
    }
    const auto block = block_of((*top_frame_tls_)->first);
    if (!block || *block == 0)
    {
        return std::nullopt;
    }
    return *block + (*top_frame_tls_)->second;
}

std::optional<unreal_reader::script_frame> unreal_reader::read_script_frame(ULONG64 module_base, ULONG64 frame)
{
    const auto frame_type = type_id(module_base, "FFrame");
    if (!frame_type || frame == 0)
    {
        return std::nullopt;
    }
    const auto node = field(module_base, *frame_type, "Node");
    const auto object = field(module_base, *frame_type, "Object");
    const auto locals = field(module_base, *frame_type, "Locals");
    if (!node || !object || !locals)
    {
        return std::nullopt;
    }
    script_frame result;
    result.frame = frame;
    result.function = read<ULONG64>(frame + node->offset).value_or(0);
    result.object = read<ULONG64>(frame + object->offset).value_or(0);
    result.locals = read<ULONG64>(frame + locals->offset).value_or(0);
    if (result.function == 0)
    {
        return std::nullopt;
    }
    result.function_name = object_name(module_base, result.function).value_or("?");
    result.object_name = object_name(module_base, result.object).value_or("");
    if (const auto outer = object_field(module_base, result.function, "UObjectBase", "OuterPrivate"))
    {
        result.owner = object_name(module_base, *outer).value_or("");
        if (ends_with(result.owner, "_C"))
        {
            result.owner.resize(result.owner.size() - 2);
        }
    }
    if (const auto function_type = type_id(module_base, "UFunction"))
    {
        if (const auto flags = field(module_base, *function_type, "FunctionFlags"))
        {
            result.native = (read<std::uint32_t>(result.function + flags->offset).value_or(0) & kFunctionNative) != 0;
        }
    }
    return result;
}

std::vector<unreal_reader::script_frame> unreal_reader::script_stack(ULONG64 teb)
{
    std::vector<script_frame> frames;
    const auto module = script_module();
    if (!module || teb == 0)
    {
        return frames;
    }
    const auto top = top_frame_address(*module, teb);
    const auto frame_type = type_id(*module, "FFrame");
    const auto previous = frame_type ? field(*module, *frame_type, "PreviousTrackingFrame") : std::nullopt;
    if (!top || !previous)
    {
        return frames;
    }
    ULONG64 frame = read<ULONG64>(*top).value_or(0);
    for (int count = 0; frame != 0 && count < kMaxScriptFrames; ++count)
    {
        if (auto info = read_script_frame(*module, frame))
        {
            frames.push_back(std::move(*info));
        }
        frame = read<ULONG64>(frame + previous->offset).value_or(0);
    }
    std::reverse(frames.begin(), frames.end());
    return frames;
}

std::optional<std::pair<ULONG64, ULONG>> unreal_reader::engine_type(const std::string &name)
{
    const auto cached = engine_types_.find(name);
    if (cached != engine_types_.end())
    {
        return cached->second;
    }
    std::optional<std::pair<ULONG64, ULONG>> result;
    for (const auto &modules : {std::initializer_list<const char *>{"UnrealEditor_CoreUObject", "UnrealGame_CoreUObject"},
                                std::initializer_list<const char *>{"UnrealEditor_Core", "UnrealGame_Core"},
                                std::initializer_list<const char *>{"UnrealEditor_Engine", "UnrealGame_Engine"}})
    {
        const auto module = find_module(modules);
        if (!module)
        {
            continue;
        }
        if (const auto id = type_id(*module, name))
        {
            result = std::make_pair(*module, *id);
            break;
        }
    }
    engine_types_[name] = result;
    return result;
}

std::string unreal_reader::property_type(ULONG64 module_base, ULONG64 property, const std::string &property_class)
{
    if (const char *numeric = numeric_type(property_class))
    {
        if (property_class == "ByteProperty")
        {
            if (const auto enum_object = object_field(module_base, property, "FByteProperty", "Enum");
                enum_object && *enum_object != 0)
            {
                return object_name(module_base, *enum_object).value_or("uint8");
            }
        }
        return numeric;
    }
    if (property_class == "BoolProperty")
    {
        return "bool";
    }
    if (property_class == "EnumProperty")
    {
        if (const auto enum_object = object_field(module_base, property, "FEnumProperty", "Enum");
            enum_object && *enum_object != 0)
        {
            return object_name(module_base, *enum_object).value_or("enum");
        }
        return "enum";
    }
    if (const char *engine = engine_value_type(property_class))
    {
        return engine;
    }
    if (is_object_property(property_class))
    {
        const char *field_name = property_class == "InterfaceProperty" ? "InterfaceClass" : "PropertyClass";
        const char *owner_type = property_class == "InterfaceProperty" ? "FInterfaceProperty" : "FObjectPropertyBase";
        if (const auto class_object = object_field(module_base, property, owner_type, field_name);
            class_object && *class_object != 0)
        {
            const std::string name = object_name(module_base, *class_object).value_or("UObject");
            if (property_class == "InterfaceProperty")
            {
                return "TScriptInterface<I" + name + ">";
            }
            if (ends_with(name, "_C"))
            {
                return name + "*";
            }
            // A/U as in C++: an actor class has Actor among its supers.
            bool actor = false;
            ULONG64 at = *class_object;
            for (int depth = 0; depth < 32 && at != 0 && !actor; ++depth)
            {
                actor = object_name(module_base, at).value_or("") == "Actor";
                at = object_field(module_base, at, "UStruct", "SuperStruct").value_or(0);
            }
            return std::string(actor ? "A" : "U") + name + "*";
        }
        return "UObject*";
    }
    if (property_class == "StructProperty")
    {
        if (const auto struct_object = object_field(module_base, property, "FStructProperty", "Struct");
            struct_object && *struct_object != 0)
        {
            return "F" + object_name(module_base, *struct_object).value_or("Struct");
        }
        return "struct";
    }
    if (property_class == "ArrayProperty")
    {
        if (const auto inner = object_field(module_base, property, "FArrayProperty", "Inner"); inner && *inner != 0)
        {
            const auto inner_class = object_field(module_base, *inner, "FField", "ClassPrivate");
            std::string inner_class_name;
            if (inner_class && *inner_class != 0)
            {
                if (const auto class_type = type_id(module_base, "FFieldClass"))
                {
                    if (const auto name = field(module_base, *class_type, "Name"))
                    {
                        inner_class_name = read_name(module_base, name->type_id, *inner_class + name->offset).value_or("");
                    }
                }
            }
            return "TArray<" + property_type(module_base, *inner, inner_class_name) + ">";
        }
        return "TArray";
    }
    if (ends_with(property_class, "Property"))
    {
        return property_class.substr(0, property_class.size() - std::strlen("Property"));
    }
    return property_class;
}

variable_node unreal_reader::property_node(ULONG64 module_base, ULONG64 property, ULONG64 address, int depth)
{
    variable_node node;
    node.address = address;
    const auto field_type = type_id(module_base, "FField");
    const auto property_struct = type_id(module_base, "FProperty");
    const auto class_type = type_id(module_base, "FFieldClass");
    if (!field_type || !property_struct || !class_type)
    {
        node.value = "{...}";
        return node;
    }
    if (const auto name = field(module_base, *field_type, "NamePrivate"))
    {
        node.name = read_name(module_base, name->type_id, property + name->offset).value_or("?");
    }
    std::string property_class;
    if (const auto class_object = object_field(module_base, property, "FField", "ClassPrivate"))
    {
        if (const auto name = field(module_base, *class_type, "Name"))
        {
            property_class = read_name(module_base, name->type_id, *class_object + name->offset).value_or("");
        }
    }
    node.type = property_type(module_base, property, property_class);
    if (const auto size = field(module_base, *property_struct, "ElementSize"))
    {
        node.size = static_cast<std::uint32_t>(read<std::int32_t>(property + size->offset).value_or(0));
    }

    if (const char *numeric = numeric_type(property_class))
    {
        node.value = plain_value(numeric, address).value_or("?");
        return node;
    }
    if (property_class == "EnumProperty")
    {
        const char *underlying = node.size == 8 ? "int64" : node.size == 4 ? "int32" : node.size == 2 ? "uint16" : "uint8";
        node.value = plain_value(underlying, address).value_or("?");
        return node;
    }
    if (property_class == "BoolProperty")
    {
        const auto bool_type = type_id(module_base, "FBoolProperty");
        const auto byte_offset = bool_type ? field(module_base, *bool_type, "ByteOffset") : std::nullopt;
        const auto field_mask = bool_type ? field(module_base, *bool_type, "FieldMask") : std::nullopt;
        std::uint8_t offset = 0;
        std::uint8_t mask = 0xFF;
        if (byte_offset && field_mask)
        {
            offset = read<std::uint8_t>(property + byte_offset->offset).value_or(0);
            mask = read<std::uint8_t>(property + field_mask->offset).value_or(0xFF);
        }
        if (const auto byte = read<std::uint8_t>(address + offset))
        {
            node.value = (*byte & mask) != 0 ? "true" : "false";
        }
        else
        {
            node.value = "?";
        }
        return node;
    }
    if (const char *engine = engine_value_type(property_class))
    {
        if (const auto type = engine_type(engine))
        {
            if (const auto shown = view(engine, type->first, type->second, address, "", /*with_children=*/false))
            {
                node.value = shown->display;
                node.lazy_expression = shown->expand_expression;
                node.is_expandable = !node.lazy_expression.empty();
                return node;
            }
        }
        node.value = "{...}";
        return node;
    }
    if (is_object_property(property_class))
    {
        const auto object = read<ULONG64>(address);
        if (!object)
        {
            node.value = "?";
            return node;
        }
        if ((*object & 1) != 0)
        {
            node.value = "(not loaded yet)";
            return node;
        }
        node.value = object_label(module_base, *object).value_or(fmt::format("0x{:x}", *object));
        if (*object != 0)
        {
            node.lazy_expression = pointee_expression(module_base, "UObject *", *object);
        }
        node.is_expandable = !node.lazy_expression.empty();
        return node;
    }
    if (property_class == "StructProperty")
    {
        if (const auto type = engine_type(node.type))
        {
            if (const auto shown = view(node.type, type->first, type->second, address, "", /*with_children=*/false))
            {
                node.value = shown->display;
                node.lazy_expression = shown->expand_expression;
            }
            else
            {
                node.value = summary(type->first, type->second, address).value_or("{...}");
                node.lazy_expression = reader_expression(type->first, type->second, address, type_name(type->first, type->second));
            }
        }
        else
        {
            node.value = "{...}";
        }
        node.is_expandable = !node.lazy_expression.empty();
        return node;
    }
    if (property_class == "ArrayProperty")
    {
        // FScriptArray: the items' pointer, then ArrayNum (Containers/ScriptArray.h).
        const auto data = read<ULONG64>(address);
        const auto num = read<std::int32_t>(address + sizeof(ULONG64));
        if (!data || !num || *num < 0)
        {
            node.value = "?";
            return node;
        }
        node.value = fmt::format("Num={}", *num);
        const auto inner = object_field(module_base, property, "FArrayProperty", "Inner");
        const auto size = field(module_base, *property_struct, "ElementSize");
        if (depth < 2 && inner && *inner != 0 && size && *data != 0)
        {
            const auto element_size = read<std::int32_t>(*inner + size->offset).value_or(0);
            for (std::int32_t i = 0; element_size > 0 && i < std::min(*num, kMaxScriptArrayItems); ++i)
            {
                variable_node item =
                    property_node(module_base, *inner, *data + static_cast<ULONG64>(i) * element_size, depth + 1);
                item.name = fmt::format("[{}]", i);
                node.children.push_back(std::move(item));
            }
            if (*num > kMaxScriptArrayItems)
            {
                node.value = fmt::format("Num={} (the first {} shown)", *num, kMaxScriptArrayItems);
            }
        }
        return node;
    }
    node.value = "{...}";
    return node;
}

std::vector<variable_node> unreal_reader::script_locals(ULONG64 frame)
{
    std::vector<variable_node> out;
    const auto module = script_module();
    if (!module)
    {
        return out;
    }
    const auto info = read_script_frame(*module, frame);
    if (!info)
    {
        return out;
    }
    variable_node self;
    self.name = "self";
    self.value = object_label(*module, info->object).value_or(info->object_name);
    self.type = info->owner.empty() ? std::string("UObject") : info->owner;
    self.address = info->object;
    if (info->object != 0)
    {
        self.lazy_expression = pointee_expression(*module, "UObject *", info->object);
    }
    self.is_expandable = !self.lazy_expression.empty();
    out.push_back(std::move(self));
    if (info->locals == 0)
    {
        return out;
    }
    const auto property_struct = type_id(*module, "FProperty");
    const auto offset = property_struct ? field(*module, *property_struct, "Offset_Internal") : std::nullopt;
    if (!offset)
    {
        return out;
    }
    ULONG64 property = object_field(*module, info->function, "UStruct", "ChildProperties").value_or(0);
    for (int count = 0; property != 0 && count < kMaxScriptProperties; ++count)
    {
        const auto at = read<std::int32_t>(property + offset->offset);
        if (at && *at >= 0)
        {
            out.push_back(property_node(*module, property, info->locals + static_cast<ULONG64>(*at), 0));
        }
        property = object_field(*module, property, "FField", "Next").value_or(0);
    }
    return out;
}
} // namespace dap_dbgeng::debugger
