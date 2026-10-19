# Progress (as of Oct 19, 2026)

Build: `XeniOS-build/` (fresh clone of xenios-jp/XeniOS + our changes, `./xb build --config=release`).
Test title: Kinect Sports (4D5308C9) from an ISO. Sensor: Kinect v1 on a direct USB port of the M4 Mac.

Run line that gets furthest:
```
Xenia-edge --allow_kinect_camera=true --allow_avatar_initialization=true --synthetic_avatar=true \
  --allow_nui_initialization=true --readback_resolve=full --readback_memexport=true --protect_zero=false <iso>
```

## Done (all verified by running the game)
| Area | Change | Files |
|---|---|---|
| Sensor driver | libusb camera layer from knuckleslee PR #1274, cross-platform, verified with a standalone dump (depth + colour frames) | `src/xenia/hid/kinect/usb_camera_libusb.cc`, `harness/` |
| Camera device | `PsCamDeviceRequest` emulation (state, configure, controls, depth table, version, registration, timing, frame reads) | `xboxkrnl_pscam.cc` |
| LZX | `LDICreate/Decompress/Reset/Destroy` (7-arg create, per-frame decode, short final frame fix) | `xboxkrnl_ldi.cc` |
| Kernel | `KeInitializeMutant`, `KeReleaseMutant` (a missing one deadlocked the NUI identity lock) | `xboxkrnl_threading.cc` |
| Content | `XamContentOpenFile` mounts STFS packages from the disc (`stexemplar:`, `nuiidentity:`) | `xam_content.cc` |
| NUI | `XamNuiCameraElevationGetAngle` level camera, tilt/floor stubs, XAM msgs `0x2A005-8`, `0x2B001/2`, `0x2C009`, `0x2C00C` | `xam_nui.cc`, `apps/xam_app.cc` |
| Avatars | `--synthetic_avatar`: empty-but-valid avatar (manifest, non-zero sizes, successful GetAssets) so avatar titles boot | `xam_avatar.cc` |
| Debug aids | crash dump disassembly, `XENIA_CRASH_DISASM`, `XENIA_CRASH_DUMP_IMAGE`, `research/ppc.py` (offline xrefs) | `emulator.cc`, `research/` |

Result: the game boots to its title screen, the IR projector lights, the game sends the full camera
init sequence and then requests frames continuously (code 5), and the camera worker completes them.

## GPU work done since (Layer 2), all ported from upstream PRs by knuckleslee
| PR | What | State here |
|---|---|---|
| #1264 | wide 1D texture fetch precision (decision-tree textures, 786k texels) | DXBC translator patched (XeniOS has no row cap) |
| #1266 | copy command 2 = *compare to reference* -> 1-bit DXT3A masks (not a constant fill), plus DXT3A bit order | new `resolve_dxt3aas1111` compute shader + `draw_util` + Metal pipeline wired |
| #1279/#1280 | resolves whose rectangles come from the title's own vertex shader, run on the CPU by the shader interpreter (texture fetch for the mask) | `resolve_vertex_processor`, interpreter texture fetch, `MetalCommandProcessor::IssueCopyVertexShaderPath` |

Result: **0 resolve/backend errors, 0 crashes**, camera frames flowing, real accelerometer. Floor still not detected.

## Open (next)
- **Performance:** the vertex-shader resolve path produced ~1.2M rectangles / 6.5k draws in 100 s (draws of 1800 indices = 600 rectangles, almost none merged), each a separate Metal resolve. Needs batching / skipping rectangles that cover no mask pixels.
- **Correctness unknown:** cannot yet see whether the classifier output is sane. Ideas: dump the DXT3A mask textures and the readback of the classifier output to PNG; compare rectangle counts when a person stands in view vs an empty room.
- Kinect Sports is untested upstream (only 5345084D), so it may need more.
- Remaining debug aids: `XENIA_CRASH_DISASM`, `XENIA_CRASH_DUMP_IMAGE`, `research/ppc.py`; log lines `PsCam:`, `Resolve vertex shader path:`.

## Older notes
### (earlier) GPU blocker analysis
Game says "The floor can't be detected". The depth -> skeleton classifier runs as GPU draws + resolves,
and the Metal backend rejects them (counts from one 100 s run):
- `Unsupported resolve copy command 2` (CopyCommand::kConstantOne) ~28k, draw `PM4_DRAW_INDX_2(3,8,2)` fails in backend
- `Unsupported resolve vertex buffer format` ~6k (vf0 not a 6-dword vertex fetch)
- `MetalRenderTargetCache::Resolve: GetResolveInfo failed` / `Resolve planning failed` ~34k
Source: `src/xenia/gpu/draw_util.cc` (`GetResolveInfo`) and the Metal resolve path. Draw sizes 3, 120, 480, 1800 vertices.
Next: implement the constant-one resolve and the alternate vertex fetch in `GetResolveInfo` + Metal resolve,
then compare against knuckleslee GPU fixes #1264/#1279/#1280 (wide 1D texture fetches etc.).

## Other open items
- Per-frame log spam from `XamXStudioRequest` (returns fail, polled ~16k times): lower its log level.
- Avatar: synthetic avatar has 0 meshes. Real avatars need the STRB model/skeleton formats (docs/avatar-research.md).
- Disc dumping drive question is separate (plan step 1).
- Port is not yet a git commit; `XeniOS-build` is a plain clone with local changes (`git status` shows them).

