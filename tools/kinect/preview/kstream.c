// Streams frames to stdout: u16 depth_mm[640*480] then u8 rgb[640*480*3]. Temp preview helper.
#include <libfreenect/libfreenect_sync.h>
#include <stdio.h>
#include <string.h>
int main(void) {
  for (;;) {
    void *d, *v; uint32_t ts;
    if (freenect_sync_get_depth(&d, &ts, 0, FREENECT_DEPTH_MM)) { fprintf(stderr, "no kinect / busy\n"); return 1; }
    if (freenect_sync_get_video(&v, &ts, 0, FREENECT_VIDEO_RGB)) { fprintf(stderr, "no video\n"); return 1; }
    if (fwrite(d, 2, 640*480, stdout) != 640*480 || fwrite(v, 3, 640*480, stdout) != 640*480*1UL) return 0;
    fflush(stdout);
  }
}
