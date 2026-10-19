#!/usr/bin/env python3
"""Sends a fixed standing skeleton to Xenia's --kinect_live_skeleton port (no deps)."""
import math, socket, struct, sys, time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 5760
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 60
H = 0.74  # camera height above the floor
J = [(0, .95), (0, 1.15), (0, 1.42), (0, 1.65), (.18, 1.4), (.22, 1.12), (.24, .88), (.25, .82),
     (-.18, 1.4), (-.22, 1.12), (-.24, .88), (-.25, .82), (.1, .93), (.11, .5), (.11, .1), (.11, .03),
     (-.1, .93), (-.11, .5), (-.11, .1), (-.11, .03)]
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
t0, seq = time.time(), 0
while time.time() - t0 < secs:
    sway = 0.03 * math.sin((time.time() - t0) * 1.5)
    joints = b"".join(struct.pack("<3f", -x + sway, h - H, 2.2)  # +x = sensor's left (user's right) for x, h in J)
    sock.sendto(struct.pack("<4sII", b"KSKL", seq := seq + 1, 1) + joints + bytes([2] * 20), ("127.0.0.1", port))
    time.sleep(1 / 30)
