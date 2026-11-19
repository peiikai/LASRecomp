// Data sheets (gsheet.h). Written for LAS Recomp from the format; the writer's layout follows oead 1.3's (see the
// header) so files come out byte for byte the same.
#include "gsheet.h"

#include <string.h>

#include <stdexcept>
#include <unordered_map>

namespace rando {

GValue *gfield(GStruct &s, const std::string &name) {
    for (auto &kv : s)
        if (kv.first == name) return &kv.second;
    return nullptr;
}
const GValue *gfield(const GStruct &s, const std::string &name) {
    for (const auto &kv : s)
        if (kv.first == name) return &kv.second;
    return nullptr;
}

// ---- reading
namespace {
struct Reader {
    const std::string &d;
    uint64_t le(size_t at, int n) const {
        if (at + n > d.size()) throw std::out_of_range("gsheet: read past the end");
        uint64_t v = 0;
        for (int i = 0; i < n; i++) v |= (uint64_t)(uint8_t)d[at + i] << (8 * i);
        return v;
    }
    std::string cstr(size_t at) const {
        if (at >= d.size()) throw std::out_of_range("gsheet: string past the end");
        size_t e = d.find('\0', at);
        if (e == std::string::npos) throw std::out_of_range("gsheet: unterminated string");
        return d.substr(at, e - at);
    }
    float f32(size_t at) const {
        uint32_t u = (uint32_t)le(at, 4);
        float f;
        memcpy(&f, &u, 4);
        return f;
    }

    GField field(size_t at, int depth) const {
        if (depth > 32) throw std::runtime_error("gsheet: fields nested too deep");
        GField f;
        uint64_t name = le(at, 8), type_name = le(at + 8, 8);
        if (!name || !type_name) throw std::runtime_error("gsheet: field without a name or type name");
        f.name = cstr((size_t)name), f.type_name = cstr((size_t)type_name);
        f.type = (uint8_t)le(at + 0x10, 1), f.x11 = (uint8_t)le(at + 0x11, 1);
        f.flags = (uint16_t)le(at + 0x12, 2), f.offset = (uint16_t)le(at + 0x14, 2);
        f.inline_size = (uint16_t)le(at + 0x16, 2), f.data_size = (uint16_t)le(at + 0x18, 2);
        uint16_t n = (uint16_t)le(at + 0x1A, 2);
        uint64_t sub = le(at + 0x20, 8);
        if (n && !sub) throw std::runtime_error("gsheet: missing sub-fields");
        for (uint16_t i = 0; i < n; i++) f.fields.push_back(field((size_t)sub + 0x30 * i, depth + 1));
        return f;
    }

    GStruct record(size_t at, const std::vector<GField> &fields) const {
        GStruct s;
        for (const GField &sf : fields) s.emplace_back(sf.name, value(at + sf.offset, sf, false, false));
        return s;
    }

    GValue value(size_t at, const GField &f, bool no_array, bool no_null) const {
        GValue v;
        if (f.is(GField::Array) && !no_array) {
            uint64_t p = le(at, 8);
            uint32_t n = (uint32_t)le(at + 8, 4);
            static const GValue::Kind arr[] = {GValue::StructArr, GValue::BoolArr, GValue::IntArr, GValue::FloatArr,
                                               GValue::StringArr};
            if (f.type > GField::String) throw std::runtime_error("gsheet: unexpected field type");
            v.kind = arr[f.type];
            for (uint32_t k = 0; k < n; k++) {
                GValue it = value((size_t)p + (size_t)k * f.data_size, f, true, no_null);
                switch (f.type) {
                case GField::Struct: v.structs.push_back(it.structs.at(0)); break;
                case GField::Bool: v.bools.push_back(it.b); break;
                case GField::Int: v.ints.push_back(it.i); break;
                case GField::Float: v.floats.push_back(it.f); break;
                default: v.strs.push_back(it.s); break;
                }
            }
        } else if (f.type == GField::String) {
            uint64_t p = le(at, 8);
            v.kind = GValue::String;
            if (p) v.s = cstr((size_t)p);
        } else if (f.is(GField::Nullable) && !no_null) {
            uint64_t p = le(at, 8);
            if (p) v = value((size_t)p, f, no_array, true);
        } else if (f.type == GField::Struct) {
            v.kind = GValue::Struct;
            v.structs.push_back(record(at, f.fields));
        } else if (f.type == GField::Bool) {
            v.kind = GValue::Bool, v.b = le(at, 1) != 0;
        } else if (f.type == GField::Int) {
            v.kind = GValue::Int, v.i = (int32_t)(uint32_t)le(at, 4);
        } else if (f.type == GField::Float) {
            v.kind = GValue::Float, v.f = f32(at);
        } else {
            throw std::runtime_error("gsheet: unexpected field type");
        }
        return v;
    }
};
}  // namespace

GSheet GSheet::parse(const std::string &data) {
    Reader r{data};
    if (data.size() < 0x30 || data.compare(0, 4, "gsht")) throw std::runtime_error("gsheet: bad magic");
    if (r.le(4, 4) != 1) throw std::runtime_error("gsheet: version isn't 1");
    if (r.le(0xC, 1) != 1 || r.le(0xD, 1) != 8) throw std::runtime_error("gsheet: bool / pointer size");
    GSheet s;
    s.hash = (uint32_t)r.le(8, 4);
    s.alignment = (uint8_t)r.le(0xE, 1);
    uint64_t name = r.le(0x10, 8);
    if (!name) throw std::runtime_error("gsheet: missing name");
    s.name = r.cstr((size_t)name);
    uint32_t nroot = (uint32_t)r.le(0x18, 4), nvalues = (uint32_t)r.le(0x28, 4), vsize = (uint32_t)r.le(0x2C, 4);
    uint64_t values = r.le(0x20, 8);
    for (uint32_t i = 0; i < nroot; i++) s.root_fields.push_back(r.field(0x30 + 0x30 * i, 0));
    for (uint32_t i = 0; i < nvalues; i++) s.values.push_back(r.record((size_t)values + (size_t)i * vsize, s.root_fields));
    return s;
}

// ---- writing
namespace {
struct Writer {
    std::string buf;
    size_t pos = 0;
    struct Obj { uint32_t ptr = 0, obj = 0; bool has_ptr = false, has_obj = false; };
    std::unordered_map<const void *, Obj> objs;
    std::vector<const void *> obj_order;  // (fix-up order doesn't change the bytes; kept for determinism)
    std::vector<std::pair<uint32_t, const std::string *>> field_strings;

