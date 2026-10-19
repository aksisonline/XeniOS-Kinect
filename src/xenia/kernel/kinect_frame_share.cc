/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/kinect_frame_share.h"

#include <atomic>
#include <cstring>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"

#if !XE_PLATFORM_WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

DEFINE_bool(kinect_share_frames, false,
            "Publish Kinect depth/colour frames to /tmp/xenia_kinect_frames for "
            "the external pose tracker.",
            "Kinect");

namespace xe {
namespace kernel {

namespace {

constexpr size_t kDepthBytes = 640 * 480 * 2;
constexpr size_t kColorBytes = 640 * 480;
constexpr size_t kDepthOffset = 0x40;
constexpr size_t kColorOffset = kDepthOffset + kDepthBytes;
constexpr size_t kFileSize = kColorOffset + kColorBytes;

#if !XE_PLATFORM_WIN32
uint8_t* Mapping() {
  static uint8_t* mapping = []() -> uint8_t* {
    if (!cvars::kinect_share_frames) return nullptr;
    int fd = open("/tmp/xenia_kinect_frames", O_RDWR | O_CREAT, 0666);
    if (fd < 0 || ftruncate(fd, kFileSize) != 0) {
      XELOGW("Kinect frame share: cannot create /tmp/xenia_kinect_frames");
      return nullptr;
    }
    void* p = mmap(nullptr, kFileSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return nullptr;
    std::memcpy(p, "KFRM", 4);
    XELOGI("Kinect frame share: publishing to /tmp/xenia_kinect_frames");
    return static_cast<uint8_t*>(p);
  }();
  return mapping;
}
#else
uint8_t* Mapping() { return nullptr; }  // ponytail: Windows not wired up
#endif

}  // namespace

bool KinectShareEnabled() { return cvars::kinect_share_frames; }

void KinectSharePublishDepth(const uint16_t* raw) {
  uint8_t* m = Mapping();
  if (!m) return;
  std::memcpy(m + kDepthOffset, raw, kDepthBytes);
  auto* seq = reinterpret_cast<std::atomic<uint32_t>*>(m + 4);
  seq->fetch_add(1, std::memory_order_release);
}

void KinectSharePublishColor(const uint8_t* bayer) {
  uint8_t* m = Mapping();
  if (!m) return;
  std::memcpy(m + kColorOffset, bayer, kColorBytes);
  auto* seq = reinterpret_cast<std::atomic<uint32_t>*>(m + 8);
  seq->fetch_add(1, std::memory_order_release);
}

}  // namespace kernel
}  // namespace xe
