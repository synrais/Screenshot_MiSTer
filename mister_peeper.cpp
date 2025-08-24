#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

// Minimal scaler + shared memory implementation so the tool is standalone
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
#define fpga_mem(x) (0x20000000 | ((x) & 0x1FFFFFFF))

void mister_scaler_free(mister_scaler *ms);

// --- shared memory helpers ---
static int memfd = -1;

void *shmem_map(uint32_t address, uint32_t size) {
    if (memfd < 0) {
        memfd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
        if (memfd == -1) {
            std::printf("Error: Unable to open /dev/mem!\n");
            return 0;
        }
    }
    void *res = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, address);
    if (res == MAP_FAILED) {
        std::printf("Error: Unable to mmap (0x%X, %d)!\n", address, size);
        return 0;
    }
    return res;
}

int shmem_unmap(void *map, uint32_t size) {
    if (munmap(map, size) < 0) {
        std::printf("Error: Unable to unmap(%p, %d)!\n", map, size);
        return 0;
    }
    return 1;
}

int shmem_put(uint32_t address, uint32_t size, void *buf) {
    void *shmem = shmem_map(address, size);
    if (shmem) {
        std::memcpy(shmem, buf, size);
        shmem_unmap(shmem, size);
    }
    return shmem != 0;
}

int shmem_get(uint32_t address, uint32_t size, void *buf) {
    void *shmem = shmem_map(address, size);
    if (shmem) {
        std::memcpy(buf, shmem, size);
        shmem_unmap(shmem, size);
    }
    return shmem != 0;
}

// --- scaler access ---
mister_scaler *mister_scaler_init() {
    mister_scaler *ms = (mister_scaler*)std::calloc(1, sizeof(mister_scaler));
    int pagesize = sysconf(_SC_PAGE_SIZE);
    if (pagesize == 0) pagesize = 4096;
    int offset = MISTER_SCALER_BASEADDR;
    int map_start = offset & ~(pagesize - 1);
    ms->map_off = offset - map_start;
    ms->num_bytes = MISTER_SCALER_BUFFERSIZE;

    ms->map = (char*)shmem_map(map_start, ms->num_bytes + ms->map_off);
    if (!ms->map) {
        mister_scaler_free(ms);
        return nullptr;
    }

    volatile unsigned char *buffer = (volatile unsigned char *)(ms->map + ms->map_off);
    if (buffer[0] != 1 || buffer[1] != 1) {
        std::fprintf(stderr, "problem\n");
        mister_scaler_free(ms);
        return nullptr;
    }

    ms->header       = buffer[2]  << 8 | buffer[3];
    ms->width        = buffer[6]  << 8 | buffer[7];
    ms->height       = buffer[8]  << 8 | buffer[9];
    ms->line         = buffer[10] << 8 | buffer[11];
    ms->output_width = buffer[12] << 8 | buffer[13];
    ms->output_height= buffer[14] << 8 | buffer[15];
    return ms;
}

void mister_scaler_free(mister_scaler *ms) {
    if (ms) {
        shmem_unmap(ms->map, ms->num_bytes + ms->map_off);
        std::free(ms);
    }
}

int mister_scaler_read(mister_scaler *ms, unsigned char *gbuf) {
    volatile unsigned char *buffer = (volatile unsigned char *)(ms->map + ms->map_off);
    for (int y = 0; y < ms->height; y++) {
        std::memcpy(&gbuf[y*(ms->width*3)],
                    const_cast<const void*>(static_cast<volatile const void*>(&buffer[ms->header + y*ms->line])),
                    ms->width*3);
    }
    return 0;
}

int mister_scaler_read_32(mister_scaler *ms, unsigned char *gbuf) {
    volatile unsigned char *buffer = (volatile unsigned char *)(ms->map + ms->map_off);
    volatile unsigned char *pixbuf;
    unsigned char *outbuf;
    for (int y = 0; y < ms->height; y++) {
        pixbuf = &buffer[ms->header + y*ms->line];
        outbuf = &gbuf[y*(ms->width*4)];
        for (int x = 0; x < ms->width; x++) {
            outbuf[2] = *pixbuf++;
            outbuf[1] = *pixbuf++;
            outbuf[0] = *pixbuf++;
            outbuf[3] = 0xFF;
            outbuf += 4;
        }
    }
    return 0;
}

int mister_scaler_read_yuv(mister_scaler *ms, int lineY, unsigned char *bufY,
                           int lineU, unsigned char *bufU,
                           int lineV, unsigned char *bufV) {
    volatile unsigned char *buffer = (volatile unsigned char *)(ms->map + ms->map_off);
    for (int y = 0; y < ms->height; y++) {
        volatile unsigned char *pixbuf = &buffer[ms->header + y*ms->line];
        unsigned char *outbufy = &bufY[y*lineY];
        unsigned char *outbufU = &bufU[y*lineU];
        unsigned char *outbufV = &bufV[y*lineV];
        for (int x = 0; x < ms->width; x++) {
            int R = *pixbuf++;
            int G = *pixbuf++;
            int B = *pixbuf++;
            int Y =  (0.257 * R) + (0.504 * G) + (0.098 * B) + 16;
            int U = -(0.148 * R) - (0.291 * G) + (0.439 * B) + 128;
            int V =  (0.439 * R) - (0.368 * G) - (0.071 * B) + 128;
            *outbufy++ = Y;
            *outbufU++ = U;
            *outbufV++ = V;
        }
    }
    return 0;
}

