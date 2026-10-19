# Xbox 360 Kinect Support in Xenia: Status and Path Forward, October 2026

Someone has already played a Kinect game with a real Xbox 360 Kinect through
Xenia, but only one game (Sonic Free Riders), only on Windows, and only with
an unofficial test build. The work is spread across a set of open (and
partly rejected) xenia-canary pull requests, and no official Xenia, Canary,
Edge or XeniOS build supports Kinect yet. The most important technical
finding changes what you need to build. On the Xbox 360, the game's own NUI
library computes the skeleton from raw camera frames, partly on the GPU. The
approach that has actually worked so far therefore feeds the game raw depth
and colour frames at the kernel camera interface. It does not translate
skeletons from the Kinect SDK or OpenNI into some NUI format.

## TL;DR
- **Status:** knuckleslee's unofficial "kinect-v1-stage1" Xenia Canary build
  is the only setup shown playing a Kinect title (Sonic Free Riders,
  5345084D) with a real Kinect for Xbox 360. It is Windows-only and tested
  on that one title. Its main camera PR (#1274) was closed by a maintainer
  on Oct 7, 2026 as "really, really, really disorganized", though some
  smaller NUI PRs have been merged into Canary.
- **What to build:** emulate the camera device (`PsCamDeviceRequest`) and
  the XAM NUI exports. Then feed the game raw 640x480 11-bit depth and Bayer
  colour from libfreenect-style USB access, and let the game's own NUI
  runtime do the skeleton tracking. The host-side skeleton-translation forks
  (KeithKirenai, tameid, ryantimjohn) are earlier-stage and unproven on real
  games.
- **Platform:** use your Windows laptop (for initial validation). XeniOS is
  real: it is an Apple-focused fork of Xenia Edge, not a mishearing. Xenia
  now runs natively on macOS through Edge and XeniOS, but no Mac build has
  any Kinect code. libfreenect does work on Apple Silicon, so a Mac port of
  the camera path is feasible — see project-plan.md for the porting plan.

## Key Findings

### 1. Official Xenia / Xenia Canary status
- The Xenia Canary FAQ still says: "Xenia does not support mice, accessories
  like the Kinect, uDraw GameTablet, or microphones."
- Upstream `xenia-project/xenia` has an NUI module
  (`src/xenia/kernel/xam/xam_nui.cc`, copyright 2022). It is mostly stubs
  plus a "NUI Troubleshooter" dialog: "The game has indicated there is a
  problem with NUI (Kinect)."
- On xenia-canary issue #1188 ("Kinect games working on Xenia", Aug 2026),
  Canary collaborator goldislead answered: "Other than a few unfinished
  forks that have popped up this past year, no main Xenia fork currently
  supports Kinect."
- **Merged into Canary so far:** release notes list "[XAM/NUI] - Improve NUI
  (21faab8)", "[XAM] Implement XamShowNuiMessageBoxUI and
  XamShowNuiDeviceSelectorUI (c3ec0ba)" and "[Kernel] Use guest object types
  in ObReferenceObjectByHandle (04f8541)". Knuckleslee's overview describes
  #1278 (Kinect identity callback 0x2C009) as merged. These are plumbing
  pieces, not usable Kinect support.

### 2. XeniOS is a real, separate project
XeniOS (github.com/xenios-jp/XeniOS, xenios.jp) is "an experimental
Apple-focused fork of Xenia, currently based on Xenia Edge."
- **Platforms:** iOS, plus macOS on both Apple Silicon and Intel.
- **Launch:** publicly released March 8, 2026, needs JIT, and is sideloaded
  on iOS.
- **Other platforms:** its site points Windows users to Xenia Canary and
  Linux users to Xenia Edge.
- **Kinect:** no Kinect or NUI work found in XeniOS. Kinect support would
  have to arrive through Canary/Edge code flowing downstream, or be built
  directly into XeniOS/Edge as this project plans to do.

### 3. The knuckleslee PR series (the most advanced real-hardware effort)
Opened Oct 4, 2026 against `xenia-canary:canary_experimental`. The overview
comment says that with all PRs combined, "the title tracks my skeleton,
goes through sign-in... and I finished a full three-lap race (2nd place)
through to the results screen, steering by leaning." It also reports "about
11,000 frame requests in 3 minutes... with no USB transfer errors."

| PR | What it does | Status (as found) |
|---|---|---|
| #1272 | Accept guest object types in ObReferenceObjectByHandle (camera stream objects) | Equivalent change in Canary release notes |
| #1273 | XamShowNuiMessageBoxUI / XamShowNuiDeviceSelectorUI | Appears merged (c3ec0ba) |
| #1274 | PsCamDeviceRequest backed by a real Kinect v1 over USB | **Closed** by goldislead, Oct 7, 2026 |
| #1275 | LDI decompression exports | Open |
| #1276 | XamContentOpenFile for packages on guest devices (Kinect identity data) | Open |
| #1277 | XamNuiCameraTiltGetStatus reports a level camera | Open |
| #1278 | Accept Kinect identity callback registration (0x2C009) | Merged |
| #1279 / #1280 | GPU shader-interpreter texture fetches; resolves without vertices | Open |
| #1264 | Fix wide 1D texture fetches used by Kinect body-part classification | Referenced |

- **Test build:** "Kinect v1 stage 1 test build (7018b91)" on
  knuckleslee/xenia-canary.
- **Requirements:** Windows, D3D12, 1x resolution scale, a model 1414 Kinect
  with its power adapter, and UsbDk or WinUSB.
- **Config:** `allow_kinect_camera = true`, `readback_resolve = "full"`,
  `readback_memexport = true`, `protect_zero = false`. Turn on "Kinect
  Initialized" in Console Settings.
- **Not done yet:**
  - the tilt motor and accelerometer (a level camera is faked),
  - face-based sign-in,
  - selecting buttons in the Kinect system UI with gestures (use the mouse),
  - applying the game's camera settings to the sensor,
  - the microphone array and speech,
  - non-Windows platforms,
  - any title other than 5345084D.
- **Caveat:** the author says the PRs were written "heavily relying on LLM
  (Claude, Anthropic)".

### 4. Other forks and side projects
- **KeithKirenai/xenia-canary-kinect** (branch `feature/kinect-support`)
  takes the host-skeleton approach.
  - It dynamically loads Kinect10.dll (SDK 1.8), OpenNI2/NiTE2 or
    libfreenect.
  - It maps "15-joint OpenNI2 to 20-joint Xbox Layout" or produces a
    synthetic T-pose.
  - It has "50+ XamNui* function stubs", PsCamDeviceRequest/McaDeviceRequest
    interfaces "returning successful states", and handlers for XAM messages
    0x2B001-0x2B005, 0x2C009, 0x2C00C, 0x2C00D, 0x58004 and 0x58035.
  - Targets are NUIView and Fruit Ninja Kinect (58410B79). It is still
    fixing black-screen/frame-loop problems, so no gameplay yet. It is
    AI-assisted and "not meant to be a production-grade" implementation.
- **tameid/xenia-canary** ("Testing fork of Xenia with Kinect support
  (vibeslopped)") plus **tameid/nui-provider** is a clean out-of-process
  design.
  - A host provider writes "guest-shaped" 20-joint skeleton frames into a
    double-buffered file IPC (`frame.idx`, `frame.0.bin`, `frame.1.bin`,
    latest-frame-wins, atomic rename).
  - A guest-side plugin reads that directory through Xenia's host path
    device.
  - The wire format is frozen, packed little-endian; the guest plugin
    byte-swaps because the console is big-endian. A two-skeleton frame is
    1,648 bytes.
  - It has record/replay (`.nui` sessions, a 60-byte 'XNRS' header) and an
    ASCII skeleton inspector.
  - Only Phase 1 exists (synthetic and replay sources). Webcam, phone and
    hardware sources are "Phase 4+". Could not read the design doc
    (`docs/kinect_support.md` on branch `nui-spike`), so could not confirm
    which exports it hooks.
- **ryantimjohn/xenia-canary** has a public branch that, according to
  knuckleslee, "adds a host-side NUI subsystem". ryantimjohn also opened
  xenia-canary issue #1290, "Adding Kinect Support" (Oct 12, 2026). Code not
  inspected.
- **weronika-saturday/xenia-canary** is an earlier attempt (tested with
  Kinect Joy Ride and Kinect Adventures) that never worked. It led to
  xenia-canary #920 and xenia-project #2347.
- **YuutaTsubasa/Free-Riders-Recompiled** is not an emulator. It is a
  GPL-3.0 static recompilation (XenonRecomp/XenosRecomp) of Sonic Free
  Riders for Windows, Linux and Android.
  - It replaces the game's NUI library calls with the PC Kinect SDK.
  - v0.4.0 (knuckleslee's PR #21) added real Kinect v1 input: "native
    skeletons, depth and colour streams reach the game's own gesture
    detectors". This needs the full Kinect for Windows SDK 1.8.
  - It is good proof that the data mapping works, but it is game-specific.

### 5. Relevant issue and PR index
- **xenia-canary:**
  - #537 [Feature Request] Kinect Support (Feb 2025)
  - #920 "I need help adding Kinect support" (closed as not planned;
    Gliniak's list of required exports is in it)
  - #1188 Kinect games working on Xenia
  - #1290 Adding Kinect Support
  - PR #740 [XAM/NUI] Improve NUI Handling (The-Little-Wolf)
  - PRs #1264 and #1272-#1280
- **xenia-project/xenia:**
  - #1247 XamContentOpenFile / ContentManager (2018), which notes it is
    "probably required for NUI (Kinect) to initialize properly"
  - PR #2317 "Implement XamContentOpenFile" (BalkoBalkho, Sep 26, 2025, open)
  - #2339 "Add support for Kinect." (Jan 31, 2026)
  - #2347 "I need help adding Kinect support..." (Mar 26, 2026)
  - The RetroDECK wiki also cites a "Xenia Issue: 1241", not verified.
- **xenia-project/game-compatibility** (label `tech-kinect-required`):
  - #1559 Kinect Adventures (stuck at the Kinect setup screen)
  - #1558 Kinect Sports (guest crash)
  - #1782 Kinect Sports Season Two
  - #1573 Just Dance 2015
  - #1125 Sonic Free Riders (intro loops because there is no Kinect cursor)

## Details: How the Xbox 360 NUI Stack Actually Works (what a shim must emulate)
Putting the PRs, Gliniak's notes and issue #1247 together, a Kinect title
depends on four layers.

1. **Kernel camera device.** `PsCamDeviceRequest` takes a request block.
   Codes 0, 27, 14 (or 11/13 depending on version) and 25 handle setup, and
   code 5 queues frame reads.
   - Depth: "640x480 raw 11-bit disparity in 16-bit words".
   - Colour: "640x480 Bayer".
   - Code 24 is a raw-to-millimetre table, code 2 stops streaming, and codes
     35/36 are camera settings.
   - `McaDeviceRequest` appears to cover the audio side.
2. **XAM NUI exports and messages.** These include
   `XamNuiCameraTiltGetStatus`, whose status buffer begins with size 0x50
   and 'XtSs'; the accelerometer is at +32/+36/+40 and flags at +44, and "1
   g is about 819" units. They also include
   `XamNuiSkeletonGetBestSkeletonIndex`, `XamNuiGetDepthCalibration`,
   `XamShowNui*UI`, and XAM messages 0x2B001-0x2B005 and 0x2C009 (an
   identity callback that handles 0x2C002-0x2C013).
3. **Content and identity files.** NUI calls
   `XamContentOpenFile(0xFF, "stexemplar", "game:\Database.xmplr", 0x43...)`
   and `"nuiidentity"`/`NuiIdentity.bin.be`. These mount PIRS/STFS packages
   from the disc. Gliniak also listed EtxProducerRegister,
   XamXStudioRequest, LDICreateDecompression, XamNuiCameraTiltSetCallback,
   XamReadBiometricData and IO requests 0002A007, 0002A008 and 0002C009 as
   needed. He added that fixing one bug is "just the beginning".
4. **The title's own NUI library, running on emulated hardware.** The
   skeleton tracker ships inside each game and runs body-part
   classification on the GPU. In Sonic Free Riders that means "three 786432
   -texel k_8_8_8_8 1D textures as decision trees". This is why knuckleslee
   also had to fix Xenia GPU bugs (#1264, #1279, #1280).

Why this matters: a pure "translator" that injects Kinect SDK or OpenNI
skeletons has to intercept the game's internal NUI calls, which differ per
title. The recompilation project gets away with this only because it has
one game's symbols. Emulating the camera device instead is title-agnostic
and only needs libfreenect-level raw data. It does depend on Xenia's GPU
emulation being accurate enough for the decision forests. Expect each new
title to expose more missing XAM exports, content-mounting gaps and GPU
edge cases, not a different data format.

### Architectural precedents
- **RPCS3** splits camera and motion input into pluggable handlers. RPCS3 PR
  #11016, "Add Qt camera handler" by Megamouse (merged Oct 24, 2021), "Adds
  the possibility to use a selected camera as PS Eye or EyeToy", and RPCS3
  announced the same day that "you can use any camera you want with it :)".
  RPCS3 PR #15788, "Implement Real PS Move Handler" by Megamouse (shipped in
  v0.0.35 Alpha), "Adds real PS Move camera tracking (well, as good as I was
  able to for now)" along with a "Real" Move handler in the IO settings tab
  and a tracker configuration dialog. This matches the
  camera-device-emulation model.
- **Dolphin and Cemu** use the DSU ("cemuhook") UDP protocol. Separate
  server processes (phones, the Steam Deck, Joy-Cons) stream motion data to
  the emulator's input layer. This matches the out-of-process provider
  model that tameid/nui-provider is reaching for.
- **Recommended hybrid:** emulate the camera (and later audio and motor)
  devices inside Xenia, and put the actual sensor access behind a pluggable
  host backend. That backend could be in-process libusb or an
  out-of-process provider over IPC/UDP, which also enables record/replay
  and Mac support.

## Details: Drivers and Platforms
- **Windows:**
  - The knuckleslee build talks to the Kinect directly with libusb via
    UsbDk (or a WinUSB binding) and reimplements libfreenect's protocol and
    calibration math. It does not use the Microsoft SDK.
  - The Kinect for Windows SDK 1.8 is still needed for the host-skeleton
    forks and for Free-Riders-Recompiled. Derivative's TouchDesigner
    "Kinect1" user guide says: "Xbox360 (models 1414 and 1473), although
    not officially supported, usually work by installing the Kinect for
    Windows SDK 1.8. The Kinect Runtime 1.8 will NOT work with the Xbox360
    models and they will not be detected, the full SDK must be installed."
  - Windows 11 reports are mixed. Users on Microsoft Q&A describe "no frame
    data" and `kinectcamera.sys` load failures, and a TroikaTronix user
    solved theirs with a clean reinstall.
  - Microsoft's own documentation says a Kinect for Xbox 360 shows
    "DeviceNotSupported" on end-user PCs without the SDK.
  - Don't bind the same sensor to WinUSB/UsbDk and the SDK driver at the
    same time.
- **macOS:**
  - libfreenect (OpenKinect, Kinect v1) is at stable 0.7.5 on Homebrew
    (depending on libusb 1.0.30). The Homebrew formulae page lists
    prebuilt Apple Silicon packages for Monterey through Tahoe, but Intel
    packages only up to Sonoma, so a recent Intel Mac would build from
    source. It provides RGB, depth, motor, LED and accelerometer data, but
    no skeleton tracking.
  - libfreenect2 is only for the Kinect v2 (Xbox One) and does nothing for
    this sensor.
  - Mac projects built on libfreenect, such as FreenectTD and the "Kinect
    360 TOX", work on Apple Silicon. One warns that macOS's USB audio
    driver grabbing the Kinect's audio interface "in rare cases... can
    panic the Mac."
  - Microsoft's SDK and NiTE skeleton tracking are Windows-only. The
    host-skeleton approach is therefore unworkable on a Mac without a VM,
    as studioHEX/Macinect does.
- **Xenia on Mac:**
  - Xenia Edge (has207) ships macOS arm64 and x86_64 builds, though its
    README warns "Many games will fail on macOS that run on Windows and
    Linux."
  - XeniOS is the Apple-focused option. wmarti/xenia-mac (Metal backend,
    macOS 15+) is an earlier experiment.
  - Whisky's GitHub README now states "Whisky is no longer actively
    maintained. Apps and games may break at any time," and the repository
    was archived on May 11, 2025. CrossOver is the maintained Wine route,
    but passing Kinect USB through Wine is an unnecessary layer now that
    native builds exist.

## Recommendations
1. **Play now (Windows, for validation):** try knuckleslee's
   kinect-v1-stage1 build with Sonic Free Riders, using your own disc dump.
   If you own Free Riders, the Free-Riders-Recompiled port with SDK 1.8 is
   the more polished way to play it on Windows. Expect no other titles to
   work yet.
2. **Contribute where it counts:** build on the camera-device approach.
   - Rebase the closed #1274 into smaller, cleaner commits, as the
     maintainer asked. Split the `hid/kinect` USB layer out on its own, as
     knuckleslee offered.
   - Then pick a second title you own (Kinect Adventures, Kinect Sports)
     and trace which exports, content mounts and GPU paths fail. That turns
     a one-game hack into general support.
3. **Make the sensor backend pluggable** (in-process libusb or an
   out-of-process provider). Borrow tameid's frozen-format, record/replay
   and inspector ideas. Recorded depth sessions let you debug without a
   sensor and make a macOS backend (libfreenect via Homebrew) easy.
4. **Still to build from scratch:**
   - the tilt motor and accelerometer,
   - the microphone array and speech (McaDeviceRequest; voice commands are
     untouched),
   - identity/biometric sign-in,
   - gesture input in the system UI,
   - applying camera settings,
   - console-accurate status codes,
   - a non-Windows USB path (the Mac port is this project's focus),
   - broad title coverage.
5. **Engage upstream early.** Post in the Xenia Discord #dev channel and on
   #920/#1290 before large PRs. Expect scrutiny of AI-generated code, given
   how #1274 was received.

## Caveats
- Almost all Kinect-emulation evidence is from the last few weeks (late
  Sep-Oct 2026). It comes from single developers, often AI-assisted, with
  no independent reproduction. Merge status may have changed since these
  pages were read.
- The "skeleton computed by the game's own library on the GPU" model is
  backed by knuckleslee's tracing of one title. Other titles or later SDK
  versions could behave differently.
- Could not read the tameid design doc, the ryantimjohn branch code or the
  2022 Gliniak/ChrisNonyminus research, which is likely on Discord.
- Kinect for Windows SDK 1.8 support on Windows 11 is unofficial and
  inconsistent.

## Sources
1. https://github.com/xenia-canary/xenia-canary/wiki/FAQ
2. https://github.com/xenia-project/xenia/blob/master/src/xenia/kernel/xam/xam_nui.cc
3. https://github.com/xenia-canary/xenia-canary/issues/1188
4. https://github.com/xenia-canary/xenia-canary/releases
5. https://github.com/xenios-jp/XeniOS
6. https://xenios.jp/
7. https://www.windowscentral.com/gaming/xbox/xbox-360-games-can-now-run-on-iphone-ipad-and-mac-thanks-to-a-new-app
8. https://pocket-gaming.org/2026/03/08/xenios-brings-xbox-360-emulation-to-apple-devices-today/
9. https://github.com/xenia-canary/xenia-canary/pull/1274
10. https://github.com/xenia-canary/xenia-canary/pull/1272
11. https://github.com/xenia-canary/xenia-canary/pull/1273
12. https://github.com/xenia-canary/xenia-canary/pull/1277
13. https://github.com/xenia-canary/xenia-canary/pull/1278
14. https://github.com/xenia-canary/xenia-canary/pull/1264
15. https://github.com/knuckleslee/xenia-canary/releases/tag/kinect-v1-stage1-7018b91
16. https://github.com/xenia-canary/xenia-canary/pull/1276
17. https://github.com/KeithKirenai/xenia-canary-kinect
18. https://github.com/tameid/xenia-canary
19. https://github.com/tameid/nui-provider
20. https://github.com/xenia-canary/xenia-canary/issues
21. https://github.com/xenia-canary/xenia-canary/issues/920
22. https://github.com/xenia-project/xenia/issues/2347
23. https://github.com/YuutaTsubasa/Free-Riders-Recompiled
24. https://emulationking.com/microsoft/xbox360/games/sonicfreeriders/ports/freeridersrecompiled/
25. https://github.com/YuutaTsubasa/Free-Riders-Recompiled/releases/tag/v0.4.0
26. https://github.com/xenia-canary/xenia-canary/issues/537
27. https://github.com/xenia-canary/xenia-canary/pull/740
28. https://github.com/xenia-project/xenia/issues/1247
29. https://github.com/xenia-project/xenia/pulls
30. https://github.com/xenia-project/xenia/issues
31. https://retrodeck.readthedocs.io/en/latest/wiki_controllers/xbox/xbox-360-kinect/
32. https://github.com/xenia-project/game-compatibility/issues/1559
33. https://github.com/xenia-project/game-compatibility/issues/1558
34. https://github.com/xenia-project/game-compatibility/issues/1782
35. https://github.com/xenia-project/game-compatibility/issues/1573
36. https://github.com/xenia-project/game-compatibility/issues/1125
37. https://wiki.dolphin-emu.org/index.php?title=DSU_Client
38. https://www.resetera.com/threads/steam-deck-ot-your-games-are-going-places.556834/page-222
39. https://github.com/joaorb64/joycond-cemuhook
40. https://learn.microsoft.com/en-us/answers/questions/1182972/xbox-360-kinect-no-frame-data-on-windows-11
41. https://community.troikatronix.com/topic/8747/solved-kinect-1414-on-windows-11-setup-issue
42. https://learn.microsoft.com/en-us/previous-versions/windows/kinect-1.8/hh855358(v=ieb.10)
43. https://github.com/OpenKinect/libfreenect2
44. https://github.com/zarvox/libfreenect
45. https://github.com/yeshbourne/libfreenect2
46. https://github.com/stosumarte/FreenectTD
47. https://derivative.ca/community-post/tutorial/kinect-360-tox-macos/74435
48. https://github.com/studioHEX/Macinect
49. https://github.com/SirLynix/obs-kinect/issues/9
50. https://github.com/Creesic/xenia-edge
51. https://github.com/goldislead/xenia-edge
52. https://github.com/wmarti/xenia-mac/releases
53. https://emulation.gametechwiki.com/index.php/Xenia
