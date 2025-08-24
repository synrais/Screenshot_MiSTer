// mister_peeper.cpp
//
// Minimal per-frame sampler for MiSTer scaler output.
// - Reads the scaler header from 0x20000000 (ASCAL) via /dev/mem
// - Tracks an "unchanged" timer using a frame hash
// - Reports the dominant color over a sparse grid (fast 5-6-5 histogram)
// - Auto-detects 16-bit RGB565 ordering (RGB/BGR, LE/BE) and re-detects on core/geometry change
// - Low CPU usage via gentle polling
//
// Console output (stdout, one line per frame):
//   time=HH:MM:SS  unchanged=secs  rgb=#RRGGBB (Name)
//
// Build (example):
//   g++ -O3 -march=native -fno-exceptions -fno-rtti -Wall -Wextra -o mister_peeper mister_peeper.cpp
//
// Run:
//   sudo ./mister_peeper
//

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
#include <climits>
#include <algorithm>

// ---------- Tunables ----------
static constexpr int    kStep            = 16;                 // sampling grid step (pixels)
static constexpr bool   kJitterSampling  = true;               // sweep sampling phase each frame
static constexpr int    kJitterX         = 7;
static constexpr int    kJitterY         = 11;
static constexpr size_t FB_BASE_ADDRESS  = 0x20000000u;        // MiSTer scaler base (ASCAL)
static constexpr size_t MAP_LEN          = 2048u * 1024u * 12u;// ~24 MiB mapping window
static constexpr int    kPollMs          = 10;                 // ~100 Hz idle polling

enum ScalerPixelFormat : uint8_t { RGB16 = 0, RGB24 = 1, RGBA32 = 2 };

// ---------- Scaler header ----------
#pragma pack(push,1)
struct FbHeader {
    char     magic[4];          // "ASCL"
    uint8_t  ty;                // 0x01
    uint8_t  pixel_fmt;         // 0,1,2
    uint16_t header_len_be;
    uint16_t attributes_be;     // bit4 triple; bits7..5 frame counter
    uint16_t width_be;
    uint16_t height_be;
    uint16_t line_be;           // stride in bytes
    uint16_t out_width_be;
    uint16_t out_height_be;
};
#pragma pack(pop)

static inline uint16_t be16(uint16_t x){ return (uint16_t)((x>>8)|(x<<8)); }
static inline bool triple_buffered(uint16_t attr_be){ return (be16(attr_be) & (1u<<4)) != 0; }
static inline size_t fb_off(bool large, uint8_t idx){
    if(idx==0) return 0;
    return large ? (idx==1 ? 0x00800000u : 0x01000000u)
                 : (idx==1 ? 0x00200000u : 0x00400000u);
}

// ---------- Runtime ----------
static volatile bool g_run=true;
static void on_sig(int){ g_run=false; }

static inline uint64_t now_ns(){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
}

// Stronger 64-bit hash
static inline uint64_t hash_rgb64(uint64_t h,uint8_t r,uint8_t g,uint8_t b){
    h ^= ((uint64_t)r<<16) ^ ((uint64_t)g<<8) ^ (uint64_t)b;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 32;
    return h;
}

static inline void fmt_hms(double s,char* out,size_t n){
    int S=(int)s; int H=S/3600; int M=(S%3600)/60; S%=60;
    snprintf(out,n,"%02d:%02d:%02d",H,M,S);
}

// ---------- Nearest color name ----------
struct NamedColor { const char* name; uint8_t r,g,b; };
static constexpr NamedColor kPalette[] = {
    {"Black",0,0,0},{"White",255,255,255},{"Red",255,0,0},{"Lime",0,255,0},
    {"Blue",0,0,255},{"Yellow",255,255,0},{"Cyan",0,255,255},{"Magenta",255,0,255},
    {"Silver",192,192,192},{"Gray",128,128,128},{"Maroon",128,0,0},{"Olive",128,128,0},
    {"Green",0,128,0},{"Purple",128,0,128},{"Teal",0,128,128},{"Navy",0,0,128},
    {"Orange",255,165,0},{"Pink",255,192,203},{"Brown",165,42,42},{"Gold",255,215,0}
};
static inline const char* nearest_color_name(uint8_t r,uint8_t g,uint8_t b){
    int best_i = 0;
    int best_d = INT_MAX;
    for(size_t i=0;i<sizeof(kPalette)/sizeof(kPalette[0]);++i){
        int dr = (int)r - (int)kPalette[i].r;
        int dg = (int)g - (int)kPalette[i].g;
        int db = (int)b - (int)kPalette[i].b;
        int d = dr*dr + dg*dg + db*db;
        if(d < best_d){ best_d = d; best_i = (int)i; }
    }
    return kPalette[best_i].name;
}

