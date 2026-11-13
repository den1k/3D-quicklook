// glTF 2.0 loader: .glb and .gltf (external or embedded buffers), node transforms,
// material base colors and vertex colors, meshopt-compressed buffers. Textures are not read.
#include "mesh.hpp"
#include "meshopt/meshoptimizer.h"

#include <memory>
#include <vector>

namespace ql3d {
namespace {

// ------------------------------------------------------------------- JSON

struct Json {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    double num = 0;
    std::string str;
    std::vector<Json> items;        // array elements, or object values
    std::vector<std::string> keys;  // object keys, parallel to items

    static const Json &null() { static const Json n; return n; }
    const Json &operator[](const char *k) const {
        for (size_t i = 0; i < keys.size(); i++)
            if (keys[i] == k) return items[i];
        return null();
    }
    const Json &at(size_t i) const { return kind == Arr && i < items.size() ? items[i] : null(); }
    // Looks up element `index` of this array, where `index` is a JSON number (or missing).
    const Json &at(const Json &index) const {
        return index.kind == Num && index.num >= 0 ? at((size_t)index.num) : null();
    }
    size_t size() const { return kind == Arr ? items.size() : 0; }
    double number(double fallback) const { return kind == Num ? num : fallback; }
    bool present() const { return kind != Null; }
};

struct JsonParser {
    const char *p, *end;
    bool ok = true;

    void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++; }
    bool lit(const char *s) {
        size_t n = strlen(s);
        if ((size_t)(end - p) < n || memcmp(p, s, n) != 0) { ok = false; return false; }
        p += n;
        return true;
    }
    std::string string() {
        std::string s;
        p++;  // opening quote
        while (p < end && *p != '"') {
            if (*p != '\\') { s += *p++; continue; }
            if (++p >= end) break;
            char c = *p++;
            switch (c) {
                case 'n': s += '\n'; break;
                case 't': s += '\t'; break;
                case 'r': s += '\r'; break;
                case 'b': s += '\b'; break;
                case 'f': s += '\f'; break;
                case 'u': {
                    unsigned v = 0;
                    for (int i = 0; i < 4 && p < end; i++, p++) {
                        char h = *p;
                        v = v * 16 + (h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : 0);
                    }
                    if (v < 0x80) s += (char)v;
                    else if (v < 0x800) { s += (char)(0xC0 | (v >> 6)); s += (char)(0x80 | (v & 0x3F)); }
                    else { s += (char)(0xE0 | (v >> 12)); s += (char)(0x80 | ((v >> 6) & 0x3F)); s += (char)(0x80 | (v & 0x3F)); }
                    break;
                }
                default: s += c;
            }
        }
        if (p >= end) ok = false; else p++;
        return s;
    }
    Json value(int depth = 0) {
        Json j;
        ws();
        if (p >= end || depth > 100) { ok = false; return j; }
        if (*p == '{' || *p == '[') {
            bool obj = *p == '{';
            char close = obj ? '}' : ']';
            j.kind = obj ? Json::Obj : Json::Arr;
            p++;
            ws();
            if (p < end && *p == close) { p++; return j; }
            while (ok) {
                if (obj) {
                    ws();
                    if (p >= end || *p != '"') { ok = false; break; }
                    j.keys.push_back(string());
                    ws();
                    if (p >= end || *p != ':') { ok = false; break; }
                    p++;
                }
                j.items.push_back(value(depth + 1));
                ws();
                if (p < end && *p == ',') { p++; continue; }
                if (p < end && *p == close) { p++; break; }
                ok = false;
            }
        } else if (*p == '"') {
            j.kind = Json::Str;
            j.str = string();
        } else if (*p == 't') {
            if (lit("true")) { j.kind = Json::Bool; j.num = 1; }
        } else if (*p == 'f') {
            if (lit("false")) j.kind = Json::Bool;
        } else if (*p == 'n') {
            lit("null");
        } else {
            char buf[64];
            size_t n = 0;
            while (p < end && n < sizeof(buf) - 1 && ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E'))
                buf[n++] = *p++;
            buf[n] = 0;
            if (n == 0) { ok = false; return j; }
            j.kind = Json::Num;
            j.num = strtod(buf, nullptr);
        }
        return j;
    }
};

// ---------------------------------------------------------------- helpers

std::string base64Decode(std::string_view s) {
    std::string out;
    out.reserve(s.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
              : c == '+' || c == '-' ? 62 : c == '/' || c == '_' ? 63 : -1;
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out += (char)((acc >> bits) & 0xFF); }
    }
    return out;
}

std::string percentDecode(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            out += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else out += s[i];
    }
    return out;
}

