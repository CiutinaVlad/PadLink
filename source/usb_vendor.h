#pragma once
#include <switch.h>
#include "padlink_proto.h"

// USB "PC receiver" mode: libnx usbComms (vendor-class bulk, VID 057E PID 3000), the
// same proven path Goldleaf/nxlink-style tools and NxUsbX360Con use. Needs
// padlink_receiver.py --usb on the PC (plus the WinUSB driver on Windows).
Result usbVendorInit(void);
void   usbVendorExit(void);
bool   usbVendorIsConfigured(void);
// Non-blocking: queues pkt if the previous transfer finished, else keeps the newest
// state and sends it as soon as the host has read the previous one.
Result usbVendorSend(const PadlinkPacket *pkt, bool *queued);
u32    usbVendorCompleted(void);
