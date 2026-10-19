// Standalone check: open Kinect via the PR #1274 libusb code, dump 1 depth + 1 color frame.
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>
#include "xenia/hid/kinect/usb_camera.h"

using namespace xe::hid::kinect;

int main() {
  auto cam = UsbCamera::Open(false, [] {});
  if (!cam) { std::fprintf(stderr, "no camera\n"); return 1; }
  const auto& c = cam->calibration();
  std::printf("calib: ref_dist=%.2f ref_px=%.4f emitter=%.2f rcmos=%.2f shift=%u\n",
              c.reference_distance, c.reference_pixel_size, c.dcmos_emitter_distance,
              c.dcmos_rcmos_distance, c.const_shift);

  std::vector<uint8_t> depth, bayer;
  for (int i = 0; i < 100 && !(cam->CopyDepthFrame(0, depth) && cam->CopyColorFrame(0, bayer)); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  if (depth.empty() || bayer.empty()) { std::fprintf(stderr, "no frames in 5s\n"); return 2; }

  // 11-bit packed depth is an MSB-first bitstream. 2047 = no reading.
  FILE* f = std::fopen("depth.pgm", "wb");
  std::fprintf(f, "P5 640 480 255\n");
  uint32_t valid = 0;
  for (int i = 0; i < 640 * 480; ++i) {
    int bit = i * 11, byte = bit >> 3, sh = bit & 7;
    uint32_t w = (depth[byte] << 16) | (depth[byte + 1] << 8) | (byte + 2 < (int)depth.size() ? depth[byte + 2] : 0);
    uint32_t v = (w >> (24 - 11 - sh)) & 0x7FF;
    valid += v != 2047;
    putc(v == 2047 ? 0 : 255 - (v >> 3), f);  // near = bright
  }
  std::fclose(f);
  std::printf("depth: %u/%d pixels valid\n", valid, 640 * 480);

  // Bayer GRBG, half-res RGB (one pixel per 2x2 block).
  f = std::fopen("color.ppm", "wb");
  std::fprintf(f, "P6 320 240 255\n");
  for (int y = 0; y < 240; ++y)
    for (int x = 0; x < 320; ++x) {
      const uint8_t* p = &bayer[(2 * y) * 640 + 2 * x];
      putc(p[1], f); putc((p[0] + p[640 + 1]) / 2, f); putc(p[640], f);
    }
  std::fclose(f);
  std::puts("wrote depth.pgm color.ppm");
  return 0;
}
