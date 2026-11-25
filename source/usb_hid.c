// usb_hid.c - Switch (Horizon, FW 5.0.0+) as a USB HID gamepad via usb:ds.
// Target: libnx v4.11.0, FW 16.0.2. Run as a full-RAM title (not applet mode).
//
// Key differences from the crashing version:
//  1. Device descriptors for Full, High AND Super speed, plus a BOS descriptor,
//     are set BEFORE registering the interface (same order as libnx usb_comms.c).
//  2. Configuration data is appended for Full, High AND Super speed
//     (Super needs an SS endpoint companion descriptor after each endpoint).
//  3. bInterfaceNumber comes from interface_index, and the endpoint address is
//     0x80 | (interface_index + 1). Never 0x80: that is endpoint 0 (control).
//  4. Interface-directed control requests (GET_DESCRIPTOR 0x22 = HID report
//     descriptor, SET_IDLE, ...) are answered on a thread via the interface's
//     SetupEvent / CtrlIn / CtrlOut calls.
//  5. Every usb:ds buffer is 0x1000-aligned, and usbDsExit() always runs on exit.
//
// PadLink adds crash bisecting: every usb:ds call is logged (fsync'd) to
// sdmc:/switch/padlink/padlink.log, and the setup runs in stages:
//   stage 1: HID interface + HID class descriptor + interrupt IN endpoint, but NO
//            control-request thread (the PC can't read the report descriptor, so it
//            won't work as a gamepad; this only tests whether descriptors crash usb).
//   stage 2: stage 1 + the control-request thread (GET_DESCRIPTOR report, SET_IDLE...).
// If stage 1 crashes with 2162-0002, the descriptors are the problem. If only stage 2
// crashes, the control-request handling is.

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <switch.h>
#include "usb_hid.h"
#include "log.h"

#define USB_VID 0x1209  // pid.codes open-source VID
#define USB_PID 0x0001  // pid.codes "test PID", OK for personal use

// --- HID report descriptor: 16 buttons, 1 hat, 4 x 8-bit axes (7-byte report) ---
static const u8 k_report_desc[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x05,       // Usage (Gamepad)
    0xA1, 0x01,       // Collection (Application)
    0x05, 0x09,       //   Usage Page (Button)
    0x19, 0x01,       //   Usage Minimum (1)
    0x29, 0x10,       //   Usage Maximum (16)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x01,       //   Logical Maximum (1)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x10,       //   Report Count (16)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0x05, 0x01,       //   Usage Page (Generic Desktop)
    0x09, 0x39,       //   Usage (Hat switch)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x07,       //   Logical Maximum (7)
    0x35, 0x00,       //   Physical Minimum (0)
    0x46, 0x3B, 0x01, //   Physical Maximum (315)
    0x65, 0x14,       //   Unit (Degrees)
    0x75, 0x04,       //   Report Size (4)
    0x95, 0x01,       //   Report Count (1)
    0x81, 0x42,       //   Input (Data,Var,Abs,Null)
    0x65, 0x00,       //   Unit (None)
    0x45, 0x00,       //   Physical Maximum (0)
    0x75, 0x04,       //   Report Size (4)
    0x95, 0x01,       //   Report Count (1)
    0x81, 0x03,       //   Input (Const) padding
    0x09, 0x30,       //   Usage (X)
    0x09, 0x31,       //   Usage (Y)
    0x09, 0x32,       //   Usage (Z)
    0x09, 0x35,       //   Usage (Rz)
    0x15, 0x00,       //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x04,       //   Report Count (4)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0xC0              // End Collection
};

typedef struct {
    u16 buttons;
    u8  hat;        // low nibble: 0..7, 8 = centered
    u8  lx, ly, rx, ry;
} NX_PACKED HidGamepadReport;
_Static_assert(sizeof(HidGamepadReport) == 7, "report must be 7 bytes");

