#!/usr/bin/env python3
"""Live pose tracker for the Xenia Kinect skeleton hook.

Reads the depth + Bayer colour frames Xenia publishes (--kinect_share_frames,
/tmp/xenia_kinect_frames), runs MediaPipe Pose on the colour image, lifts the
joints to 3D with the depth image, maps them to the Kinect v1 20-joint skeleton
and sends one UDP packet per frame to Xenia (--kinect_live_skeleton).

Packet (little endian, 272 bytes): 'KSKL', u32 seq, u32 tracked,
float32 joints[20][3] (metres, sensor space: +y up, +z forward, +x to the sensor's LEFT),
u8 joint_state[20] (0 none, 1 inferred, 2 tracked).

usage: python kinect_tracker.py [--port 5760] [--complexity 1]
"""
import argparse
import mmap
import socket
import struct
import time

import cv2
import mediapipe as mp
import numpy as np

W, H = 640, 480
DEPTH_OFF = 0x40
COLOR_OFF = DEPTH_OFF + W * H * 2
# ponytail: nominal Kinect v1 intrinsics, no per-device calibration
FX_D = 594.2  # depth camera focal length (px)
FX_C = 529.2  # colour camera focal length (px)
CX, CY = 320.0, 240.0
D_SCALE = FX_D / FX_C  # colour pixel offset from centre -> depth pixel offset

# MediaPipe landmark indices
NOSE, LEAR, REAR = 0, 7, 8
LSH, RSH, LEL, REL, LWR, RWR = 11, 12, 13, 14, 15, 16
LPINKY, RPINKY, LINDEX, RINDEX, LTHUMB, RTHUMB = 17, 18, 19, 20, 21, 22
LHIP, RHIP, LKNEE, RKNEE, LANK, RANK, LFOOT, RFOOT = 23, 24, 25, 26, 27, 28, 31, 32

# raw 11-bit disparity -> metres (Magnenat's fit), 0 = invalid
RAW_TO_M = np.zeros(2048, np.float32)
_raw = np.arange(2048, dtype=np.float32)
_ok = (_raw > 0) & (_raw < 1085)
RAW_TO_M[_ok] = 1.0 / (_raw[_ok] * -0.0030711016 + 3.3309495161)


