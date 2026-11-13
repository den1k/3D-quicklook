// Entry point: picks a loader by file extension and finalizes the output mesh.
#include "ql3d.h"
#include "mesh.hpp"

#include <dirent.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

namespace ql3d {

bool MappedFile::open(const char *path) {
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size == 0) { close(fd); return false; }
    void *p = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return false;
    data = (const char *)p;
    size = (size_t)st.st_size;
    return true;
}

MappedFile::~MappedFile() {
    if (data) munmap((void *)data, size);
}

uint32_t parseHexColor(std::string_view s) {
    if (s.size() < 7 || s[0] != '#') return kNoColor;
    uint32_t v[3];
    for (int i = 0; i < 3; i++) {
        uint32_t x = 0;
        for (int j = 0; j < 2; j++) {
            char c = s[1 + i * 2 + j];
            uint32_t d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return kNoColor;
            x = x * 16 + d;
        }
        v[i] = x;
    }
    return rgb(v[0], v[1], v[2]);
}

#ifndef QL3D_WITH_STEP
bool loadSTEP(const char *, Mesh &, std::string &err) {
    err = "STEP support was not built (OpenCASCADE missing)";
    return false;
}
#endif

// ------------------------------------------------------------- mesh cache
//
// One file per source model, named by a hash of its path, size and modification
// time, holding the raw output arrays. Total size is capped; oldest files go first.

static std::string gCacheDir;
static const uint64_t kCacheLimit = 512ull << 20;
struct CacheHeader { char magic[8]; uint64_t vertexCount; uint32_t zUp, pad; };
static const char kCacheMagic[8] = {'Q', 'L', '3', 'D', 'm', '0', '0', '1'};

static std::string cachePath(const char *path) {
    struct stat st;
    if (gCacheDir.empty() || stat(path, &st) != 0) return "";
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    auto mix = [&](const void *d, size_t n) {
        for (size_t i = 0; i < n; i++) { h ^= ((const uint8_t *)d)[i]; h *= 1099511628211ull; }
    };
    mix(path, strlen(path));
    mix(&st.st_size, sizeof(st.st_size));
    mix(&st.st_mtimespec, sizeof(st.st_mtimespec));
    char name[32];
    snprintf(name, sizeof(name), "/%016llx.mesh", (unsigned long long)h);
    return gCacheDir + name;
}

static bool readCache(const std::string &file, Mesh &m) {
    int fd = open(file.c_str(), O_RDONLY);
    if (fd < 0) return false;
    CacheHeader h;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && read(fd, &h, sizeof(h)) == (ssize_t)sizeof(h) &&
              memcmp(h.magic, kCacheMagic, 8) == 0 && (uint64_t)st.st_size == sizeof(h) + h.vertexCount * 28;
    if (ok) {
        m.resize((size_t)h.vertexCount);
        m.zUp = h.zUp != 0;
        size_t n = m.n;
        ok = read(fd, m.pos, n * 12) == (ssize_t)(n * 12) && read(fd, m.nrm, n * 12) == (ssize_t)(n * 12) &&
             read(fd, m.col, n * 4) == (ssize_t)(n * 4);
        if (!ok) m.n = 0;
    }
    close(fd);
    if (ok) utimes(file.c_str(), nullptr);  // mark as recently used
    return ok;
}

static void pruneCache() {
    DIR *d = opendir(gCacheDir.c_str());
    if (!d) return;
    struct Entry { std::string path; time_t mtime; uint64_t size; };
    std::vector<Entry> entries;
    uint64_t total = 0;
    while (struct dirent *e = readdir(d)) {
        std::string p = gCacheDir + "/" + e->d_name;
        struct stat st;
        if (e->d_name[0] == '.' || stat(p.c_str(), &st) != 0) continue;
        entries.push_back({p, st.st_mtime, (uint64_t)st.st_size});
        total += (uint64_t)st.st_size;
    }
    closedir(d);
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.mtime < b.mtime; });
    for (size_t i = 0; i < entries.size() && total > kCacheLimit; i++) {
        unlink(entries[i].path.c_str());
        total -= entries[i].size;
    }
}

