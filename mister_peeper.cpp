#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>

typedef struct {
    int header;
    int width;
    int height;
    int line;
    int output_width;
    int output_height;

    char *map;
    int num_bytes;
    int map_off;
} mister_scaler;

#define MISTER_SCALER_BASEADDR   0x20000000
#define MISTER_SCALER_BUFFERSIZE (2048*3*1024)

mister_scaler *mister_scaler_init();
int mister_scaler_read(mister_scaler *, unsigned char *buffer);
int mister_scaler_read_32(mister_scaler *ms, unsigned char *buffer);
int mister_scaler_read_yuv(mister_scaler *ms,int,unsigned char *y,int, unsigned char *U,int, unsigned char *V);
void mister_scaler_free(mister_scaler *);

// FNV-1a 64-bit hash
static uint64_t fnv1a(const unsigned char *data, size_t len) {
    uint64_t hash = 1469598103934665603ULL;
    const uint64_t prime = 1099511628211ULL;
    for (size_t i = 0; i < len; ++i) {
        hash ^= data[i];
        hash *= prime;
    }
    return hash;
}

static uint32_t dominant_color(const unsigned char *base, int width, int height, int line, int bpp) {
    uint32_t counts[4096];
    memset(counts, 0, sizeof(counts));

    for (int y = 0; y < height; ++y) {
        const unsigned char *pix = base + y * line;
        for (int x = 0; x < width; ++x) {
            int r = 0, g = 0, b = 0;
            if (bpp == 2) {
                uint16_t v = pix[0] | (pix[1] << 8);
                r = ((v >> 11) & 0x1F);
                g = ((v >> 5) & 0x3F);
                b = (v & 0x1F);
                r = (r << 3) | (r >> 2);
                g = (g << 2) | (g >> 4);
                b = (b << 3) | (b >> 2);
                pix += 2;
            } else if (bpp == 4) {
                b = *pix++;
                g = *pix++;
                r = *pix++;
                pix++; // skip alpha
            } else { // assume 3 bytes RGB
                r = *pix++;
                g = *pix++;
                b = *pix++;
            }
            int idx = ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
            counts[idx]++;
        }
    }

    uint32_t best = 0;
    for (int i = 1; i < 4096; ++i) {
        if (counts[i] > counts[best]) best = i;
    }
    int r = ((best >> 8) & 0xF) * 17;
    int g = ((best >> 4) & 0xF) * 17;
    int b = (best & 0xF) * 17;
    return (r << 16) | (g << 8) | b;
}

int main(int, char**) {
    mister_scaler *ms = mister_scaler_init();
    if (!ms) {
        std::fprintf(stderr, "scaler init failed\n");
        return 1;
    }

    unsigned char *buffer = (unsigned char*)(ms->map + ms->map_off);
    int bpp = buffer[4];
    int width = ms->width;
    int height = ms->height;
    int line = ms->line;

    const char *pixfmt = "Unknown";
    switch (bpp) {
        case 1: pixfmt = "8-bit"; break;
        case 2: pixfmt = "RGB565"; break;
        case 3: pixfmt = "RGB888"; break;
        case 4: pixfmt = "ARGB8888"; break;
    }

    std::printf("Width: %d\n", width);
    std::printf("Height: %d\n", height);
    std::printf("Color depth: %d bits\n", bpp * 8);
    std::printf("Pixel format: %s\n", pixfmt);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    std::printf("Endianness: little\n");
#else
    std::printf("Endianness: big\n");
#endif

    auto last_change = std::chrono::steady_clock::now();
    uint64_t last_hash = 0;
    uint32_t last_color = 0;
    bool first = true;

    while (1) {
        unsigned char *frame = buffer + ms->header;
        uint64_t hash = fnv1a(frame, line * height);
        uint32_t color = dominant_color(frame, width, height, line, bpp);
        auto now = std::chrono::steady_clock::now();

        if (first || hash != last_hash) {
            last_hash = hash;
            last_color = color;
            last_change = now;
            std::printf("changed 0.00 rgb=%06X\n", color);
            first = false;
        } else {
            double secs = std::chrono::duration<double>(now - last_change).count();
            std::printf("unchanged %.2f rgb=%06X\n", secs, last_color);
        }
        std::fflush(stdout);
        usleep(50000);
    }
    return 0;
}
