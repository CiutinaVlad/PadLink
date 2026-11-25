#!/usr/bin/env python3
"""Pretends to be the Switch app so you can test padlink_receiver.py without the console.
  python fake_switch.py [pc_ip] [seconds]            Wi-Fi mode
  python fake_switch.py [pc_ip] [seconds] --cemu     Wii U (Cemu) mode: adds motion and touch
Presses A every half second, spins the left stick (and in Cemu mode rocks the motion
and drags a touch across the screen); prints the measured round trip."""
import socket, struct, sys, time, math
FMT = "<2sBBIIhhhhHHHHQ"
args = [a for a in sys.argv[1:] if not a.startswith("--")]
cemu = "--cemu" in sys.argv
ip = args[0] if len(args) > 0 else "127.0.0.1"
dur = float(args[1]) if len(args) > 1 else 10
sk = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); sk.setblocking(False)
t0 = time.perf_counter(); seq = 0; rtts = []
while time.perf_counter() - t0 < dur:
    t = time.perf_counter() - t0
    btn = 1 if int(t * 2) % 2 else 0
    lx, ly = int(20000 * math.cos(t)), int(20000 * math.sin(t))
    us = int(time.perf_counter() * 1e6)
    touching = cemu and int(t) % 2 == 0
    tx, ty = int(640 + 400 * math.sin(t)), 360
    pkt = struct.pack(FMT, b"PL", 1, 3 if cemu else 1, seq, btn, lx, ly, 0, 0,
                      tx if touching else 0, ty if touching else 0, 1 if touching else 0, 0, us)
    if cemu:   # rocking forward/back: accel in G, gyro in deg/s (Pro Controller axes)
        a = 0.4 * math.sin(t * 2)
        pkt += struct.pack("<6f", math.sin(a), 0.0, math.cos(a), 0.0, 45.0 * math.cos(t * 2), 0.0)
    sk.sendto(pkt, (ip, 47111)); seq += 1
    try:
        while True:
            d = sk.recv(128)
            rtts.append((time.perf_counter() * 1e6 - struct.unpack(FMT, d[:36])[-1]) / 1000)
    except BlockingIOError:
        pass
    time.sleep(0.004)
rtts.sort()
if rtts:
    print(f"sent {seq}, echoes {len(rtts)}, RTT median {rtts[len(rtts)//2]:.3f} ms, p99 {rtts[int(len(rtts)*.99)]:.3f} ms")
else:
    print(f"sent {seq}, no echoes - is padlink_receiver.py running and the firewall open?")
