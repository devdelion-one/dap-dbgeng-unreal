#pragma once

#include "stdafx.h"

#include "debugger/debugger_types.h"

namespace dap_dbgeng::debugger
{
// How an Unreal value reads: the text shown for it, and, when the reader knows
// the value's real contents (an array's items, the object behind an object
// pointer), the children to show instead of the type's raw members.
struct unreal_view
{
    std::string display;
    bool replaces_children = false;
    std::vector<unreal_child> children;
    // Opening the value shows this expression's members instead (an object
    // pointer opens onto the object).
    std::string expand_expression;
    // The children are worth reading only when the value is opened (a map's
    // items): the value is shown closed, with a lazy expression of its own.
    bool lazy_children = false;
};

// Reads Unreal Engine's core types straight from memory with the program's own
// type information (field offsets from its PDBs, never hard-coded layouts), the
// way the engine's Unreal.natvis describes them. Windows' own natvis support
// gets FName, object names and FGuid wrong, which is why this exists.
class unreal_reader
{
  public:
    // `dbghelp` is the engine's own copy (the one dbgeng uses): a type's
    // members are read from it in one pass instead of one dbgeng query each.
    unreal_reader(IDebugSymbols3 *symbols, IDebugDataSpaces4 *data, IDebugControl7 *control,
                  IDebugSystemObjects *system_objects, HMODULE dbghelp);

    // `value_text` is the engine's own text for the value; a pointer held in a
    // register (often `this`) has no address, and its value is read from there.
    // `with_children` false: a value whose children are lazy (a map) comes
    // back without them, and is read again when opened.
    std::optional<unreal_view> view(const std::string &type_name, ULONG64 module_base, ULONG type_id,
                                    ULONG64 address, const std::string &value_text, bool with_children = true);

    // "(R=0.2, G=0.8, B=0.2, A=1)" for a small struct of plain numbers, read
    // from memory with the type's own fields.
    std::optional<std::string> summary(ULONG64 module_base, ULONG type_id, ULONG64 address);
    // A plain number or bool read from memory, as the engine would print it.
    std::optional<std::string> plain_value(const std::string &type, ULONG64 address);
    // A typed expression for a value at an address: `*(Module!Type *)0x...`.
    std::string typed_expression(ULONG64 module_base, const std::string &type, ULONG64 address);
    // A value the reader opens itself: "reader:<module>:<type id>:<address>:<type>".
    // dbgeng parsing a long template name (TSet<...>) in a typed expression
    // takes seconds; this needs no parsing at all.
    static std::string reader_expression(ULONG64 module_base, ULONG type_id, ULONG64 address,
                                         const std::string &type);
    struct reader_target
    {
        ULONG64 module_base = 0;
        ULONG type_id = 0;
        ULONG64 address = 0;
        std::string type;
    };
    static std::optional<reader_target> parse_reader_expression(const std::string &expression);
    std::string module_name(ULONG64 module_base);
    // A value in a few words, as an item's or a key's text: a number, the
    // reader's own text, a pointer's address, a small struct's fields, or
    // "{...}".
    std::string brief(ULONG64 module_base, const std::string &type, ULONG type_id, ULONG64 address);
    std::optional<ULONG64> pointer_at(ULONG64 address)
    {
        return read<ULONG64>(address);
    }
    // What opening a plain pointer shows: `*(Module!Type *)0x...`, or nothing
    // for a pointer to a built-in type.
    std::string pointee_expression(ULONG64 module_base, const std::string &pointer_type, ULONG64 pointer);
    // A value's own data members as children (a container's [Raw View]),
    // from the type's layout; nullopt when there is none.
    std::optional<std::vector<unreal_child>> members(ULONG64 module_base, ULONG type_id, ULONG64 address);

    // Names are global and never change once made, so their cache lives for the
    // whole session; this only drops what depends on loaded modules.
    void forget_modules();

  private:
    struct field_info
    {
        ULONG type_id = 0;
        ULONG offset = 0;
    };

    std::optional<field_info> field(ULONG64 module_base, ULONG type_id, const char *name);
    std::optional<ULONG> type_id(ULONG64 module_base, const std::string &name);
    std::string type_name(ULONG64 module_base, ULONG type_id);
    ULONG type_size(ULONG64 module_base, ULONG type_id);

    template <class T> std::optional<T> read(ULONG64 address);
    std::optional<std::string> read_text(ULONG64 address, std::size_t count, bool wide);