// Linear color component (0..1) to an sRGB byte.
uint32_t toSRGB(double v) {
    v = v <= 0 ? 0 : v >= 1 ? 1 : v;
    v = v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
    return (uint32_t)(v * 255 + 0.5);
}

// 4x4 matrices are column-major, as in glTF.
struct Mat { double m[16]; };
const Mat kIdentity = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
Mat mul(const Mat &a, const Mat &b) {
    Mat r;
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

struct View {
    const uint8_t *data = nullptr;
    size_t stride = 0, count = 0;
    int type = 0, comps = 0;
    bool normalized = false;
};

int componentSize(int type) {
    switch (type) {
        case 5120: case 5121: return 1;
        case 5122: case 5123: return 2;
        case 5125: case 5126: return 4;
    }
    return 0;
}

float component(const View &v, size_t i, int c) {
    const uint8_t *p = v.data + i * v.stride + (size_t)c * (size_t)componentSize(v.type);
    switch (v.type) {
        case 5120: { float x = (int8_t)*p; return v.normalized ? std::fmax(x / 127, -1.f) : x; }
        case 5121: { float x = *p; return v.normalized ? x / 255 : x; }
        case 5122: { int16_t s; memcpy(&s, p, 2); return v.normalized ? std::fmax(s / 32767.f, -1.f) : s; }
        case 5123: { uint16_t s; memcpy(&s, p, 2); return v.normalized ? s / 65535.f : s; }
        case 5125: { uint32_t s; memcpy(&s, p, 4); return (float)s; }
        default: { float f; memcpy(&f, p, 4); return f; }
    }
}

uint32_t indexAt(const View &v, size_t i) {
    const uint8_t *p = v.data + i * v.stride;
    switch (v.type) {
        case 5121: return *p;
        case 5123: { uint16_t s; memcpy(&s, p, 2); return s; }
        case 5125: { uint32_t s; memcpy(&s, p, 4); return s; }
    }
    return UINT32_MAX;
}

struct Loader {
    Json root;
    Mesh &out;
    std::vector<std::pair<const uint8_t *, size_t>> buffers;
    std::vector<std::unique_ptr<MappedFile>> files;
    std::vector<std::unique_ptr<std::string>> decoded;
    // Decompressed meshopt buffer views by bufferView index; an empty string marks a failed one.
    mutable std::vector<std::unique_ptr<std::string>> meshoptViews;

    const std::string &decompress(size_t viewIndex, const Json &ext) const {
        if (viewIndex >= meshoptViews.size()) meshoptViews.resize(viewIndex + 1);
        std::unique_ptr<std::string> &slot = meshoptViews[viewIndex];
        if (slot) return *slot;
        slot = std::make_unique<std::string>();
        double bi = ext["buffer"].number(-1), off = ext["byteOffset"].number(0), len = ext["byteLength"].number(0);
        size_t stride = (size_t)ext["byteStride"].number(0), count = (size_t)ext["count"].number(0);
        if (bi < 0 || bi >= (double)buffers.size() || stride == 0 || stride > 256 || count == 0 || count > (1u << 30)) return *slot;
        auto [data, size] = buffers[(size_t)bi];
        if (!data || off < 0 || len <= 0 || off + len > (double)size) return *slot;
        const unsigned char *src = data + (size_t)off;
        std::string raw(count * stride, '\0');
        const std::string &mode = ext["mode"].str, &filter = ext["filter"].str;
        int rc = -1;
        if (mode == "ATTRIBUTES" && stride % 4 == 0) rc = meshopt_decodeVertexBuffer(raw.data(), count, stride, src, (size_t)len);
        else if (mode == "TRIANGLES" && (stride == 2 || stride == 4) && count % 3 == 0) rc = meshopt_decodeIndexBuffer(raw.data(), count, stride, src, (size_t)len);
        else if (mode == "INDICES" && (stride == 2 || stride == 4)) rc = meshopt_decodeIndexSequence(raw.data(), count, stride, src, (size_t)len);
        if (rc != 0) return *slot;
        if (filter == "OCTAHEDRAL" && (stride == 4 || stride == 8)) meshopt_decodeFilterOct(raw.data(), count, stride);
        else if (filter == "QUATERNION" && stride == 8) meshopt_decodeFilterQuat(raw.data(), count, stride);
        else if (filter == "EXPONENTIAL" && stride % 4 == 0) meshopt_decodeFilterExp(raw.data(), count, stride);
        else if (filter == "COLOR" && (stride == 4 || stride == 8)) meshopt_decodeFilterColor(raw.data(), count, stride);
        else if (!filter.empty() && filter != "NONE") return *slot;
        *slot = std::move(raw);
        return *slot;
    }

    // Resolves an accessor (given by its JSON index) to raw data, checking bounds.
    bool view(const Json &index, View &v) const {
        const Json &a = root["accessors"].at(index);
        const Json &bv = root["bufferViews"].at(a["bufferView"]);
        if (!a.present() || !bv.present()) return false;
        const Json &exts = bv["extensions"];
        const Json &meshopt = exts["EXT_meshopt_compression"].present() ? exts["EXT_meshopt_compression"] : exts["KHR_meshopt_compression"];
        const uint8_t *data;
        size_t size;
        double viewOffset = 0, viewLength;
        v.stride = (size_t)(meshopt.present() ? meshopt : bv)["byteStride"].number(0);
        if (meshopt.present()) {
            const std::string &raw = decompress((size_t)a["bufferView"].num, meshopt);
            data = (const uint8_t *)raw.data();
            size = raw.size();
            viewLength = (double)size;
        } else {
            const Json &bi = bv["buffer"];
            if (bi.kind != Json::Num || bi.num < 0 || bi.num >= (double)buffers.size()) return false;
            data = buffers[(size_t)bi.num].first;
            size = buffers[(size_t)bi.num].second;
            viewOffset = bv["byteOffset"].number(0);
            viewLength = bv["byteLength"].number(0);
        }
        const std::string &t = a["type"].str;
        v.comps = t == "SCALAR" ? 1 : t == "VEC2" ? 2 : t == "VEC3" ? 3 : t == "VEC4" ? 4 : 0;
        v.type = (int)a["componentType"].number(0);
        v.count = (size_t)a["count"].number(0);
        v.normalized = a["normalized"].num != 0;
        size_t elem = (size_t)(componentSize(v.type) * v.comps);
        if (!data || size == 0 || elem == 0 || v.count == 0) return false;
        if (v.stride == 0) v.stride = elem;
        double offset = viewOffset + a["byteOffset"].number(0);
        double viewEnd = viewOffset + viewLength;
        double last = offset + (double)(v.count - 1) * (double)v.stride + (double)elem;
        if (offset < 0 || last > viewEnd || viewEnd > (double)size) return false;
        v.data = data + (size_t)offset;
        return true;
    }

    void primitive(const Json &prim, const Mat &M) {
        int mode = (int)prim["mode"].number(4);
        if (mode < 4 || mode > 6) return;  // points and lines are not drawn
        const Json &attrs = prim["attributes"];
        View pos, nrm, col, idx;
        if (!view(attrs["POSITION"], pos) || pos.comps != 3) return;
        bool hasN = view(attrs["NORMAL"], nrm) && nrm.comps == 3 && nrm.count >= pos.count;
        bool hasC = view(attrs["COLOR_0"], col) && col.comps >= 3 && col.count >= pos.count;
        bool hasI = prim["indices"].present();
        if (hasI && !view(prim["indices"], idx)) return;

        // Material: base color factor. A textured material without vertex colors gets the
        // default color, since the factor alone (usually white) says nothing about its look.
        const Json &mat = root["materials"].at(prim["material"]);
        const Json &pbr = mat["pbrMetallicRoughness"];
        double factor[3] = {1, 1, 1};
        for (size_t i = 0; i < 3 && pbr["baseColorFactor"].size() >= 3; i++) factor[i] = pbr["baseColorFactor"].at(i).number(1);
        uint32_t base = mat.present() && !pbr["baseColorTexture"].present()
            ? rgb(toSRGB(factor[0]), toSRGB(factor[1]), toSRGB(factor[2])) : kNoColor;

        const double *m = M.m;
        // Normals transform by the cofactor matrix of the upper 3x3 (inverse transpose up to scale).
        double cof[9] = {m[5] * m[10] - m[6] * m[9], m[6] * m[8] - m[4] * m[10], m[4] * m[9] - m[5] * m[8],
                         m[9] * m[2] - m[10] * m[1], m[10] * m[0] - m[8] * m[2], m[8] * m[1] - m[9] * m[0],
                         m[1] * m[6] - m[2] * m[5], m[2] * m[4] - m[0] * m[6], m[0] * m[5] - m[1] * m[4]};
        double det = m[0] * cof[0] + m[1] * cof[1] + m[2] * cof[2];
        bool mirrored = det < 0;

        size_t count = pos.count;
        std::vector<V3> P(count), N(hasN ? count : 0);
        std::vector<uint32_t> C(hasC ? count : 0);
        for (size_t i = 0; i < count; i++) {
            double x = component(pos, i, 0), y = component(pos, i, 1), z = component(pos, i, 2);
            P[i] = {(float)(m[0] * x + m[4] * y + m[8] * z + m[12]), (float)(m[1] * x + m[5] * y + m[9] * z + m[13]),
                    (float)(m[2] * x + m[6] * y + m[10] * z + m[14])};
            if (hasN) {
                double a = component(nrm, i, 0), b = component(nrm, i, 1), c = component(nrm, i, 2);
                double nx = cof[0] * a + cof[3] * b + cof[6] * c, ny = cof[1] * a + cof[4] * b + cof[7] * c,
                       nz = cof[2] * a + cof[5] * b + cof[8] * c;
                double l = std::sqrt(nx * nx + ny * ny + nz * nz) * (mirrored ? -1 : 1);
                if (l != 0) { nx /= l; ny /= l; nz /= l; }
                N[i] = {(float)nx, (float)ny, (float)nz};
            }
            if (hasC)
                C[i] = rgb(toSRGB(component(col, i, 0) * factor[0]), toSRGB(component(col, i, 1) * factor[1]),
                           toSRGB(component(col, i, 2) * factor[2]));
        }

        size_t n = hasI ? idx.count : count;
        auto at = [&](size_t k) { return hasI ? indexAt(idx, k) : (uint32_t)k; };
        auto emit = [&](uint32_t a, uint32_t b, uint32_t c) {
            if (a >= count || b >= count || c >= count) return;
            if (mirrored) std::swap(b, c);
            uint32_t cs[3] = {base, base, base};
            if (hasC) { cs[0] = C[a]; cs[1] = C[b]; cs[2] = C[c]; }
            if (hasN) {
                out.tri(P[a], P[b], P[c], N[a], N[b], N[c], cs);
            } else {
                V3 fn = faceNormal(P[a], P[b], P[c]);
                out.tri(P[a], P[b], P[c], fn, fn, fn, cs);
            }
        };
        if (mode == 4) {
            if (out.n + n > out.cap) out.reserve(std::max(out.n + n, out.cap * 2));
            for (size_t k = 0; k + 2 < n; k += 3) emit(at(k), at(k + 1), at(k + 2));
        } else if (mode == 5) {
            for (size_t k = 0; k + 2 < n; k++) (k & 1) ? emit(at(k + 1), at(k), at(k + 2)) : emit(at(k), at(k + 1), at(k + 2));
        } else {
            for (size_t k = 1; k + 1 < n; k++) emit(at(0), at(k), at(k + 1));
        }
    }

    void node(const Json &index, const Mat &parent, int depth) {
        const Json &n = root["nodes"].at(index);
        if (!n.present() || depth > 64) return;
        Mat local = kIdentity;
        if (n["matrix"].size() == 16) {
            for (size_t i = 0; i < 16; i++) local.m[i] = n["matrix"].at(i).number(0);
        } else {
            double t[3] = {0, 0, 0}, q[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1};
            for (size_t i = 0; i < 3 && n["translation"].size() == 3; i++) t[i] = n["translation"].at(i).number(0);
            for (size_t i = 0; i < 4 && n["rotation"].size() == 4; i++) q[i] = n["rotation"].at(i).number(0);
            for (size_t i = 0; i < 3 && n["scale"].size() == 3; i++) s[i] = n["scale"].at(i).number(1);
            double x = q[0], y = q[1], z = q[2], w = q[3];
            double r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
                           2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
                           2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)};
            for (int c = 0; c < 3; c++)
                for (int row = 0; row < 3; row++) local.m[c * 4 + row] = r[c * 3 + row] * s[c];
            local.m[12] = t[0]; local.m[13] = t[1]; local.m[14] = t[2];
        }
        Mat world = mul(parent, local);
        const Json &prims = root["meshes"].at(n["mesh"])["primitives"];
        for (size_t i = 0; i < prims.size(); i++) primitive(prims.at(i), world);
        for (size_t i = 0; i < n["children"].size(); i++) node(n["children"].at(i), world, depth + 1);
    }
};

}  // namespace

