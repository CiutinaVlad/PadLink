// xinput.c - wired Xbox 360-style controller (XInput) over usb:ds.
// Layout from public USB descriptor dumps of wired Xbox 360 pads: vendor-specific
// interface (FF/5D/01) with a class descriptor (type 0x21), interrupt IN for the
// 20-byte input report and interrupt OUT for rumble/LED (read and ignored).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include "xinput.h"
#include "settings.h"
#include "log.h"

#define X_VID 0x045E
#define X_PID 0x028E

static UsbDsInterface *s_intf;
static UsbDsEndpoint  *s_ep_in, *s_ep_out;
static u8 *s_in_buf, *s_out_buf, *s_ctrl_buf;
static Thread s_ctrl_thread, s_rx_thread;
static volatile bool s_run;
static bool s_pending, s_out_posted;
static u32 s_urb, s_out_urb, s_reports;
static u64 s_post_tick, s_last_report;
static u8 s_last[20];

// --------------------------------------------------------------- control ----
static Result ctrl_wait(Event *ev) { Result rc = eventWait(ev, 1000000000ULL); eventClear(ev); return rc; }

static void ctrl_thread(void *arg) {
    (void)arg;
    while (s_run) {
        if (R_FAILED(eventWait(&s_intf->SetupEvent, 100000000ULL))) continue;
        eventClear(&s_intf->SetupEvent);
        struct usb_control_setup su __attribute__((aligned(8))) = {0};
        if (R_FAILED(usbDsInterface_GetSetupPacket(s_intf, &su, sizeof su))) continue;
        u32 urb;
        bool vendor = (su.bmRequestType & 0x60) == 0x40;
        if (vendor && (su.bmRequestType & 0x80) && su.wLength <= 64) {
            // Vendor IN requests (driver capability queries): answer with zeros.
            memset(s_ctrl_buf, 0, su.wLength);
            if (R_SUCCEEDED(usbDsInterface_CtrlInPostBufferAsync(s_intf, s_ctrl_buf, su.wLength, &urb)) &&
                R_SUCCEEDED(ctrl_wait(&s_intf->CtrlInCompletionEvent)) &&
                R_SUCCEEDED(usbDsInterface_CtrlOutPostBufferAsync(s_intf, s_ctrl_buf, 0, &urb)))
                ctrl_wait(&s_intf->CtrlOutCompletionEvent);
        } else if (vendor && su.wLength <= 64) {
            // Vendor OUT: take the data (if any) and acknowledge.
            if (su.wLength && (R_FAILED(usbDsInterface_CtrlOutPostBufferAsync(s_intf, s_ctrl_buf, su.wLength, &urb)) ||
                               R_FAILED(ctrl_wait(&s_intf->CtrlOutCompletionEvent)))) continue;
            if (R_SUCCEEDED(usbDsInterface_CtrlInPostBufferAsync(s_intf, s_ctrl_buf, 0, &urb)))
                ctrl_wait(&s_intf->CtrlInCompletionEvent);
        } else {
            LOG("xinput ctrl %02X/%02X wValue %04X -> stall", su.bmRequestType, su.bRequest, su.wValue);
            usbDsInterface_StallCtrl(s_intf);
        }
    }
}

// Drain rumble/LED output reports so the host never stalls on them.
static void rx_thread(void *arg) {
    (void)arg;
    while (s_run) {
        UsbState st;
        if (!s_out_posted) {
            if (R_FAILED(usbDsGetState(&st)) || st != UsbState_Configured ||
                R_FAILED(usbDsEndpoint_PostBufferAsync(s_ep_out, s_out_buf, 32, &s_out_urb))) {
                svcSleepThread(20000000ULL);
                continue;
            }
            s_out_posted = true;
        }
        if (R_FAILED(eventWait(&s_ep_out->CompletionEvent, 100000000ULL))) continue;
        eventClear(&s_ep_out->CompletionEvent);
        UsbDsReportData rd;
        usbDsEndpoint_GetReportData(s_ep_out, &rd);
        s_out_posted = false;
    }
}

