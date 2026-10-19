# Xbox 360 avatar emulation: what exists, what's missing (research, Oct 2026)

Why this matters: Kinect Sports (World) calls `XamAvatarInitialize`, then the avatar manifest/asset APIs,
before it ever touches the camera. XeniOS stubs all of them, so the game crashes (see session notes).
Evidence from our own runs: `XamAvatarInitialize(4, 1, 5, &callbacks[5], 3MB buffer, 0x300000)`,
then `XamAvatarGetManifestLocalUser`, `XamAvatarManifestGetBodyType`, `XamAvatarSetCustomAsset` x3 (STRB tags
`53545242`), `XamAvatarGetAssetsResultSize`, `XamAvatarGetAssets` (overlapped).

## What the game does itself vs what XAM must provide
- **Title side (in the game, statically linked XAvatar library):** the 5-pointer table passed to
  `XamAvatarInitialize` is **input**: the game fills it with its own callbacks (0x82AE51D8, 52C8, 4F68, 5000, 50A8
  in Kinect Sports) before calling. It also uses `LDI*` (LZX) to decompress its own avatar data. Parser,
  renderer and animation playback live in the game. We can read all of it with the crash-dump disassembler
  (`XENIA_CRASH_DISASM=START:END`) and treat the game as the format spec.
- **XAM side (what we must emulate):** the local user's avatar *manifest* (the `0x3E8`-byte
  `AvatarDescription` blob), and the *asset packages* returned by `XamAvatarGetAssets` (a result buffer plus a GPU
  resource buffer) containing body, clothing, textures, animations, skeleton.

## Public sources
| Source | What it gives |
|---|---|
| [Velocity `AvatarAsset`](https://github.com/hetelek/Velocity/tree/master/XboxInternals/AvatarAsset) (GPL) | **STRB container**: magic `0x53545242`, header (GUID, block alignment, endianness), block ids: Animation 1, Texture 2, Model 3, ShapeOverrides 4, Skeleton 5, AssetMetadata 6, CustomColorTable 7, AssetMetadataVersioned 8. Metadata, color table, animation frame count / fps parsed. **Block bodies for Model, Skeleton, Texture are not parsed.** `SkeletonVersion` has `Nxe=1, Natal=2` (a Kinect-era avatar skeleton exists). XeniOS's `xam_avatar.h` already mirrors this. |
| [XNA Game Studio 4.0 avatar docs](https://learn.microsoft.com/en-us/previous-versions/windows/xna/dd940289(v=xnagamestudio.41)) | Public API shape: `AvatarDescription` (bytes), `AvatarRenderer` (Draw, bind pose, bones), `AvatarAnimation`, [`AvatarBone`](https://learn.microsoft.com/zh-cn/previous-versions/windows/xna/dd940212(v=xnagamestudio.41)) enum (named bone indices, Root between the feet). Describes behaviour, not file formats. |
| [free60 STFS](https://free60.org/System-Software/Formats/STFS/) | The package container avatar items ship in (PIRS/LIVE/CON). |
| [360sona](https://www.purexbox.com/news/2026/09/random-someone-has-rebuilt-the-xbox-360-avatar-editor-with-a-full-list-of-21425-items) (also [godisageek](https://godisageek.com/2026/09/xbox-360-avatar-editor-browser-rebuild/)) | A fan who **reverse engineered the whole pipeline** (skeleton, clip format, shaders) and rebuilt it in WebGL 2, exporting rigged GLB. Proves it is tractable. I found no published source or format spec. Assets are Microsoft's, not licensed for reuse. |
| rexglue-sdk `xam_avatar.cpp` | Only a 2-function stub. Nothing to reuse. |
| xenia-canary issues | No avatar implementation work found (only issue 855, a movie game that won't boot). |

## Gaps (nobody has documented these)
1. Model / Skeleton / Texture / Animation **block bodies** inside STRB.
2. Layout of the `0x3E8`-byte `AvatarDescription` beyond weight / height / a few colours (`X_AVATAR_METADATA`).
3. The `XamAvatarGetAssets` result buffer + GPU resource buffer layout the title's parser expects.
4. How the five title callbacks are used by XAM (alloc/free/IO style hooks), and what completes asynchronously.

## Feasible plan (own assets, no Microsoft data needed)
1. **Oracle method:** disassemble the game's avatar loader (parser for the `GetAssets` buffers) with the
   crash-dump probe, derive the minimal valid buffer layout for 1 body + skeleton + no clothing.
2. **Synthetic avatar:** emit a hand-built STRB (Skeleton + Model blocks from our own simple mesh + bind pose,
   `Natal` skeleton version) and a default manifest; return them from `XamAvatarGetManifestLocalUser`/`GetAssets`.
3. **Per-profile customisation later:** swap in user-supplied meshes through the same writer.
4. Cross-check against any real avatar item the user dumps from their own console (STFS, via Velocity) to
   validate our understanding of the block bodies.

## Parallel track that does not need avatars
Get the skeleton path working on a non-avatar title first (camera device is already ported, see
`camera-port-plan.md`), then return to avatars for titles that need them.
