/*
Copyright 2019 alanswx
with help from the MiSTer contributors including Grabulosaure 
*/

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <sched.h>
#include <inttypes.h>
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "lodepng.h"
#include "scaler.h"

const char *version = "$VER:ScreenShot" VDATE;

#define BASEADDR 536870912 

unsigned char buffer[2048*3*1024];

void mister_scaler_free(mister_scaler *);

int main(int argc, char *argv[])
{
    // Always write into RAM tmp folder
    if (mkdir("/tmp/.SAM_tmp/screenshots", 0777) != 0 && errno != EEXIST) {
        perror("mkdir");
        return 1;
    }
    if (chdir("/tmp/.SAM_tmp/screenshots") != 0) {
        perror("chdir");
        return 1;
    }

    char filename[4096];
    strcpy(filename,"MiSTer_screenshot.png");
    if (argc > 1) 
    {
        fprintf(stderr,"output name: %s\n", argv[1]);
        strcpy(filename,argv[1]);
    }

    mister_scaler *ms = mister_scaler_init();
    if (ms == NULL)
    {
        fprintf(stderr,"some problem with the mister scaler, maybe this core doesn't support it\n");
        exit(1);
    } 
    fprintf(stderr,"\nScreenshot code by alanswx\n\n");
    fprintf(stderr,"Version %s\n\n", version + 5);
   
    unsigned char *outputbuf = (unsigned char*)calloc(ms->width*ms->height*3,1);
    mister_scaler_read(ms,outputbuf);

    unsigned error = lodepng_encode24_file(filename, outputbuf, ms->width, ms->height);
    if(error) {
        fprintf(stderr,"error %u: %s\n", error, lodepng_error_text(error));
    } else {
        printf("saved: /tmp/.SAM_tmp/screenshots/%s\n", filename);
    }

    // No scaled image anymore
    mister_scaler_free(ms);
    free(outputbuf);
    return 0;
}
