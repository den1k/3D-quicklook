// 3MF loader.
//
// Handles the core spec (objects, components, build items, base materials), the
// materials extension (color groups), the production extension (objects split
// across several .model files), and the slicer conventions used for multicolor
// prints by Bambu Studio / OrcaSlicer / PrusaSlicer:
//   - per-object and per-part filament assignment (Metadata/*.config)
//   - painted triangles ("paint_color" / "slic3rpe:mmu_segmentation")
//
// Model XML is scanned with a purpose-built tokenizer and objects are parsed in
// parallel, since these files routinely hold hundreds of MB of XML.
#include "mesh.hpp"
#include "zip.hpp"

#include <dispatch/dispatch.h>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

namespace ql3d {
namespace {

using sv = std::string_view;

// ---------------------------------------------------------------- transforms

// 3MF affine transform: row-major 4x3, applied to row vectors.
struct Xf {
    float m[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
};

inline V3 apply(const Xf &t, V3 p) {
    return {p.x * t.m[0] + p.y * t.m[3] + p.z * t.m[6] + t.m[9],
            p.x * t.m[1] + p.y * t.m[4] + p.z * t.m[7] + t.m[10],
            p.x * t.m[2] + p.y * t.m[5] + p.z * t.m[8] + t.m[11]};
}

// Result applies `a` first, then `b`.
Xf mul(const Xf &a, const Xf &b) {
    Xf r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 3; j++)
            r.m[i * 3 + j] = a.m[i * 3] * b.m[j] + a.m[i * 3 + 1] * b.m[3 + j] + a.m[i * 3 + 2] * b.m[6 + j] +
                             (i == 3 ? b.m[9 + j] : 0);
    return r;
}

float det(const Xf &t) {
    const float *m = t.m;
    return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
}

Xf parseXf(sv s) {
    Xf t;
    const char *p = s.data(), *e = p + s.size();
    for (int i = 0; i < 12; i++) {
        const char *q = parseFloat(p, e, t.m[i]);
        if (q == p) return Xf();
        p = q;
    }
    return t;
}

// ------------------------------------------------------------- XML tokenizer

struct Attr { sv name, value; };
struct Tag {
    sv name;  // namespace prefix stripped
    bool closing;
    int nattrs;
    Attr attrs[16];  // names have their namespace prefix stripped
    sv get(sv key) const {
        for (int i = 0; i < nattrs; i++)
            if (attrs[i].name == key) return attrs[i].value;
        return {};
    }
};

inline sv localName(const char *b, const char *e) {
    const char *c = (const char *)memchr(b, ':', (size_t)(e - b));
    return c ? sv(c + 1, (size_t)(e - c - 1)) : sv(b, (size_t)(e - b));
}

inline bool isSpace(char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; }

// Reads the next element tag at or after `p`. Returns the position after it, or nullptr at the end.
const char *nextTag(const char *p, const char *end, Tag &t) {
    for (;;) {
        p = (const char *)memchr(p, '<', (size_t)(end - p));
        if (!p || ++p >= end) return nullptr;
        if (*p == '?' || *p == '!') {
            bool comment = end - p >= 3 && p[1] == '-' && p[2] == '-';
            const char *close = comment ? (const char *)memmem(p, (size_t)(end - p), "-->", 3)
                                        : (const char *)memchr(p, '>', (size_t)(end - p));
            if (!close) return nullptr;
            p = close + 1;
            continue;
        }
        t.closing = *p == '/';
        if (t.closing) p++;
        const char *nb = p;
        while (p < end && !isSpace(*p) && *p != '>' && *p != '/') p++;
        t.name = localName(nb, p);
        t.nattrs = 0;
        for (;;) {
            while (p < end && (isSpace(*p) || *p == '/')) p++;
            if (p >= end) return nullptr;
            if (*p == '>') return p + 1;
            const char *ab = p;
            while (p < end && *p != '=' && *p != '>' && !isSpace(*p)) p++;
            const char *ae = p;
            while (p < end && *p != '"' && *p != '\'' && *p != '>') p++;
            if (p >= end) return nullptr;
            if (*p == '>') return p + 1;
            char quote = *p++;
            const char *ve = (const char *)memchr(p, quote, (size_t)(end - p));
            if (!ve) return nullptr;
            if (t.nattrs < 16) t.attrs[t.nattrs++] = {localName(ab, ae), sv(p, (size_t)(ve - p))};
            p = ve + 1;
        }
    }
}

int toInt(sv s, int fallback = -1) {
    uint32_t v;
    return parseUInt(s, v) ? (int)v : fallback;
}

// ------------------------------------------------------- slicer configuration

struct BambuPart { int extruder = 0; bool normal = true; };
struct BambuObj { int extruder = 0; std::unordered_map<int, BambuPart> parts; };
struct PrusaVol { uint32_t first = 0, last = 0; int extruder = 0; bool model = true; };
struct PrusaObj { int extruder = 0; std::vector<PrusaVol> vols; };

struct Config {
    std::vector<uint32_t> filaments;
    std::unordered_map<int, BambuObj> bambu;
    std::unordered_map<int, PrusaObj> prusa;

