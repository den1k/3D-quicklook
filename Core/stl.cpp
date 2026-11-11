// STL loader: binary and ASCII.
#include "mesh.hpp"

namespace ql3d {

static bool looksAscii(const MappedFile &f) {
    // Binary files have an exact size implied by their triangle count.
    if (f.size >= 84) {
        uint32_t n;
        memcpy(&n, f.data + 80, 4);
        if (84 + (uint64_t)n * 50 == f.size) return false;
    }
    size_t probe = f.size < 1024 ? f.size : 1024;
    return memmem(f.data, probe, "solid", 5) != nullptr && memmem(f.data, f.size, "vertex", 6) != nullptr;
}

bool loadSTL(const char *path, Mesh &out, std::string &err) {
    MappedFile f;
    if (!f.open(path)) { err = "Could not open file"; return false; }

    if (looksAscii(f)) {
        const char *p = f.data, *end = f.data + f.size;
        V3 v[3];
        int k = 0;
        while ((p = (const char *)memmem(p, (size_t)(end - p), "vertex", 6))) {
            p += 6;
            float c[3];
            bool ok = true;
            for (int i = 0; i < 3; i++) {
                const char *q = parseFloat(p, end, c[i]);
                if (q == p) { ok = false; break; }
                p = q;
            }
            if (!ok) continue;
            v[k++] = {c[0], c[1], c[2]};
            if (k == 3) { out.tri(v[0], v[1], v[2], kNoColor); k = 0; }
        }
        return true;
    }

    if (f.size < 84) { err = "Not a valid STL file"; return false; }
    uint32_t n;
    memcpy(&n, f.data + 80, 4);
    uint64_t avail = (f.size - 84) / 50;
    if (n > avail) n = (uint32_t)avail;
    out.reserve((size_t)n * 3);
    const char *p = f.data + 84;
    for (uint32_t i = 0; i < n; i++, p += 50) {
        float v[9];
        memcpy(v, p + 12, 36);
        out.tri({v[0], v[1], v[2]}, {v[3], v[4], v[5]}, {v[6], v[7], v[8]}, kNoColor);
    }
    return true;
}

}  // namespace ql3d
