// Entry point: picks a loader by file extension and finalizes the output mesh.
#include "ql3d.h"
#include "mesh.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

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

}  // namespace ql3d

using namespace ql3d;

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
    else if (ext == "step" || ext == "stp") ok = loadSTEP(path, m, err);
    else err = "Unsupported file type";

    if (ok && m.n == 0) { ok = false; err = "The file contains no geometry"; }
    if (!ok) {
        free(m.pos); free(m.nrm); free(m.col);
        snprintf(out->error, sizeof(out->error), "%s", err.empty() ? "Could not read file" : err.c_str());
        return 0;
    }

    float mn[3] = {INFINITY, INFINITY, INFINITY}, mx[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (size_t i = 0; i < m.n * 3; i += 3)
        for (int k = 0; k < 3; k++) {
            float v = m.pos[i + k];
            if (v < mn[k]) mn[k] = v;
            if (v > mx[k]) mx[k] = v;
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
