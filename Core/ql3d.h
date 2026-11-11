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

#ifdef __cplusplus
}
#endif
#endif