    // Color of 1-based filament `n`; kNoColor for 0.
    uint32_t filament(int n) const {
        static const uint32_t palette[8] = {rgb(0xE8, 0xE8, 0xE8), rgb(0xE6, 0x4B, 0x35), rgb(0x3C, 0x8D, 0xDE),
                                            rgb(0x4C, 0xB0, 0x50), rgb(0xF2, 0xC0, 0x37), rgb(0x9B, 0x59, 0xB6),
                                            rgb(0xF3, 0x8B, 0x2E), rgb(0x30, 0x30, 0x30)};
        if (n <= 0) return kNoColor;
        if ((size_t)n <= filaments.size() && filaments[(size_t)n - 1]) return filaments[(size_t)n - 1];
        return palette[(n - 1) % 8];
    }
};

// Bambu Studio / OrcaSlicer: Metadata/model_settings.config
void parseBambuModelSettings(const Buffer &b, Config &cfg) {
    const char *p = b.data, *e = b.data + b.size;
    Tag t;
    BambuObj *obj = nullptr;
    BambuPart *part = nullptr;
    while ((p = nextTag(p, e, t))) {
        if (t.closing) {
            if (t.name == "part") part = nullptr;
            else if (t.name == "object") obj = nullptr;
        } else if (t.name == "object") {
            obj = &cfg.bambu[toInt(t.get("id"))];
            part = nullptr;
        } else if (t.name == "part" && obj) {
            part = &obj->parts[toInt(t.get("id"))];
            sv sub = t.get("subtype");
            part->normal = sub.empty() || sub == "normal_part";
        } else if (t.name == "metadata" && obj && t.get("key") == "extruder") {
            int x = toInt(t.get("value"), 0);
            if (part) part->extruder = x; else obj->extruder = x;
        }
    }
}

// PrusaSlicer: Metadata/Slic3r_PE_model.config
void parsePrusaModelConfig(const Buffer &b, Config &cfg) {
    const char *p = b.data, *e = b.data + b.size;
    Tag t;
    PrusaObj *obj = nullptr;
    PrusaVol *vol = nullptr;
    while ((p = nextTag(p, e, t))) {
        if (t.closing) {
            if (t.name == "volume") vol = nullptr;
            else if (t.name == "object") obj = nullptr;
        } else if (t.name == "object") {
            obj = &cfg.prusa[toInt(t.get("id"))];
            vol = nullptr;
        } else if (t.name == "volume" && obj) {
            obj->vols.emplace_back();
            vol = &obj->vols.back();
            vol->first = (uint32_t)toInt(t.get("firstid"), 0);
            vol->last = (uint32_t)toInt(t.get("lastid"), 0);
        } else if (t.name == "metadata" && obj) {
            sv key = t.get("key"), value = t.get("value");
            if (key == "extruder") {
                int x = toInt(value, 0);
                if (vol) vol->extruder = x; else obj->extruder = x;
            } else if (vol && key == "volume_type") {
                vol->model = value == "ModelPart";
            } else if (vol && key == "modifier") {
                if (value == "1") vol->model = false;
            }
        }
    }
}

// Bambu/Orca: "filament_colour": ["#RRGGBB", ...] in Metadata/project_settings.config (JSON)
void parseBambuFilaments(const Buffer &b, Config &cfg) {
    const char *k = (const char *)memmem(b.data, b.size, "\"filament_colour\"", 17);
    if (!k) return;
    const char *e = b.data + b.size;
    const char *open = (const char *)memchr(k, '[', (size_t)(e - k));
    if (!open) return;
    const char *close = (const char *)memchr(open, ']', (size_t)(e - open));
    if (!close) return;
    for (const char *p = open; p < close;) {
        const char *q1 = (const char *)memchr(p, '"', (size_t)(close - p));
        if (!q1) break;
        const char *q2 = (const char *)memchr(q1 + 1, '"', (size_t)(close - q1 - 1));
        if (!q2) break;
        cfg.filaments.push_back(parseHexColor(sv(q1 + 1, (size_t)(q2 - q1 - 1))));
        p = q2 + 1;
    }
}

// PrusaSlicer: "; extruder_colour = #..;#.." and "; filament_colour = ..." in Metadata/Slic3r_PE.config
void parsePrusaFilaments(const Buffer &b, Config &cfg) {
    auto list = [&](const char *key) {
        std::vector<uint32_t> out;
        const char *k = (const char *)memmem(b.data, b.size, key, strlen(key));
        if (!k) return out;
        const char *p = k + strlen(key), *e = b.data + b.size;
        const char *nl = (const char *)memchr(p, '\n', (size_t)(e - p));
        if (nl) e = nl;
        while (p < e) {
            const char *semi = (const char *)memchr(p, ';', (size_t)(e - p));
            const char *ve = semi ? semi : e;
            while (p < ve && (isSpace(*p) || *p == '"')) p++;
            out.push_back(parseHexColor(sv(p, (size_t)(ve - p))));
            p = ve + 1;
        }
        return out;
    };
    std::vector<uint32_t> ext = list("; extruder_colour = "), fil = list("; filament_colour = ");
    size_t n = ext.size() > fil.size() ? ext.size() : fil.size();
    for (size_t i = 0; i < n; i++) {
        uint32_t c = i < ext.size() ? ext[i] : kNoColor;
        if (!c && i < fil.size()) c = fil[i];
        cfg.filaments.push_back(c);
    }
}

// ------------------------------------------------------------ painted triangles

// Slicer paint data: a hex string, read back to front, one nibble per tree node.
// Low 2 bits = number of split edges (0 = leaf). For a split node the high 2 bits
// name the "special" vertex; for a leaf they hold the state (3 = extended: the
// next nibble + 3). Children are stored in reverse order of their creation.
struct PaintReader {
    const char *s;
    long i;
    int nibble() {
        if (i < 0) return 0;
        char c = s[i--];
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return 0;
    }
};

template <class Emit>
void decodePaint(PaintReader &r, V3 a, V3 b, V3 c, Emit &emit, int depth) {
    int code = r.nibble();
    int sides = code & 3;
    if (sides == 0 || depth > 24) {
        int state = code >> 2;
        if (state == 3) {
            int n = r.nibble();
            state += n;
            while (n == 15) { n = r.nibble(); state += n; }
        }
        emit(a, b, c, state);
        return;
    }
    V3 v[3] = {a, b, c};
    int s = (code >> 2) % 3;
    V3 v0 = v[s], v1 = v[(s + 1) % 3], v2 = v[(s + 2) % 3];
    V3 t[4][3];
    int n;
    if (sides == 1) {
        V3 m12 = mid(v1, v2);
        t[0][0] = v0; t[0][1] = v1; t[0][2] = m12;
        t[1][0] = m12; t[1][1] = v2; t[1][2] = v0;
        n = 2;
    } else if (sides == 2) {
        V3 m01 = mid(v0, v1), m20 = mid(v2, v0);
        t[0][0] = v0; t[0][1] = m01; t[0][2] = m20;
        t[1][0] = m01; t[1][1] = v1; t[1][2] = m20;
        t[2][0] = v1; t[2][1] = v2; t[2][2] = m20;
        n = 3;
    } else {
        V3 m01 = mid(v0, v1), m12 = mid(v1, v2), m20 = mid(v2, v0);
        t[0][0] = v0; t[0][1] = m01; t[0][2] = m20;
        t[1][0] = m01; t[1][1] = v1; t[1][2] = m12;
        t[2][0] = m12; t[2][1] = v2; t[2][2] = m20;
        t[3][0] = m01; t[3][1] = m12; t[3][2] = m20;
        n = 4;
    }
    for (int k = n - 1; k >= 0; k--) decodePaint(r, t[k][0], t[k][1], t[k][2], emit, depth + 1);
}

// ------------------------------------------------------------------ model data

struct Comp { int objectId; std::string path; Xf t; };
struct Item { int objectId; std::string path; Xf t; };

struct Obj {
    int id = 0;
    std::vector<float> pos;     // 9 floats per triangle, object space
    std::vector<uint32_t> col;  // 3 colors per triangle; kNoColor = inherit
    std::vector<Comp> comps;
    uint32_t color = kNoColor;  // object-level default from pid/pindex
};

using Groups = std::unordered_map<int, std::vector<uint32_t>>;

std::string normPath(sv s) {
    while (!s.empty() && s.front() == '/') s.remove_prefix(1);
    return std::string(s);
}

uint32_t groupColor(const Groups &g, int pid, int index) {
    auto it = g.find(pid);
    if (it == g.end() || index < 0 || (size_t)index >= it->second.size()) return kNoColor;
    return it->second[(size_t)index];
}

// Collects <basematerials> and <m:colorgroup> color lists.
void parseGroups(const char *b, const char *e, Groups &groups) {
    Tag t;
    std::vector<uint32_t> *cur = nullptr;
    const char *p = b;
    while ((p = nextTag(p, e, t))) {
        if (t.closing) {
            if (t.name == "basematerials" || t.name == "colorgroup") cur = nullptr;
        } else if (t.name == "basematerials" || t.name == "colorgroup") {
            cur = &groups[toInt(t.get("id"))];
        } else if (cur && t.name == "base") {
            cur->push_back(parseHexColor(t.get("displaycolor")));
        } else if (cur && t.name == "color") {
            cur->push_back(parseHexColor(t.get("color")));
        }
    }
}

// One independently parseable slice of a model file (at most one <object>).
struct Chunk {
    const char *b, *e;
    size_t file;
    std::unique_ptr<Obj> obj;
    std::vector<Item> items;
};

void parseChunk(Chunk &ch, const Groups &groups, const Config &cfg, bool isRoot) {
    Tag t;
    Obj *o = nullptr;
    std::vector<float> verts;
    int objPid = -1;
    uint32_t triIndex = 0;
    const PrusaObj *prusa = nullptr;
    size_t volCursor = 0;

    const char *p = ch.b;
    while ((p = nextTag(p, ch.e, t))) {
        if (t.closing) {
            if (t.name == "object") o = nullptr;
            continue;
        }
        if (o && t.name == "vertex") {
            float c[3] = {0, 0, 0};
            for (int i = 0; i < t.nattrs; i++) {
                const Attr &a = t.attrs[i];
                if (a.name.size() != 1) continue;
                int k = a.name[0] - 'x';
                if (k >= 0 && k < 3) parseFloat(a.value.data(), a.value.data() + a.value.size(), c[k]);
            }
            verts.insert(verts.end(), c, c + 3);
        } else if (o && t.name == "triangle") {
            uint32_t vi[3] = {0, 0, 0};
            int pid = -1, pi[3] = {-1, -1, -1};
            sv paint;
            for (int i = 0; i < t.nattrs; i++) {
                const Attr &a = t.attrs[i];
                if (a.name.size() == 2 && a.name[0] == 'v') {
                    int k = a.name[1] - '1';
                    if (k >= 0 && k < 3) parseUInt(a.value, vi[k]);
                } else if (a.name.size() == 2 && a.name[0] == 'p') {
                    int k = a.name[1] - '1';
                    if (k >= 0 && k < 3) pi[k] = toInt(a.value);
                } else if (a.name == "pid") {
                    pid = toInt(a.value);
                } else if (a.name == "paint_color" || a.name == "mmu_segmentation") {
                    paint = a.value;
                }
            }
            uint32_t index = triIndex++;
            size_t nv = verts.size() / 3;
            if (vi[0] >= nv || vi[1] >= nv || vi[2] >= nv) continue;

            uint32_t base = kNoColor;
            if (prusa) {
                while (volCursor < prusa->vols.size() && index > prusa->vols[volCursor].last) volCursor++;
                if (volCursor < prusa->vols.size() && index >= prusa->vols[volCursor].first) {
                    const PrusaVol &vol = prusa->vols[volCursor];
                    if (!vol.model) continue;  // modifier / support blocker meshes are not part of the model
                    base = cfg.filament(vol.extruder);
                }
            }

            const float *f = verts.data();
            V3 a = {f[vi[0] * 3], f[vi[0] * 3 + 1], f[vi[0] * 3 + 2]};
            V3 b = {f[vi[1] * 3], f[vi[1] * 3 + 1], f[vi[1] * 3 + 2]};
            V3 c = {f[vi[2] * 3], f[vi[2] * 3 + 1], f[vi[2] * 3 + 2]};

            if (!paint.empty()) {
                PaintReader r{paint.data(), (long)paint.size() - 1};
                auto emit = [&](V3 x, V3 y, V3 z, int state) {
                    float q[9] = {x.x, x.y, x.z, y.x, y.y, y.z, z.x, z.y, z.z};
                    o->pos.insert(o->pos.end(), q, q + 9);
                    uint32_t col = state > 0 ? cfg.filament(state) : base;
                    o->col.insert(o->col.end(), 3, col);
                };
                decodePaint(r, a, b, c, emit, 0);
                continue;
            }

            uint32_t cs[3] = {base, base, base};
            if (pi[0] >= 0) {
                int g = pid >= 0 ? pid : objPid;
                cs[0] = groupColor(groups, g, pi[0]);
                cs[1] = pi[1] >= 0 ? groupColor(groups, g, pi[1]) : cs[0];
                cs[2] = pi[2] >= 0 ? groupColor(groups, g, pi[2]) : cs[0];
            }
            float q[9] = {a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z};
            o->pos.insert(o->pos.end(), q, q + 9);
            o->col.insert(o->col.end(), cs, cs + 3);
        } else if (t.name == "object") {
            ch.obj = std::make_unique<Obj>();
            o = ch.obj.get();
            o->id = toInt(t.get("id"));
            objPid = toInt(t.get("pid"));
            if (objPid >= 0) o->color = groupColor(groups, objPid, toInt(t.get("pindex"), 0));
            verts.clear();
            triIndex = 0;
            volCursor = 0;
            prusa = nullptr;
            if (isRoot) {
                auto it = cfg.prusa.find(o->id);
                if (it != cfg.prusa.end() && !it->second.vols.empty()) prusa = &it->second;
            }
        } else if (o && t.name == "component") {
            o->comps.push_back({toInt(t.get("objectid")), normPath(t.get("path")), parseXf(t.get("transform"))});
        } else if (t.name == "item") {
            ch.items.push_back({toInt(t.get("objectid")), normPath(t.get("path")), parseXf(t.get("transform"))});
        }
    }
}

// Offsets of every "<object" start tag. Scans for 'j', which is rare in mesh data.
std::vector<size_t> findObjects(const char *d, size_t n) {
    std::vector<size_t> out;
    const char *p = d + 3, *end = d + n;
    while (p < end && (p = (const char *)memchr(p, 'j', (size_t)(end - p)))) {
        if (end - p > 4 && memcmp(p - 3, "<object", 7) == 0 && (isSpace(p[4]) || p[4] == '>'))
            out.push_back((size_t)(p - 3 - d));
        p++;
    }
    return out;
}

struct Inst {
    const Obj *o;
    Xf t;
    uint32_t inherit;
    size_t offset;  // first output vertex
};

struct Scene {
    const Config &cfg;
    std::map<std::pair<std::string, int>, const Obj *> objects;
    std::vector<Inst> insts;
    size_t verts = 0;