// ------------------------------------------------------------------- init ---
static Result set_device(void) {
    Result rc;
    u8 iM = 0, iP = 0, iS = 0;
    static const u16 langs[1] = { 0x0409 };
    rc = usbDsAddUsbLanguageStringDescriptor(NULL, langs, 1);
    if (R_SUCCEEDED(rc)) rc = usbDsAddUsbStringDescriptor(&iM, "PadLink");
    if (R_SUCCEEDED(rc)) rc = usbDsAddUsbStringDescriptor(&iP, "Controller (PadLink)");
    if (R_SUCCEEDED(rc)) rc = usbDsAddUsbStringDescriptor(&iS, "PL0001");
    if (R_FAILED(rc)) return rc;
    struct usb_device_descriptor dev = {
        .bLength = USB_DT_DEVICE_SIZE, .bDescriptorType = USB_DT_DEVICE, .bcdUSB = 0x0200,
        .bDeviceClass = 0xFF, .bDeviceSubClass = 0xFF, .bDeviceProtocol = 0xFF, .bMaxPacketSize0 = 0x40,
        .idVendor = X_VID, .idProduct = X_PID, .bcdDevice = 0x0114,
        .iManufacturer = iM, .iProduct = iP, .iSerialNumber = iS, .bNumConfigurations = 1,
    };
    if (R_FAILED(rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Full, &dev))) return rc;
    if (R_FAILED(rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &dev))) return rc;
    dev.bcdUSB = 0x0300; dev.bMaxPacketSize0 = 0x09;
    if (R_FAILED(rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &dev))) return rc;
    static const u8 bos[0x16] = {
        0x05, USB_DT_BOS, 0x16, 0x00, 0x02,
        0x07, USB_DT_DEVICE_CAPABILITY, 0x02, 0x02, 0x00, 0x00, 0x00,
        0x0A, USB_DT_DEVICE_CAPABILITY, 0x03, 0x00, 0x0E, 0x00, 0x03, 0x00, 0x00, 0x00
    };
    return usbDsSetBinaryObjectStore(bos, sizeof bos);
}

static Result set_interface(void) {
    Result rc = usbDsRegisterInterface(&s_intf);
    if (R_FAILED(rc)) return rc;
    u8 n = s_intf->interface_index;
    struct usb_interface_descriptor intf = {
        .bLength = USB_DT_INTERFACE_SIZE, .bDescriptorType = USB_DT_INTERFACE, .bInterfaceNumber = n,
        .bNumEndpoints = 2, .bInterfaceClass = 0xFF, .bInterfaceSubClass = 0x5D, .bInterfaceProtocol = 0x01,
    };
    u8 ep_in = USB_ENDPOINT_IN | (n + 1), ep_out = USB_ENDPOINT_OUT | (n + 1);
    const u8 xdesc[17] = { 0x11, 0x21, 0x00, 0x01, 0x01, 0x25, ep_in, 0x14, 0x00, 0x00, 0x00, 0x00,
                           0x13, ep_out, 0x08, 0x00, 0x00 };
    struct usb_endpoint_descriptor ein = {
        .bLength = USB_DT_ENDPOINT_SIZE, .bDescriptorType = USB_DT_ENDPOINT, .bEndpointAddress = ep_in,
        .bmAttributes = USB_TRANSFER_TYPE_INTERRUPT, .wMaxPacketSize = 0x20, .bInterval = 1,
    };
    struct usb_endpoint_descriptor eout = ein;
    eout.bEndpointAddress = ep_out; eout.bInterval = 8;
    struct usb_ss_endpoint_companion_descriptor ss = {
        .bLength = USB_DT_SS_ENDPOINT_COMPANION_SIZE, .bDescriptorType = USB_DT_SS_ENDPOINT_COMPANION,
        .wBytesPerInterval = 0x20,
    };
    UsbDeviceSpeed sp[3] = { UsbDeviceSpeed_Full, UsbDeviceSpeed_High, UsbDeviceSpeed_Super };
    for (int i = 0; i < 3; i++) {
        ein.bInterval = (i == 0) ? 1 : 4;          // 1 ms at every speed
        eout.bInterval = (i == 0) ? 8 : 7;
        if (R_FAILED(rc = usbDsInterface_AppendConfigurationData(s_intf, sp[i], &intf, sizeof intf))) return rc;
        if (R_FAILED(rc = usbDsInterface_AppendConfigurationData(s_intf, sp[i], xdesc, sizeof xdesc))) return rc;
        if (R_FAILED(rc = usbDsInterface_AppendConfigurationData(s_intf, sp[i], &ein, sizeof ein))) return rc;
        if (i == 2 && R_FAILED(rc = usbDsInterface_AppendConfigurationData(s_intf, sp[i], &ss, sizeof ss))) return rc;
        if (R_FAILED(rc = usbDsInterface_AppendConfigurationData(s_intf, sp[i], &eout, sizeof eout))) return rc;
        if (i == 2 && R_FAILED(rc = usbDsInterface_AppendConfigurationData(s_intf, sp[i], &ss, sizeof ss))) return rc;
    }
    if (R_FAILED(rc = usbDsInterface_RegisterEndpoint(s_intf, &s_ep_in, ep_in))) return rc;
    if (R_FAILED(rc = usbDsInterface_RegisterEndpoint(s_intf, &s_ep_out, ep_out))) return rc;
    return usbDsInterface_EnableInterface(s_intf);
}

