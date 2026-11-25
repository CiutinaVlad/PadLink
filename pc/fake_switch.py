#!/usr/bin/env python3
"""Pretends to be the Switch app (Wi-Fi mode) so you can test padlink_receiver.py
without the console:  python fake_switch.py [pc_ip] [seconds]
Presses A every half second and spins the left stick; prints the measured round trip."""
import socket, struct, sys, time, math
FMT = "<2sBBIIhhhhHHHHQ"
ip = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
dur = float(sys.argv[2]) if len(sys.argv) > 2 else 10
sk = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); sk.setblocking(False)
t0 = time.perf_counter(); seq = 0; rtts = []
while time.perf_counter() - t0 < dur:
    t = time.perf_counter() - t0
    btn = 1 if int(t * 2) % 2 else 0
    lx, ly = int(20000 * math.cos(t)), int(20000 * math.sin(t))
    us = int(time.perf_counter() * 1e6)
    sk.sendto(struct.pack(FMT, b"PL", 1, 1, seq, btn, lx, ly, 0, 0, 0, 0, 0, 0, us), (ip, 47111)); seq += 1
    try:
        while True:
            d = sk.recv(64)
            rtts.append((time.perf_counter() * 1e6 - struct.unpack(FMT, d)[-1]) / 1000)
    except BlockingIOError:
        pass
    time.sleep(0.004)
rtts.sort()
if rtts:
    print(f"sent {seq}, echoes {len(rtts)}, RTT median {rtts[len(rtts)//2]:.3f} ms, p99 {rtts[int(len(rtts)*.99)]:.3f} ms")
else:
    print(f"sent {seq}, no echoes - is padlink_receiver.py running and the firewall open?")