    void bytes(const void *p, size_t n) {
        if (pos + n > buf.size()) buf.resize(pos + n, '\0');
        memcpy(&buf[pos], p, n);
        pos += n;
    }
    void le(uint64_t v, int n) {
        char b[8];
        for (int i = 0; i < n; i++) b[i] = (char)(uint8_t)(v >> (8 * i));
        bytes(b, n);
    }
    void f32(float f) {
        uint32_t u;
        memcpy(&u, &f, 4);
        le(u, 4);
    }
    void cstr(const std::string &s) { bytes(s.data(), s.size()), le(0, 1); }
    void align(size_t a) { pos = (pos + a - 1) / a * a; }

    Obj &entry(const void *key) {
        auto it = objs.find(key);
        if (it == objs.end()) {
            obj_order.push_back(key);
            it = objs.emplace(key, Obj{}).first;
        }
        return it->second;
    }
    void reg(const void *key) {
        Obj &o = entry(key);
        if (o.has_obj) throw std::logic_error("gsheet: object written twice");
        o.obj = (uint32_t)pos, o.has_obj = true;
    }
    void reg_ptr(const void *key) {
        Obj &o = entry(key);
        if (o.has_ptr) throw std::logic_error("gsheet: pointer written twice");
        o.ptr = (uint32_t)pos, o.has_ptr = true;
        le(~0ull, 8);
    }
    template <typename F> void at(size_t p, F fn) {
        size_t keep = pos;
        pos = p;
        fn();
        pos = keep;
    }

    static size_t count_fields(const std::vector<GField> &fs) {
        size_t n = 0;
        for (const GField &f : fs) n += 1 + count_fields(f.fields);
        return n;
    }

    void field(const GField &f) {
        size_t off = pos;
        le(0, 8), le(0, 8);  // (name, type name: below)
        le(f.type, 1), le(f.x11, 1), le(f.flags, 2), le(f.offset, 2), le(f.inline_size, 2), le(f.data_size, 2);
        le(f.fields.size(), 2), le(0, 4), le(0, 8);
        le(0xdeadbeefdeadbeefull, 8);  // (parent: as oead writes it)
        field_strings.emplace_back((uint32_t)off, &f.name);
        field_strings.emplace_back((uint32_t)(off + 8), &f.type_name);
        if (!f.fields.empty()) at(off + 0x20, [&] { reg_ptr(&f.fields); });
    }
    void fields(const std::vector<GField> &fs, bool root) {
        if (!root && !fs.empty()) reg(&fs);
        for (const GField &f : fs) field(f);
        for (const GField &f : fs) fields(f.fields, false);
    }

    static const GValue &get(const GStruct &s, const GField &f) {
        const GValue *v = gfield(s, f.name);
        if (!v) throw std::invalid_argument("gsheet: row without field " + f.name);
        return *v;
    }
    static void need(const GValue &v, GValue::Kind k, const GField &f) {
        if (v.kind != k) throw std::invalid_argument("gsheet: mismatched data for field " + f.name);
    }

    void string_ptr(const std::string &s, const GField &f) {
        if (!s.empty() || !f.is(GField::Nullable)) reg_ptr(&s);
        else le(0, 8);
        le(s.size(), 4), le(0, 4);
    }

