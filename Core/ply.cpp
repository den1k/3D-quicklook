// PLY loader: ASCII and binary (either byte order), vertex normals and colors, polygon faces.
#include "mesh.hpp"

#include <vector>

namespace ql3d {
namespace {

enum Type { I8, U8, I16, U16, I32, U32, F32, F64, Bad };

Type parseType(std::string_view s) {
    static const struct { const char *a, *b; Type t; } names[] = {
        {"char", "int8", I8}, {"uchar", "uint8", U8}, {"short", "int16", I16}, {"ushort", "uint16", U16},
        {"int", "int32", I32}, {"uint", "uint32", U32}, {"float", "float32", F32}, {"double", "float64", F64}};
    for (auto &n : names)
        if (s == n.a || s == n.b) return n.t;
    return Bad;
}

struct Prop { std::string name; Type type; bool list; Type countType; };
struct Element { std::string name; size_t count; std::vector<Prop> props; };

struct Reader {
    const char *p, *end;
    bool ascii, swap, ok = true;

    double scalar(Type t) {
        if (ascii) {
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
            // Integers are parsed exactly (vertex indices can exceed float precision).
            const char *q = p;
            bool neg = q < end && *q == '-';
            if (neg) q++;
            int64_t v = 0;
            const char *digits = q;
            while (q < end && *q >= '0' && *q <= '9') v = v * 10 + (*q++ - '0');
            if (q > digits && (q == end || *q == ' ' || *q == '\n' || *q == '\r' || *q == '\t')) {
                p = q;
                return (double)(neg ? -v : v);
            }
            float f;
            q = parseFloat(p, end, f);
            if (q == p) { ok = false; return 0; }
            p = q;
            return f;
        }
        static const int sizes[] = {1, 1, 2, 2, 4, 4, 4, 8};
        int n = sizes[t];
        if (end - p < n) { ok = false; return 0; }
        uint8_t b[8];
        for (int i = 0; i < n; i++) b[i] = (uint8_t)p[swap ? n - 1 - i : i];
        p += n;
        switch (t) {
            case I8: return (int8_t)b[0];
            case U8: return b[0];
            case I16: { int16_t v; memcpy(&v, b, 2); return v; }
            case U16: { uint16_t v; memcpy(&v, b, 2); return v; }
            case I32: { int32_t v; memcpy(&v, b, 4); return v; }
            case U32: { uint32_t v; memcpy(&v, b, 4); return v; }
            case F32: { float v; memcpy(&v, b, 4); return v; }
            default: { double v; memcpy(&v, b, 8); return v; }
        }
    }
};

// Which of our 9 vertex slots (x y z, nx ny nz, r g b) a property fills, or -1.
int slotFor(const std::string &n) {
    static const char *names[] = {"x", "y", "z", "nx", "ny", "nz", "red", "green", "blue"};
    for (int i = 0; i < 9; i++)
        if (n == names[i]) return i;
    if (n == "diffuse_red" || n == "r") return 6;
    if (n == "diffuse_green" || n == "g") return 7;
    if (n == "diffuse_blue" || n == "b") return 8;
    return -1;
}

uint32_t colorByte(double v, Type t) {
    if (t == F32 || t == F64) v *= 255;
    else if (t == U16) v /= 257;
    return v <= 0 ? 0 : v >= 255 ? 255 : (uint32_t)(v + 0.5);
}

}  // namespace

bool loadPLY(const char *path, Mesh &out, std::string &err) {
    MappedFile f;
    if (!f.open(path)) { err = "Could not open file"; return false; }
    const char *p = f.data, *end = f.data + f.size;

    // --- header
    std::vector<Element> elements;
    Reader r{nullptr, end, true, false};
    bool first = true, haveFormat = false, done = false;
    while (p < end && !done) {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        if (!nl) break;
        std::string_view line(p, (size_t)(nl - p));
        p = nl + 1;
        std::vector<std::string_view> w;
        for (size_t i = 0; i < line.size();) {
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) i++;
            size_t s = i;
            while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') i++;
            if (i > s) w.push_back(line.substr(s, i - s));
        }
        if (first) {
            if (w.empty() || w[0] != "ply") break;
            first = false;
        } else if (w.empty() || w[0] == "comment" || w[0] == "obj_info") {
        } else if (w[0] == "format" && w.size() >= 2) {
            haveFormat = true;
            r.ascii = w[1] == "ascii";
            r.swap = w[1] == "binary_big_endian";
            if (!r.ascii && !r.swap && w[1] != "binary_little_endian") haveFormat = false;
        } else if (w[0] == "element" && w.size() >= 3) {
            uint32_t n = 0;
            if (!parseUInt(w[2], n)) { haveFormat = false; break; }
            elements.push_back({std::string(w[1]), n, {}});
        } else if (w[0] == "property" && !elements.empty() && w.size() >= 3) {
            Prop pr{std::string(w.back()), Bad, false, Bad};
            if (w[1] == "list" && w.size() >= 5) { pr.list = true; pr.countType = parseType(w[2]); pr.type = parseType(w[3]); }
            else pr.type = parseType(w[1]);
            if (pr.type == Bad || (pr.list && pr.countType == Bad)) { haveFormat = false; break; }
            elements.back().props.push_back(pr);
        } else if (w[0] == "end_header") {
            done = true;
        }
    }
    if (!done || !haveFormat) { err = "Not a valid PLY file"; return false; }
    r.p = p;

