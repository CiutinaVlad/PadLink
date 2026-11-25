#pragma once
#include <switch.h>
#include "input.h"

// USB HID gamepad mode (no PC driver needed). Experimental: see the stage notes in usb_hid.c.
Result usbHidInit(int stage);   // 1 = descriptors only, 2 = + control-request thread
void   usbHidExit(void);
bool   usbHidIsConfigured(void);
Result usbHidSendPad(const InputState *in, bool *queued);
u32    usbHidCompleted(void);
