// padlink_proto.h - wire format shared by the Switch app and pc-receiver/padlink_receiver.py.
// Used by the Wi-Fi (UDP) mode and the USB "PC receiver" (vendor bulk) mode.
// The USB HID mode does not use this; it sends a standard HID gamepad report.
#pragma once
#include <stdint.h>

#define PADLINK_UDP_PORT   47111
#define PADLINK_MAGIC0     'P'
#define PADLINK_MAGIC1     'L'
#define PADLINK_VERSION    1

#define PADLINK_TYPE_STATE 0x01   // Switch -> PC: controller state
#define PADLINK_TYPE_ECHO  0x02   // PC -> Switch: echo of a state packet (Wi-Fi only, for RTT)
#define PADLINK_TYPE_STATE_MOTION 0x03 // Switch -> PC: PadlinkMotionPacket (state + motion)

#define PADLINK_FLAG_TOUCH 0x0001 // touch_x / touch_y are valid

// Buttons use the libnx HidNpadButton bit layout (low 32 bits):
// 0 A, 1 B, 2 X, 3 Y, 4 StickL, 5 StickR, 6 L, 7 R, 8 ZL, 9 ZR, 10 Plus, 11 Minus,
// 12 Left, 13 Up, 14 Right, 15 Down.
// Sticks are libnx units (about -32767..32767, +Y = up). Touch is 0..1279 x 0..719.
typedef struct __attribute__((packed)) {
    uint8_t  magic0, magic1;  // 'P', 'L'
    uint8_t  version;         // PADLINK_VERSION
    uint8_t  type;            // PADLINK_TYPE_*
    uint32_t seq;             // increments by one per packet sent
    uint32_t buttons;
    int16_t  lx, ly, rx, ry;
    uint16_t touch_x, touch_y;
    uint16_t flags;
    uint16_t reserved;
    uint64_t t_send_us;       // Switch clock when sent; echoed back unchanged for RTT
} PadlinkPacket;

_Static_assert(sizeof(PadlinkPacket) == 36, "PadlinkPacket must be 36 bytes");

// Cemu mode: the state plus motion, in Pro Controller axes after the Settings
// (invert / sensitivity). The receiver serves it to Cemu over the DSU protocol.
typedef struct __attribute__((packed)) {
    PadlinkPacket base;       // base.type = PADLINK_TYPE_STATE_MOTION
    float accel_g[3];         // G
    float gyro_dps[3];        // degrees per second
} PadlinkMotionPacket;

_Static_assert(sizeof(PadlinkMotionPacket) == 60, "PadlinkMotionPacket must be 60 bytes");