    void inline_value(const GValue &v, const GField &f) {
        switch (v.kind) {
        case GValue::Struct: need(v, f.type == GField::Struct ? v.kind : GValue::Null, f); record(v.structs.at(0), f.fields); break;
        case GValue::Bool: need(v, f.type == GField::Bool ? v.kind : GValue::Null, f); le(v.b, 1); break;
        case GValue::Int: need(v, f.type == GField::Int ? v.kind : GValue::Null, f); le((uint32_t)v.i, 4); break;
        case GValue::Float: need(v, f.type == GField::Float ? v.kind : GValue::Null, f); f32(v.f); break;
        case GValue::String: need(v, f.type == GField::String ? v.kind : GValue::Null, f); cstr(v.s); break;
        default: throw std::logic_error("gsheet: unexpected value for field " + f.name);
        }
    }

    size_t array_size(const GValue &v, const GField &f) {
        switch (v.kind) {
        case GValue::StructArr: return v.structs.size();
        case GValue::BoolArr: return v.bools.size();
        case GValue::IntArr: return v.ints.size();
        case GValue::FloatArr: return v.floats.size();
        case GValue::StringArr: return v.strs.size();
        default: throw std::logic_error("gsheet: not an array for field " + f.name);
        }
    }

    void record(const GStruct &s, const std::vector<GField> &fs) {
        size_t base = pos;
        for (const GField &f : fs) {
            pos = base + f.offset;
            const GValue &v = get(s, f);
            if (f.is(GField::Array)) {
                reg_ptr(&v);
                le(array_size(v, f), 4), le(0, 4);
            } else if (f.type == GField::String) {
                need(v, GValue::String, f);
                string_ptr(v.s, f);
            } else if (f.is(GField::Nullable)) {
                if (v.kind == GValue::Null) le(0, 8);
                else reg_ptr(&v);
            } else {
                inline_value(v, f);
            }
        }
    }

    void data(const GStruct &s, const std::vector<GField> &fs) {
        for (const GField &f : fs) {
            const GValue &v = get(s, f);
            if (f.is(GField::Array)) {
                if (f.type == GField::Struct) {
                    need(v, GValue::StructArr, f);
                    for (const GStruct &it : v.structs) data(it, f.fields);
                    align(8);
                    reg(&v);
                    for (const GStruct &it : v.structs) record(it, f.fields), align(8);
                } else if (f.type == GField::String) {
                    need(v, GValue::StringArr, f);
                    for (const std::string &it : v.strs) reg(&it), cstr(it);
                    align(8);
                    reg(&v);
                    for (const std::string &it : v.strs) string_ptr(it, f);
                } else {
                    if (f.type != GField::Bool) align(4);
                    reg(&v);
                    if (v.kind == GValue::BoolArr) for (bool b : v.bools) le(b, 1), align(4);
                    else if (v.kind == GValue::IntArr) for (int32_t i : v.ints) le((uint32_t)i, 4), align(4);
                    else if (v.kind == GValue::FloatArr) for (float x : v.floats) f32(x), align(4);
                    else throw std::logic_error("gsheet: not an array for field " + f.name);
                }
            } else if (f.type == GField::String) {
                if (!v.s.empty() || !f.is(GField::Nullable)) reg(&v.s), cstr(v.s);
            } else if (f.is(GField::Nullable)) {
                if (v.kind == GValue::Null) continue;
                if (f.type == GField::Struct) {
                    need(v, GValue::Struct, f);
                    data(v.structs.at(0), f.fields);
                    align(8);
                    reg(&v);
                    record(v.structs.at(0), f.fields);
                } else {
                    reg(&v);
                    inline_value(v, f);
                }
            } else if (f.type == GField::Struct) {
                need(v, GValue::Struct, f);
                data(v.structs.at(0), f.fields);
            }
        }
    }

    std::string write(const GSheet &s) {
        le(0x74687367, 4);  // "gsht"
        le(1, 4), le(s.hash, 4), le(1, 1), le(8, 1), le(s.alignment, 1), le(0, 1);
        le(0, 8);  // (name: below)
        le(s.root_fields.size(), 4), le(count_fields(s.root_fields), 4);
        le(0, 8);  // (values)
        le(s.values.size(), 4);
        uint32_t vsize = 0;
        if (!s.root_fields.empty()) {
            const GField &last = s.root_fields.back();
            vsize = (last.offset + last.inline_size + s.alignment - 1) / s.alignment * s.alignment;
        }
        le(vsize, 4);
        field_strings.emplace_back(0x10, &s.name);
        at(0x20, [&] { reg_ptr(&s.values); });
        fields(s.root_fields, true);
        reg(&s.values);
        for (const GStruct &v : s.values) record(v, s.root_fields), align(s.alignment);
        align(0x10);
        for (const auto &fs : field_strings) {
            uint64_t here = pos;
            at(fs.first, [&] { le(here, 8); });
            cstr(*fs.second);
        }
        for (const GStruct &v : s.values) data(v, s.root_fields);
        align(0x10);
        if (buf.size() < pos) buf.resize(pos, '\0');
        for (const void *k : obj_order) {
            const Obj &o = objs[k];
            if (!o.has_ptr || !o.has_obj) throw std::logic_error("gsheet: object without its pointer, or the reverse");
            at(o.ptr, [&] { le(o.obj, 8); });
        }
        return buf;
    }
};
}  // namespace

std::string GSheet::write() const { return Writer{}.write(*this); }

}  // namespace rando