static UsbDsInterface *s_intf;
static UsbDsEndpoint  *s_ep_in;
static bool s_pending;
static u32  s_urb;
static u64  s_post_tick;
static u32  s_done;
static u8 *s_ep_buf;    // 0x1000-aligned
static u8 *s_ctrl_buf;  // 0x1000-aligned
static Thread s_ctrl_thread;
static volatile bool s_ctrl_run;

// ---------------------------------------------------------------- control ---

static Result ctrl_wait(Event *ev, u64 timeout_ns) {
    Result rc = eventWait(ev, timeout_ns);
    eventClear(ev);
    return rc;
}

// Data stage IN, then zero-length OUT status stage.
static void ctrl_send_in(const void *data, u32 len, u16 wLength) {
    u32 urb;
    if (len > wLength) len = wLength;
    memcpy(s_ctrl_buf, data, len);
    if (R_FAILED(usbDsInterface_CtrlInPostBufferAsync(s_intf, s_ctrl_buf, len, &urb))) return;
    if (R_FAILED(ctrl_wait(&s_intf->CtrlInCompletionEvent, 1000000000ULL))) return;
    if (R_FAILED(usbDsInterface_CtrlOutPostBufferAsync(s_intf, s_ctrl_buf, 0, &urb))) return;
    ctrl_wait(&s_intf->CtrlOutCompletionEvent, 1000000000ULL);
}

// No data stage: zero-length IN status stage (ACK).
static void ctrl_ack(void) {
    u32 urb;
    if (R_SUCCEEDED(usbDsInterface_CtrlInPostBufferAsync(s_intf, s_ctrl_buf, 0, &urb)))
        ctrl_wait(&s_intf->CtrlInCompletionEvent, 1000000000ULL);
}

static void ctrl_thread_func(void *arg) {
    (void)arg;
    while (s_ctrl_run) {
        if (R_FAILED(eventWait(&s_intf->SetupEvent, 100000000ULL))) continue; // 100 ms poll
        eventClear(&s_intf->SetupEvent);

        struct usb_control_setup setup __attribute__((aligned(8))) = {0};
        Result grc = usbDsInterface_GetSetupPacket(s_intf, &setup, sizeof(setup));
        if (R_FAILED(grc)) { LOG_RC("GetSetupPacket", grc); continue; }
        LOG("ctrl req: bmRequestType=%02X bRequest=%02X wValue=%04X wIndex=%04X wLength=%u",
            setup.bmRequestType, setup.bRequest, setup.wValue, setup.wIndex, setup.wLength);

        if (setup.bmRequestType == 0x81 && setup.bRequest == 0x06) {          // GET_DESCRIPTOR (interface)
            u8 type = setup.wValue >> 8;
            if (type == USB_DT_REPORT) {
                ctrl_send_in(k_report_desc, sizeof(k_report_desc), setup.wLength);
                continue;
            }
        } else if (setup.bmRequestType == 0xA1 && setup.bRequest == 0x01) {   // HID GET_REPORT
            HidGamepadReport idle = { .hat = 8, .lx = 128, .ly = 128, .rx = 128, .ry = 128 };
            ctrl_send_in(&idle, sizeof(idle), setup.wLength);
            continue;
        } else if (setup.bmRequestType == 0x21 &&
                   (setup.bRequest == 0x0A || setup.bRequest == 0x0B)) {      // SET_IDLE / SET_PROTOCOL
            ctrl_ack();
            continue;
        }
        LOG_RC("  -> stall (unhandled)", usbDsInterface_StallCtrl(s_intf));      // anything else
    }
}

// ------------------------------------------------------------------- init ---

