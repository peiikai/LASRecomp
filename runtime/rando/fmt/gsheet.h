// Data sheets (region_common/datasheets/*.gsheet): typed tables (Items, Npc, Conditions, GlobalFlags, ...). A header,
// the field definitions (each with its sub-fields), then one fixed-size record per row; strings, arrays and nullable
// values live after the records, reached by 64-bit offsets from the file start.
//
// Written for LAS Recomp from the format (no code copied): GSheet::write lays a sheet out exactly as oead 1.3's
// gsheet writer does (the library the Python generator writes sheets with), so a sheet edited here is the same file:
//   header (0x30), every field record (0x30 each: the root fields, then for each in turn its sub-fields, then theirs),
//   the row records (each aligned to the sheet's alignment), at 0x10 the sheet name and every field's name and type
//   name (no sharing), then each row's data (strings, nullables, arrays: in field order, nested data before the
//   records that point at it), aligned to 0x10. Field records' parent pointers are written as 0xDEADBEEFDEADBEEF.
// Values are kept as the game stores them: whole numbers and floats in 32 bits.
#pragma once
#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

namespace rando {

struct GField {
    enum Type : uint8_t { Struct = 0, Bool = 1, Int = 2, Float = 3, String = 4 };
    enum Flag : uint16_t { Nullable = 1, Array = 2, Key = 4, Enum = 16 };
    std::string name, type_name;
    uint8_t type = Struct, x11 = 0;
    uint16_t flags = 0, offset = 0, inline_size = 0, data_size = 0;
    std::vector<GField> fields;  // (a Struct's sub-fields)
    bool is(Flag f) const { return flags & f; }
};

struct GValue;
using GStruct = std::vector<std::pair<std::string, GValue>>;  // (the fields' names and values)

struct GValue {
    enum Kind : uint8_t { Null, Struct, Bool, Int, Float, String, StructArr, BoolArr, IntArr, FloatArr, StringArr };
    Kind kind = Null;
    bool b = false;
    int32_t i = 0;
    float f = 0;
    std::string s;
    std::vector<GStruct> structs;  // (Struct: one; StructArr: the items)
    std::vector<bool> bools;
    std::vector<int32_t> ints;
    std::vector<float> floats;
    std::vector<std::string> strs;

    static GValue of_bool(bool v) { GValue x; x.kind = Bool, x.b = v; return x; }
    static GValue of_int(int32_t v) { GValue x; x.kind = Int, x.i = v; return x; }
    static GValue of_float(float v) { GValue x; x.kind = Float, x.f = v; return x; }
    static GValue of_str(const std::string &v) { GValue x; x.kind = String, x.s = v; return x; }
    static GValue of_struct(const GStruct &v) { GValue x; x.kind = Struct, x.structs = {v}; return x; }
};

// a struct's field by name (nullptr: none)
GValue *gfield(GStruct &s, const std::string &name);
const GValue *gfield(const GStruct &s, const std::string &name);

struct GSheet {
    uint8_t alignment = 8;
    uint32_t hash = 0;
    std::string name;
    std::vector<GField> root_fields;
    std::vector<GStruct> values;  // (the rows)

    static GSheet parse(const std::string &data);  // throws on bad data
    std::string write() const;                     // throws on a value that doesn't fit its field
};

}  // namespace rando
