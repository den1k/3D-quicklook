// C interface to the model loading core. Used from Swift via a bridging header.
#ifndef QL3D_H
#define QL3D_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A triangle soup: every 3 consecutive vertices form one triangle.
// All buffers are malloc'ed; ownership passes to the caller (free() each, or ql3d_free).
// Each is aligned to 16 KB and its allocation padded to a multiple of 16 KB, so it can
// back a no-copy Metal buffer.
typedef struct {
    float *positions;    // 3 floats per vertex
    float *normals;      // 3 floats per vertex
    uint8_t *colors;     // 4 bytes per vertex, sRGB RGBA
    size_t vertexCount;  // multiple of 3
    float boundsMin[3];
    float boundsMax[3];
    int zUp;             // 1 if the model's up axis is +Z, 0 if +Y
    char error[256];     // set when ql3d_load returns 0
} QL3DMesh;

// Loads .stl, .obj, .3mf, .step or .stp. Returns 1 on success, 0 on failure.
int ql3d_load(const char *path, QL3DMesh *out);
void ql3d_free(QL3DMesh *mesh);

// Optional. Directory for caching the tessellation of slow-to-convert files (STEP),
// so reopening an unchanged file is instant. Caching is off until this is called.
void ql3d_set_cache_dir(const char *dir);

#ifdef __cplusplus
}
#endif
#endif