#define STEP(name, call) do { rc = (call); LOG_RC(name, rc); if (R_FAILED(rc)) goto fail; } while (0)

Result xinputInit(void) {
    Result rc;
    s_in_buf = aligned_alloc(0x1000, 0x1000);
    s_out_buf = aligned_alloc(0x1000, 0x1000);
    s_ctrl_buf = aligned_alloc(0x1000, 0x1000);
    if (!s_in_buf || !s_out_buf || !s_ctrl_buf) { xinputExit(); return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory); }
    memset(s_in_buf, 0, 0x1000); memset(s_out_buf, 0, 0x1000); memset(s_ctrl_buf, 0, 0x1000);
    memset(s_last, 0, sizeof s_last);
    STEP("usbDsInitialize", usbDsInitialize());
    STEP("Xbox descriptors", set_device());
    STEP("Xbox interface", set_interface());
    s_run = true;
    STEP("threadCreate ctrl", threadCreate(&s_ctrl_thread, ctrl_thread, NULL, NULL, 0x4000, 0x2C, -2));
    STEP("threadStart ctrl", threadStart(&s_ctrl_thread));
    STEP("threadCreate rx", threadCreate(&s_rx_thread, rx_thread, NULL, NULL, 0x4000, 0x2C, -2));
    STEP("threadStart rx", threadStart(&s_rx_thread));
    STEP("usbDsEnable", usbDsEnable());
    return 0;
fail:
    xinputExit();
    return rc;
}

void xinputExit(void) {
    if (s_run) {
        s_run = false;
        if (s_ep_out) usbDsEndpoint_Cancel(s_ep_out);
        threadWaitForExit(&s_ctrl_thread); threadClose(&s_ctrl_thread);
        threadWaitForExit(&s_rx_thread);   threadClose(&s_rx_thread);
    }
    if (s_ep_in && s_pending) usbDsEndpoint_Cancel(s_ep_in);
    s_pending = s_out_posted = false;
    usbDsExit();
    s_intf = NULL; s_ep_in = s_ep_out = NULL;
    free(s_in_buf); free(s_out_buf); free(s_ctrl_buf);
    s_in_buf = s_out_buf = s_ctrl_buf = NULL;
}

bool xinputIsConfigured(void) {
    UsbState st;
    return s_intf && R_SUCCEEDED(usbDsGetState(&st)) && st == UsbState_Configured;
}

u32 xinputReports(void) { return s_reports; }

