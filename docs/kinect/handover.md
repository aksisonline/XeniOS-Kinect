# Handover: Kinect v1 NUI on XeniOS (Mac M4 Pro) — continue in Claude Code

## Where this stands right now
This project folder was set up and populated from a Claude.ai chat session
(research + planning done there, files dropped here via the device bridge).
Picking this up in Claude Code now. Everything below is what's already done
and what was mid-flight when the handover happened.

## What's in this folder
- `docs/project-plan.md` — the actual plan: 3-layer breakdown of the Kinect
  NUI problem (camera device emulation / GPU skeleton classifier / gesture
  UI), current state of the art, and the phased porting plan targeting
  XeniOS on this Mac (M4 Pro, Apple Silicon).
- `docs/research-report.md` — full research dump backing the plan: PR-by-PR
  status on xenia-canary's Kinect work, every known fork and its approach,
  the Xbox 360 NUI stack internals (kernel camera device, XAM exports,
  content mounting, GPU shader classifier), driver/platform notes for
  Windows and macOS, architectural precedents from RPCS3/Dolphin/Cemu,
  and a full source list.
- `forks/` — 7 cloned reference repos (trimmed: no `.git` history, no PPC
  test fixtures/large binary assets, to keep the transfer small):
  - `XeniOS/` — **the actual target** to modify. Apple-focused Xenia Edge
    fork, macOS arm64/Intel + iOS. No Kinect code in it yet.
  - `xenia-canary-knuckleslee/` — **the reference implementation to port**.
    This is the only build anyone has gotten actually playing a Kinect game
    (Sonic Free Riders) with a real sensor. Windows-only currently; talks to
    the Kinect over libusb/UsbDk, not the MS SDK. Has the GPU emulation
    fixes needed for the game's own body-part-classifier shaders to run
    (wide 1D texture fetches etc). See project-plan.md's PR table for exact
    commit/PR references.
  - `xenia-canary-kinect/` (KeithKirenai) — alternate, less-proven approach:
    host-side skeleton translation (Kinect SDK/OpenNI → synthetic skeleton)
    instead of camera-device emulation. No working gameplay yet. Kept as a
    fallback reference if the camera-device approach hits a wall.
  - `xenia-canary-tameid/` + `nui-provider/` — another alternate approach:
    out-of-process skeleton provider over file IPC. Also unproven on real
    games, kept for its record/replay tooling ideas (useful for debugging
    without the sensor attached).
  - `Free-Riders-Recompiled/` — not an emulator; single-title static
    recompilation that proves the data-mapping concept (real Kinect v1 →
    game's NUI calls) works, game-specific only.
  - `libfreenect/` — **the macOS-side driver to build on**. Kinect v1 driver,
    actively works on Apple Silicon via Homebrew (0.7.5 stable). This is
    what the Mac port of the camera-device layer should use instead of
    knuckleslee's Windows-specific UsbDk/WinUSB backend.

## What was just interrupted (pick up here first)
User said they'd connected the physical Kinect v1 sensor to "my laptop" —
this session is linked to the Mac (M4 Pro, darwin/arm64), so that's almost
certainly what they meant, but worth a quick confirm if ambiguous (they also
have a Windows laptop mentioned in project-plan.md).

Was in the middle of checking whether the sensor is visible over USB on the
Mac, to answer "can we install the official drivers and check if it's
sending data?" The answer to give once this is picked up:
- **There are no official Microsoft Kinect drivers for macOS.** The Kinect
  for Windows SDK (incl. its drivers) is Windows-only; Kinect v1 was never
  officially supported on Mac at all. The real path on Mac is
  **libfreenect** (already cloned into `forks/libfreenect/`), which is the
  open-source driver this whole project's Layer-1 plan depends on anyway —
  so "install official drivers" isn't a thing here, but "install libfreenect
  and confirm the sensor streams data" is exactly the right next step and
  doable right now.

**Important environment gotcha discovered during this session:** the
`device_bash` tool (used for file operations in this chat) is a **sandboxed
Linux VM**, not the Mac's real shell — `uname -a` returned Linux/aarch64,
and macOS-only tools (`system_profiler`, etc.) aren't present there. It can
only see files under the connected folder (`~/mnt/Projects` →
a local projects folder). It has **no USB passthrough**, so
it cannot be used to check if the Kinect is connected.

To actually check USB/driver state on the real Mac, options are:
1. **AppleScript via the "Control your Mac" tool** (`osascript`) — can run
   `do shell script "..."` to execute real macOS commands like
   `system_profiler SPUSBDataType` or `ioreg -p IOUSB -l`. Mid-attempt this
   hit a quoting/escaping error (nested double-quotes inside the
   `do shell script` string broke AppleScript's parser) — needs the shell
   command string properly escaped for AppleScript (e.g. use single quotes
   inside the shell command, or escape embedded double quotes as `\"`, or
   write the shell command to a temp script file first and just call that
   file from `do shell script`).
2. Once Claude Code is running **locally on the Mac itself** (not through
   this cloud-session device-bridge), this whole indirection disappears —
   just run `system_profiler SPUSBDataType | grep -i kinect` or
   `ioreg -p IOUSB -l | grep -i kinect` directly in a normal terminal. This
   is probably the simpler path going forward.

## Suggested next steps (in order)
1. Confirm the Kinect v1 is enumerating over USB on the Mac at all
   (`system_profiler SPUSBDataType`, looking for "Xbox NUI Camera" /
   "Xbox NUI Motor" / "Xbox NUI Audio" — Kinect v1 shows up as **three**
   separate USB devices, not one).
2. Install libfreenect via Homebrew (`brew install libfreenect`) and run its
   bundled test utility (e.g. `freenect-glview` or `freenect-wavrecord` if
   available) to confirm actual depth/RGB frames are streaming — this is the
   real "is it sending data" check, not a driver install, since there's no
   separate official driver on Mac.
3. Per project-plan.md: once the sensor path is confirmed, start on getting
   a baseline XeniOS build running on the M4 Pro with any non-Kinect title,
   before touching the camera-device porting work.
4. Also still outstanding from the plan: validate whether the USB optical
   drive on hand can actually read Xbox 360 disc security sectors (separate
   from all Kinect work, gates loading any game at all).

## Links (also in research-report.md)
- Target to modify: https://github.com/xenios-jp/XeniOS
- Reference implementation to port: https://github.com/knuckleslee/xenia-canary
  (branch `canary_experimental`, PRs #1272-#1280, release tag
  `kinect-v1-stage1-7018b91`)
- Mac driver: https://github.com/OpenKinect/libfreenect