static void writeCache(const std::string &file, const Mesh &m) {
    std::string tmp = file + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    CacheHeader h;
    memcpy(h.magic, kCacheMagic, 8);
    h.vertexCount = m.n;
    h.zUp = m.zUp ? 1 : 0;
    h.pad = 0;
    size_t n = m.n;
    bool ok = write(fd, &h, sizeof(h)) == (ssize_t)sizeof(h) && write(fd, m.pos, n * 12) == (ssize_t)(n * 12) &&
              write(fd, m.nrm, n * 12) == (ssize_t)(n * 12) && write(fd, m.col, n * 4) == (ssize_t)(n * 4);
    close(fd);
    if (ok && rename(tmp.c_str(), file.c_str()) == 0) pruneCache();
    else unlink(tmp.c_str());
}

}  // namespace ql3d

using namespace ql3d;

void ql3d_set_cache_dir(const char *dir) {
    gCacheDir = dir ? dir : "";
}

int ql3d_load(const char *path, QL3DMesh *out) {
    memset(out, 0, sizeof(*out));
    std::string ext;
    if (const char *dot = strrchr(path, '.'))
        for (const char *c = dot + 1; *c; c++) ext += (char)tolower(*c);

    Mesh m;
    std::string err;
    bool ok = false;
    if (ext == "stl") ok = loadSTL(path, m, err);
    else if (ext == "obj") ok = loadOBJ(path, m, err);
    else if (ext == "3mf") ok = load3MF(path, m, err);
    else if (ext == "ply") ok = loadPLY(path, m, err);
    else if (ext == "glb" || ext == "gltf") ok = loadGLTF(path, m, err);
    else if (ext == "step" || ext == "stp") {
        std::string cached = cachePath(path);
        ok = !cached.empty() && readCache(cached, m);
        if (!ok) {
            ok = loadSTEP(path, m, err);
            if (ok && m.n > 0 && !cached.empty()) writeCache(cached, m);
        }
    }
    else err = "Unsupported file type";

    if (ok && m.n == 0) { ok = false; err = "The file contains no geometry"; }
    if (!ok) {
        free(m.pos); free(m.nrm); free(m.col);
        snprintf(out->error, sizeof(out->error), "%s", err.empty() ? "Could not read file" : err.c_str());
        return 0;
    }

    // Bounding box, computed in parallel slices.
    const size_t slices = std::max<size_t>(1, std::min<size_t>(64, m.n / 100000));
    std::vector<float> lo(slices * 3, INFINITY), hi(slices * 3, -INFINITY);
    float *loPtr = lo.data(), *hiPtr = hi.data();
    const float *pos = m.pos;
    const size_t count = m.n;
    dispatch_apply(slices, DISPATCH_APPLY_AUTO, ^(size_t s) {
        float a[3] = {INFINITY, INFINITY, INFINITY}, b[3] = {-INFINITY, -INFINITY, -INFINITY};
        for (size_t i = count * s / slices * 3, e = count * (s + 1) / slices * 3; i < e; i += 3)
            for (int k = 0; k < 3; k++) {
                float v = pos[i + k];
                if (v < a[k]) a[k] = v;
                if (v > b[k]) b[k] = v;
            }
        memcpy(loPtr + s * 3, a, sizeof(a));
        memcpy(hiPtr + s * 3, b, sizeof(b));
    });
    float mn[3] = {INFINITY, INFINITY, INFINITY}, mx[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (size_t s = 0; s < slices; s++)
        for (int k = 0; k < 3; k++) {
            mn[k] = std::min(mn[k], lo[s * 3 + k]);
            mx[k] = std::max(mx[k], hi[s * 3 + k]);
        }
    memcpy(out->boundsMin, mn, sizeof(mn));
    memcpy(out->boundsMax, mx, sizeof(mx));
    out->positions = m.pos;
    out->normals = m.nrm;
    out->colors = m.col;
    out->vertexCount = m.n;
    out->zUp = m.zUp ? 1 : 0;
    return 1;
}

void ql3d_free(QL3DMesh *mesh) {
    free(mesh->positions);
    free(mesh->normals);
    free(mesh->colors);
    mesh->positions = mesh->normals = nullptr;
    mesh->colors = nullptr;
    mesh->vertexCount = 0;
}