    // --- body
    std::vector<V3> pos, nrm;
    std::vector<uint32_t> col;
    std::vector<uint32_t> poly;
    bool sawFaces = false;
    for (const Element &e : elements) {
        if (e.name == "vertex") {
            std::vector<int> slots;
            bool hasN = false, hasC = false;
            for (const Prop &pr : e.props) {
                int s = pr.list ? -1 : slotFor(pr.name);
                slots.push_back(s);
                hasN |= s >= 3 && s <= 5;
                hasC |= s >= 6;
            }
            if ((size_t)(end - r.p) < e.count) { err = "PLY file is truncated"; return false; }
            pos.resize(e.count);
            if (hasN) nrm.resize(e.count);
            if (hasC) col.resize(e.count);
            for (size_t i = 0; i < e.count && r.ok; i++) {
                double v[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
                for (size_t k = 0; k < e.props.size(); k++) {
                    const Prop &pr = e.props[k];
                    if (pr.list) {
                        for (double n = r.scalar(pr.countType); n > 0 && r.ok; n--) r.scalar(pr.type);
                    } else {
                        double x = r.scalar(pr.type);
                        if (slots[k] >= 6) x = colorByte(x, pr.type);
                        if (slots[k] >= 0) v[slots[k]] = x;
                    }
                }
                pos[i] = {(float)v[0], (float)v[1], (float)v[2]};
                if (hasN) nrm[i] = {(float)v[3], (float)v[4], (float)v[5]};
                if (hasC) col[i] = rgb((uint32_t)v[6], (uint32_t)v[7], (uint32_t)v[8]);
            }
        } else {
            bool isFace = e.name == "face";
            sawFaces |= isFace && e.count > 0;
            std::vector<int> slots;
            bool faceColor = false;
            for (const Prop &pr : e.props) {
                int s = pr.list ? -1 : slotFor(pr.name);
                slots.push_back(s >= 6 ? s : -1);
                faceColor |= isFace && s >= 6;
            }
            for (size_t i = 0; i < e.count && r.ok; i++) {
                double c[3] = {0, 0, 0};
                poly.clear();
                for (size_t k = 0; k < e.props.size(); k++) {
                    const Prop &pr = e.props[k];
                    if (pr.list) {
                        bool indices = isFace && (pr.name == "vertex_indices" || pr.name == "vertex_index");
                        for (double n = r.scalar(pr.countType); n > 0 && r.ok; n--) {
                            double x = r.scalar(pr.type);
                            if (indices) poly.push_back(x >= 0 && x < (double)pos.size() ? (uint32_t)x : UINT32_MAX);
                        }
                    } else {
                        double x = r.scalar(pr.type);
                        if (slots[k] >= 6) c[slots[k] - 6] = colorByte(x, pr.type);
                    }
                }
                if (!r.ok) break;
                uint32_t fc = faceColor ? rgb((uint32_t)c[0], (uint32_t)c[1], (uint32_t)c[2]) : kNoColor;
                for (size_t k = 1; k + 1 < poly.size(); k++) {
                    uint32_t a = poly[0], b = poly[k], d = poly[k + 1];
                    if (a == UINT32_MAX || b == UINT32_MAX || d == UINT32_MAX) continue;
                    uint32_t cs[3] = {fc, fc, fc};
                    if (!faceColor && !col.empty()) { cs[0] = col[a]; cs[1] = col[b]; cs[2] = col[d]; }
                    if (!nrm.empty()) {
                        out.tri(pos[a], pos[b], pos[d], nrm[a], nrm[b], nrm[d], cs);
                    } else {
                        V3 fn = faceNormal(pos[a], pos[b], pos[d]);
                        out.tri(pos[a], pos[b], pos[d], fn, fn, fn, cs);
                    }
                }
            }
        }
    }
    if (out.n == 0 && !pos.empty() && !sawFaces) { err = "This PLY file is a point cloud (no faces), which is not supported"; return false; }
    return true;
}

}  // namespace ql3d
