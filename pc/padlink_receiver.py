#!/usr/bin/env python3
"""
padlink_receiver.py - PC side of PadLink. Receives the Switch's controls and feeds a
virtual gamepad.

  Wi-Fi (default):   python padlink_receiver.py
  USB:               python padlink_receiver.py --usb
  Just print input:  python padlink_receiver.py --backend print

Virtual gamepad backends:
  Windows: vgamepad (Xbox 360 pad via ViGEmBus)   pip install vgamepad
  Linux:   evdev/uinput                           pip install evdev   (needs access to /dev/uinput)
  macOS:   no virtual gamepad API without a signed driver -> "print" (shows input + stats only)
USB mode also needs:  pip install pyusb libusb-package   (Windows: WinUSB driver via Zadig, see README)

Every 5 s it prints MEASURED stats: packet rate, lost packets (sequence gaps), and the
spread of packet arrival times (jitter). In Wi-Fi mode it also echoes every packet back
so the Switch screen shows the measured round-trip time.
"""
import argparse, socket, struct, sys, time, platform, statistics

PORT = 47111
FMT = "<2sBBIIhhhhHHHHQ"          # matches PadlinkPacket in padlink_proto.h (36 bytes)
SIZE = struct.calcsize(FMT)
assert SIZE == 36
TYPE_STATE, TYPE_ECHO = 1, 2
FLAG_TOUCH = 1

# libnx HidNpadButton bits
A, B, X, Y, LS, RS, L, R, ZL, ZR, PLUS, MINUS, LEFT, UP, RIGHT, DOWN = range(16)


def parse(data):
    if len(data) != SIZE:
        return None
    m, ver, typ, seq, btn, lx, ly, rx, ry, tx, ty, flags, _res, t_us = struct.unpack(FMT, data)
    if m != b"PL" or ver != 1:
        return None
    return dict(type=typ, seq=seq, buttons=btn, lx=lx, ly=ly, rx=rx, ry=ry,
                touch=(tx, ty) if flags & FLAG_TOUCH else None, t_us=t_us)


# ------------------------------------------------------------------ backends --

