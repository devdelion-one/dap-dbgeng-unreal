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
};

// Reads Unreal Engine's core types straight from memory with the program's own
// type information (field offsets from its PDBs, never hard-coded layouts), the
// way the engine's Unreal.natvis describes them. Windows' own natvis support
// gets FName, object names and FGuid wrong, which is why this exists.
class unreal_reader
{
  public:
    unreal_reader(IDebugSymbols3 *symbols, IDebugDataSpaces4 *data, IDebugControl7 *control);

    // `value_text` is the engine's own text for the value; a pointer held in a
    // register (often `this`) has no address, and its value is read from there.
    std::optional<unreal_view> view(const std::string &type_name, ULONG64 module_base, ULONG type_id,
                                    ULONG64 address, const std::string &value_text);

    // "(R=0.2, G=0.8, B=0.2, A=1)" for a small struct of plain numbers, read
    // from memory with the type's own fields.
    std::optional<std::string> summary(ULONG64 module_base, ULONG type_id, ULONG64 address);
    // A plain number or bool read from memory, as the engine would print it.
    std::optional<std::string> plain_value(const std::string &type, ULONG64 address);
    // A typed expression for a value at an address: `*(Module!Type *)0x...`.
    std::string typed_expression(ULONG64 module_base, const std::string &type, ULONG64 address);
    std::string module_name(ULONG64 module_base);

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
    std::optional<std::int32_t> set_count(ULONG64 module_base, ULONG type_id, ULONG64 address);

    IDebugSymbols3 *symbols_;
    IDebugDataSpaces4 *data_;
    IDebugControl7 *control_;

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
    ULONG64 module_with_type(ULONG64 module_base, const char *type, const char *probe_field);
};
} // namespace dap_dbgeng::debugger