bool loadGLTF(const char *path, Mesh &out, std::string &err) {
    MappedFile f;
    if (!f.open(path)) { err = "Could not open file"; return false; }

    const char *json = f.data;
    size_t jsonLen = f.size;
    const uint8_t *bin = nullptr;
    size_t binLen = 0;
    if (f.size >= 12 && memcmp(f.data, "glTF", 4) == 0) {  // binary container (.glb)
        json = nullptr;
        for (size_t off = 12; off + 8 <= f.size;) {
            uint32_t len, type;
            memcpy(&len, f.data + off, 4);
            memcpy(&type, f.data + off + 4, 4);
            off += 8;
            if (len > f.size - off) break;
            if (type == 0x4E4F534A && !json) { json = f.data + off; jsonLen = len; }
            else if (type == 0x004E4942 && !bin) { bin = (const uint8_t *)f.data + off; binLen = len; }
            off += len;
        }
        if (!json) { err = "Not a valid GLB file"; return false; }
    }

    JsonParser parser{json, json + jsonLen};
    Loader L{parser.value(), out, {}, {}, {}, {}};
    if (!parser.ok || L.root.kind != Json::Obj) { err = "Not a valid glTF file"; return false; }

    const Json &required = L.root["extensionsRequired"];
    for (size_t i = 0; i < required.size(); i++) {
        const std::string &e = required.at(i).str;
        if (e == "KHR_draco_mesh_compression") {
            err = "This glTF file uses Draco mesh compression, which is not supported";
            return false;
        }
    }

    std::string dir(path);
    size_t slash = dir.rfind('/');
    dir = slash == std::string::npos ? "" : dir.substr(0, slash + 1);
    const Json &bufs = L.root["buffers"];
    for (size_t i = 0; i < bufs.size(); i++) {
        const Json &uri = bufs.at(i)["uri"];
        if (uri.kind != Json::Str) {
            L.buffers.push_back({bin, binLen});
        } else if (uri.str.compare(0, 5, "data:") == 0) {
            size_t comma = uri.str.find(',');
            L.decoded.push_back(std::make_unique<std::string>(
                base64Decode(comma == std::string::npos ? std::string_view() : std::string_view(uri.str).substr(comma + 1))));
            L.buffers.push_back({(const uint8_t *)L.decoded.back()->data(), L.decoded.back()->size()});
        } else {
            auto file = std::make_unique<MappedFile>();
            if (!file->open((dir + percentDecode(uri.str)).c_str())) { err = "Could not open " + uri.str; return false; }
            L.buffers.push_back({(const uint8_t *)file->data, file->size});
            L.files.push_back(std::move(file));
        }
    }

    out.zUp = false;  // glTF is Y-up
    const Json &scenes = L.root["scenes"];
    const Json &scene = scenes.at(L.root["scene"]).present() ? scenes.at(L.root["scene"]) : scenes.at(0);
    if (scene.present()) {
        for (size_t i = 0; i < scene["nodes"].size(); i++) L.node(scene["nodes"].at(i), kIdentity, 0);
    } else {  // no scene: draw every node that is not another node's child
        const Json &nodes = L.root["nodes"];
        std::vector<bool> child(nodes.size(), false);
        for (size_t i = 0; i < nodes.size(); i++)
            for (size_t k = 0; k < nodes.at(i)["children"].size(); k++) {
                double c = nodes.at(i)["children"].at(k).number(-1);
                if (c >= 0 && c < (double)child.size()) child[(size_t)c] = true;
            }
        for (size_t i = 0; i < nodes.size(); i++)
            if (!child[i]) {
                Json index;
                index.kind = Json::Num;
                index.num = (double)i;
                L.node(index, kIdentity, 0);
            }
    }
    return true;
}

}  // namespace ql3d
