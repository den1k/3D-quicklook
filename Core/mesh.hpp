// Shared internals for the loaders: output mesh, file mapping, fast number parsing.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace ql3d {

struct V3 { float x, y, z; };
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 mid(V3 a, V3 b) { return {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f}; }
inline V3 faceNormal(V3 a, V3 b, V3 c) {
    V3 u = b - a, v = c - a;
    V3 n = {u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
    float l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (l > 0) { n.x /= l; n.y /= l; n.z /= l; }
    return n;
}

// Colors are packed so the in-memory byte order is R,G,B,A. Alpha 0 means "no color".
inline uint32_t rgb(uint32_t r, uint32_t g, uint32_t b) { return r | (g << 8) | (b << 16) | 0xFF000000u; }
const uint32_t kNoColor = 0;
const uint32_t kDefaultColor = rgb(0xAE, 0xB6, 0xC2);

// Parses "#RRGGBB" or "#RRGGBBAA" (alpha ignored). Returns kNoColor if malformed.
uint32_t parseHexColor(std::string_view s);

// Writes one triangle into raw output arrays at vertex index `v`.
inline void writeTri(float *pos, float *nrm, uint8_t *col, size_t v,
                     V3 a, V3 b, V3 c, V3 na, V3 nb, V3 nc, const uint32_t rgba[3]) {
    float *p = pos + v * 3, *n = nrm + v * 3;
    p[0] = a.x; p[1] = a.y; p[2] = a.z; p[3] = b.x; p[4] = b.y; p[5] = b.z; p[6] = c.x; p[7] = c.y; p[8] = c.z;
    n[0] = na.x; n[1] = na.y; n[2] = na.z; n[3] = nb.x; n[4] = nb.y; n[5] = nb.z; n[6] = nc.x; n[7] = nc.y; n[8] = nc.z;
    uint32_t out[3];
    for (int i = 0; i < 3; i++) out[i] = rgba[i] ? rgba[i] : kDefaultColor;
    memcpy(col + v * 4, out, 12);
}

struct Mesh {
    float *pos = nullptr, *nrm = nullptr;
    uint8_t *col = nullptr;
    size_t n = 0, cap = 0;  // in vertices
    bool zUp = true;

    void reserve(size_t v) {
        if (v <= cap) return;
        cap = v;
        pos = (float *)realloc(pos, cap * 12);
        nrm = (float *)realloc(nrm, cap * 12);
        col = (uint8_t *)realloc(col, cap * 4);
    }
    void resize(size_t v) { reserve(v); n = v; }
    void grow() { if (n + 3 > cap) reserve(cap ? cap * 2 : 3 * 4096); }

    // Flat-shaded triangle.
    void tri(V3 a, V3 b, V3 c, uint32_t color) {
        uint32_t cs[3] = {color, color, color};
        V3 fn = faceNormal(a, b, c);
        grow();
        writeTri(pos, nrm, col, n, a, b, c, fn, fn, fn, cs);
        n += 3;
    }
    // Triangle with explicit normals and per-vertex colors.
    void tri(V3 a, V3 b, V3 c, V3 na, V3 nb, V3 nc, const uint32_t cs[3]) {
        grow();
        writeTri(pos, nrm, col, n, a, b, c, na, nb, nc, cs);
        n += 3;
    }
};

struct MappedFile {
    const char *data = nullptr;
    size_t size = 0;
    bool open(const char *path);
    ~MappedFile();
};

// Minimal fast decimal parser (enough precision for display). Skips leading
// whitespace; returns the position after the number, or `p` unchanged if none.
inline const char *parseFloat(const char *p, const char *end, float &out) {
    const char *s = p;
    while (s < end && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) s++;
    bool neg = false;
    if (s < end && (*s == '-' || *s == '+')) { neg = *s == '-'; s++; }
    uint64_t mant = 0;
    int digits = 0, exp10 = 0;
    const char *start = s;
    while (s < end && *s >= '0' && *s <= '9') {
        if (digits < 18) { mant = mant * 10 + (*s - '0'); if (mant) digits++; } else exp10++;
        s++;
    }
    if (s < end && *s == '.') {
        s++;
        while (s < end && *s >= '0' && *s <= '9') {
            if (digits < 18) { mant = mant * 10 + (*s - '0'); if (mant) digits++; exp10--; }
            s++;
        }
    }
    if (s == start || (s == start + 1 && *start == '.')) return p;
    if (s < end && (*s == 'e' || *s == 'E')) {
        const char *e = s + 1;
        bool eneg = false;
        if (e < end && (*e == '-' || *e == '+')) { eneg = *e == '-'; e++; }
        if (e < end && *e >= '0' && *e <= '9') {
            int x = 0;
            while (e < end && *e >= '0' && *e <= '9') { if (x < 10000) x = x * 10 + (*e - '0'); e++; }
            exp10 += eneg ? -x : x;
            s = e;
        }
    }
    double v = (double)mant;
    if (exp10) v *= std::pow(10.0, exp10);
    out = (float)(neg ? -v : v);
    return s;
}

inline bool parseUInt(std::string_view s, uint32_t &out) {
    if (s.empty()) return false;
    uint32_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (uint32_t)(c - '0');
    }
    out = v;
    return true;
}

bool loadSTL(const char *path, Mesh &out, std::string &err);
bool loadOBJ(const char *path, Mesh &out, std::string &err);
bool load3MF(const char *path, Mesh &out, std::string &err);
bool loadSTEP(const char *path, Mesh &out, std::string &err);

}  // namespace ql3d