class PrintBackend:
    def __init__(self, args):
        self.last = None
        self.verbose = args.verbose

    def update(self, s):
        key = (s["buttons"], s["lx"] // 2048, s["ly"] // 2048, s["rx"] // 2048, s["ry"] // 2048, s["touch"])
        if key != self.last or self.verbose:
            names = "A B X Y LS RS L R ZL ZR + - Left Up Right Down".split()
            held = [n for i, n in enumerate(names) if s["buttons"] >> i & 1]
            t = f" touch={s['touch']}" if s["touch"] else ""
            print(f"  {' '.join(held) or '-':30s} L({s['lx']:6d},{s['ly']:6d}) R({s['rx']:6d},{s['ry']:6d}){t}")
            self.last = key

    def close(self):
        pass


class VigemBackend:
    """Windows: Xbox 360 pad. Default layout is positional (Switch A, the east button,
    becomes Xbox B). --label-layout maps by letter instead."""
    def __init__(self, args):
        import vgamepad as vg
        self.vg = vg
        self.pad = vg.VX360Gamepad()
        XB = vg.XUSB_BUTTON
        face = ({A: XB.XUSB_GAMEPAD_A, B: XB.XUSB_GAMEPAD_B, X: XB.XUSB_GAMEPAD_X, Y: XB.XUSB_GAMEPAD_Y}
                if args.label_layout else
                {A: XB.XUSB_GAMEPAD_B, B: XB.XUSB_GAMEPAD_A, X: XB.XUSB_GAMEPAD_Y, Y: XB.XUSB_GAMEPAD_X})
        self.map = dict(face)
        self.map.update({L: XB.XUSB_GAMEPAD_LEFT_SHOULDER, R: XB.XUSB_GAMEPAD_RIGHT_SHOULDER,
                         LS: XB.XUSB_GAMEPAD_LEFT_THUMB, RS: XB.XUSB_GAMEPAD_RIGHT_THUMB,
                         PLUS: XB.XUSB_GAMEPAD_START, MINUS: XB.XUSB_GAMEPAD_BACK,
                         UP: XB.XUSB_GAMEPAD_DPAD_UP, DOWN: XB.XUSB_GAMEPAD_DPAD_DOWN,
                         LEFT: XB.XUSB_GAMEPAD_DPAD_LEFT, RIGHT: XB.XUSB_GAMEPAD_DPAD_RIGHT})
        self.prev = 0
        print("Virtual Xbox 360 controller created (ViGEmBus).")

    def update(self, s):
        b = s["buttons"]
        for bit, xb in self.map.items():
            if b >> bit & 1:
                self.pad.press_button(button=xb)
            else:
                self.pad.release_button(button=xb)
        self.pad.left_trigger(value=255 if b >> ZL & 1 else 0)
        self.pad.right_trigger(value=255 if b >> ZR & 1 else 0)
        clamp = lambda v: max(-32768, min(32767, v))
        self.pad.left_joystick(x_value=clamp(s["lx"]), y_value=clamp(s["ly"]))   # both +Y = up
        self.pad.right_joystick(x_value=clamp(s["rx"]), y_value=clamp(s["ry"]))
        self.pad.update()

    def close(self):
        self.pad.reset(); self.pad.update()


class UinputBackend:
    """Linux: a generic gamepad via uinput (shows up in Steam / SDL / evtest)."""
    def __init__(self, args):
        from evdev import UInput, ecodes as e, AbsInfo
        self.e = e
        ax = AbsInfo(value=0, min=-32768, max=32767, fuzz=0, flat=0, resolution=0)
        hat = AbsInfo(value=0, min=-1, max=1, fuzz=0, flat=0, resolution=0)
        trig = AbsInfo(value=0, min=0, max=255, fuzz=0, flat=0, resolution=0)
        face = ({A: e.BTN_SOUTH, B: e.BTN_EAST, X: e.BTN_NORTH, Y: e.BTN_WEST} if args.label_layout else
                {A: e.BTN_EAST, B: e.BTN_SOUTH, X: e.BTN_NORTH, Y: e.BTN_WEST})
        self.map = dict(face)
        self.map.update({L: e.BTN_TL, R: e.BTN_TR, ZL: e.BTN_TL2, ZR: e.BTN_TR2, LS: e.BTN_THUMBL,
                         RS: e.BTN_THUMBR, PLUS: e.BTN_START, MINUS: e.BTN_SELECT})
        caps = {e.EV_KEY: list(self.map.values()),
                e.EV_ABS: [(e.ABS_X, ax), (e.ABS_Y, ax), (e.ABS_RX, ax), (e.ABS_RY, ax),
                           (e.ABS_Z, trig), (e.ABS_RZ, trig), (e.ABS_HAT0X, hat), (e.ABS_HAT0Y, hat)]}
        self.ui = UInput(caps, name="PadLink Switch Controller", vendor=0x057E, product=0x3000)
        print("Virtual uinput gamepad created.")

    def update(self, s):
        e, b, w = self.e, s["buttons"], self.ui.write
        for bit, code in self.map.items():
            w(e.EV_KEY, code, b >> bit & 1)
        w(e.EV_ABS, e.ABS_X, s["lx"]); w(e.EV_ABS, e.ABS_Y, -s["ly"])     # evdev: +Y = down
        w(e.EV_ABS, e.ABS_RX, s["rx"]); w(e.EV_ABS, e.ABS_RY, -s["ry"])
        w(e.EV_ABS, e.ABS_Z, 255 if b >> ZL & 1 else 0); w(e.EV_ABS, e.ABS_RZ, 255 if b >> ZR & 1 else 0)
        w(e.EV_ABS, e.ABS_HAT0X, (b >> RIGHT & 1) - (b >> LEFT & 1))
        w(e.EV_ABS, e.ABS_HAT0Y, (b >> DOWN & 1) - (b >> UP & 1))
        self.ui.syn()

    def close(self):
        self.ui.close()


def make_backend(args):
    name = args.backend
    if name == "auto":
        name = {"Windows": "vigem", "Linux": "uinput"}.get(platform.system(), "print")
    try:
        return {"vigem": VigemBackend, "uinput": UinputBackend, "print": PrintBackend}[name](args)
    except ImportError as ex:
        hint = {"vigem": "pip install vgamepad", "uinput": "pip install evdev"}.get(name, "")
        print(f"Backend '{name}' unavailable ({ex}). {hint}\nFalling back to printing input.")
        return PrintBackend(args)


# --------------------------------------------------------------------- stats --

class Stats:
    def __init__(self, csv_path=None):
        self.reset()
        self.total = 0
        self.total_lost = 0
        self.last_seq = None
        self.csv = open(csv_path, "w") if csv_path else None
        if self.csv:
            self.csv.write("pc_time_s,seq,switch_t_us,buttons\n")

    def reset(self):
        self.n = 0
        self.lost = 0
        self.gaps = []
        self.last_arrival = None
        self.t0 = time.perf_counter()

    def packet(self, s):
        now = time.perf_counter()
        if self.last_seq is not None:
            d = (s["seq"] - self.last_seq) & 0xFFFFFFFF
            if 1 < d < 100000:
                self.lost += d - 1
                self.total_lost += d - 1
            elif d == 0 or d >= 100000:
                pass                       # duplicate or the Switch app restarted
        self.last_seq = s["seq"]
        if self.last_arrival is not None:
            self.gaps.append((now - self.last_arrival) * 1000)
        self.last_arrival = now
        self.n += 1
        self.total += 1
        if self.csv:
            self.csv.write(f"{now:.6f},{s['seq']},{s['t_us']},{s['buttons']}\n")

    def maybe_print(self, every=5.0):
        el = time.perf_counter() - self.t0
        if el < every:
            return
        if self.n > 2:
            g = sorted(self.gaps)
            p = lambda q: g[min(len(g) - 1, int(q * len(g)))]
            loss = 100.0 * self.lost / max(1, self.n + self.lost)
            print(f"[MEASURED {el:4.1f}s] {self.n / el:6.1f} pkt/s  lost {self.lost} ({loss:.2f}%)  "
                  f"arrival gap ms: median {p(0.5):.2f}  p99 {p(0.99):.2f}  max {g[-1]:.2f}  "
                  f"(jitter sd {statistics.pstdev(g):.2f})")
        else:
            print(f"[{el:4.1f}s] waiting for the Switch...")
        self.reset()


# ----------------------------------------------------------------- transports --

def run_udp(args, backend, stats):
    sk = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sk.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sk.bind(("0.0.0.0", args.port))
    sk.settimeout(0.5)
    print(f"Listening for the Switch on UDP port {args.port}. Allow Python through the firewall if asked.")
    peer = None
    while True:
        try:
            data, addr = sk.recvfrom(64)
        except socket.timeout:
            stats.maybe_print()
            continue
        s = parse(data)
        if not s or s["type"] != TYPE_STATE:
            continue
        sk.sendto(data[:3] + bytes([TYPE_ECHO]) + data[4:], addr)    # echo for RTT on the Switch
        if addr != peer:
            print(f"Switch connected from {addr[0]}")
            peer = addr
        stats.packet(s)
        backend.update(s)
        stats.maybe_print()


def run_usb(args, backend, stats):
    import usb.core, usb.util
    backend_kw = {}
    try:
        import libusb_package
        backend_kw["backend"] = libusb_package.get_libusb1_backend()
    except ImportError:
        pass
    print("Waiting for the Switch on USB (057E:3000). Start PadLink in 'USB to receiver' mode.")
    while True:
        dev = usb.core.find(idVendor=0x057E, idProduct=0x3000, **backend_kw)
        if dev is None:
            time.sleep(0.5)
            stats.maybe_print()
            continue
        try:
            try:
                if dev.is_kernel_driver_active(0):
                    dev.detach_kernel_driver(0)
            except (NotImplementedError, usb.core.USBError):
                pass
            dev.set_configuration()
            intf = dev.get_active_configuration()[(0, 0)]
            ep_in = usb.util.find_descriptor(
                intf, custom_match=lambda ep: usb.util.endpoint_direction(ep.bEndpointAddress) == usb.util.ENDPOINT_IN)
            print(f"Switch connected over USB (IN endpoint 0x{ep_in.bEndpointAddress:02X}).")
            while True:
                try:
                    data = bytes(ep_in.read(512, timeout=200))
                except usb.core.USBTimeoutError:
                    stats.maybe_print()
                    continue
                for off in range(0, len(data) - SIZE + 1, SIZE):
                    s = parse(data[off:off + SIZE])
                    if s and s["type"] == TYPE_STATE:
                        stats.packet(s)
                        backend.update(s)
                stats.maybe_print()
        except usb.core.USBError as ex:
            print(f"USB disconnected ({ex}). Waiting again...")
            usb.util.dispose_resources(dev)
            time.sleep(0.5)


def main():
    ap = argparse.ArgumentParser(description="PadLink PC receiver")
    ap.add_argument("--usb", action="store_true", help="receive over USB instead of Wi-Fi")
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--backend", default="auto", choices=["auto", "vigem", "uinput", "print"])
    ap.add_argument("--label-layout", action="store_true",
                    help="map Switch A->Xbox A by letter (default: by position, Switch A->Xbox B)")
    ap.add_argument("--csv", help="write every packet's arrival time to this CSV file")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    backend = make_backend(args)
    stats = Stats(args.csv)
    try:
        (run_usb if args.usb else run_udp)(args, backend, stats)
    except KeyboardInterrupt:
        pass
    finally:
        backend.close()
        print(f"\nTotal packets {stats.total}, lost {stats.total_lost}.")


if __name__ == "__main__":
    main()
