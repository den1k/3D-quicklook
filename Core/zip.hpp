// Minimal read-only ZIP reader over a memory-mapped file (stored + deflate, zip64 aware).
#pragma once

#include <compression.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ql3d {

struct ZipEntry {
    std::string name;
    uint64_t compSize = 0, size = 0, localOffset = 0;
    uint16_t method = 0;
};

// Owned, malloc'ed decompressed bytes.
struct Buffer {
    char *data = nullptr;
    size_t size = 0;
    Buffer() = default;
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;
    Buffer(Buffer &&o) noexcept : data(o.data), size(o.size) { o.data = nullptr; o.size = 0; }
    ~Buffer() { free(data); }
};

class Zip {
public:
    std::vector<ZipEntry> entries;

    bool open(const char *data, size_t size) {
        d = (const uint8_t *)data;
        n = size;
        if (n < 22) return false;
        size_t i = n - 22, stop = n > 22 + 65535 ? n - 22 - 65535 : 0;
        for (;; i--) {
            if (u32(i) == 0x06054b50) break;
            if (i == stop) return false;
        }
        uint64_t count = u16(i + 10), cdOffset = u32(i + 16);
        if ((count == 0xFFFF || cdOffset == 0xFFFFFFFF) && i >= 20 && u32(i - 20) == 0x07064b50) {
            uint64_t z = u64(i - 20 + 8);
            if (z + 56 <= n && u32(z) == 0x06064b50) { count = u64(z + 32); cdOffset = u64(z + 48); }
        }
        uint64_t p = cdOffset;
        for (uint64_t k = 0; k < count; k++) {
            if (p + 46 > n || u32(p) != 0x02014b50) break;
            ZipEntry e;
            e.method = u16(p + 10);
            e.compSize = u32(p + 20);
            e.size = u32(p + 24);
            uint16_t nameLen = u16(p + 28), extraLen = u16(p + 30), commentLen = u16(p + 32);
            e.localOffset = u32(p + 42);
            if (p + 46 + nameLen + extraLen > n) break;
            e.name.assign((const char *)d + p + 46, nameLen);
            // zip64 extended information
            uint64_t x = p + 46 + nameLen, xe = x + extraLen;
            while (x + 4 <= xe) {
                uint16_t id = u16(x), len = u16(x + 2);
                if (id == 1) {
                    uint64_t q = x + 4;
                    if (e.size == 0xFFFFFFFF && q + 8 <= xe) { e.size = u64(q); q += 8; }
                    if (e.compSize == 0xFFFFFFFF && q + 8 <= xe) { e.compSize = u64(q); q += 8; }
                    if (e.localOffset == 0xFFFFFFFF && q + 8 <= xe) { e.localOffset = u64(q); }
                }
                x += 4 + len;
            }
            entries.push_back(std::move(e));
            p += 46 + nameLen + extraLen + commentLen;
        }
        return !entries.empty();
    }

    const ZipEntry *find(const std::string &name) const {
        for (auto &e : entries)
            if (e.name == name) return &e;
        return nullptr;
    }

    bool read(const ZipEntry &e, Buffer &out) const {
        uint64_t l = e.localOffset;
        if (l + 30 > n || u32(l) != 0x04034b50) return false;
        uint64_t start = l + 30 + u16(l + 26) + u16(l + 28);
        if (start + e.compSize > n) return false;
        out.data = (char *)malloc(e.size + 1);
        if (!out.data) return false;
        if (e.method == 0) {
            if (e.compSize != e.size) return false;
            memcpy(out.data, d + start, e.size);
        } else if (e.method == 8) {
            size_t got = compression_decode_buffer((uint8_t *)out.data, e.size, d + start, e.compSize,
                                                   nullptr, COMPRESSION_ZLIB);
            if (got != e.size) return false;
        } else {
            return false;
        }
        out.size = e.size;
        return true;
    }

private:
    const uint8_t *d = nullptr;
    size_t n = 0;
    uint16_t u16(uint64_t o) const { uint16_t v; memcpy(&v, d + o, 2); return v; }
    uint32_t u32(uint64_t o) const { uint32_t v; memcpy(&v, d + o, 4); return v; }
    uint64_t u64(uint64_t o) const { uint64_t v; memcpy(&v, d + o, 8); return v; }
};

}  // namespace ql3d