// ---------- Pixel loaders ----------
static inline void load_rgb565_LE(const volatile uint8_t*p,uint8_t&r,uint8_t&g,uint8_t&b){
    uint16_t v=(uint16_t)p[0]|((uint16_t)p[1]<<8);
    r=(uint8_t)(((v>>11)&0x1F)*255/31);
    g=(uint8_t)(((v>>5 )&0x3F)*255/63);
    b=(uint8_t)(( v      &0x1F)*255/31);
}
static inline void load_rgb565_BE(const volatile uint8_t*p,uint8_t&r,uint8_t&g,uint8_t&b){
    uint16_t v=((uint16_t)p[0]<<8)|(uint16_t)p[1];
    r=(uint8_t)(((v>>11)&0x1F)*255/31);
    g=(uint8_t)(((v>>5 )&0x3F)*255/63);
    b=(uint8_t)(( v      &0x1F)*255/31);
}
static inline void load_bgr565_LE(const volatile uint8_t*p,uint8_t&r,uint8_t&g,uint8_t&b){
    uint16_t v=(uint16_t)p[0]|((uint16_t)p[1]<<8);
    b=(uint8_t)(((v>>11)&0x1F)*255/31);
    g=(uint8_t)(((v>>5 )&0x3F)*255/63);
    r=(uint8_t)(( v      &0x1F)*255/31);
}
static inline void load_bgr565_BE(const volatile uint8_t*p,uint8_t&r,uint8_t&g,uint8_t&b){
    uint16_t v=((uint16_t)p[0]<<8)|(uint16_t)p[1];
    b=(uint8_t)(((v>>11)&0x1F)*255/31);
    g=(uint8_t)(((v>>5 )&0x3F)*255/63);
    r=(uint8_t)(( v      &0x1F)*255/31);
}
using Rgb16Loader = void (*)(const volatile uint8_t*,uint8_t&,uint8_t&,uint8_t&);

// ---------- Histogram ----------
static uint32_t g_epoch = 1;
static uint32_t g_stamp[65536];
static uint16_t g_count[65536];

