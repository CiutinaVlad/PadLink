#pragma once
#include <switch.h>
#include "input.h"

// "PC: Xbox controller" mode: the Switch shows up as a wired Xbox 360-style
// controller (XInput). Windows and Linux (xpad) have the driver built in, and
// Steam and most PC games support it directly.
Result xinputInit(void);
void   xinputExit(void);
bool   xinputIsConfigured(void);
Result xinputUpdate(const InputState *in);
u32    xinputReports(void);