    std::optional<std::string> read_name(ULONG64 module_base, ULONG type_id, ULONG64 address);
    std::optional<std::string> name_from_index(ULONG64 module_base, std::uint32_t index,
                                               std::uint32_t comparison_index);
    std::optional<std::string> read_string(ULONG64 module_base, ULONG type_id, ULONG64 address);
    struct array_info
    {
        ULONG64 data = 0;
        std::int32_t num = 0;
        std::string element;
    };
    std::optional<array_info> read_array(ULONG64 module_base, ULONG type_id, ULONG64 address,
                                         const std::string &array_type_name);
    std::optional<ULONG64> object_pointer(ULONG64 module_base, ULONG type_id, ULONG64 address, int depth = 0);
    std::optional<std::string> object_label(ULONG64 module_base, ULONG64 object);
    std::optional<std::string> soft_path(ULONG64 module_base, ULONG type_id, ULONG64 address, int depth = 0);
    // Where an allocator keeps its items: the heap pointer, or the inline
    // storage of an inline allocator that hasn't spilled.
    ULONG64 allocator_data(ULONG64 module_base, ULONG allocator_type, ULONG64 allocator_address);
    struct set_contents
    {
        std::int32_t num = 0;
        // The allocated elements' addresses, the first kMaxShownItems.
        std::vector<ULONG64> items;
        std::string element;
        std::optional<ULONG> element_id;
    };
    std::optional<set_contents> read_set(ULONG64 module_base, ULONG type_id, ULONG64 address, bool with_items);
    // The object a weak pointer names, through GUObjectArray; nullopt when
    // it can't be read, 0 for nullptr, and `stale` set for a dead object.
    std::optional<ULONG64> weak_object(ULONG64 module_base, ULONG type_id, ULONG64 address, bool &stale);
    std::optional<std::string> read_text_value(ULONG64 module_base, ULONG type_id, ULONG64 address);
    std::optional<std::string> display_string(ULONG64 module_base, ULONG type_id, ULONG64 address);
    std::optional<ULONG64> find_module(std::initializer_list<const char *> names);
    // The class an object really is, from its vtable's symbol
    // ("Module!Class::`vftable'"), the module whose PDB describes it, and how
    // far into the whole object a pointer of `static_type` is (a base's part
    // of it, from the class's own base list: Unreal builds have no RTTI).
    struct dynamic_class
    {
        ULONG64 module_base = 0;
        std::string name;
        ULONG offset = 0;
    };
    std::optional<dynamic_class> dynamic_type(ULONG64 object, const std::string &static_type);
    std::map<std::pair<ULONG64, std::string>, std::optional<dynamic_class>> dynamic_types_;

    IDebugSymbols3 *symbols_;
    IDebugDataSpaces4 *data_;
    IDebugControl7 *control_;
    IDebugSystemObjects *system_objects_;

    // A type's instance data members (its bases' first) with their offsets,
    // read once from dbghelp: dbgeng's GetFieldName / GetFieldTypeAndOffset
    // take up to half a second each on a big template (UE::Math::TVector).
    // Null when dbghelp can't answer; dbgeng is asked instead.
    struct type_layout
    {
        std::vector<std::pair<std::string, field_info>> fields;
        std::unordered_map<std::string, field_info> by_name;
        // The type's own nested typedefs (TArray::ElementType...): looking a
        // template's argument up by name costs up to seconds.
        std::unordered_map<std::string, ULONG> typedefs;
        // Every base class, however deep, and where it sits in the type.
        std::vector<std::pair<std::string, ULONG>> bases;
    };
    std::optional<ULONG> nested_type(ULONG64 module_base, ULONG type_id, const char *name);
    const type_layout *layout(ULONG64 module_base, ULONG type_id);
    bool collect_members(HANDLE process, ULONG64 module_base, ULONG type_id, ULONG base_offset, type_layout &out,
                         int depth);
    using sym_get_type_info_t = BOOL(WINAPI *)(HANDLE, DWORD64, ULONG, IMAGEHLP_SYMBOL_TYPE_INFO, PVOID);
    sym_get_type_info_t get_type_info_ = nullptr;
    // Checked once against dbgeng: its type ids must be dbghelp's.
    std::optional<bool> layouts_agree_;
    std::map<std::pair<ULONG64, ULONG>, std::unique_ptr<type_layout>> layouts_;

    std::map<std::uint32_t, std::string> names_;
    // dbghelp type queries cost milliseconds each; every answer is kept.
    std::map<std::tuple<ULONG64, ULONG, std::string>, std::optional<field_info>> fields_;
    std::map<std::pair<ULONG64, std::string>, std::optional<ULONG>> type_ids_;
    std::map<std::pair<ULONG64, ULONG>, std::string> type_names_;
    std::map<std::pair<ULONG64, ULONG>, ULONG> type_sizes_;
    struct summary_field
    {
        std::string name;
        ULONG offset = 0;
        std::string type;
    };
    std::map<std::pair<ULONG64, ULONG>, std::optional<std::vector<summary_field>>> summary_layouts_;
    std::map<ULONG64, std::string> module_names_;
    std::map<ULONG64, ULONG64> name_blocks_;
    struct name_layout
    {
        ULONG header = 0;
        ULONG chars = 2;
        ULONG stride = 0;
        bool long_length = true;
        bool has_comparison_id = false;
    };
    // alignof(FNameEntry) isn't in the PDB, and it differs by build (8 in a UE
    // 5.7 editor); it's worked out once from a name whose index is known.
    void calibrate_stride(name_layout &layout, ULONG64 blocks, std::uint32_t comparison_index);
    std::map<ULONG64, name_layout> name_layouts_;
    // A module whose PDB describes the name table's own types (FNameEntry): the
    // value's module often doesn't; Core always does.
    std::optional<ULONG64> core_module_;
    // GUObjectArray's chunked table, worked out once from CoreUObject's PDB.
    struct object_array
    {
        bool ok = false;
        ULONG64 objects = 0;
        ULONG64 num_elements = 0;
        ULONG item_size = 0;
        ULONG object_offset = 0;
        ULONG serial_offset = 0;
    };
    std::optional<object_array> object_array_;
    // An FText's history class, from its vtable: the module that describes
    // it and its type there.
    std::map<ULONG64, std::optional<std::pair<ULONG64, ULONG>>> text_histories_;
    ULONG64 module_with_type(ULONG64 module_base, const char *type, const char *probe_field);
};
} // namespace dap_dbgeng::debugger
