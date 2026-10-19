/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_KINECT_SKELETON_H_
#define XENIA_KERNEL_KINECT_SKELETON_H_

#include <cstdint>

#include "xenia/cpu/module.h"

namespace xe {
namespace kernel {

class KernelState;

// Kinect v1 skeleton tracking runs inside the title (its linked NUI library),
// driven by a GPU classifier that is not reliable here. For known titles this
// replaces the library's NuiSkeletonGetNextFrame with a host function that
// writes an NUI_SKELETON_FRAME (0xAB0 bytes) from a host skeleton source.
void InstallKinectSkeletonHooks(KernelState* kernel_state,
                                cpu::Module* module, uint32_t title_id);

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_KINECT_SKELETON_H_