## Update (Oct 20): NUI findings
- **Perf fixed:** vertex-shader resolve rectangles now always merge (was skipped when the resolve also clears EDRAM): 600 -> ~8-40 rects/draw.
- **Metal had no resolve readback.** Added `ReadbackResolvedRange` (readback_resolve=full): waits for the GPU and copies the resolved range from the shared-memory buffer into guest memory. Frame resolves are now real data on the CPU.
- **Camera writes did not invalidate the GPU copy.** `xboxkrnl_pscam.cc` now calls `PhysicalHeap::TriggerCallbacks` after writing depth/colour frames (like `xfile.cc`). Before, the GPU kept an all-zero copy of the depth ring (`1E430000`, viewed by the classifier as a wide 1D k_16_16_16_16 texture). Now GPU copy == guest.
- **Tilt callback:** `XamNuiCameraTiltSetCallback` registers the title's callback `829FFE40`, a message handler for XAM messages `0x2B005`-`0x2B00B`; we now send `0x2B009` then `0x2B005` on registration (`--nui_tilt_events`).
- **Floor works:** the title's own tracker now calls `XamNuiCameraRememberFloor` with a plane (normal ~0.9995, distance ~676). The title then asks for a player ("please stand in front of the sensor").
- **Player stage still dead:** per-stage stats show the first 640x240 k_8_8_8_8 resolve (fed by pixel shader `78471535A0D832C6`, which unpacks 1-bit DXT3A mask textures bound as k_16_16_16_16) is always zero, and so is the fmt32 mip pyramid built from it. Mask resolves (cmd2 fmt61) have few distinct values.
- NUI is computed **inside the title** (its linked NUI library); there is no OS skeleton API to implement. Fallback "route 2": patch the title's skeleton getter and feed joints from a Mac-side tracker.
- Debug aids (env): `XENIA_RESOLVE_DUMP=dir`, `XENIA_CHECK_RANGES=hexaddr:len,...`, `--dump_shaders=dir`; log lines `Stage cmd.. fmt..`, `Mask resolve #`, `Draws before ...`. `research/untile.py` detiles 32bpp dumps.

## Update (Oct 20, later): PLAYER TRACKED IN KINECT SPORTS (route 2 works)
Skeleton data is computed **inside the title**, so we feed it in instead of relying on the broken GPU classifier:
- `src/xenia/kernel/kinect_skeleton.cc`: patches the call site `bl 0x82A0B4B0` (Kinect Sports' `NuiSkeletonGetNextFrame`) at `0x82F8E058` to a host stub (an unused import thunk, `0x8317A244`). The stub runs the *real* SDK function first (it must keep draining, it also provides the real floor and `RememberFloor`), then writes the player into skeleton slot 0 of the real `NUI_SKELETON_FRAME` (0xAB0 bytes; slot = 0x1C0 bytes at `+0x30`; tracking state 2 = tracked). Debug accessor hooks `0x82F8DBE0`/`0x82F8DC18` log game-level readers.
- Sources: `--kinect_fake_skeleton` (+ `--kinect_fake_hands_up`, `--kinect_fake_user_index`) or `--kinect_live_skeleton` (UDP `127.0.0.1:5760`).
- `src/xenia/kernel/kinect_frame_share.cc`: `--kinect_share_frames` publishes depth + Bayer colour to `/tmp/xenia_kinect_frames` (the sensor is claimed by Xenia, so the tracker cannot open it).
- `harness/kinect_tracker.py`: MediaPipe Pose on the colour frame, lifted to 3D with depth, mapped to the 20 Kinect joints, sent over UDP. `harness/send_test_skeleton.py` is a dependency-free stand-in sender.

Run (tracker venv via uv; mediapipe 0.10.14 + opencv-python-headless + numpy, ~680 MB):
```
Xenia-edge --kinect_live_skeleton=true --kinect_share_frames=true --allow_kinect_camera=true \
  --allow_avatar_initialization=true --synthetic_avatar=true --allow_nui_initialization=true \
  --readback_resolve=full --readback_memexport=true --protect_zero=false <iso>
cd harness && uv run --python .venv-tracker/bin/python kinect_tracker.py
```
Notes: replacing `NuiSkeletonGetNextFrame` outright starved the SDK floor engine (no `RememberFloor`), so always call through. The early "update stall" was just the game's loading pause. Next: tracking quality (smoothing, joint depth, room/space), hand cursor / gestures, per-profile avatars.

## Open: garbled UI tiles in the Kinect Sports menus (not Kinect related)
Sport-selection posters and the "Play" tile render as flat-colour hue bands (looks like a gradient/LUT sampled at low precision); 3D scenes, crowd and text are fine.
Ruled out: my GPU changes (identical with the original `src/xenia/gpu`, A/B tested), `--readback_resolve=full`, `--metal_force_bc_decompress=true`, `--metal_direct_host_resolve=false`.
Tried together (no fix, introduced a grass-patch glitch): `--metal_texture_cache_use_private=false --metal_texture_upload_via_blit=false --metal_allow_gamma_unorm16=true --metal_shared_memory_direct_write=false`.
Ideas: capture a Metal frame and find the tile's texture/shader; check 1D/LUT ramp textures and exp-adjust/precision in the DXBC->Metal path (same family as the Kinect wide-1D precision bug).
Note: "Press X to disable Controller mode" - the hand cursor only works once Controller mode is off.