static Result set_device_descriptors(void) {
    Result rc;
    u8 iManufacturer = 0, iProduct = 0, iSerial = 0;
    static const u16 langs[1] = {0x0409};

    rc = usbDsAddUsbLanguageStringDescriptor(NULL, langs, 1);
    if (R_SUCCEEDED(rc)) rc = usbDsAddUsbStringDescriptor(&iManufacturer, "Homebrew");
    if (R_SUCCEEDED(rc)) rc = usbDsAddUsbStringDescriptor(&iProduct, "Switch HID Pad");
    if (R_SUCCEEDED(rc)) rc = usbDsAddUsbStringDescriptor(&iSerial, "0001");
    if (R_FAILED(rc)) return rc;

    struct usb_device_descriptor dev = {
        .bLength = USB_DT_DEVICE_SIZE,
        .bDescriptorType = USB_DT_DEVICE,
        .bcdUSB = 0x0110,
        .bDeviceClass = 0x00,          // class defined per interface
        .bDeviceSubClass = 0x00,
        .bDeviceProtocol = 0x00,
        .bMaxPacketSize0 = 0x40,
        .idVendor = USB_VID,
        .idProduct = USB_PID,
        .bcdDevice = 0x0100,
        .iManufacturer = iManufacturer,
        .iProduct = iProduct,
        .iSerialNumber = iSerial,
        .bNumConfigurations = 0x01,
    };
    rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Full, &dev);
    LOG_RC("  SetUsbDeviceDescriptor Full", rc);
    if (R_FAILED(rc)) return rc;

    dev.bcdUSB = 0x0200;
    rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &dev);
    LOG_RC("  SetUsbDeviceDescriptor High", rc);
    if (R_FAILED(rc)) return rc;

    dev.bcdUSB = 0x0300;
    dev.bMaxPacketSize0 = 0x09;        // 2^9 = 512 for SuperSpeed
    rc = usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &dev);
    LOG_RC("  SetUsbDeviceDescriptor Super", rc);
    if (R_FAILED(rc)) return rc;

    static const u8 bos[0x16] = {
        0x05, USB_DT_BOS, 0x16, 0x00, 0x02,
        0x07, USB_DT_DEVICE_CAPABILITY, 0x02, 0x02, 0x00, 0x00, 0x00,                // USB 2.0 ext (LPM)
        0x0A, USB_DT_DEVICE_CAPABILITY, 0x03, 0x00, 0x0E, 0x00, 0x03, 0x00, 0x00, 0x00 // SuperSpeed caps
    };
    return usbDsSetBinaryObjectStore(bos, sizeof(bos));
}

static Result append(UsbDeviceSpeed spd, const void *d, size_t n) {
    Result rc = usbDsInterface_AppendConfigurationData(s_intf, spd, d, n);
    static const char *names[] = {"?", "Full", "High", "Super", "Super+"};
    char step[48];
    snprintf(step, sizeof step, "  Append %s desc type 0x%02X", names[spd < 5 ? spd : 0], ((const u8 *)d)[1]);
    LOG_RC(step, rc);
    return rc;
}