static s16 axis(s32 v, bool invert) {
    if (invert) v = -v;
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (s16)v;
}

static void build(const InputState *in, u8 r[20]) {
    u64 b = in->buttons;
    bool guide = (b & HidNpadButton_Minus) && (b & HidNpadButton_StickR);   // HOME is taken by the Switch itself
    if (guide) b &= ~(u64)(HidNpadButton_Minus | HidNpadButton_StickR);
    bool label = g_settings.xbox_layout == LAYOUT_LABEL;
    // Xbox A is the bottom button. By position: Switch B (bottom) -> Xbox A. By label: Switch A -> Xbox A.
    bool xa = label ? (b & HidNpadButton_A) : (b & HidNpadButton_B);
    bool xb = label ? (b & HidNpadButton_B) : (b & HidNpadButton_A);
    bool xx = label ? (b & HidNpadButton_X) : (b & HidNpadButton_Y);
    bool xy = label ? (b & HidNpadButton_Y) : (b & HidNpadButton_X);
    memset(r, 0, 20);
    r[0] = 0x00; r[1] = 0x14;
    r[2] = ((b & HidNpadButton_Up) ? 0x01 : 0) | ((b & HidNpadButton_Down) ? 0x02 : 0) |
           ((b & HidNpadButton_Left) ? 0x04 : 0) | ((b & HidNpadButton_Right) ? 0x08 : 0) |
           ((b & HidNpadButton_Plus) ? 0x10 : 0) | ((b & HidNpadButton_Minus) ? 0x20 : 0) |
           ((b & HidNpadButton_StickL) ? 0x40 : 0) | ((b & HidNpadButton_StickR) ? 0x80 : 0);
    r[3] = ((b & HidNpadButton_L) ? 0x01 : 0) | ((b & HidNpadButton_R) ? 0x02 : 0) | (guide ? 0x04 : 0) |
           (xa ? 0x10 : 0) | (xb ? 0x20 : 0) | (xx ? 0x40 : 0) | (xy ? 0x80 : 0);
    r[4] = (b & HidNpadButton_ZL) ? 0xFF : 0;
    r[5] = (b & HidNpadButton_ZR) ? 0xFF : 0;
    s16 v[4] = { axis(in->ls.x, false), axis(in->ls.y, false), axis(in->rs.x, false), axis(in->rs.y, false) };
    for (int i = 0; i < 4; i++) { r[6 + 2*i] = (u8)(v[i] & 0xFF); r[7 + 2*i] = (u8)((u16)v[i] >> 8); }
}

Result xinputUpdate(const InputState *in) {
    if (!s_ep_in) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    if (!xinputIsConfigured()) return 0;
    if (s_pending) {
        if (R_FAILED(eventWait(&s_ep_in->CompletionEvent, 0))) {
            if (armTicksToNs(armGetSystemTick() - s_post_tick) < 100000000ULL) return 0;
            usbDsEndpoint_Cancel(s_ep_in);
            eventWait(&s_ep_in->CompletionEvent, 50000000ULL);
        }
        eventClear(&s_ep_in->CompletionEvent);
        UsbDsReportData rd;
        usbDsEndpoint_GetReportData(s_ep_in, &rd);
        s_pending = false;
    }
    u8 r[20];
    build(in, r);
    u64 now = armTicksToNs(armGetSystemTick());
    // Send on change, plus a refresh every 8 ms (or every poll in low-latency mode).
    u64 refresh = g_settings.low_latency ? 0 : 8000000ULL;
    if (!memcmp(r, s_last, 20) && now - s_last_report < refresh) return 0;
    memcpy(s_in_buf, r, 20);
    Result rc = usbDsEndpoint_PostBufferAsync(s_ep_in, s_in_buf, 20, &s_urb);
    if (R_SUCCEEDED(rc)) {
        s_pending = true; s_post_tick = armGetSystemTick(); s_last_report = now;
        memcpy(s_last, r, 20); s_reports++;
    }
    return rc;
}
