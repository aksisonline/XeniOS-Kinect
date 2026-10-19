# Project: Native Kinect v1 NUI Support for Xbox 360 Emulation on Apple Silicon (XeniOS)

## Goal
Play my own physical Xbox 360 Kinect game discs, using my real Kinect v1 sensor
for input, on my Mac (M4 Pro), through XeniOS (the Apple-focused Xenia fork),
at a playable 30-60fps. My Xbox 360 console itself is dead; the games and
sensor are legitimately owned. I'm a developer and plan to do real
implementation/reverse-engineering work, not just ask "is this possible."

## Hardware/software I have
- Mac: Apple Silicon, M4 Pro
- Kinect for Xbox 360 sensor (model 1414), with its external power adapter
- A collection of physical Kinect game discs (Xbox 360)
- A USB optical drive to read the discs
- A Windows laptop available as a fallback/cross-reference machine if something
  is Windows-only and needs to be ported
- Developer background; comfortable with C/C++, reverse engineering, GPU/shader
  debugging, build systems

## Why this is a three-layer problem (established via research, Oct 2026)

**Layer 1 — Camera device emulation (host <-> Xenia).** The Xbox 360 kernel
exposes Kinect as a camera device (`PsCamDeviceRequest`) and an audio device
(`McaDeviceRequest`). A game expects to read raw 640x480 11-bit depth frames
and 640x480 Bayer colour frames (plus a raw-to-mm calibration table) from this
device. This layer is just "get real Kinect v1 bytes off USB and hand them to
the guest in the right shape" — architecture-agnostic, not GPU-dependent.

**Layer 2 — Skeleton/body-part classification (runs INSIDE each game, on the
emulated GPU).** Each Xbox 360 Kinect title does NOT call out to a console-level
"give me a skeleton" service. Instead, the skeleton tracker is Microsoft's
actual patented Kinect algorithm (per-pixel body-part classification via
randomized decision forests, the Shotton et al. method), shipped to developers
as licensed middleware and statically linked into each title's executable.
On Xbox 360 it runs as a GPU shader: large 1D textures encode the decision
trees, and depth pixels are classified via texture fetches. Because this is
shared Microsoft middleware (not something each studio reimplemented), the
classifier logic itself is likely identical or near-identical across most
titles — what's actually "per-title" is whether Xenia's GPU emulator correctly
supports the specific instruction patterns that shared shader code uses (wide
1D texture fetches, shader-interpreter resolve-without-vertices, etc.), not a
unique reverse-engineering project per game. Open question: whether the Kinect
SDK/classifier build was perfectly stable across the console's 2010-2013
lifetime, or revised at some point (would show up as a *new* GPU-emulation gap
on some other title, not a new classifier to reverse-engineer).

**Layer 3 — Gesture/cursor UI.** Splits into two sub-cases:
  - *In-game menus*: ordinary CPU code inside each game's own binary (the
    hover-to-select state machine runs on hand-joint coordinates the game
    already has once Layer 2 works). Xenia already executes this via normal
    CPU emulation — no translator needed here, just correct skeleton data
    reaching the right memory structure/export. Unproven either way yet (no
    build has exercised an in-game Kinect menu).
  - *System-level Kinect UI* (the Xbox dashboard-style device-selector /
    message-box dialogs, i.e. `XamShowNuiMessageBoxUI`,
    `XamShowNuiDeviceSelectorUI`): this genuinely is "the console," since
    there's no game code behind it — it's XAM/firmware functionality that
    Xenia has only stubbed (current known Windows build fakes this with mouse
    clicks). This needs a real implementation inside Xenia: track a hand
    joint -> screen-space cursor -> dwell timer -> fire select, once, reusable
    across every title. The hover-select dwell-timer behavior is publicly
    documented Xbox dashboard UX, and Microsoft's own patent filings on this
    exact mechanic describe the algorithm in implementable detail (cursor
    stays within a small timing/positional boundary for a dwell period ->
    transitions to "press" state).