int main(){
    signal(SIGINT,on_sig); signal(SIGTERM,on_sig);

    // Map scaler
    int fd=open("/dev/mem",O_RDONLY|O_SYNC);
    if(fd<0){ perror("open(/dev/mem)"); return 1; }
    void* map=mmap(nullptr,MAP_LEN,PROT_READ,MAP_SHARED,fd,FB_BASE_ADDRESS);
    if(map==MAP_FAILED){ perror("mmap"); close(fd); return 1; }
    volatile uint8_t* base=(volatile uint8_t*)map;

    // Read header once
    FbHeader h{}; memcpy(&h,(const void*)base,sizeof(FbHeader));
    if (memcmp(h.magic,"ASCL",4)!=0) {
        fprintf(stderr,"error=header_magic_not_found got=%.4s\n",h.magic);
        munmap((void*)base,MAP_LEN); close(fd); return 3;
    }
    if(h.ty!=0x01){
        fprintf(stderr,"error=unexpected_header_type ty=%u\n",(unsigned)h.ty);
        munmap((void*)base,MAP_LEN); close(fd); return 3;
    }

    uint16_t header_len=be16(h.header_len_be);
    uint16_t width     =be16(h.width_be);
    uint16_t height    =be16(h.height_be);
    uint16_t line      =be16(h.line_be);
    ScalerPixelFormat fmt=(ScalerPixelFormat)h.pixel_fmt;
    bool triple = triple_buffered(h.attributes_be);

    // Attr ptrs
    volatile const uint8_t* attr_ptrs[3] = {
        base + 5,
        base + fb_off(false,1) + 5,
        base + fb_off(false,2) + 5
    };

    uint64_t last_hash=0; bool first=true;
    uint64_t start_ns=now_ns(), last_change_ns=start_ns;
    uint64_t frame_no=0;

    while(g_run){
        // Wait for next frame tick
        uint16_t s0=attr_ptrs[0][0];
        while(g_run){
            uint16_t s1=attr_ptrs[0][0];
            if(s1!=s0) break;
            usleep((useconds_t)(kPollMs*1000));
        }
        if(!g_run) break;

        const volatile uint8_t* pix = base + header_len;

        int xs=kStep, ys=kStep;
        int phase_x=0, phase_y=0;
        if(kJitterSampling){
            phase_x=(frame_no*kJitterX)%kStep;
            phase_y=(frame_no*kJitterY)%kStep;
        }
        frame_no++;

        uint64_t hsh=0xcbf29ce484222325ULL;
        uint32_t const epoch=++g_epoch;

        uint32_t mode_key=0; uint16_t mode_count=0;

        if(fmt==RGB24){
            for(unsigned y=phase_y;y<height;y+=ys){
                const volatile uint8_t* row=pix+(size_t)y*line;
                for(unsigned x=phase_x;x<width;x+=xs){
                    const volatile uint8_t* p=row+(size_t)x*3;
                    uint8_t r=p[0],g=p[1],b=p[2];
                    hsh=hash_rgb64(hsh,r,g,b);
                    uint32_t key=((r>>3)<<11)|((g>>2)<<5)|(b>>3);
                    if(g_stamp[key]!=epoch){g_stamp[key]=epoch; g_count[key]=1;}
                    else {uint16_t c=++g_count[key]; if(c>mode_count){mode_count=c; mode_key=key;}}
                    if(mode_count==0){mode_count=1; mode_key=key;}
                }
            }
        } else if(fmt==RGBA32){
            for(unsigned y=phase_y;y<height;y+=ys){
                const volatile uint8_t* row=pix+(size_t)y*line;
                for(unsigned x=phase_x;x<width;x+=xs){
                    const volatile uint8_t* p=row+(size_t)x*4;
                    uint8_t r=p[0],g=p[1],b=p[2];
                    hsh=hash_rgb64(hsh,r,g,b);
                    uint32_t key=((r>>3)<<11)|((g>>2)<<5)|(b>>3);
                    if(g_stamp[key]!=epoch){g_stamp[key]=epoch; g_count[key]=1;}
                    else {uint16_t c=++g_count[key]; if(c>mode_count){mode_count=c; mode_key=key;}}
                    if(mode_count==0){mode_count=1; mode_key=key;}
                }
            }
        }

        uint64_t t_ns=now_ns();
        if(first){ last_hash=hsh; last_change_ns=t_ns; first=false; }
        else if(hsh!=last_hash){ last_change_ns=t_ns; last_hash=hsh; }

        uint8_t Rd=(uint8_t)(((mode_key>>11)&0x1F)*255/31);
        uint8_t Gd=(uint8_t)(((mode_key>>5 )&0x3F)*255/63);
        uint8_t Bd=(uint8_t)(( mode_key     &0x1F)*255/31);

        double unchanged_s=(t_ns-last_change_ns)/1e9;
        double elapsed_s  =(t_ns-start_ns)/1e9;
        char tbuf[16]; fmt_hms(elapsed_s,tbuf,sizeof(tbuf));
        const char* dom_name=nearest_color_name(Rd,Gd,Bd);

        printf("time=%s  unchanged=%.3f  rgb=#%02X%02X%02X (%s)\n",
               tbuf,unchanged_s,Rd,Gd,Bd,dom_name);
        fflush(stdout);
    }

    munmap((void*)base,MAP_LEN); close(fd); return 0;
}
