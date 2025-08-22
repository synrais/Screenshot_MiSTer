/*
Copyright 2019 alanswx
with help from the MiSTer contributors including Grabulosaure
*/

#include <stdlib.h>
#include <stdio.h>
#include <sched.h>
#include <inttypes.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "lib/imlib2/Imlib2.h"
#include "scaler.h"

const char *version = "$VER:ScreenShot" VDATE;

#define BASEADDR 536870912
unsigned char buffer[2048*3*1024];

void mister_scaler_free(mister_scaler *);

int main(int argc, char *argv[])
{
    // Always write to RAM tmp folder
    const char *outdir = "/tmp/screenshots";
    mkdir(outdir, 0777);
    if (chdir(outdir) != 0) {
        perror("chdir(/tmp/screenshots)");
        return 1;
    }

    // Filename: optional arg, else default
    char filename[4096];
    if (argc > 1 && argv[1] && argv[1][0]) {
        snprintf(filename, sizeof(filename), "%s", argv[1]);
        fprintf(stderr, "output name: %s\n", filename);
    } else {
        snprintf(filename, sizeof(filename), "%s", "MiSTer_small.png");
    }

    mister_scaler *ms = mister_scaler_init();
    if (ms == NULL) {
        fprintf(stderr, "some problem with the mister scaler, maybe this core doesn't support it\n");
        return 1;
    }

    fprintf(stderr, "\nScreenshot code by alanswx\n\n");
    fprintf(stderr, "Version %s\n\n", version + 5);
    fprintf(stderr, "Image: Width=%u Height=%u  Line=%u  Header=%u output_width=%u output_height=%u\n",
            ms->width, ms->height, ms->line_length, ms->header_length, ms->output_width, ms->output_height);

    // Grab native (unscaled) frame
    unsigned char *outputbuf = (unsigned char*)calloc(ms->width * ms->height * 4, 1);
    if (!outputbuf) {
        perror("calloc");
        mister_scaler_free(ms);
        return 1;
    }
    mister_scaler_read_32(ms, outputbuf);

    Imlib_Image im = imlib_create_image_using_data(ms->width, ms->height, (unsigned int *)outputbuf);
    imlib_context_set_image(im);

    // Save ONLY the native image
    imlib_save_image(filename);
    printf("saved: %s/%s\n", outdir, filename);

    // Clean up
    // NOTE: original code leaves outputbuf managed by Imlib2; do not free() here to avoid double-free.
    // imlib_free_image_and_decache(); // optional if you want to force free
    mister_scaler_free(ms);
    return 0;
}
