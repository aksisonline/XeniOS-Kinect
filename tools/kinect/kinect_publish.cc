// Streams the Kinect to /tmp/xenia_kinect_frames (same layout Xenia publishes with
// --kinect_share_frames) so the pose tracker/viewer can be developed without the game.
// Build: clang++ -std=c++20 -O2 -Ishim -I/opt/homebrew/include $(pkg-config --cflags libusb-1.0) kinect_publish.cc \
//          usb_camera_libusb.cc $(pkg-config --libs libusb-1.0) -o kinect_publish
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include "xenia/hid/kinect/usb_camera.h"

using namespace xe::hid::kinect;

int main() {
  auto cam = UsbCamera::Open(false, [] {});
  if (!cam) { std::fprintf(stderr, "no camera (is Xenia still running?)\n"); return 1; }
  constexpr size_t kDepth = 640 * 480 * 2, kColor = 640 * 480, kOffset = 0x40, kSize = kOffset + kDepth + kColor;
  int fd = open("/tmp/xenia_kinect_frames", O_RDWR | O_CREAT, 0666);
  if (fd < 0 || ftruncate(fd, kSize)) { std::perror("frames file"); return 2; }
  auto* m = static_cast<uint8_t*>(mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
  std::memcpy(m, "KFRM", 4);
  std::puts("publishing Kinect frames to /tmp/xenia_kinect_frames (Ctrl-C to stop)");

  std::vector<uint8_t> packed, bayer;
  uint32_t depth_seq = 0, color_seq = 0;
  for (;;) {
    bool any = false;
    if (uint32_t s = cam->CopyDepthFrame(depth_seq, packed)) {
      depth_seq = s; any = true;
      auto* out = reinterpret_cast<uint16_t*>(m + kOffset);
      uint32_t bits = 0; int count = 0; size_t in = 0;
      for (int i = 0; i < 640 * 480; ++i) {          // 11-bit MSB-first stream
        while (count < 11) { bits = (bits << 8) | packed[in++]; count += 8; }
        count -= 11;
        uint16_t v = (bits >> count) & 0x7FF;
        out[i] = v == 2047 ? 0 : v;
      }
      reinterpret_cast<std::atomic<uint32_t>*>(m + 4)->fetch_add(1, std::memory_order_release);
    }
    if (uint32_t s = cam->CopyColorFrame(color_seq, bayer)) {
      color_seq = s; any = true;
      std::memcpy(m + kOffset + kDepth, bayer.data(), kColor);
      reinterpret_cast<std::atomic<uint32_t>*>(m + 8)->fetch_add(1, std::memory_order_release);
    }
    if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}
