#include <string.h>
#include <stdlib.h>
#include "usb_vendor.h"
#include "log.h"

static u8  *s_buf;          // 0x1000-aligned, required by usbCommsWriteAsync
static bool s_up, s_pending;
static u32  s_urb, s_done;

Result usbVendorInit(void) {
    s_buf = aligned_alloc(0x1000, 0x1000);
    if (!s_buf) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    memset(s_buf, 0, 0x1000);
    // NULL info = one vendor-class interface with a bulk IN and a bulk OUT endpoint.
    Result rc = usbCommsInitializeEx(1, NULL, 0x057E, 0x3000);
    LOG_RC("usbCommsInitializeEx (vendor)", rc);
    if (R_FAILED(rc)) { free(s_buf); s_buf = NULL; return rc; }
    s_up = true;
    return 0;
}

void usbVendorExit(void) {
    if (s_up) { usbCommsExit(); s_up = false; LOG("usbCommsExit done"); }
    free(s_buf); s_buf = NULL;
    s_pending = false;
}

bool usbVendorIsConfigured(void) {
    UsbState st;
    return s_up && R_SUCCEEDED(usbDsGetState(&st)) && st == UsbState_Configured;
}

Result usbVendorSend(const PadlinkPacket *pkt, bool *queued) {
    *queued = false;
    if (!s_up) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    if (s_pending) {
        Event *ev = usbCommsGetWriteCompletionEvent(0);
        if (!ev || R_FAILED(eventWait(ev, 0))) return 0;    // host hasn't read the last one yet
        u32 xfer = 0;
        Result rc = usbCommsGetWriteResult(s_urb, &xfer, 0);  // also clears the event
        s_pending = false;
        if (R_FAILED(rc)) return rc;
        s_done++;
    }
    if (!usbVendorIsConfigured()) return 0;
    memcpy(s_buf, pkt, sizeof *pkt);
    Result rc = usbCommsWriteAsync(s_buf, sizeof *pkt, &s_urb, 0);
    if (R_SUCCEEDED(rc)) { s_pending = true; *queued = true; }
    return rc;
}

u32 usbVendorCompleted(void) { return s_done; }
