// wifi.c - Wi-Fi mode: UDP to padlink_receiver.py on the PC.
// Only uses the normal socket service, so it can't disturb usb or bluetooth sysmodules.
//
// Discovery: until the PC answers, packets go to the subnet broadcast address
// (e.g. 192.168.1.255). The receiver echoes every packet back, and the first echo
// tells us the PC's address; after that we send unicast. If your router blocks
// broadcast, put the PC's IP in sdmc:/switch/padlink/pc_ip.txt (one line).
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "wifi.h"
#include "log.h"

#define RTT_RING 512

static int  s_sock = -1;
static bool s_sockets_up, s_nifm_up;
static struct sockaddr_in s_bcast, s_peer;
static bool s_peer_known, s_fixed_peer;
static u64  s_last_echo_us;
static u32  s_sent, s_echoed, s_send_errors;
static float s_rtt[RTT_RING];
static u32  s_rtt_n, s_rtt_i;

static u64 now_us(void) { return armTicksToNs(armGetSystemTick()) / 1000ULL; }

static void load_fixed_peer(void) {
    FILE *f = fopen("sdmc:/switch/padlink/pc_ip.txt", "r");
    if (!f) return;
    char buf[32] = {0};
    if (fgets(buf, sizeof buf, f)) {
        buf[strcspn(buf, "\r\n ")] = 0;
        struct in_addr a;
        if (inet_aton(buf, &a)) {
            s_peer.sin_family = AF_INET;
            s_peer.sin_port = htons(PADLINK_UDP_PORT);
            s_peer.sin_addr = a;
            s_peer_known = s_fixed_peer = true;
            LOG("pc_ip.txt: sending to %s", buf);
        }
    }
    fclose(f);
}

Result wifiInit(void) {
    Result rc = nifmInitialize(NifmServiceType_User);
    LOG_RC("nifmInitialize", rc);
    if (R_FAILED(rc)) return rc;
    s_nifm_up = true;

    u32 ip = 0, mask = 0, gw = 0, dns1 = 0, dns2 = 0;
    rc = nifmGetCurrentIpConfigInfo(&ip, &mask, &gw, &dns1, &dns2);
    LOG_RC("nifmGetCurrentIpConfigInfo", rc);
    if (R_FAILED(rc) || ip == 0) {
        LOG("No network. Connect the Switch to the same Wi-Fi as the PC first.");
        return R_FAILED(rc) ? rc : MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }

    rc = socketInitializeDefault();
    LOG_RC("socketInitializeDefault", rc);
    if (R_FAILED(rc)) return rc;
    s_sockets_up = true;

    s_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_sock < 0) { LOG("socket() failed"); return MAKERESULT(Module_Libnx, LibnxError_IoError); }
    int one = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    fcntl(s_sock, F_SETFL, fcntl(s_sock, F_GETFL, 0) | O_NONBLOCK);

    struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = 0, .sin_addr.s_addr = INADDR_ANY };
    bind(s_sock, (struct sockaddr *)&local, sizeof local);

    memset(&s_bcast, 0, sizeof s_bcast);
    s_bcast.sin_family = AF_INET;
    s_bcast.sin_port = htons(PADLINK_UDP_PORT);
    s_bcast.sin_addr.s_addr = (ip & mask) | ~mask;      // both in network byte order

    struct in_addr a = { .s_addr = ip }, b = s_bcast.sin_addr;
    char ipstr[16]; strncpy(ipstr, inet_ntoa(a), sizeof ipstr - 1); ipstr[15] = 0;
    LOG("Switch IP %s, looking for the PC via %s:%d", ipstr, inet_ntoa(b), PADLINK_UDP_PORT);

    load_fixed_peer();
    return 0;
}

void wifiExit(void) {
    if (s_sock >= 0) { close(s_sock); s_sock = -1; }
    if (s_sockets_up) { socketExit(); s_sockets_up = false; }
    if (s_nifm_up) { nifmExit(); s_nifm_up = false; }
}

Result wifiSend(const PadlinkPacket *pkt) {
    if (s_sock < 0) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    const struct sockaddr_in *dst = s_peer_known ? &s_peer : &s_bcast;
    ssize_t n = sendto(s_sock, pkt, sizeof *pkt, 0, (const struct sockaddr *)dst, sizeof *dst);
    if (n != (ssize_t)sizeof *pkt) { s_send_errors++; return MAKERESULT(Module_Libnx, LibnxError_IoError); }
    s_sent++;
    // PC went quiet for 3 s (receiver restarted?): fall back to broadcast discovery.
    if (s_peer_known && !s_fixed_peer && now_us() - s_last_echo_us > 3000000ULL) {
        s_peer_known = false;
        LOG("No echo for 3 s, searching for the PC again");
    }
    return 0;
}

void wifiPoll(void) {
    if (s_sock < 0) return;
    for (int i = 0; i < 64; i++) {
        PadlinkPacket p;
        struct sockaddr_in from; socklen_t fl = sizeof from;
        ssize_t n = recvfrom(s_sock, &p, sizeof p, 0, (struct sockaddr *)&from, &fl);
        if (n != (ssize_t)sizeof p) break;
        if (p.magic0 != PADLINK_MAGIC0 || p.magic1 != PADLINK_MAGIC1 || p.type != PADLINK_TYPE_ECHO) continue;
        u64 t = now_us();
        s_last_echo_us = t;
        s_echoed++;
        if (!s_peer_known) {
            s_peer = from;
            s_peer.sin_port = htons(PADLINK_UDP_PORT);
            s_peer_known = true;
            LOG("Found PC receiver at %s", inet_ntoa(from.sin_addr));
        }
        if (t > p.t_send_us) {
            s_rtt[s_rtt_i] = (float)(t - p.t_send_us) / 1000.0f;
            s_rtt_i = (s_rtt_i + 1) % RTT_RING;
            if (s_rtt_n < RTT_RING) s_rtt_n++;
        }
    }
}

static int cmpf(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

void wifiGetStats(WifiStats *o) {
    memset(o, 0, sizeof *o);
    o->peer_known = s_peer_known;
    if (s_peer_known) { strncpy(o->peer_ip, inet_ntoa(s_peer.sin_addr), sizeof o->peer_ip - 1); }
    o->sent = s_sent; o->echoed = s_echoed; o->send_errors = s_send_errors;
    if (s_rtt_n) {
        static float tmp[RTT_RING];
        memcpy(tmp, s_rtt, s_rtt_n * sizeof(float));
        qsort(tmp, s_rtt_n, sizeof(float), cmpf);
        o->rtt_median_ms = tmp[s_rtt_n / 2];
        o->rtt_p99_ms = tmp[(s_rtt_n * 99) / 100];
        o->rtt_max_ms = tmp[s_rtt_n - 1];
    }
}