    const Obj *find(const std::string &path, int id) const {
        auto it = objects.find({path, id});
        return it == objects.end() ? nullptr : it->second;
    }

    // `forced` is a slicer-assigned color that beats the object's own; `parent` is the fallback.
    void visit(const Obj *o, const std::string &path, const Xf &t, uint32_t parent, uint32_t forced,
               const BambuObj *bambu, int depth) {
        if (depth > 16) return;
        uint32_t color = forced ? forced : (o->color ? o->color : parent);
        if (!o->pos.empty()) {
            insts.push_back({o, t, color, verts});
            verts += o->pos.size() / 3;
        }
        for (const Comp &c : o->comps) {
            const std::string &childPath = c.path.empty() ? path : c.path;
            const Obj *child = find(childPath, c.objectId);
            if (!child) continue;
            uint32_t childForced = kNoColor;
            if (bambu) {
                auto it = bambu->parts.find(c.objectId);
                if (it != bambu->parts.end()) {
                    if (!it->second.normal) continue;  // modifiers, negative parts, support painting helpers
                    childForced = cfg.filament(it->second.extruder);
                }
            }
            visit(child, childPath, mul(c.t, t), color, childForced, nullptr, depth + 1);
        }
    }
};

bool endsWith(const std::string &s, const char *suffix) {
    size_t n = strlen(suffix);
    return s.size() >= n && strcasecmp(s.c_str() + s.size() - n, suffix) == 0;
}

}  // namespace

bool load3MF(const char *path, Mesh &out, std::string &err) {
    MappedFile f;
    if (!f.open(path)) { err = "Could not open file"; return false; }
    Zip zip;
    if (!zip.open(f.data, f.size)) { err = "Not a valid 3MF (zip) file"; return false; }

    Config cfg;
    {
        Buffer b;
        if (auto e = zip.find("Metadata/model_settings.config"); e && zip.read(*e, b)) parseBambuModelSettings(b, cfg);
    }
    {
        Buffer b;
        if (auto e = zip.find("Metadata/project_settings.config"); e && zip.read(*e, b)) parseBambuFilaments(b, cfg);
    }
    {
        Buffer b;
        if (auto e = zip.find("Metadata/Slic3r_PE_model.config"); e && zip.read(*e, b)) parsePrusaModelConfig(b, cfg);
    }
    if (cfg.filaments.empty()) {
        Buffer b;
        if (auto e = zip.find("Metadata/Slic3r_PE.config"); e && zip.read(*e, b)) parsePrusaFilaments(b, cfg);
    }

    // Decompress every model part in parallel.
    std::vector<const ZipEntry *> modelEntries;
    for (auto &e : zip.entries)
        if (endsWith(e.name, ".model")) modelEntries.push_back(&e);
    if (modelEntries.empty()) { err = "The 3MF file contains no model"; return false; }

    std::vector<Buffer> bufs(modelEntries.size());
    const ZipEntry *const *entryPtr = modelEntries.data();
    Buffer *bufPtr = bufs.data();
    const Zip *zipPtr = &zip;
    dispatch_apply(modelEntries.size(), DISPATCH_APPLY_AUTO, ^(size_t i) {
        zipPtr->read(*entryPtr[i], bufPtr[i]);
    });

    size_t rootFile = 0;
    for (size_t i = 0; i < modelEntries.size(); i++)
        if (strcasecmp(modelEntries[i]->name.c_str(), "3D/3dmodel.model") == 0) rootFile = i;

    // Split each file at object boundaries; gather color groups up front so chunks can be parsed independently.
    std::vector<Groups> groups(modelEntries.size());
    std::vector<Chunk> chunks;
    for (size_t fi = 0; fi < bufs.size(); fi++) {
        const char *d = bufs[fi].data;
        size_t n = bufs[fi].size;
        if (!d || n < 8) continue;
        std::vector<size_t> starts = findObjects(d, n);
        size_t headerEnd = starts.empty() ? n : starts[0];
        parseGroups(d, d + headerEnd, groups[fi]);
        chunks.push_back({d, d + headerEnd, fi, nullptr, {}});
        for (size_t k = 0; k < starts.size(); k++) {
            const char *b = d + starts[k], *e = k + 1 < starts.size() ? d + starts[k + 1] : d + n;
            chunks.push_back({b, e, fi, nullptr, {}});
            // Resources declared after this object's end tag (before the next object).
            const char *lo = e - b > (1 << 20) ? e - (1 << 20) : b;
            for (const char *q = e - 9; q >= lo; q--)
                if (*q == '<' && memcmp(q, "</object>", 9) == 0) {
                    if (k + 1 < starts.size()) parseGroups(q, e, groups[fi]);
                    break;
                }
        }
    }

    Chunk *chunkPtr = chunks.data();
    const Groups *groupPtr = groups.data();
    const Config *cfgPtr = &cfg;
    dispatch_apply(chunks.size(), DISPATCH_APPLY_AUTO, ^(size_t i) {
        parseChunk(chunkPtr[i], groupPtr[chunkPtr[i].file], *cfgPtr, chunkPtr[i].file == rootFile);
    });

    Scene scene{cfg, {}, {}, 0};
    std::vector<Item> items;
    for (Chunk &ch : chunks) {
        if (ch.obj) scene.objects[{modelEntries[ch.file]->name, ch.obj->id}] = ch.obj.get();
        if (ch.file == rootFile) items.insert(items.end(), ch.items.begin(), ch.items.end());
    }

    const std::string &rootPath = modelEntries[rootFile]->name;
    auto addRoot = [&](const Obj *o, const std::string &p, const Xf &t) {
        const BambuObj *bambu = nullptr;
        uint32_t forced = kNoColor;
        if (auto it = cfg.bambu.find(o->id); it != cfg.bambu.end()) {
            bambu = &it->second;
            forced = cfg.filament(bambu->extruder);
        } else if (auto pit = cfg.prusa.find(o->id); pit != cfg.prusa.end()) {
            forced = cfg.filament(pit->second.extruder);
        }
        scene.visit(o, p, t, kNoColor, forced, bambu, 0);
    };
    for (const Item &it : items) {
        const std::string &p = it.path.empty() ? rootPath : it.path;
        if (const Obj *o = scene.find(p, it.objectId)) addRoot(o, p, it.t);
    }
    if (scene.insts.empty())  // no usable <build>: show every object as-is
        for (auto &kv : scene.objects)
            if (!kv.second->pos.empty()) {
                scene.insts.push_back({kv.second, Xf(), kv.second->color, scene.verts});
                scene.verts += kv.second->pos.size() / 3;
            }

    out.resize(scene.verts);
    float *pos = out.pos, *nrm = out.nrm;
    uint8_t *col = out.col;
    const Inst *instPtr = scene.insts.data();
    dispatch_apply(scene.insts.size(), DISPATCH_APPLY_AUTO, ^(size_t i) {
        const Inst &in = instPtr[i];
        bool flip = det(in.t) < 0;
        const float *p = in.o->pos.data();
        const uint32_t *c = in.o->col.data();
        size_t ntri = in.o->pos.size() / 9;
        for (size_t k = 0; k < ntri; k++, p += 9, c += 3) {
            V3 a = apply(in.t, {p[0], p[1], p[2]}), b = apply(in.t, {p[3], p[4], p[5]}), cc = apply(in.t, {p[6], p[7], p[8]});
            uint32_t cs[3] = {c[0] ? c[0] : in.inherit, c[1] ? c[1] : in.inherit, c[2] ? c[2] : in.inherit};
            if (flip) { std::swap(b, cc); std::swap(cs[1], cs[2]); }
            V3 fn = faceNormal(a, b, cc);
            writeTri(pos, nrm, col, in.offset + k * 3, a, b, cc, fn, fn, fn, cs);
        }
    });
    return true;
}

}  // namespace ql3d