## Current state of the art (as of Oct 2026 research pass)
- No official Xenia, Xenia Canary, Xenia Edge, or XeniOS build has any Kinect
  support. Xenia Canary's own FAQ explicitly says Kinect is unsupported.
- The only working proof-of-concept is **knuckleslee's unofficial Xenia
  Canary fork/build ("kinect-v1-stage1")**, Windows-only, tested with exactly
  one title: Sonic Free Riders (title ID 5345084D). It talks to a real Kinect
  v1 over USB via libusb/UsbDk (reimplementing libfreenect-style protocol and
  calibration, not the Microsoft SDK), and reports a full race completed,
  steering by leaning, skeleton tracking live.
  - Build: `knuckleslee/xenia-canary`, release tag `kinect-v1-stage1-7018b91`.
  - Requires: Windows, D3D12, 1x resolution scale, model 1414 Kinect + its
    power adapter, UsbDk or WinUSB.
  - Config flags: `allow_kinect_camera = true`, `readback_resolve = "full"`,
    `readback_memexport = true`, `protect_zero = false`; enable "Kinect
    Initialized" in console settings.
  - NOT done yet in this build: tilt motor/accelerometer (faked level),
    face-based sign-in, in-game or system-UI gesture/hover selection (mouse
    is used instead), applying the game's camera settings to the sensor,
    microphone array/speech, any non-Windows platform, any title other than
    Free Riders.
  - The author states the PRs were written "heavily relying on LLM (Claude,
    Anthropic)".
- Relevant PRs against `xenia-canary/xenia-canary` (`canary_experimental`
  branch), opened ~Oct 4-7, 2026:
  | PR | What it does | Status |
  |---|---|---|
  | #1272 | Accept guest object types for camera stream objects | Equivalent landed in Canary release notes |
  | #1273 | XamShowNuiMessageBoxUI / XamShowNuiDeviceSelectorUI | Merged (c3ec0ba) |
  | #1274 | PsCamDeviceRequest backed by real Kinect v1 over USB | **Closed** by a maintainer (goldislead), Oct 7 2026, as too disorganized to review — resubmission wanted as smaller commits |
  | #1275 | LDI decompression exports | Open |
  | #1276 | XamContentOpenFile for packages on guest devices (Kinect identity data) | Open |
  | #1277 | XamNuiCameraTiltGetStatus reports a level camera | Open |
  | #1278 | Accept Kinect identity callback registration (0x2C009) | Merged |
  | #1264, #1279, #1280 | GPU emulation fixes: wide 1D texture fetches used by Kinect body-part classification; shader-interpreter texture fetches; resolves without vertices | Needed for the classifier shaders to run correctly at all |
- Other forks, less mature / different approach (host-side skeleton
  translation instead of camera emulation — NOT the approach that has
  actually produced working gameplay):
  - `KeithKirenai/xenia-canary-kinect` (branch `feature/kinect-support`) —
    loads Kinect10.dll/OpenNI2/NiTE2/libfreenect on the host, maps joints to
    a 20-joint Xbox layout, 50+ `XamNui*` stubs. Still has black-screen/frame
    issues, no working gameplay yet.
  - `tameid/xenia-canary` + `tameid/nui-provider` — out-of-process design:
    a host provider writes guest-shaped skeleton frames to a double-buffered
    file IPC, a guest-side plugin reads them via Xenia's host path device.
    Has record/replay tooling. Only synthetic/replay sources implemented so
    far (Phase 1); real sensor input is "Phase 4+".
  - `ryantimjohn/xenia-canary` — a public branch said to add a host-side NUI
    subsystem; opened xenia-canary issue #1290 ("Adding Kinect Support").
  - `weronika-saturday/xenia-canary` — earlier attempt, never got games
    working (xenia-canary #920, xenia-project #2347).
  - `YuutaTsubasa/Free-Riders-Recompiled` — NOT an emulator; a static
    recompilation (XenonRecomp/XenosRecomp) of Sonic Free Riders for
    Windows/Linux/Android that replaces the game's NUI calls with the PC
    Kinect SDK. v0.4.0 (via knuckleslee's PR #21) added real Kinect v1 input.
    Good proof the data mapping concept works, but single-game and not
    emulation.
- Known-broken Kinect titles in `xenia-project/game-compatibility` (label
  `tech-kinect-required`) — filed BEFORE any Kinect camera support existed,
  so they don't indicate whether the classifier/GPU work generalizes, only
  that nobody has tried yet: #1559 Kinect Adventures (stuck at the Kinect
  setup screen), #1558 Kinect Sports (guest crash), #1782 Kinect Sports
  Season Two, #1573 Just Dance 2015, #1125 Sonic Free Riders (now superseded
  by the working build above).

