# Plan: port knuckleslee PR #1274 (PsCamDeviceRequest over Kinect v1 USB) to XeniOS-build

Source: https://github.com/xenia-canary/xenia-canary/pull/1274 (closed, head `knuckleslee:kinect-pscam`,
11 files, +1339/-2). Diff saved locally while studying; re-fetch with
`gh pr diff 1274 --repo xenia-canary/xenia-canary`.

## What the PR actually is
- `hid/kinect/usb_camera.h`: tiny interface. `UsbCamera::Open(use_usbdk, on_frame)`,
  `calibration()`, `CopyDepthFrame(after, buf)`, `CopyColorFrame(after, buf)`.
  Depth = 640x480 packed 11-bit, color = 640x480 Bayer, 30 fps.
- `usb_camera_win.cc` (~580 lines): **plain libusb 1.0, not libfreenect.** VID 045E, PID 02AE, iso endpoints
  0x81 color / 0x82 depth, 'GM'/'RB' command packets over vendor control transfers, own `FrameAssembler`,
  reads calibration (registration, ref distance, pixel size) itself. Only Windows-specific line:
  `libusb_set_option(LIBUSB_OPTION_USE_USBDK)`.
- `usb_camera_posix.cc`: stub returning nullptr (`TODO: libusb only built for Windows`).
- `xboxkrnl_pscam.cc` (~650 lines): the real work. Guest-facing `PsCamDeviceRequest` emulation: request
  codes 0 state, 2 stop, 5 read frame, 13 version, 14 registration, 15 time sync, 24 depth table, 25 timing,
  27/33 no-op, 35/36 controls. A host worker thread completes queued frame reads from the camera.
  cvars: `allow_kinect_camera`, `kinect_use_usbdk`.
- Plus CMake wiring, export-group entry, `xboxkrnl_private.h`, `xbox.h` structs.

## Decision: reuse the PR's libusb code, don't link libfreenect
The Windows file is already portable libusb 1.0 and is the only code proven to play a game. libfreenect would
mean re-deriving the same protocol behind a different API (callback-driven, own threads, its own frame
format conversion) and the PR's frame/calibration path would need rewriting. So:
`usb_camera_win.cc` -> rename `usb_camera_libusb.cc`, `#ifdef _WIN32` around the USBDK option. libfreenect
stays as the protocol reference (`src/usb_libusb10.c`, `cameras.c`, `registration.c`) for debugging.

## Port steps (XeniOS-build)
1. **libusb on macOS.** `third_party/CMakeLists.txt:855` builds libusb "Windows only". Dev shortcut: link
   Homebrew libusb (`brew install libusb` already done, 1.0.30) via pkg-config. Later: vendor the
   darwin backend (`os/darwin_usb.c`, `events_posix.c`, `threads_posix.c`) so the .app is self-contained.
2. **Add files** under `src/xenia/hid/kinect/` and `src/xenia/kernel/xboxkrnl/xboxkrnl_pscam.cc`;
   add `add_subdirectory(hid/kinect)` and the kernel source in the CMakeLists.
3. **Remove the stub** `PsCamDeviceRequest_entry` in `xboxkrnl_video.cc:526` (it returns X_E_FAIL), and
   register the new one via `xboxkrnl_module_export_groups.inc`.
4. **API drift to fix** (XeniOS is an Edge fork, canary APIs differ): `kernel_state->GetSystemProcess()`
   returns `uint32_t` in XeniOS (PR passes it to `XHostThread` as a pointer); check `XHostThread`
   ctor, `object_ref`, `xe_mutex`, `memory_->LookupHeap`, `DEFINE_bool` category arg, `X_STATUS_*` names.
5. **macOS USB specifics.** No driver claims the camera on macOS (libfreenect already streams there).
   Packet loss seen earlier in glview: keep PR's 8 transfers x 32 iso packets, tune if frames drop;
   test on a direct port, not a hub. Camera needs no firmware upload (only the audio device does).
6. **NUI XAM plumbing** (`xam_nui.cc`): `XamNuiGetDeviceStatus` / `XamNuiIsDeviceReady` read
   "kinect initialized" from a state the camera device must set. Confirm what the PR changes there
   (it is not in the 11-file list, so it comes from #1272/#1273/#1275-#1278, plan step 4).

## Verification ladder (cheap first)
1. Build compiles with `allow_kinect_camera=false`: no behavior change.
2. Standalone harness: link `usb_camera_libusb.cc` into a 30-line CLI, dump one depth + one color frame to
   PGM/PPM and eyeball them. Confirms the libusb path on M4 without booting a game.
3. `allow_kinect_camera=true`, launch any title, check log for "PsCamDeviceRequest" request codes
   0, 13, 14, 25 arriving in order and the camera worker streaming.
4. Sonic Free Riders (known good on Windows) for the full path, needs a disc image.

## Known unknowns
- PR was closed as disorganized; the `release tag kinect-v1-stage1-7018b91` build has later fixes not in the
  PR diff. Diff against the `canary_experimental` branch in `forks/xenia-canary-knuckleslee` once the
  PsCam code is located there (the trimmed copy only has the table entry + NUI comments).
- GPU side (render_target_cache / readback_resolve / wide 1D fetch fixes) is plan step 5, separate.