static Result setup_interface(void) {
    Result rc = usbDsRegisterInterface(&s_intf);
    LOG_RC("  usbDsRegisterInterface", rc);
    if (R_FAILED(rc)) return rc;
    LOG("  interface_index = %u", s_intf->interface_index);

    struct usb_interface_descriptor intf = {
        .bLength = USB_DT_INTERFACE_SIZE,
        .bDescriptorType = USB_DT_INTERFACE,
        .bInterfaceNumber = s_intf->interface_index,
        .bNumEndpoints = 1,
        .bInterfaceClass = USB_CLASS_HID,
        .bInterfaceSubClass = 0x00,     // no boot interface
        .bInterfaceProtocol = 0x00,
    };
    struct usb_hid_descriptor hid = {
        .bLength = USB_DT_HID_SIZE,
        .bDescriptorType = USB_DT_HID,
        .bcdHID = 0x0111,
        .bCountryCode = 0,
        .bNumDescriptors = 1,
        .bClassDescriptorType = USB_DT_REPORT,
        .wClassDescriptorLength = sizeof(k_report_desc),
    };
    struct usb_endpoint_descriptor ep = {
        .bLength = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_IN | (s_intf->interface_index + 1),
        .bmAttributes = USB_TRANSFER_TYPE_INTERRUPT,
        .wMaxPacketSize = 0x40,
        .bInterval = 1,                 // Full speed: 1 ms
    };
    struct usb_ss_endpoint_companion_descriptor ss = {
        .bLength = USB_DT_SS_ENDPOINT_COMPANION_SIZE,
        .bDescriptorType = USB_DT_SS_ENDPOINT_COMPANION,
        .bMaxBurst = 0,
        .bmAttributes = 0,
        .wBytesPerInterval = 0x40,
    };

    // Full speed
    if (R_FAILED(rc = append(UsbDeviceSpeed_Full, &intf, sizeof intf))) return rc;
    if (R_FAILED(rc = append(UsbDeviceSpeed_Full, &hid,  sizeof hid)))  return rc;
    if (R_FAILED(rc = append(UsbDeviceSpeed_Full, &ep,   sizeof ep)))   return rc;
    // High speed: bInterval is 2^(n-1) microframes; 4 -> 1 ms
    ep.bInterval = 4;
    if (R_FAILED(rc = append(UsbDeviceSpeed_High, &intf, sizeof intf))) return rc;
    if (R_FAILED(rc = append(UsbDeviceSpeed_High, &hid,  sizeof hid)))  return rc;
    if (R_FAILED(rc = append(UsbDeviceSpeed_High, &ep,   sizeof ep)))   return rc;
    // Super speed: same, plus companion after the endpoint
    if (R_FAILED(rc = append(UsbDeviceSpeed_Super, &intf, sizeof intf))) return rc;
    if (R_FAILED(rc = append(UsbDeviceSpeed_Super, &hid,  sizeof hid)))  return rc;
    if (R_FAILED(rc = append(UsbDeviceSpeed_Super, &ep,   sizeof ep)))   return rc;
    if (R_FAILED(rc = append(UsbDeviceSpeed_Super, &ss,   sizeof ss)))   return rc;

    rc = usbDsInterface_RegisterEndpoint(s_intf, &s_ep_in, ep.bEndpointAddress);
    LOG_RC("  RegisterEndpoint", rc);
    if (R_FAILED(rc)) return rc;
    rc = usbDsInterface_EnableInterface(s_intf);
    LOG_RC("  EnableInterface", rc);
    return rc;
}

#define STEP(name, call) do { rc = (call); LOG_RC(name, rc); if (R_FAILED(rc)) goto fail; } while (0)

Result usbHidInit(int stage) {
    Result rc;
    LOG("USB HID init, stage %d", stage);
    if (hosversionBefore(5, 0, 0)) return MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);

    s_ep_buf   = aligned_alloc(0x1000, 0x1000);
    s_ctrl_buf = aligned_alloc(0x1000, 0x1000);
    if (!s_ep_buf || !s_ctrl_buf) { usbHidExit(); return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory); }
    memset(s_ep_buf, 0, 0x1000);
    memset(s_ctrl_buf, 0, 0x1000);

    STEP("usbDsInitialize", usbDsInitialize());          // also calls usbDsClearDeviceData()
    STEP("device/string/BOS descriptors", set_device_descriptors());
    STEP("interface + endpoint setup", setup_interface());
    if (stage >= 2) {
        s_ctrl_run = true;
        rc = threadCreate(&s_ctrl_thread, ctrl_thread_func, NULL, NULL, 0x4000, 0x2C, -2);
        LOG_RC("threadCreate (control)", rc);
        if (R_SUCCEEDED(rc)) { rc = threadStart(&s_ctrl_thread); LOG_RC("threadStart (control)", rc); }
        if (R_FAILED(rc)) { s_ctrl_run = false; goto fail; }
    } else {
        LOG("stage 1: control thread NOT started (PC can't read the report descriptor)");
    }
    STEP("usbDsEnable (PC can enumerate now)", usbDsEnable());
    return 0;

fail:
    usbHidExit();
    return rc;
}

