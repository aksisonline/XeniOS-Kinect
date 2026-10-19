/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstdlib>
#include <cstring>

#include "xenia/base/logging.h"
#include "xenia/cpu/processor.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xthread.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/ui/imgui_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"
#include "xenia/xbox.h"

DECLARE_bool(allow_kinect_camera);

DEFINE_bool(nui_tilt_events, true,
            "Send the initial tilt controller messages to the title's tilt callback.",
            "Kernel");

namespace xe {
namespace kernel {
namespace xam {
// https://web.cs.ucdavis.edu/~okreylos/ResDev/Kinect/MainPage.html

struct X_NUI_DEVICE_STATUS {
  /* Notes:
     - for one side func of XamNuiGetDeviceStatus
       - if some data addressis less than zero then unk1 = it
       - else another func is called and its return can set unk1 = c0051200 or
     some value involving DetroitDeviceRequest
       - next PsCamDeviceRequest is called and if its return is less than zero
     then X_NUI_DEVICE_STATUS = return of PsCamDeviceRequest
       - else it equals an unknown local_1c
       - finally McaDeviceRequest is called and if its return is less than zero
     then unk2 = return of McaDeviceRequest
       - else it equals an unknown local_14
     - status can be set to X_NUI_DEVICE_STATUS[3] | 0x44 or | 0x40
  */
  xe::be<uint32_t> unk0;
  xe::be<uint32_t> unk1;
  xe::be<uint32_t> unk2;
  xe::be<uint32_t> status;
  xe::be<uint32_t> unk4;
  xe::be<uint32_t> unk5;
};
static_assert(sizeof(X_NUI_DEVICE_STATUS) == 24, "Size matters");

// Get
dword_result_t XamNuiGetDeviceStatus_entry(
    pointer_t<X_NUI_DEVICE_STATUS> status_ptr) {
  /* Notes:
     - it does return a value that is not always used
     - returns values are X_ERROR_SUCCESS, 0xC0050006, and others
     - 1) On func start *status_ptr = 0, status_ptr->unk1 = 0, status_ptr->unk2
     = 0, and status_ptr->status = 0
     - 2) calls XamXStudioRequest(6,&var <- = 0);
     - if return is greater than -1 && var & 0x80000000 != 0 then set
     status_ptr->unk1 = 0xC000009D, status_ptr->unk2 = 0xC000009D, and
     status_ptr->status = status_ptr[3] = 0x20
     - lots of branching functions after
  */

  status_ptr.Zero();

  const bool kinect_initialized =
      kernel_state()->xconfig()->ReadSetting<uint32_t>(
          X_CONFIG_CATEGORY::XCONFIG_USER_CATEGORY, XCONFIG_USER_RETAIL_FLAGS) &
      X_RETAIL_FLAGS::KinectInitialized;

  status_ptr->status = kinect_initialized;
  return kinect_initialized ? X_ERROR_SUCCESS : 0xC0050006;
}
DECLARE_XAM_EXPORT1(XamNuiGetDeviceStatus, kNone, kStub);

dword_result_t XamXStudioRequest_entry(dword_t request_type,
                                       lpdword_t result_ptr) {
  // Return failure to indicate XStudio/dev features not available.
  static bool logged = false;
  if (!logged) {
    logged = true;
    XELOGW("XamXStudioRequest called, returning X_E_FAIL (logged once)");
  }
  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamXStudioRequest, kNone, kStub);

dword_result_t XamUserNuiGetUserIndex_entry(unknown_t unk, lpdword_t index) {
  return X_E_NO_SUCH_USER;
}
DECLARE_XAM_EXPORT1(XamUserNuiGetUserIndex, kNone, kStub);

dword_result_t XamUserNuiGetUserIndexForSignin_entry(lpdword_t index) {
  for (uint32_t i = 0; i < XUserMaxUserCount; i++) {
    auto profile = kernel_state()->xam_state()->GetUserProfile(i);
    if (profile) {
      *index = i;
      return X_E_SUCCESS;
    }
  }

  return X_E_ACCESS_DENIED;
}
DECLARE_XAM_EXPORT1(XamUserNuiGetUserIndexForSignin, kNone, kImplemented);

dword_result_t XamUserNuiGetUserIndexForBind_entry(lpdword_t index) {
  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamUserNuiGetUserIndexForBind, kNone, kStub);

dword_result_t XamNuiGetDepthCalibration_entry(lpdword_t unk1) {
  /* Notes:
     - Possible returns X_STATUS_NO_SUCH_FILE, and 0x10000000
  */
  return X_STATUS_NO_SUCH_FILE;
}
DECLARE_XAM_EXPORT1(XamNuiGetDepthCalibration, kNone, kStub);

// Skeleton
qword_result_t XamNuiSkeletonGetBestSkeletonIndex_entry(int_t unk) {
  return 0xffffffffffffffff;
}
DECLARE_XAM_EXPORT1(XamNuiSkeletonGetBestSkeletonIndex, kNone, kStub);

/* XamNuiCamera Notes
   - most require message calls to xam in 0x0002Bxxx area
*/

struct X_NUI_TILT_STATUS {
  xe::be<uint32_t> size;
  xe::be<uint32_t> signature;  // 'XtSs'
  xe::be<uint32_t> unknown_08[6];
  // Gravity direction.
  xe::be<int32_t> accelerometer[3];
  // 0x1 and 0x2 moving, 0x4 stalled, 0x8 error.
  xe::be<uint32_t> flags;
  xe::be<uint32_t> unknown_30[8];
};
static_assert_size(X_NUI_TILT_STATUS, 0x50);

dword_result_t XamNuiCameraTiltGetStatus_entry(lpvoid_t status_ptr) {
  /* Notes:
     - Used by XamNuiCameraElevationGetAngle, and XamNuiCameraSetFlags
     - Both funcs send in a param of *unk = 0x50 bytes to copy
     - Titles use the gravity vector for floor detection
  */
  // ponytail: always a level, motionless camera. A real reading would come from
  // the sensor's accelerometer (libfreenect freenect_get_mks_accel).
  constexpr int32_t kAccelerometerOneG = 819;
  auto status = status_ptr.as<X_NUI_TILT_STATUS*>();
  if (!status || status->size < sizeof(X_NUI_TILT_STATUS) ||
      status->signature != make_fourcc("XtSs")) {
    static int rejected_log_count = 0;
    if (status && rejected_log_count++ < 3) {
      XELOGW("XamNuiCameraTiltGetStatus: rejected request size={:08X} sig={:08X}",
             uint32_t(status->size), uint32_t(status->signature));
    }
    return X_E_FAIL;
  }
  std::memset(status->unknown_08, 0, sizeof(status->unknown_08));
  int32_t accel[3] = {0, kAccelerometerOneG, 0};
  xboxkrnl::GetKinectAccelerometer(accel);  // keeps the level default if absent
  static int logged = 0;
  static uint64_t tilt_status_calls = 0;
  if (++tilt_status_calls % 100 == 0) {
    XELOGI("XamNuiCameraTiltGetStatus call #{}", tilt_status_calls);
  }
  if (logged++ < 3) {
    XELOGI("XamNuiCameraTiltGetStatus: accelerometer {} {} {}", accel[0],
           accel[1], accel[2]);
  }
  status->accelerometer[0] = accel[0];
  status->accelerometer[1] = accel[1];
  status->accelerometer[2] = accel[2];
  status->flags = 0;
  std::memset(status->unknown_30, 0, sizeof(status->unknown_30));
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiCameraTiltGetStatus, kNone, kStub);

dword_result_t XamNuiCameraElevationGetAngle_entry(lpdword_t angle_ptr,
                                                   lpdword_t unk2) {
  /* Notes:
     - Xam 12611 does not show what unk1 is used for (Ghidra)
     - Titles poll this every frame as a "Kinect attached" probe: a failure
       makes them open the NUI troubleshooter. They read *angle_ptr as a
       signed 32-bit angle (Kinect Sports).
  */
  if (cvars::allow_kinect_camera) {
    // ponytail: no tilt motor, report a level camera.
    if (!angle_ptr || !unk2) {
      return X_E_INVALIDARG;
    }
    *angle_ptr = 0;
    *unk2 = 0;
    return X_ERROR_SUCCESS;
  }
  X_NUI_TILT_STATUS tilt_status = {};
  tilt_status.size = sizeof(tilt_status);
  tilt_status.signature = make_fourcc("XtSs");
  X_STATUS result = XamNuiCameraTiltGetStatus_entry(&tilt_status);
  if (XSUCCEEDED(result)) {
    // operation here
    // *unk1 = output1
    // *unk2 = output2
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamNuiCameraElevationGetAngle, kNone, kStub);

// The sensor has no tilt motor here; titles only need these calls to succeed.
dword_result_t XamNuiCameraTiltSetCallback_entry(lpvoid_t callback,
                                                 lpvoid_t context) {
  XELOGI("XamNuiCameraTiltSetCallback({:08X}, {:08X})",
         callback.guest_address(), context.guest_address());
  // The title's tilt controller is a state machine driven by XAM messages
  // 0x2B005-0x2B00B sent to this callback as (message, buffer). Without them it
  // never leaves its initial state. ponytail: only a fixed "ready, level" start
  // sequence; no motor events since the camera has no motor here.
  if (cvars::nui_tilt_events && callback) {
    auto* thread = XThread::GetCurrentThread();
    auto* buffer = kernel_state()->memory()->TranslateVirtual<uint32_t*>(
        kernel_state()->memory()->SystemHeapAlloc(0x20));
    std::memset(buffer, 0, 0x20);
    const uint32_t buffer_address =
        kernel_state()->memory()->HostToGuestVirtual(buffer);
    for (uint32_t message : {0x2B009u, 0x2B005u}) {
      uint64_t args[] = {message, buffer_address};
      uint64_t result = kernel_state()->processor()->Execute(
          thread->thread_state(), callback.guest_address(), args,
          xe::countof(args));
      XELOGI("Tilt callback message {:X} -> {:08X}", message,
             uint32_t(result));
    }
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiCameraTiltSetCallback, kNone, kStub);

dword_result_t XamNuiCameraRememberFloor_entry(lpvoid_t unk1, dword_t unk2) {
  static int logged = 0;
  // Debug aid (XENIA_FIND_FLOOR=1): find where the title keeps the floor plane
  // it just reported, to locate its NUI_SKELETON_FRAME (vFloorClipPlane).
  static const bool find_floor = std::getenv("XENIA_FIND_FLOOR") != nullptr;
  if (find_floor && logged < 2) {
    uint32_t value = unk1.guest_address();
    Memory* memory = kernel_state()->memory();
    int hits = 0;
    for (uint64_t base = 0x40000000; base < 0xA0000000ull && hits < 12;
         base += 0x1000) {
      xe::BaseHeap* heap = memory->LookupHeap(uint32_t(base));
      uint32_t protect = 0;
      if (!heap || !heap->QueryProtect(uint32_t(base), &protect) || !protect) {
        continue;
      }
      const uint32_t* page = memory->TranslateVirtual<const uint32_t*>(uint32_t(base));
      for (uint32_t i = 0; i < 0x400 && hits < 12; ++i) {
        if (xe::byte_swap(page[i]) == value) {
          uint32_t address = uint32_t(base) + i * 4;
          std::string context;
          const uint32_t* around = page + i;
          for (int k = -8; k < 24 && (int(i) + k) >= 0 && (i + k) < 0x400; ++k) {
            context += fmt::format("{:08X} ", uint32_t(xe::byte_swap(around[k])));
          }
          XELOGI("Floor value found at {:08X}: {}", address, context);
          ++hits;
        }
      }
    }
  }
  if (find_floor && logged < 6) {
    // The title's NuiSensor wrapper object is at *0x832A124C; its copy of the
    // SDK skeleton frame (0xAB0 bytes) is at +0x2060.
    Memory* memory = kernel_state()->memory();
    uint32_t object = xe::load_and_swap<uint32_t>(memory->TranslateVirtual(0x832A124C));
    if (object) {
      const uint8_t* frame = memory->TranslateVirtual(object + 0x2060);
      std::string text;
      for (uint32_t i = 0; i < 0x80; i += 4) {
        text += fmt::format("{:08X} ", uint32_t(xe::load_and_swap<uint32_t>(frame + i)));
        if (i % 16 == 12) text += "\n    ";
      }
      XELOGI("Skeleton frame copy at {:08X}:\n    {}", object + 0x2060, text);
    }
  }
  if (logged++ < 20) {
    const uint32_t* words = unk1.as<uint32_t*>();
    XELOGI(
        "XamNuiCameraRememberFloor({:08X}, {}): {:08X} {:08X} {:08X} {:08X} "
        "{:08X} {:08X}",
        unk1.guest_address(), uint32_t(unk2),
        words ? uint32_t(xe::byte_swap(words[0])) : 0,
        words ? uint32_t(xe::byte_swap(words[1])) : 0,
        words ? uint32_t(xe::byte_swap(words[2])) : 0,
        words ? uint32_t(xe::byte_swap(words[3])) : 0,
        words ? uint32_t(xe::byte_swap(words[4])) : 0,
        words ? uint32_t(xe::byte_swap(words[5])) : 0);
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiCameraRememberFloor, kNone, kStub);

dword_result_t XamNuiCameraElevationSetAngle_entry(dword_t angle,
                                                   dword_t unk2) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiCameraElevationSetAngle, kNone, kStub);

dword_result_t XamNuiCameraElevationStopMovement_entry() {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiCameraElevationStopMovement, kNone, kStub);

dword_result_t XamNuiCameraTiltReportStatus_entry(lpvoid_t unk1) {
  static int logged = 0;
  if (logged++ < 5) {
    XELOGI("XamNuiCameraTiltReportStatus({:08X})", unk1.guest_address());
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiCameraTiltReportStatus, kNone, kStub);

dword_result_t XamNuiCameraGetTiltControllerType_entry() {
  /* Notes:
     - undefined unk[8]
     - undefined8 local_28;
     - undefined8 local_20;
     - undefined8 local_18;
     - undefined4 local_10;
     - local_20 = 0;
     - local_18 = 0;
     - local_10 = 0;
     - local_28 = 0xf030000000000;
     - calls DetroitDeviceRequest(unk) -> result
     - returns (ulonglong)(LZCOUNT(result) << 0x20) >> 0x25
  */
  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamNuiCameraGetTiltControllerType, kNone, kStub);

dword_result_t XamNuiCameraSetFlags_entry(qword_t unk1, dword_t unk2) {
  /* Notes:
     - if XamNuiCameraGetTiltControllerType returns 1 then operation is done
     - else 0xffffffff8007048f
  */
  X_STATUS result = X_E_DEVICE_NOT_CONNECTED;
  int Controller_Type = XamNuiCameraGetTiltControllerType_entry();

  if (Controller_Type == 1) {
    X_NUI_TILT_STATUS tilt_status = {};
    tilt_status.size = sizeof(tilt_status);
    tilt_status.signature = make_fourcc("XtSs");
    result = XamNuiCameraTiltGetStatus_entry(&tilt_status);
    if (XSUCCEEDED(result)) {
      // op here
      // result =
    }
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamNuiCameraSetFlags, kNone, kStub);

dword_result_t XamIsNuiUIActive_entry() {
  return kernel_state()->xam_state()->xam_nui_dialogs_shown_ > 0;
}
DECLARE_XAM_EXPORT1(XamIsNuiUIActive, kNone, kImplemented);

dword_result_t XamNuiIsDeviceReady_entry() {
  /* device_state Notes:
   - used with XNotifyBroadcast(kXNotificationSystemNUIHardwareStatusChanged,
   device_state)
   - known values:
     - 0x0001
     - 0x0004
     - 0x0040
  */
  const bool kinect_initialized =
      kernel_state()->xconfig()->ReadSetting<uint32_t>(
          X_CONFIG_CATEGORY::XCONFIG_USER_CATEGORY, XCONFIG_USER_RETAIL_FLAGS) &
      X_RETAIL_FLAGS::KinectInitialized;

  return kinect_initialized >> 1 & 1;
}
DECLARE_XAM_EXPORT1(XamNuiIsDeviceReady, kNone, kImplemented);

dword_result_t XamIsNuiAutomationEnabled_entry(unknown_t unk1, unknown_t unk2) {
  /* Notes:
     - XamIsNuiAutomationEnabled = XamIsNatalPlaybackEnabled
     - Always returns X_E_SUCCESS? Maybe check later versions
     - Recieves param but never interacts with them
     - No Operations
  */
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT2(XamIsNuiAutomationEnabled, kNone, kStub, kHighFrequency);

dword_result_t XamIsNatalPlaybackEnabled_entry(unknown_t unk1, unknown_t unk2) {
  /* Notes:
     - XamIsNuiAutomationEnabled = XamIsNatalPlaybackEnabled
     - Always returns X_E_SUCCESS? Maybe check later versions
     - Recieves param but never interacts with them
     - No Operations
  */
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT2(XamIsNatalPlaybackEnabled, kNone, kStub, kHighFrequency);

dword_result_t XamNuiIsChatMicEnabled_entry() {
  /* Notes:
     - calls a second function with a param of uint local_20 [4];
     - Second function calls ExGetXConfigSetting(7,9,local_30,0x1c,local_40);
     - Result is sent to *local_20[0] = ^
     - Once sent back to XamNuiIsChatMicEnabled it looks for byte that
     correlates to NUI mic setting
     - return uVar2 = (~(ulonglong)local_20[0] << 0x20) >> 0x23 & 1;
     - unless the second function returns something -1 or less then
     XamNuiIsChatMicEnabled 1
  */
  return false;
}
DECLARE_XAM_EXPORT1(XamNuiIsChatMicEnabled, kNone, kImplemented);

/* HUD Notes:
   - XamNuiHudGetEngagedTrackingID, XamNuiHudIsEnabled,
   XamNuiHudSetEngagedTrackingID, XamNuiHudInterpretFrame, and
   XamNuiHudGetEngagedEnrollmentIndex all utilize the same data address
   - engaged_tracking_id set second param of XamShowNuiTroubleshooterUI
*/
uint32_t nui_unknown_1 = 0;
uint32_t engaged_tracking_id = 0;
char nui_unknown_2 = '\0';

dword_result_t XamNuiHudSetEngagedTrackingID_entry(dword_t id) {
  if (!id) {
    return X_STATUS_SUCCESS;
  }

  if (nui_unknown_1 != 0) {
    engaged_tracking_id = id;
    return X_STATUS_SUCCESS;
  }

  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamNuiHudSetEngagedTrackingID, kNone, kImplemented);

qword_result_t XamNuiHudGetEngagedTrackingID_entry() {
  if (nui_unknown_1 != 0) {
    return engaged_tracking_id;
  }

  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamNuiHudGetEngagedTrackingID, kNone, kImplemented);

dword_result_t XamNuiHudIsEnabled_entry() {
  /* Notes:
     - checks if XamNuiIsDeviceReady false, if nui_unknown_1 exists, and
     nui_unknown_2 is equal to null terminated string
     - only returns true if one check fails and allows for other NUI functions
     to progress
  */
  bool result = XamNuiIsDeviceReady_entry();
  if (nui_unknown_1 != 0 && nui_unknown_2 != '\0' && result) {
    return true;
  }
  return false;
}
DECLARE_XAM_EXPORT1(XamNuiHudIsEnabled, kNone, kImplemented);

uint32_t XeXamNuiHudCheck(dword_t unk1) {
  uint32_t check = XamNuiHudIsEnabled_entry();
  if (check == 0) {
    return X_ERROR_ACCESS_DENIED;
  }

  check = XamNuiHudSetEngagedTrackingID_entry(unk1);
  if (check != 0) {
    return X_ERROR_FUNCTION_FAILED;
  }
  return X_STATUS_SUCCESS;
}

dword_result_t XamNuiHudGetInitializeFlags_entry() {
  /* HUD_Flags Notes:
     - set by 0x2B003
     - set to 0 by unnamed func alongside version_id
     - known values:
       - 0x40000000
       - 0x200
  */
  return 0;
}
DECLARE_XAM_EXPORT1(XamNuiHudGetInitializeFlags, kNone, kImplemented);

void XamNuiHudGetVersions_entry(lpqword_t unk1, lpqword_t unk2) {
  /* version_id Notes:
     - set by 0x2B003
     - set to 0 by unnamed func alongside HUD_Flags
  */
  if (unk1) {
    *unk1 = 0;
  }
  if (unk2) {
    *unk2 = 0;
  }
}
DECLARE_XAM_EXPORT1(XamNuiHudGetVersions, kNone, kImplemented);

// UI
dword_result_t XamShowNuiTroubleshooterUI_entry(dword_t user_index,
                                                dword_t tracking_id,
                                                dword_t flags) {
  /* Notes:
     - calls XamPackageManagerGetExperienceMode(&var) with var = 1
     - If returns less than zero or (var & 1) == 0 then get error message:
       - if XamPackageManagerGetExperienceMode = 0 then call XamShowMessageBoxUI
         - if XamShowMessageBoxUI returns 0x3e5 then XamShowNuiTroubleshooterUI
     returns 0
       - else XamShowNuiTroubleshooterUI returns 0x65b and call another func
     - else:
       - call XamNuiHudSetEngagedTrackingID(tracking_id) and doesn't care aboot
     return and set var2 = 2
       - checks if (flag & 0x800000) == 0
         - if true call XamNuiGetDeviceStatus.
           - if XamNuiGetDeviceStatus != 0 set var2 = 3
       - else var2 = 4
       - XamAppRequestLoadEx(var2);
       - if return = 0 then XamShowNuiTroubleshooterUI returns 5
       - else set buffer[8] and call
     XMsgSystemProcessCall(0xfe,0x21028,buffer,0xc);
     - XamNuiNatalCameraUpdateComplete calls
     XamShowNuiTroubleshooterUI(0xff,0,0) if param = -0x7ff8fffe
  */

  if (cvars::headless) {
    return 0;
  }

  const Emulator* emulator = kernel_state()->emulator();
  ui::Window* display_window = emulator->display_window();
  ui::ImGuiDrawer* imgui_drawer = emulator->imgui_drawer();
  if (display_window && imgui_drawer) {
    xe::threading::Fence fence;
    if (display_window->app_context().CallInUIThreadSynchronous([&]() {
          xe::ui::ImGuiDialog::ShowMessageBox(
              imgui_drawer, "NUI Troubleshooter",
              "The game has indicated there is a problem with NUI (Kinect).")
              ->Then(&fence);
        })) {
      kernel_state()->xam_state()->xam_dialogs_shown_++;
      fence.Wait();
      kernel_state()->xam_state()->xam_dialogs_shown_--;
    }
  }

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamShowNuiTroubleshooterUI, kNone, kStub);

dword_result_t XamShowNuiHardwareRequiredUI_entry(unknown_t unk1) {
  if (unk1 != 0) {
    return X_ERROR_INVALID_PARAMETER;
  }

  return XamShowNuiTroubleshooterUI_entry(0xff, 0, 0x400000);
}
DECLARE_XAM_EXPORT1(XamShowNuiHardwareRequiredUI, kNone, kImplemented);

dword_result_t XamShowNuiGuideUI_entry(unknown_t unk1, unknown_t unk2) {
  /* Notes:
   - calls an unnamed function that checks XamNuiHudIsEnabled and
   XamNuiHudSetEngagedTrackingID
     - if XamNuiHudIsEnabled returns false then fuctions fails return
   X_ERROR_ACCESS_DENIED
     - else calls XamNuiHudSetEngagedTrackingID and if returns less than 0 then
   returns X_ERROR_FUNCTION_FAILED
     - else return X_ERROR_SUCCESS
   - if return offunc is X_ERROR_SUCCESS then call up ui screen
   - else return value of func
  */

  // decompiler error stops me from knowing which param gets used here
  uint32_t result = XeXamNuiHudCheck(0);
  if (!result) {
    // operations here
    // XMsgSystemProcessCall(0xfe,0x21030, undefined local_30[8] ,0xc);
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamShowNuiGuideUI, kNone, kStub);

/* XamNuiIdentity Notes:
   - most require message calls to xam in 0x0002Cxxx area
*/
uint64_t NUI_Session_Id = 0;

qword_result_t XamNuiIdentityGetSessionId_entry() {
  if (NUI_Session_Id == 0) {
    // xboxkrnl::XeCryptRandom_entry(NUI_Session_Id, 8);
    NUI_Session_Id = 0xDEADF00DDEADF00D;
  }
  return NUI_Session_Id;
}
DECLARE_XAM_EXPORT1(XamNuiIdentityGetSessionId, kNone, kImplemented);

dword_result_t XamNuiIdentityEnrollForSignIn_entry(dword_t unk1, qword_t unk2,
                                                   qword_t unk3, dword_t unk4) {
  /* Notes:
     - Decompiler issues so double check
  */
  if (XamNuiHudIsEnabled_entry() == false) {
    return X_E_FAIL;
  }
  // buffer [2]
  // buffer[0] = unk
  // var = unk4
  // return func(0xfe,0x2c010,buffer,0xc,unk3);
  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamNuiIdentityEnrollForSignIn, kNone, kStub);

dword_result_t XamNuiIdentityAbort_entry(dword_t unk) {
  if (XamNuiHudIsEnabled_entry() == false) {
    return X_E_FAIL;
  }
  // buffer [4]
  // buffer[0] = unk
  // return func(0xfe,0x2c00e,buffer,4,0)
  return X_E_FAIL;
}
DECLARE_XAM_EXPORT1(XamNuiIdentityAbort, kNone, kStub);

// Other
dword_result_t XamUserNuiEnableBiometric_entry(dword_t user_index,
                                               int_t enable) {
  return X_E_INVALIDARG;
}
DECLARE_XAM_EXPORT1(XamUserNuiEnableBiometric, kNone, kStub);

void XamNuiPlayerEngagementUpdate_entry(qword_t unk1, unknown_t unk2,
                                        lpunknown_t unk3) {
  /* Notes:
     - Only calls a second function with the params unk3, 0, and 0x1c in that
     order
  */
}
DECLARE_XAM_EXPORT1(XamNuiPlayerEngagementUpdate, kNone, kStub);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(NUI);
