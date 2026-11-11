// Loads each file given on the command line and prints timing and mesh statistics.
#include "../Core/ql3d.h"
#include <chrono>
#include <cstdio>
#include <map>
#include <cstring>

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        QL3DMesh m;
        auto t0 = std::chrono::steady_clock::now();
        int ok = ql3d_load(argv[i], &m);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const char *name = strrchr(argv[i], '/');
        name = name ? name + 1 : argv[i];
        if (!ok) { printf("FAIL %7.1f ms  %s: %s\n", ms, name, m.error); continue; }
        std::map<uint32_t, size_t> colors;
        for (size_t v = 0; v < m.vertexCount; v++) { uint32_t c; memcpy(&c, m.colors + v * 4, 4); colors[c]++; }
        printf("ok   %7.1f ms  %9zu tris  size %.1f x %.1f x %.1f  %zu colors  %s\n", ms, m.vertexCount / 3,
               m.boundsMax[0] - m.boundsMin[0], m.boundsMax[1] - m.boundsMin[1], m.boundsMax[2] - m.boundsMin[2],
               colors.size(), name);
        if (colors.size() <= 8)
            for (auto &kv : colors)
                printf("       #%02X%02X%02X  %zu tris\n", kv.first & 255, (kv.first >> 8) & 255, (kv.first >> 16) & 255, kv.second / 3);
        ql3d_free(&m);
    }
}
