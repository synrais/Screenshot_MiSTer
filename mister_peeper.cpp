// mister_peeper.cpp
//
// MiSTer framebuffer monitor
// Reports: timestamp, resolution, depth, format, endian, main color, Δt since last change
// Prints all info on one line per change
//
// Build: g++ -O2 -std=c++17 mister_peeper.cpp -o mister_peeper
// Run as root: sudo ./mister_peeper

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <unordered_map>
#include <thread>
#include <ctime>
#include <string>

// Correct ASCAL header definition (from MiSTer Screenshot source)
struct AscalHeader {
    uint32_t magic;      // "ASCL" = 0x4C534341
    uint16_t version;
    uint16_t header_size;
    uint16_t width;
    uint16_t height;
    uint16_t pitch;
    uint16_t format;     // 0=RGB, 1=ARGB, 2=YUV
    uint16_t depth;      // bits per pixel
    uint16_t flags;      // bit0=endian (0=LE, 1=BE)
    uint32_t fb_offset;  // framebuffer offset
    uint32_t fb_size;    // framebuffer size
};

constexpr off_t ASCAL_BASE = 0x20000000; // ASCAL base address
constexpr size_t MAP_SIZE = 2*1024*1024; // map 2MB window (safe)

uint8_t* map_dev_mem(off_t offset, size_t size) {
    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) { perror("open"); exit(1); }
    long page = sysconf(_SC_PAGE_SIZE);
    off_t aligned = offset & ~(page - 1);
    off_t delta = offset - aligned;
    void* map = mmap(nullptr, size + delta, PROT_READ, MAP_SHARED, fd, aligned);
    if (map == MAP_FAILED) { perror("mmap"); exit(1); }
    close(fd);
    return reinterpret_cast<uint8_t*>(map) + delta;
}

std::string format_desc(uint16_t fmt, uint16_t depth, bool endian) {
    std::string f;
    switch(fmt) {
        case 0: f = "RGB"; break;
        case 1: f = "ARGB"; break;
        case 2: f = "YUV"; break;
        default: f = "UNK"; break;
    }
    f += " " + std::to_string(depth) + "-bit";
    f += (endian ? " (BE)" : " (LE)");
    return f;
}

int main() {
    uint8_t* base = map_dev_mem(ASCAL_BASE, MAP_SIZE);
    auto* hdr = reinterpret_cast<AscalHeader*>(base);

    // Verify header
    if (hdr->magic != 0x4C534341) { // "ASCL"
        std::cerr << "ASCAL header magic not found (expected 'ASCL')" << std::endl;
        return 1;
    }

    uint8_t* counter_ptr = base + 5;
    uint8_t last_counter = *counter_ptr;
    auto last_time = std::chrono::steady_clock::now();

    while (true) {
        uint8_t counter = *counter_ptr;
        if (counter != last_counter) {
            last_counter = counter;

            auto now = std::chrono::steady_clock::now();
            std::chrono::duration<double> diff = now - last_time;
            last_time = now;

            int w      = hdr->width;
            int h      = hdr->height;
            int depth  = hdr->depth;
            int fmt    = hdr->format;
            bool endian = hdr->flags & 1;
            off_t fb_off = hdr->fb_offset;
            size_t fb_size = hdr->fb_size;
            uint8_t* fb = base + fb_off;

            // Sample pixels
            std::unordered_map<uint32_t, size_t> freq;
            size_t step = (depth/8 > 0) ? (depth/8) * 100 : 3;
            for (size_t i = 0; i + (depth/8) <= fb_size; i += step) {
                uint32_t rgb = 0;
                if (depth == 16) {
                    uint16_t px = *reinterpret_cast<uint16_t*>(fb+i);
                    if (endian) px = (px>>8) | (px<<8);
                    uint8_t r = ((px >> 11) & 0x1F) << 3;
                    uint8_t g = ((px >> 5) & 0x3F) << 2;
                    uint8_t b = (px & 0x1F) << 3;
                    rgb = (r<<16)|(g<<8)|b;
                } else if (depth == 24) {
                    uint8_t r = fb[i];
                    uint8_t g = fb[i+1];
                    uint8_t b = fb[i+2];
                    rgb = (r<<16)|(g<<8)|b;
                } else if (depth == 32) {
                    uint32_t px = *reinterpret_cast<uint32_t*>(fb+i);
                    if (endian) px = __builtin_bswap32(px);
                    rgb = px & 0xFFFFFF;
                }
                freq[rgb]++;
            }

            // Find dominant color
            uint32_t top_rgb = 0;
            size_t top_count = 0;
            for (auto& [rgb, count] : freq) {
                if (count > top_count) {
                    top_count = count;
                    top_rgb = rgb;
                }
            }

            // Timestamp
            auto sys_now = std::chrono::system_clock::now();
            std::time_t tt = std::chrono::system_clock::to_time_t(sys_now);
            std::tm tm;
            localtime_r(&tt, &tm);

            // One-line output
            std::cout << std::put_time(&tm, "[%H:%M:%S] ")
                      << w << "x" << h << " @ " << format_desc(fmt, depth, endian)
                      << " Main:#" << std::hex << std::uppercase
                      << std::setw(6) << std::setfill('0') << top_rgb
                      << std::dec << " Δt:" << std::fixed << std::setprecision(2)
                      << diff.count() << "s"
                      << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return 0;
}