## The Mac-specific part (this is the part not yet attempted by anyone)
- **XeniOS** (github.com/xenios-jp/XeniOS, xenios.jp) is a real, separate,
  Apple-focused fork of Xenia Edge — iOS + macOS (Apple Silicon and Intel),
  publicly released March 8, 2026. No Kinect/NUI code exists in it yet.
- **Xenia Edge** (`has207/xenia-edge` or similar) ships native macOS arm64 and
  x86_64 builds already; this is the lineage XeniOS is based on. General
  macOS compatibility is weaker than Windows/Linux Xenia Canary for some
  titles, but it is native and should give the M4 Pro full use of its CPU/GPU
  (Metal) without a Windows/Wine translation layer in between.
- **libfreenect** (OpenKinect project) is the relevant open driver for Kinect
  v1 on macOS: stable at 0.7.5, with Homebrew-prebuilt Apple Silicon packages
  from Monterey through current macOS. Gives RGB, depth, motor, LED, and
  accelerometer data — no skeleton tracking (that's Layer 2, done by the
  game itself on the emulated GPU, not by the driver).
  - One caveat found in community projects built on libfreenect: macOS's
    core-audio layer grabbing the Kinect's USB audio interface has, in rare
    cases, caused instability. Worth isolating/disabling that interface if
    it's not needed yet (microphone/voice is listed as not-yet-implemented
    in Layer 1 anyway).
  - libfreenect2 is for Kinect v2 (Xbox One) only — not relevant to this
    sensor.