def demosaic_grbg(bayer):
    """Half-resolution RGB from a GRBG Bayer frame, upscaled back to 640x480."""
    g1, r = bayer[0::2, 0::2], bayer[0::2, 1::2]
    b, g2 = bayer[1::2, 0::2], bayer[1::2, 1::2]
    rgb = np.dstack([r, ((g1.astype(np.uint16) + g2) // 2).astype(np.uint8), b])
    return cv2.resize(rgb, (W, H), interpolation=cv2.INTER_LINEAR)


def depth_at(depth_m, u, v, r=4):
    """Median valid depth around (u, v), or 0."""
    u, v = int(round(u)), int(round(v))
    patch = depth_m[max(v - r, 0):v + r + 1, max(u - r, 0):u + r + 1]
    valid = patch[patch > 0]
    return float(np.median(valid)) if valid.size else 0.0


def to_kinect_joints(lm, depth_m):
    """MediaPipe landmarks -> (20x3 metres, 20 states, tracked)."""
    px = np.array([[p.x * W, p.y * H] for p in lm], np.float32)
    vis = np.array([p.visibility for p in lm], np.float32)

    def mid(*idx):
        return px[list(idx)].mean(axis=0), float(vis[list(idx)].mean())

    hip_c, v_hip = mid(LHIP, RHIP)
    sh_c, v_sh = mid(LSH, RSH)
    spine = ((hip_c + sh_c) / 2, min(v_hip, v_sh))
    head_xy, v_head = mid(LEAR, REAR, NOSE)
    # ponytail: head centre approximated from ears + nose
    pts = [
        (hip_c, v_hip), spine, (sh_c, v_sh), (head_xy, v_head),
        (px[LSH], vis[LSH]), (px[LEL], vis[LEL]), (px[LWR], vis[LWR]),
        mid(LPINKY, LINDEX, LTHUMB),
        (px[RSH], vis[RSH]), (px[REL], vis[REL]), (px[RWR], vis[RWR]),
        mid(RPINKY, RINDEX, RTHUMB),
        (px[LHIP], vis[LHIP]), (px[LKNEE], vis[LKNEE]), (px[LANK], vis[LANK]),
        (px[LFOOT], vis[LFOOT]),
        (px[RHIP], vis[RHIP]), (px[RKNEE], vis[RKNEE]), (px[RANK], vis[RANK]),
        (px[RFOOT], vis[RFOOT]),
    ]
    # Reference depth from the torso; joints off the body fall back to it.
    def dep(p):
        return depth_at(depth_m, CX + (p[0] - CX) * D_SCALE, CY + (p[1] - CY) * D_SCALE)

    ref = [dep(sh_c), dep(hip_c), dep(spine[0])]
    ref = [z for z in ref if z > 0]
    ref_z = float(np.median(ref)) if ref else 0.0
    tracked = ref_z > 0.8 and ref_z < 4.5 and min(v_sh, v_hip) > 0.5

    joints = np.zeros((20, 3), np.float32)
    states = np.zeros(20, np.uint8)
    for j, (xy, visibility) in enumerate(pts):
        u, v = float(xy[0]), float(xy[1])
        z = depth_at(depth_m, CX + (u - CX) * D_SCALE, CY + (v - CY) * D_SCALE)
        if z <= 0 or abs(z - ref_z) > 0.6:
            z = ref_z
        joints[j] = ((CX - u) * z / FX_C, (CY - v) * z / FX_C, z)  # +x = sensor's left
        states[j] = 2 if visibility > 0.6 else (1 if visibility > 0.25 else 0)
    return joints, states, tracked


class OneEuro:
    """One-Euro filter on an array: heavy smoothing when still, light when moving."""

    def __init__(self, min_cutoff=1.0, beta=6.0, d_cutoff=1.0):
        self.min_cutoff, self.beta, self.d_cutoff = min_cutoff, beta, d_cutoff
        self.prev = self.dx = self.t = None

    @staticmethod
    def _alpha(cutoff, dt):
        return 1.0 / (1.0 + (1.0 / (2 * np.pi * cutoff)) / dt)

    def reset(self):
        self.prev = self.dx = self.t = None

    def __call__(self, x, t):
        if self.prev is None:
            self.prev, self.dx, self.t = x.copy(), np.zeros_like(x), t
            return x
        dt = max(t - self.t, 1e-3)
        dx_hat = self._alpha(self.d_cutoff, dt) * (x - self.prev) / dt + \
            (1 - self._alpha(self.d_cutoff, dt)) * self.dx
        a = self._alpha(self.min_cutoff + self.beta * np.abs(dx_hat), dt)
        x_hat = a * x + (1 - a) * self.prev
        self.prev, self.dx, self.t = x_hat, dx_hat, t
        return x_hat


class BodyDepth:
    """Torso depth (metres): median of the torso region, then of the last 7 frames."""

    def __init__(self):
        self.hist = []

    def reset(self):
        self.hist = []

    def update(self, depth_m, px, vis):
        us = [px[i][0] for i in (LSH, RSH, LHIP, RHIP)]
        vs = [px[i][1] for i in (LSH, RSH, LHIP, RHIP)]
        # shrink the torso box to its middle half so arms/background mostly miss it
        cu, cv = (min(us) + max(us)) / 2, (min(vs) + max(vs)) / 2
        hu, hv = (max(us) - min(us)) / 4, (max(vs) - min(vs)) / 4
        u0, u1 = CX + (cu - hu - CX) * D_SCALE, CX + (cu + hu - CX) * D_SCALE
        v0, v1 = CY + (cv - hv - CY) * D_SCALE, CY + (cv + hv - CY) * D_SCALE
        patch = depth_m[max(int(v0), 0):int(v1) + 1, max(int(u0), 0):int(u1) + 1]
        valid = patch[patch > 0]
        if valid.size < 20:
            return self.hist[-1] if self.hist else 0.0
        z = float(np.median(valid))
        if self.hist and abs(z - self.hist[-1]) > 0.5:  # reject jumps
            return self.hist[-1]
        self.hist = (self.hist + [z])[-7:]
        return float(np.median(self.hist))


def to_kinect_joints_world(result, depth_m, body, body_offset, world_z_sign, zfilter=None, was_tracked=False,
                           depth_blend=0.5, hand_len=0.09):
    """MediaPipe metric world pose (smooth body shape) placed in the sensor frame
    with the torso depth (only the overall position comes from depth)."""
    lm = result.pose_landmarks.landmark
    px = np.array([[p.x * W, p.y * H] for p in lm], np.float32)
    vis = np.array([p.visibility for p in lm], np.float32)
    w = np.array([[p.x, p.y, p.z] for p in result.pose_world_landmarks.landmark], np.float32)

    def mid(a, idx):
        return a[list(idx)].mean(axis=0)

    hip_w = mid(w, (LHIP, RHIP))
    off = w - hip_w

    def j3(*idx):  # world offset of the mean of landmarks
        return mid(off, idx)

    sh_off = j3(LSH, RSH)
    offs = np.stack([
        np.zeros(3), (sh_off) / 2, sh_off, j3(LEAR, REAR, NOSE),
        off[LSH], off[LEL], off[LWR], j3(LPINKY, LINDEX, LTHUMB),
        off[RSH], off[REL], off[RWR], j3(RPINKY, RINDEX, RTHUMB),
        off[LHIP], off[LKNEE], off[LANK], off[LFOOT],
        off[RHIP], off[RKNEE], off[RANK], off[RFOOT],
    ])
    vis20 = np.array([
        min(vis[LHIP], vis[RHIP]), min(vis[LHIP], vis[RHIP], vis[LSH], vis[RSH]),
        min(vis[LSH], vis[RSH]), vis[[LEAR, REAR, NOSE]].mean(),
        vis[LSH], vis[LEL], vis[LWR], vis[[LPINKY, LINDEX, LTHUMB]].mean(),
        vis[RSH], vis[REL], vis[RWR], vis[[RPINKY, RINDEX, RTHUMB]].mean(),
        vis[LHIP], vis[LKNEE], vis[LANK], vis[LFOOT],
        vis[RHIP], vis[RKNEE], vis[RANK], vis[RFOOT]])

    z = body.update(depth_m, px, vis)
    if zfilter is not None and z > 0:
        z = float(zfilter(np.array([z], np.float32), time.time())[0])  # depth steps are ~2 cm: smooth hard
    vis_floor = 0.3 if was_tracked else 0.5  # hysteresis: easier to keep than to acquire
    tracked = 0.8 < z < 4.5 and min(vis[LSH], vis[RSH], vis[LHIP], vis[RHIP]) > vis_floor
    z += body_offset  # joints sit inside the body, behind its surface
    hu, hv = mid(px, (LHIP, RHIP))
    hip_k = np.array([(CX - hu) * z / FX_C, (CY - hv) * z / FX_C, z], np.float32)
    # MediaPipe world axes: x right, y down, z away from the camera.
    off_k = np.stack([-offs[:, 0], -offs[:, 1], world_z_sign * offs[:, 2]], axis=1)
    joints = (hip_k + off_k).astype(np.float32)

    # Hand = wrist extended along the forearm (stable); the finger centroid only nudges the direction.
    for elbow, wrist, hand in ((5, 6, 7), (9, 10, 11)):
        fore = joints[wrist] - joints[elbow]
        fore /= max(np.linalg.norm(fore), 1e-6)
        fing = joints[hand] - joints[wrist]
        fing /= max(np.linalg.norm(fing), 1e-6)
        d = 0.7 * fore + 0.3 * fing
        joints[hand] = joints[wrist] + hand_len * d / max(np.linalg.norm(d), 1e-6)

    # Wrists/hands: MediaPipe's depth is weakest there, so blend in the measured depth when it agrees.
    if depth_blend > 0 and tracked:
        for lm_idx, kj in ((LWR, 6), (RWR, 10)):
            u, v = px[lm_idx]
            zd = depth_at(depth_m, CX + (u - CX) * D_SCALE, CY + (v - CY) * D_SCALE, r=3)
            if zd > 0 and abs(zd - joints[kj][2]) < 0.25 and vis[lm_idx] > 0.5:
                dz = depth_blend * ((zd + 0.03) - joints[kj][2])
                joints[kj][2] += dz
                joints[kj + 1][2] += dz  # carry the hand along
    states = np.where(vis20 > 0.6, 2, np.where(vis20 > 0.25, 1, 0)).astype(np.uint8)
    return joints, states, bool(tracked)


# Bone chains parent -> child (Kinect joint indices), enforced in this order.
BONE_CHAINS = [(0, 1), (1, 2), (2, 3), (4, 5), (5, 6), (6, 7), (8, 9), (9, 10), (10, 11),
               (12, 13), (13, 14), (14, 15), (16, 17), (17, 18), (18, 19)]


class BoneLock:
    """Learns each bone's length over time and keeps it constant, so limbs cannot stretch
    or shrink frame to frame. Roots (shoulders, hips, spine base) are left as measured."""

    def __init__(self, alpha=0.02, warmup=45):
        self.len = {}
        self.count = 0
        self.alpha, self.warmup = alpha, warmup

    def reset(self):
        self.count = 0

    def __call__(self, joints, states):
        out = joints.copy()
        for a, b in BONE_CHAINS:
            v = joints[b] - joints[a]
            n = float(np.linalg.norm(v))
            if n < 1e-6:
                continue
            if states[a] == 2 and states[b] == 2:
                self.len[(a, b)] = n if (a, b) not in self.len else \
                    (1 - self.alpha) * self.len[(a, b)] + self.alpha * n
            if self.count >= self.warmup and (a, b) in self.len:
                out[b] = out[a] + (v / n) * self.len[(a, b)]
        self.count += 1
        return out


BONES = [(0, 1), (1, 2), (2, 3), (2, 4), (4, 5), (5, 6), (6, 7), (2, 8), (8, 9), (9, 10),
         (10, 11), (0, 12), (12, 13), (13, 14), (14, 15), (0, 16), (16, 17), (17, 18), (18, 19)]


def write_debug(dirname, rgb, depth_m, result, joints, tracked):
    """Colour image with MediaPipe landmarks (green) and the mapped Kinect
    joints re-projected through the colour intrinsics (red) next to depth."""
    import os
    os.makedirs(dirname, exist_ok=True)
    img = cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR)
    if result.pose_landmarks:
        for p in result.pose_landmarks.landmark:
            cv2.circle(img, (int(p.x * W), int(p.y * H)), 3, (0, 255, 0), -1)
    if tracked:
        pts = []
        for j, (x, y, z) in enumerate(joints):
            u, v = int(CX - x * FX_C / z), int(CY - y * FX_C / z)
            pts.append((u, v))
            cv2.circle(img, (u, v), 5, (0, 0, 255), 1)
            cv2.putText(img, str(j), (u + 4, v - 4), cv2.FONT_HERSHEY_SIMPLEX, 0.4, (0, 0, 255), 1)
        for a, b in BONES:
            cv2.line(img, pts[a], pts[b], (0, 0, 255), 1)
    dep = np.clip((depth_m - 0.5) / 3.5 * 255, 0, 255).astype(np.uint8)
    dep = cv2.applyColorMap(dep, cv2.COLORMAP_JET)
    dep[depth_m == 0] = 0
    cv2.imwrite(os.path.join(dirname, "overlay.png"), np.hstack([img, dep]))
    if tracked:
        with open(os.path.join(dirname, "joints.txt"), "w") as f:
            for j, row in enumerate(joints):
                f.write(f"{j:2d} x={row[0]:+.3f} y={row[1]:+.3f} z={row[2]:.3f}\n")


def learn_background(shm, delay):
    """Median depth (metres) of the empty scene; the user steps out of view."""
    print(f"Learning the empty background in {delay:.0f} s: step out of the sensor's view...", flush=True)
    time.sleep(delay)
    frames, last = [], -1
    while len(frames) < 45:
        seq = struct.unpack_from("<I", shm, 4)[0]
        if seq != last:
            last = seq
            raw = np.frombuffer(shm, np.uint16, W * H, DEPTH_OFF).reshape(H, W)
            frames.append(RAW_TO_M[np.minimum(raw, 2047)].copy())
        time.sleep(0.005)
    stack = np.stack(frames)
    stack[stack == 0] = np.nan
    with np.errstate(all="ignore"):
        bg = np.nanmedian(stack, axis=0)
    bg = np.nan_to_num(bg, nan=0.0).astype(np.float32)
    print("Background learned; you can step in now.", flush=True)
    return bg


def foreground_mask_colour(depth_m, bg):
    """255 where something is closer than the learned background, in colour-image pixels."""
    closer = (depth_m > 0) & ((bg == 0) | (depth_m < bg - 0.12))
    fg = closer.astype(np.uint8) * 255
    fg = cv2.morphologyEx(fg, cv2.MORPH_OPEN, np.ones((5, 5), np.uint8))
    fg = cv2.dilate(fg, np.ones((25, 25), np.uint8))
    s = 1.0 / D_SCALE  # depth -> colour pixel scaling about the image centre
    m = np.float32([[s, 0, CX * (1 - s)], [0, s, CY * (1 - s)]])
    return cv2.warpAffine(fg, m, (W, H), flags=cv2.INTER_NEAREST)


class Viewer:
    """Matplotlib debug window: colour overlay, 3D skeleton (raw vs filtered),
    rolling time series. Updated every few frames to keep the tracker fast."""

    def __init__(self, every):
        import collections
        import matplotlib
        matplotlib.use("MacOSX")
        import matplotlib.pyplot as plt
        from mpl_toolkits.mplot3d import Axes3D  # noqa: F401
        self.plt, self.every, self.n = plt, every, 0
        self.hist = {k: collections.deque(maxlen=180) for k in ("t", "hz_raw", "hz_f", "hy_raw", "hy_f", "hx_raw", "hx_f")}
        self.fig = plt.figure(figsize=(16, 5.6))
        self.ax_img = self.fig.add_subplot(1, 3, 1)
        self.ax3d = self.fig.add_subplot(1, 3, 2, projection="3d")
        self.ax_ts = self.fig.add_subplot(1, 3, 3)
        self.im = self.ax_img.imshow(np.zeros((H, W, 3), np.uint8))
        self.ax_img.axis("off")
        self.lm_scatter = self.ax_img.scatter([], [], s=6, c="lime")
        self.img_bones = [self.ax_img.plot([], [], "r-", lw=1.5)[0] for _ in BONES]
        self.bones_f = [self.ax3d.plot([], [], [], "b-", lw=2.5)[0] for _ in BONES]
        self.bones_r = [self.ax3d.plot([], [], [], "r-", lw=1, alpha=0.5)[0] for _ in BONES]
        self.title = self.ax_img.set_title("")
        self.ts_lines = {
            "hz_raw": self.ax_ts.plot([], [], "r-", lw=0.8, alpha=0.6, label="head z raw")[0],
            "hz_f": self.ax_ts.plot([], [], "b-", lw=1.5, label="head z filtered")[0],
            "hy_raw": self.ax_ts.plot([], [], "m-", lw=0.8, alpha=0.6, label="R hand y raw")[0],
            "hy_f": self.ax_ts.plot([], [], "g-", lw=1.5, label="R hand y filtered")[0],
        }
        self.ax_ts.legend(loc="upper left", fontsize=7)
        self.ax_ts.set_xlabel("seconds")
        self.ax_ts.set_ylabel("metres")
        self.fig.tight_layout()
        plt.ion()
        plt.show(block=False)

    def update(self, rgb, result, raw, filt, tracked, stats, now):
        h = self.hist
        if tracked:
            h["t"].append(now)
            h["hz_raw"].append(raw[3, 2]); h["hz_f"].append(filt[3, 2])
            h["hy_raw"].append(raw[11, 1]); h["hy_f"].append(filt[11, 1])
        self.n += 1
        if self.n % self.every:
            return
        self.im.set_data(rgb)
        if result.pose_landmarks:
            self.lm_scatter.set_offsets([[p.x * W, p.y * H] for p in result.pose_landmarks.landmark])
        else:
            self.lm_scatter.set_offsets(np.zeros((0, 2)))
        for line in self.img_bones:
            line.set_data([], [])
        if tracked:
            pts = np.array([[CX - x * FX_C / z, CY - y * FX_C / z] for x, y, z in filt])
            for line, (a, b) in zip(self.img_bones, BONES):
                line.set_data([pts[a][0], pts[b][0]], [pts[a][1], pts[b][1]])
            centre = filt[0]
            for lines, joints in ((self.bones_f, filt), (self.bones_r, raw)):
                for line, (a, b) in zip(lines, BONES):
                    xs = [-joints[a][0], -joints[b][0]]; ys = [joints[a][2], joints[b][2]]; zs = [joints[a][1], joints[b][1]]
                    line.set_data_3d(xs, ys, zs)
            self.ax3d.set_xlim(-centre[0] - 1.2, -centre[0] + 1.2)
            self.ax3d.set_ylim(centre[2] - 1.2, centre[2] + 1.2)
            self.ax3d.set_zlim(centre[1] - 1.6, centre[1] + 0.9)
            self.ax3d.set_box_aspect((2.4, 2.4, 2.5))
            self.ax3d.set_xlabel("x (right)"); self.ax3d.set_ylabel("z (away)"); self.ax3d.set_zlabel("y (up)")
        self.title.set_text(stats)
        if len(h["t"]) > 2:
            t = np.array(h["t"]) - h["t"][-1]
            for k, line in self.ts_lines.items():
                line.set_data(t, np.array(h[k]))
            self.ax_ts.set_xlim(t[0], 0)
            allv = np.concatenate([np.array(h[k]) for k in self.ts_lines])
            self.ax_ts.set_ylim(allv.min() - 0.05, allv.max() + 0.05)
        self.fig.canvas.draw_idle()
        self.fig.canvas.flush_events()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=5760)
    ap.add_argument("--complexity", type=int, default=1)
    ap.add_argument("--smooth", type=float, default=0.5, help="EMA weight of the new frame")
    ap.add_argument("--debug-dir", help="write an overlay image here every 2 s")
    ap.add_argument("--calibrate-delay", type=float, default=8.0,
                    help="seconds to wait (step out of view) before learning the empty background")
    ap.add_argument("--no-background", action="store_true", help="skip background subtraction")
    ap.add_argument("--mode", choices=["world", "depth"], default="world",
                    help="world: smooth MediaPipe 3D body + torso depth; depth: per-joint depth")
    ap.add_argument("--body-offset", type=float, default=0.08,
                    help="metres added to the torso surface depth (joints are inside the body)")
    ap.add_argument("--world-z-sign", type=float, default=1.0, help="+1 if world z points away from the camera")
    ap.add_argument("--min-cutoff", type=float, default=1.0, help="One-Euro min cutoff (Hz); lower = smoother")
    ap.add_argument("--beta", type=float, default=6.0, help="One-Euro speed coefficient; higher = less lag")
    ap.add_argument("--grace-frames", type=int, default=20, help="frames to hold the last pose after losing the player")
    ap.add_argument("--depth-blend", type=float, default=0.5, help="weight of measured depth on wrists (0 = off)")
    ap.add_argument("--hand-len", type=float, default=0.09, help="wrist-to-hand-joint distance (m)")
    ap.add_argument("--no-bone-lock", action="store_true", help="do not enforce constant bone lengths")
    ap.add_argument("--z-smooth", type=float, default=0.3, help="z min-cutoff as a fraction of --min-cutoff")
    ap.add_argument("--z-cutoff", type=float, default=0.3, help="min cutoff (Hz) of the torso-depth filter")
    ap.add_argument("--csv", help="log raw/filtered joints of interest here")
    ap.add_argument("--view", action="store_true", help="open the matplotlib debug viewer")
    ap.add_argument("--view-every", type=int, default=3, help="redraw the viewer every N frames")
    ap.add_argument("--bg-file", default="/tmp/kinect_background.npy",
                    help="learned empty-room depth, reused on restart if the sensor did not move")
    ap.add_argument("--recalibrate", action="store_true", help="ignore --bg-file and learn the background again")
    ap.add_argument("--z-offset", type=float, default=0.0,
                    help="metres added to the player's distance (small rooms: 0.8-1.0)")
    args = ap.parse_args()

    with open("/tmp/xenia_kinect_frames", "r+b") as f:
        shm = mmap.mmap(f.fileno(), COLOR_OFF + W * H)
    if shm[0:4] != b"KFRM":
        raise SystemExit("frame file has no magic; start Xenia with --kinect_share_frames")

    pose = mp.solutions.pose.Pose(
        model_complexity=args.complexity, smooth_landmarks=True,
        min_detection_confidence=0.5, min_tracking_confidence=0.5)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = ("127.0.0.1", args.port)

    import os
    if args.no_background:
        bg = None
    elif os.path.exists(args.bg_file) and not args.recalibrate:
        bg = np.load(args.bg_file)
        print(f"Using saved background {args.bg_file} (--recalibrate to relearn)", flush=True)
    else:
        bg = learn_background(shm, args.calibrate_delay)
        np.save(args.bg_file, bg)
    # x/y track the pose closely; z (depth) is the noisy axis, so smooth it harder.
    # Per joint: torso/legs steady, elbows a little looser, wrists/hands smoothest at rest but
    # with a high speed term so fast arm motion does not lag.
    cut_f = np.ones(20, np.float32)
    beta_f = np.ones(20, np.float32)
    for j in (5, 9):
        cut_f[j], beta_f[j] = 0.8, 1.3
    for j in (6, 7, 10, 11):
        cut_f[j], beta_f[j] = 0.6, 2.2
    mc = args.min_cutoff * cut_f[:, None] * np.array([1.0, 1.0, args.z_smooth], np.float32)
    bt = args.beta * beta_f[:, None] * np.array([1.0, 1.0, 0.25], np.float32)
    smoother = OneEuro(mc, bt)
    bone_lock = None if args.no_bone_lock else BoneLock()
    zfilter = OneEuro(np.array([args.z_cutoff], np.float32), np.array([2.0], np.float32))
    body = BodyDepth()
    csv = open(args.csv, "w") if args.csv else None
    if csv:
        csv.write("t,tracked,raw_head_x,raw_head_y,raw_head_z,raw_hr_x,raw_hr_y,raw_hr_z,"
                  "f_head_x,f_head_y,f_head_z,f_hr_x,f_hr_y,f_hr_z,"
                  "raw_hip_x,raw_hip_y,raw_hip_z,f_hip_x,f_hip_y,f_hip_z\n")
    viewer = Viewer(args.view_every) if args.view else None
    process_time = 0.0
    was_tracked, lost_frames, last_joints, last_states = False, 0, None, None
    tracked_frames = total_frames = 0

    seq = 0
    last_color = -1
    t0, frames = time.time(), 0
    last_debug = 0.0
    while True:
        color_seq = struct.unpack_from("<I", shm, 8)[0]
        if color_seq == last_color:
            time.sleep(0.003)
            continue
        last_color = color_seq
        bayer = np.frombuffer(shm, np.uint8, W * H, COLOR_OFF).reshape(H, W)
        raw = np.frombuffer(shm, np.uint16, W * H, DEPTH_OFF).reshape(H, W)
        depth_m = RAW_TO_M[np.minimum(raw, 2047)]

        rgb = demosaic_grbg(bayer)
        if bg is not None:
            mask = foreground_mask_colour(depth_m, bg)
            rgb = np.where(mask[..., None] > 0, rgb, np.uint8(110))  # grey out the background
        t_proc = time.time()
        result = pose.process(rgb)
        process_time += time.time() - t_proc
        tracked, joints, states = False, np.zeros((20, 3), np.float32), np.zeros(20, np.uint8)
        raw = None
        if result.pose_landmarks:
            if args.mode == "world" and result.pose_world_landmarks:
                joints, states, tracked = to_kinect_joints_world(
                    result, depth_m, body, args.body_offset, args.world_z_sign, zfilter, was_tracked,
                    args.depth_blend, args.hand_len)
            else:
                joints, states, tracked = to_kinect_joints(result.pose_landmarks.landmark, depth_m)
            if tracked and bone_lock is not None:
                joints = bone_lock(joints, states)
            raw = joints
            if tracked:
                joints = smoother(joints, time.time())
        if tracked:
            lost_frames = 0
            last_joints, last_states = joints, states
        elif last_joints is not None and lost_frames < args.grace_frames:
            # ponytail: brief detection loss: hold the last pose (marked inferred) instead of dropping the player
            lost_frames += 1
            tracked, joints, states = True, last_joints, np.minimum(last_states, 1).astype(np.uint8)
        else:
            smoother.reset()
            zfilter.reset()
            body.reset()
            if bone_lock is not None:
                bone_lock.reset()
            last_joints = None
        was_tracked = tracked
        total_frames += 1
        tracked_frames += int(tracked)
        if raw is None:
            raw = joints  # grace period: no fresh detection this frame
        if viewer is not None:
            viewer.update(rgb, result, raw, joints, tracked,
                          f"{frames / max(time.time() - t0, 1e-3):.0f} fps  tracked {100 * tracked_frames / max(total_frames, 1):.0f}%"
                          + (f"  hip z {joints[0][2]:.2f} m" if tracked else "  (no player)"), time.time())
        if csv is not None and tracked:
            csv.write(f"{time.time():.4f},1," + ",".join(f"{v:.4f}" for v in (*raw[3], *raw[11], *joints[3], *joints[11], *raw[0], *joints[0])) + "\n")

        if args.debug_dir and time.time() - last_debug > 2:
            last_debug = time.time()
            write_debug(args.debug_dir, rgb, depth_m, result, joints, tracked)
        seq += 1
        if tracked:
            joints = joints.copy()
            joints[:, 2] += args.z_offset  # push the player back in small rooms
        sock.sendto(
            struct.pack("<4sII", b"KSKL", seq, int(tracked))
            + joints.astype("<f4").tobytes() + states.tobytes(), dest)
        frames += 1
        if time.time() - t0 > 5:
            hip = joints[0] if tracked else None
            print(f"{frames / (time.time() - t0):.1f} fps, pose {1000 * process_time / max(frames, 1):.0f} ms/frame, "
                  f"tracked={tracked} ({100 * tracked_frames / max(total_frames, 1):.0f}% of frames), hip={hip}", flush=True)
            t0, frames, process_time = time.time(), 0, 0.0
            if csv is not None:
                csv.flush()


if __name__ == "__main__":
    main()
