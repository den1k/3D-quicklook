// Wavefront OBJ loader: polygons, normals, vertex colors, and material colors (Kd) from .mtl files.
#include "mesh.hpp"

#include <unordered_map>
#include <vector>

namespace ql3d {

static std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

static uint32_t colorFromFloats(const float c[3]) {
    uint32_t b[3];
    for (int i = 0; i < 3; i++) {
        float x = c[i] < 0 ? 0 : (c[i] > 1 ? 1 : c[i]);
        b[i] = (uint32_t)(x * 255.0f + 0.5f);
    }
    return rgb(b[0], b[1], b[2]);
}

static void loadMTL(const std::string &path, std::unordered_map<std::string, uint32_t> &mats) {
    MappedFile f;
    if (!f.open(path.c_str())) return;
    const char *p = f.data, *end = f.data + f.size;
    std::string cur;
    while (p < end) {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        std::string_view line = trim({p, (size_t)(le - p)});
        if (line.substr(0, 7) == "newmtl ") {
            cur = std::string(trim(line.substr(7)));
        } else if (line.substr(0, 3) == "Kd " && !cur.empty()) {
            float c[3] = {0, 0, 0};
            const char *q = line.data() + 3, *qe = line.data() + line.size();
            for (int i = 0; i < 3; i++) q = parseFloat(q, qe, c[i]);
            mats[cur] = colorFromFloats(c);
        }
        p = le + 1;
    }
}

bool loadOBJ(const char *path, Mesh &out, std::string &err) {
    MappedFile f;
    if (!f.open(path)) { err = "Could not open file"; return false; }
    out.zUp = false;

    std::string dir(path);
    size_t slash = dir.rfind('/');
    dir = slash == std::string::npos ? "" : dir.substr(0, slash + 1);

    std::vector<V3> v, vn;
    std::vector<uint32_t> vc;  // per-vertex colors; filled only if the file has them
    std::unordered_map<std::string, uint32_t> mats;
    uint32_t matColor = kNoColor;
    struct Corner { V3 p, n; uint32_t c; bool hasN; };
    std::vector<Corner> poly;

    const char *p = f.data, *end = f.data + f.size;
    while (p < end) {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        while (p < le && (*p == ' ' || *p == '\t')) p++;
        if (le - p >= 2 && p[0] == 'v' && p[1] == ' ') {
            float c[6];
            int k = 0;
            const char *q = p + 2;
            while (k < 6) {
                const char *r = parseFloat(q, le, c[k]);
                if (r == q) break;
                q = r;
                k++;
            }
            if (k >= 3) {
                v.push_back({c[0], c[1], c[2]});
                if (k == 6) {
                    vc.resize(v.size() - 1, kNoColor);
                    vc.push_back(colorFromFloats(c + 3));
                }
            }
        } else if (le - p >= 3 && p[0] == 'v' && p[1] == 'n' && p[2] == ' ') {
            float c[3] = {0, 0, 0};
            const char *q = p + 3;
            for (int i = 0; i < 3; i++) q = parseFloat(q, le, c[i]);
            vn.push_back({c[0], c[1], c[2]});
        } else if (le - p >= 2 && p[0] == 'f' && p[1] == ' ') {
            poly.clear();
            const char *q = p + 2;
            while (q < le) {
                while (q < le && (*q == ' ' || *q == '\t' || *q == '\r')) q++;
                if (q >= le) break;
                // v, v/vt, v//vn or v/vt/vn; indices are 1-based, negative = relative to end.
                long idx[3] = {0, 0, 0};
                for (int i = 0; i < 3; i++) {
                    char *e;
                    idx[i] = strtol(q, &e, 10);
                    q = e;
                    if (q < le && *q == '/') q++; else break;
                }
                while (q < le && *q != ' ' && *q != '\t') q++;
                long vi = idx[0] < 0 ? (long)v.size() + idx[0] : idx[0] - 1;
                long ni = idx[2] < 0 ? (long)vn.size() + idx[2] : idx[2] - 1;
                if (vi < 0 || vi >= (long)v.size()) continue;
                Corner c;
                c.p = v[(size_t)vi];
                c.hasN = ni >= 0 && ni < (long)vn.size();
                if (c.hasN) c.n = vn[(size_t)ni];
                uint32_t vcol = (size_t)vi < vc.size() ? vc[(size_t)vi] : kNoColor;
                c.c = vcol ? vcol : matColor;
                poly.push_back(c);
            }
            for (size_t i = 2; i < poly.size(); i++) {
                const Corner &a = poly[0], &b = poly[i - 1], &c = poly[i];
                uint32_t cs[3] = {a.c, b.c, c.c};
                if (a.hasN && b.hasN && c.hasN) {
                    out.tri(a.p, b.p, c.p, a.n, b.n, c.n, cs);
                } else {
                    V3 fn = faceNormal(a.p, b.p, c.p);
                    out.tri(a.p, b.p, c.p, fn, fn, fn, cs);
                }
            }
        } else if (le - p > 7 && memcmp(p, "usemtl ", 7) == 0) {
            auto it = mats.find(std::string(trim({p + 7, (size_t)(le - p - 7)})));
            matColor = it == mats.end() ? kNoColor : it->second;
        } else if (le - p > 7 && memcmp(p, "mtllib ", 7) == 0) {
            loadMTL(dir + std::string(trim({p + 7, (size_t)(le - p - 7)})), mats);
        }
        p = le + 1;
    }
    return true;
}

}  // namespace ql3d