- **No one has ported knuckleslee's camera-device code off Windows.** It
  currently talks to the sensor via libusb through UsbDk/WinUSB specifically.
  The actual sensor protocol (frame request/response, depth/colour format,
  calibration) is reimplemented on top of libusb rather than using the
  Microsoft SDK, which is actually good news for portability — libusb itself
  is cross-platform and libfreenect on macOS proves the same USB protocol
  works fine through macOS's native USB stack. The porting work is mostly:
  swap the Windows-specific USB backend (UsbDk/WinUSB open/claim/transfer
  calls) for libusb's macOS backend (or link against libfreenect directly and
  reuse its battle-tested transfer/calibration code instead of
  reimplementing it), and make sure this lands inside XeniOS/Xenia Edge's
  camera-device plumbing (equivalent of the closed #1274) rather than
  xenia-canary's.
- **Disc reading:** Xbox 360 discs are not plain data DVDs — they use a
  challenge-response "Security Sector" scheme, which is why 360 disc
  backup/ripping historically required specific drive firmware/hardware
  (certain Samsung/TSST/BenQ drives, or Kreon-firmware drives, were the
  known-working tools in the Xbox 360 modding community) rather than any
  generic USB DVD drive. **This is a real, separate unknown to validate
  early** — check whether your specific USB optical drive's chipset/firmware
  can actually read the raw security sectors, or whether you'll need a
  Kreon-flashed drive (commonly a Samsung SH-D162C or similar) to get clean
  ISO/game dumps Xenia can load. This is independent of all the Kinect work
  above and worth resolving first since it gates "can I even load the game"
  at all.
- **Performance expectation:** once the base game runs on XeniOS and the
  classifier shaders execute correctly (Layer 2), the Kinect work itself adds
  relatively little GPU/CPU overhead (a camera-frame decode and the existing
  decision-tree shader work the game already budgets for) — 30-60fps should
  be realistic on an M4 Pro for titles that already run acceptably on Xenia
  in general, since the bottleneck is Xenia's CPU/GPU emulation of the base
  Xbox 360 hardware, not the Kinect layer on top.

## Proposed phased plan
1. **De-risk disc reading first.** Dump 2-3 of the owned discs with the
   available USB drive; confirm you get clean, Xenia-loadable images before
   investing in anything else. If the drive can't read security sectors,
   solve that (Kreon-firmware drive) before continuing.
2. **Stand up a baseline.** Get XeniOS (or Xenia Edge) running at all on the
   M4 Pro with a non-Kinect Xbox 360 title first, to confirm general
   emulation performance/compatibility on this Mac independent of Kinect.
3. **Port the camera-device layer.** Take knuckleslee's closed #1274
   (camera device backed by real Kinect v1 over USB) as the reference
   implementation, and reimplement its USB transfer layer against libusb's
   macOS backend (or link libfreenect directly for the transfer/calibration
   code) instead of UsbDk/WinUSB. Land this inside XeniOS/Xenia Edge's
   equivalent kernel camera-device code.
4. **Bring over the supporting XAM/NUI exports** from the merged/open PRs
   (#1272, #1273, #1275-#1278) that aren't Windows-specific — these are
   kernel/XAM-level and should port with little or no change.
5. **Validate Layer 2 (classifier) on Mac/Metal.** Run Sonic Free Riders
   first (known-working reference) to confirm the GPU fixes (#1264, #1279,
   #1280 equivalents) are present/ported and the decision-tree shaders
   execute correctly under XeniOS's Metal backend, not just D3D12.
6. **Pick a second title** (Kinect Adventures or Kinect Sports are good
   candidates — simple, menu-driven, already flagged as blocked in the
   compat tracker) to test whether the classifier genuinely generalizes
   across titles once camera + GPU support is solid, or whether it exposes a
   new GPU-emulation gap.
7. **Implement the system-level gesture/cursor engine** (Layer 3, system
   dialogs) as a generic Xenia-side feature: hand-joint -> screen cursor ->
   dwell timer -> select, replacing the current mouse-click stand-in. This
   is reusable across every Kinect title and isn't gated on per-title GPU
   work.
8. **Only then** chase in-game hover-select menus per title, debugging
   whichever specific missing export/stub each title turns out to need.

## Open questions to resolve empirically, not by more research
- Does the USB drive on hand actually read Xbox 360 security sectors, or is
  a Kreon-firmware drive required?
- Does a second Kinect title die on the same GPU instruction patterns
  knuckleslee already fixed, or a new one?
- Does XeniOS's Metal backend handle the same shader patterns D3D12 does, or
  does Metal translation introduce its own gap?
- Was the Kinect SDK/classifier build stable across 2010-2013, or did any
  title ship a meaningfully different decision-tree/shader version?

## Key links
- https://github.com/xenia-canary/xenia-canary (main dev, Windows/Linux)
- https://github.com/knuckleslee/xenia-canary (Kinect camera work; see PRs
  #1272-#1280 and release tag kinect-v1-stage1-7018b91)
- https://github.com/xenios-jp/XeniOS and https://xenios.jp (Mac/iOS fork,
  no Kinect yet — this is where the porting work above would land)
- https://github.com/OpenKinect/libfreenect (Kinect v1 driver, Apple Silicon
  builds via Homebrew)
- https://github.com/YuutaTsubasa/Free-Riders-Recompiled (reference for how
  Kinect data maps into one specific title's NUI calls)
- https://github.com/tameid/nui-provider and tameid/xenia-canary (alternate
  out-of-process skeleton-provider architecture, in case camera-emulation
  approach hits a wall)
- xenia-canary issues #920, #1188, #1290; xenia-project/xenia issues #1247,
  #2339, #2347; xenia-project/game-compatibility issues #1559, #1558,
  #1782, #1573, #1125
