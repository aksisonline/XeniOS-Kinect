# XeniOS + Kinect v1 on macOS (experimental fork)

An experimental fork of [XeniOS](https://github.com/xenios-jp/XeniOS) that runs **Xbox 360 Kinect v1 games on a Mac**
with a real Kinect sensor. Everything lives on the `kinect-support` branch.

> **Status: experimental, one title.** Kinect Sports (`4D5308C9`) boots, sees the Kinect depth/colour streams,
> detects the floor, tracks a live player and drives its menu hand cursor. Other Kinect titles are untested, and
> the skeleton hooks are address-specific to that one disc revision. This is a research project, not a finished
> product. No game data is included: bring your own legally obtained dump.

## What works

| Area | State |
|---|---|
| Sensor on macOS | libusb driver for the Kinect v1 (depth, colour, accelerometer, LED) |
| Camera device (`PsCamDeviceRequest`) | emulated, frames delivered to the title |
| NUI/XAM plumbing | LZX `LDI*` exports, mutants, STFS `XamContentOpenFile`, tilt/floor, identity messages, synthetic avatar |
| GPU classifier path | ported (Metal) but **not reliable**; the title's GPU skeleton classifier does not produce a player |
| **Skeleton** | injected: a live external tracker feeds joints into the title's own `NuiSkeletonGetNextFrame` result |
| Hand cursor (menus) | computed from the tracked hand |
| Trackers | Python (MediaPipe) and native Swift (Apple Vision 3D pose + Kinect depth) with viewers |

## How it works

Skeleton tracking on the Xbox 360 runs **inside the title** (its linked NUI library), driven by a GPU decision-forest
classifier; the OS has no skeleton API. Because the classifier path is not reliable on the Metal backend, the
skeleton is supplied from outside:

```
Kinect --libusb--> Xenia (PsCamDeviceRequest) --> game (depth for its own floor estimate)
                       |
                       +-- --kinect_share_frames --> /tmp/xenia_kinect_frames (mmap: depth + Bayer colour)
                                                          |
                          tools/kinect tracker (Python or Swift) <--+
                                |  pose -> 20 Kinect joints, filtered
                                +-- UDP 127.0.0.1:5760 --> Xenia --kinect_live_skeleton-->
                                    patches the game's skeleton getter call site, runs the real SDK
                                    function (keeps the real floor) and writes the player into slot 0
```

Code: `src/xenia/kernel/kinect_skeleton.cc` (hooks, hand cursor), `kinect_frame_share.cc`, `xboxkrnl_pscam.cc`,
`src/xenia/hid/kinect/`. Reverse-engineering notes are in `docs/kinect/progress.md`.

## Requirements

- Apple Silicon Mac, **macOS 14+** (the native tracker uses Vision's 3D body pose)
- Xbox 360 **Kinect v1** with the USB power adapter, plugged into a direct USB port
- `brew install libusb`, Xcode command line tools, and the usual XeniOS build requirements
- Python 3.9+ and [`uv`](https://github.com/astral-sh/uv) for the optional Python tracker
- A legally obtained Kinect Sports (`4D5308C9`) disc image

## Build and run

```sh
# emulator (see docs/building.md for prerequisites)
./xb build --config=release

# 1. the game, publishing the sensor frames and accepting a live skeleton
Xenia-edge --kinect_live_skeleton=true --kinect_share_frames=true --allow_kinect_camera=true \
  --allow_avatar_initialization=true --synthetic_avatar=true --allow_nui_initialization=true \
  --protect_zero=false <game.iso>

# 2a. native tracker + viewer (Swift, Apple Vision, lowest latency)
tools/kinect/tracker-native/build.sh && tools/kinect/tracker-native/kinect_tracker_native
#     viewer-only by default; press `s` in its window to start sending to the game.
#     keys: [ ]  smoothing at rest, , .  speed response, ; '  depth smoothing, r reset

# 2b. or the Python tracker (MediaPipe)
cd tools/kinect && uv venv .venv-tracker && uv pip install --python .venv-tracker/bin/python -r requirements.txt
uv run --python .venv-tracker/bin/python kinect_tracker.py --view --z-offset 0.8
```

Stand ~2 m from the sensor facing it. `--z-offset` pushes the tracked player back for small rooms. In the game,
press X once to leave "Controller mode" so the hand cursor works. The Python tracker learns the empty background
at startup (step out of view for ~10 s).

To develop the trackers **without the game**, stream the sensor yourself (the game must not be running, it owns the
sensor):

```sh
cd tools/kinect && clang++ -std=c++20 -O2 -Ishim -I/opt/homebrew/include $(pkg-config --cflags libusb-1.0) \
  kinect_publish.cc usb_camera_libusb.cc $(pkg-config --libs libusb-1.0) -o kinect_publish && ./kinect_publish
```

## Known issues

- Only Kinect Sports (this disc revision) is hooked; addresses are hard-coded in `kinect_skeleton.cc`.
- Menu poster/tile textures render with wrong colours on the Metal backend (also with the unmodified GPU code).
- The tracked skeleton is a *pose model*, not Kinect's depth classifier: arms and hands are the weakest part.
- No Kinect microphone array, no identity/face recognition, no gestures beyond the hand cursor.
- Hand mapping, sensitivities and filters are tuned by feel (`--kinect_cursor_gain_*`).

## Credits and licensing

- [Xenia](https://github.com/xenia-project/xenia) (BSD-3-Clause, see `LICENSE`) and [XeniOS](https://github.com/xenios-jp/XeniOS).
- The Kinect device, LDI, content, NUI and GPU-resolve work builds on **knuckleslee's** xenia-canary Kinect pull requests
  (#1264, #1266, #1272 to #1280), adapted to XeniOS and its Metal backend.
- Depth conversion and calibration formulas follow [libfreenect](https://github.com/OpenKinect/libfreenect).
- Pose models: [Apple Vision](https://developer.apple.com/documentation/vision) and [MediaPipe](https://github.com/google-ai-edge/mediapipe) (Apache-2.0, optional).

This project is not affiliated with Microsoft, Xenia, XeniOS or Apple. It contains no game code or data.
