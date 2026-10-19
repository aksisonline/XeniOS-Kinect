/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_KINECT_FRAME_SHARE_H_
#define XENIA_KERNEL_KINECT_FRAME_SHARE_H_

#include <cstdint>

namespace xe {
namespace kernel {

// Publishes the Kinect camera frames to a memory-mapped file so an external
// tool (the pose tracker) can read them: the sensor is claimed by this process,
// so nothing else can open it. Layout of /tmp/xenia_kinect_frames (little
// endian):
//   0x00 u32 magic 'KFRM'   0x04 u32 depth sequence   0x08 u32 color sequence
//   0x40 u16[640*480] raw 11-bit depth (0 = no data)
//   0x40 + 614400 u8[640*480] Bayer GRBG colour
// Enabled by --kinect_share_frames.
bool KinectShareEnabled();
void KinectSharePublishDepth(const uint16_t* raw_640x480);
void KinectSharePublishColor(const uint8_t* bayer_640x480);

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_KINECT_FRAME_SHARE_H_
