#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <unistd.h>
#include <unordered_map>
#include <chrono>

#include "scaler.h"

static const char* pixel_format_name(uint8_t code, int &depth) {
    switch(code) {
        case 0: depth = 24; return "RGB888";
        case 1: depth = 32; return "ARGB8888";
        case 2: depth = 16; return "RGB565";
        case 3: depth = 16; return "YUV422";
        default: depth = 0; return "Unknown";
    }
}

static uint32_t dominant_color(const unsigned char* buf, int w, int h, int stride, int bpp) {
    std::unordered_map<uint32_t, uint32_t> counts;
    uint32_t bestColor = 0, bestCount = 0;
    int step = 4; // sample every 4 pixels to reduce CPU load
    for (int y = 0; y < h; y += step) {
        const unsigned char* row = buf + y * stride;
        for (int x = 0; x < w; x += step) {
            uint32_t color;
            if (bpp == 32) {
                const unsigned char* p = row + x * 4;
                color = (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[2]);
            } else {
                const unsigned char* p = row + x * 3;
                color = (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[2]);
            }
            uint32_t c = ++counts[color];
            if (c > bestCount) {
                bestCount = c;
                bestColor = color;
            }
        }
    }
    return bestColor;
}

int main() {
    mister_scaler* ms = mister_scaler_init();
    if (!ms) {
        std::fprintf(stderr, "Failed to init MiSTer scaler\n");
        return 1;
    }

    unsigned char* raw = (unsigned char*)(ms->map + ms->map_off);
    uint8_t header5 = raw[5];
    uint8_t format_code = (header5 >> 5) & 0x07;
    int depth = 0;
    const char* format_name = pixel_format_name(format_code, depth);

    const char* endian = "little"; // scaler outputs little endian data

    std::printf("Width: %d\n", ms->width);
    std::printf("Height: %d\n", ms->height);
    std::printf("Color depth: %d\n", depth);
    std::printf("Pixel format: %s\n", format_name);
    std::printf("Endianness: %s\n", endian);

    int bytes_per_pixel = (depth == 32) ? 4 : 3;
    size_t bufsize = size_t(ms->width) * ms->height * bytes_per_pixel;
    unsigned char* frame = (unsigned char*)std::malloc(bufsize);
    if (!frame) {
        std::fprintf(stderr, "Out of memory\n");
        mister_scaler_free(ms);
        return 1;
    }

    uint32_t lastColor = 0;
    auto lastChange = std::chrono::steady_clock::now();
    bool first = true;

    while (true) {
        if (depth == 32) {
            mister_scaler_read_32(ms, frame);
        } else {
            mister_scaler_read(ms, frame);
        }

        uint32_t color = dominant_color(frame, ms->width, ms->height, ms->width * bytes_per_pixel, bytes_per_pixel * 8);

        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - lastChange).count();
        if (first || color != lastColor) {
            lastColor = color;
            lastChange = now;
            elapsed = 0.0;
            first = false;
            std::printf("%.2f changed rgb=%06X\n", elapsed, color);
        } else {
            std::printf("%.2f unchanged rgb=%06X\n", elapsed, color);
        }
        std::fflush(stdout);
        usleep(100000); // 100 ms polling
    }

    std::free(frame);
    mister_scaler_free(ms);
    return 0;
}