void usbHidExit(void) {
    if (s_ctrl_run) {
        s_ctrl_run = false;
        threadWaitForExit(&s_ctrl_thread);
        threadClose(&s_ctrl_thread);
    }
    if (s_ep_in && s_pending) usbDsEndpoint_Cancel(s_ep_in);
    s_pending = false;
    LOG("usbDsExit");
    usbDsExit();                                  // closes interface/endpoints, restores the console's USB state
    s_intf = NULL; s_ep_in = NULL;
    free(s_ep_buf);   s_ep_buf = NULL;
    free(s_ctrl_buf); s_ctrl_buf = NULL;
}

// ---------------------------------------------------------------- reports ---

static u8 axis8(s32 v, bool invert) {               // libnx stick range is about -32767..32767
    s32 x = (v + 32768) >> 8;
    if (x < 0) x = 0;
    if (x > 255) x = 255;
    return invert ? (u8)(255 - x) : (u8)x;
}

static u8 hat_from(u64 b) {
    bool u = b & HidNpadButton_Up, d = b & HidNpadButton_Down;
    bool l = b & HidNpadButton_Left, r = b & HidNpadButton_Right;
    if (u && r) return 1;
    if (r && d) return 3;
    if (d && l) return 5;
    if (l && u) return 7;
    if (u) return 0;
    if (r) return 2;
    if (d) return 4;
    if (l) return 6;
    return 8;
}

static const u64 k_btn_map[16] = {
    HidNpadButton_B, HidNpadButton_A, HidNpadButton_Y, HidNpadButton_X,
    HidNpadButton_L, HidNpadButton_R, HidNpadButton_ZL, HidNpadButton_ZR,
    HidNpadButton_Minus, HidNpadButton_Plus, HidNpadButton_StickL, HidNpadButton_StickR,
    0, 0, 0, 0,   // Home/Capture are taken by the system while a title runs
};

bool usbHidIsConfigured(void) {
    UsbState st;
    return R_SUCCEEDED(usbDsGetState(&st)) && st == UsbState_Configured;
}


u32 usbHidCompleted(void) { return s_done; }

// Non-blocking: if the previous report was picked up by the host, queue a new one.
// A report the host hasn't polled for 100 ms is cancelled so the endpoint never jams.
Result usbHidSendPad(const InputState *in, bool *queued) {
    *queued = false;
    if (!s_ep_in) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    if (s_pending) {
        if (R_FAILED(eventWait(&s_ep_in->CompletionEvent, 0))) {
            if (armTicksToNs(armGetSystemTick() - s_post_tick) < 100000000ULL) return 0;
            usbDsEndpoint_Cancel(s_ep_in);
            eventWait(&s_ep_in->CompletionEvent, 50000000ULL);
        }
        eventClear(&s_ep_in->CompletionEvent);
        s_pending = false;
        UsbDsReportData rd; u32 req, xfer;
        Result rc = usbDsEndpoint_GetReportData(s_ep_in, &rd);
        if (R_SUCCEEDED(rc)) rc = usbDsParseReportData(&rd, s_urb, &req, &xfer);
        if (R_SUCCEEDED(rc)) s_done++;
    }
    if (!usbHidIsConfigured()) return 0;

    u64 b = in->buttons;
    HidGamepadReport rep = {0};
    for (int i = 0; i < 16; i++) if (k_btn_map[i] && (b & k_btn_map[i])) rep.buttons |= (1u << i);
    rep.hat = hat_from(b);
    rep.lx = axis8(in->ls.x, false); rep.ly = axis8(in->ls.y, true);   // HID Y grows downward
    rep.rx = axis8(in->rs.x, false); rep.ry = axis8(in->rs.y, true);

    memcpy(s_ep_buf, &rep, sizeof rep);
    Result rc = usbDsEndpoint_PostBufferAsync(s_ep_in, s_ep_buf, sizeof rep, &s_urb);
    if (R_SUCCEEDED(rc)) { s_pending = true; *queued = true; s_post_tick = armGetSystemTick(); }
    return rc;
}