// Sampled FNV-1a hash to cheaply detect changes
static uint64_t sample_hash(const volatile unsigned char *base, int width, int height,
                            int line, int bpp, int step) {
    uint64_t hash = 1469598103934665603ULL;
    const uint64_t prime = 1099511628211ULL;
    for (int y = 0; y < height; y += step) {
        const volatile unsigned char *row = base + y * line;
        for (int x = 0; x < width; x += step) {
            const volatile unsigned char *p = row + x * bpp;
            for (int i = 0; i < bpp; ++i) {
                hash ^= p[i];
                hash *= prime;
            }
        }
    }
    return hash;
}

static uint32_t dominant_color(const volatile unsigned char *base, int width, int height,
                               int line, int bpp, int step) {
    uint32_t counts[4096];
    memset(counts, 0, sizeof(counts));

    for (int y = 0; y < height; y += step) {
        const volatile unsigned char *pix = base + y * line;
        for (int x = 0; x < width; x += step) {
            int r = 0, g = 0, b = 0;
            if (bpp == 2) {
                uint16_t v = static_cast<uint16_t>(pix[0]) | (static_cast<uint16_t>(pix[1]) << 8);
                r = ((v >> 11) & 0x1F);
                g = ((v >> 5) & 0x3F);
                b = (v & 0x1F);
                r = (r << 3) | (r >> 2);
                g = (g << 2) | (g >> 4);
                b = (b << 3) | (b >> 2);
                pix += 2 * step;
            } else if (bpp == 4) {
                b = pix[0];
                g = pix[1];
                r = pix[2];
                pix += 4 * step;
            } else { // assume 3 bytes RGB
                r = pix[0];
                g = pix[1];
                b = pix[2];
                pix += 3 * step;
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

    volatile unsigned char *buffer =
        (volatile unsigned char *)(ms->map + ms->map_off);

    auto last_change = std::chrono::steady_clock::now();
    uint64_t last_hash = 0;
    uint32_t last_color = 0;
    bool first = true;
    const int step = 4; // sample every 4th pixel to reduce CPU usage

    // Track current scaler state.  These values may change even when the
    // framebuffer offset remains the same, so refresh them every iteration.
    int header = ms->header;
    int width  = ms->width;
    int height = ms->height;
    int line   = ms->line;
    int out_w  = ms->output_width;
    int out_h  = ms->output_height;
    int bpp    = 0;

    while (1) {
        int new_header = buffer[2] << 8 | buffer[3];
        int new_width  = buffer[6]  << 8 | buffer[7];
        int new_height = buffer[8]  << 8 | buffer[9];
        int new_line   = buffer[10] << 8 | buffer[11];
        int new_out_w  = buffer[12] << 8 | buffer[13];
        int new_out_h  = buffer[14] << 8 | buffer[15];
        int new_bpp    = (buffer[4] & 0xFF) + 1; // encoded as bytes per pixel - 1

        bool meta_changed = (new_header != header) || (new_width != width) ||
                            (new_height != height) || (new_line != line) ||
                            (new_bpp != bpp);

        header = new_header;
        width  = new_width;
        height = new_height;
        line   = new_line;
        out_w  = new_out_w;
        out_h  = new_out_h;
        bpp    = new_bpp;

        uint8_t hdr5 = buffer[5];
        (void)out_w;
        (void)out_h;
        (void)hdr5;

        const char *pixfmt = "Unknown";
        switch (bpp) {
            case 1: pixfmt = "8-bit"; break;
            case 2: pixfmt = "RGB565"; break;
            case 3: pixfmt = "RGB888"; break;
            case 4: pixfmt = "ARGB8888"; break;
        }

        const volatile unsigned char *frame = buffer + header;
        uint64_t hash =
            sample_hash(frame, width, height, line, bpp, step);
        uint32_t color =
            dominant_color(frame, width, height, line, bpp, step);
        auto now = std::chrono::steady_clock::now();

        if (first || meta_changed || hash != last_hash) {
            last_hash = hash;
            last_color = color;
            last_change = now;
            first = false;
        }

        double secs =
            std::chrono::duration<double>(now - last_change).count();

        const char *endian =
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            "little";
#else
            "big";
#endif

        char status[256];
        std::snprintf(status, sizeof(status),
                      "%dx%d %d-bit %s %s %.2fs rgb=%06X",
                      width, height, bpp * 8, pixfmt, endian, secs,
                      last_color);
        std::printf("\r%-80s", status);
        std::fflush(stdout);
        usleep(50000);
    }
    return 0;
}
