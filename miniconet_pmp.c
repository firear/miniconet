/*
 * miniconet_pmp.c — NAT-PMP client (RFC 6886).
 * 协议编解码、结果码与重试节奏（250ms 起倍增，9 次封顶）对齐 libnatpmp
 * natpmp.c；网关探测用其 getdefaultgateway（compat/pmp/getgateway.c），失败时
 * 回落官方 openp2p potentialGateways() 的 "(私网IP & 掩码) | 1" 启发式。
 * 与 libnatpmp 的差异：阻塞 socket + 自旋重试改为 mco_udp_* 协程式收发，
 * 重试受调用方 timeout_ms 预算约束。
 * Only built with MINICONET_WITH_PMP=ON.
 */
#include "miniconet_pmp.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "hbase.h"          /* hv_strncpy */
#include "netadapters.h"    /* hv_get_interface_addresses（启发式候选网关） */
#include "pmp/getgateway.h" /* getdefaultgateway */

#if !MINICONET_WITH_PMP

/* ---- stub when the switch is off (keeps linking) ---- */
mco_pmp_t* mco_pmp_new(void) { return NULL; }
void mco_pmp_free(mco_pmp_t* pmp) { (void)pmp; }
int mco_pmp_discover(mco_pmp_t* pmp, int timeout_ms) {
    (void)pmp; (void)timeout_ms; return MCO_ERR_IO;
}
int mco_pmp_discover_gateway(mco_pmp_t* pmp, const char* gw, int timeout_ms) {
    (void)pmp; (void)gw; (void)timeout_ms; return MCO_ERR_IO;
}
int mco_pmp_get_external_ip(mco_pmp_t* pmp, char* out_ip, int out_ip_len, int timeout_ms) {
    (void)pmp; (void)out_ip; (void)out_ip_len; (void)timeout_ms; return MCO_ERR_IO;
}
int mco_pmp_add_port_mapping(mco_pmp_t* pmp, const char* p, int ep, int ip,
                             int lt, int* mp, int t) {
    (void)pmp; (void)p; (void)ep; (void)ip; (void)lt; (void)mp; (void)t; return MCO_ERR_IO;
}
int mco_pmp_delete_port_mapping(mco_pmp_t* pmp, const char* p, int ip, int t) {
    (void)pmp; (void)p; (void)ip; (void)t; return MCO_ERR_IO;
}

#else /* MINICONET_WITH_PMP */

/* ---- protocol constants (libnatpmp natpmp.h) ---- */
#define PMP_VERSION        0
#define PMP_OPCODE_PUBLIC  0   /* public address request */
#define PMP_OPCODE_UDP     1
#define PMP_OPCODE_TCP     2
#define PMP_RESULT_SUCCESS 0

/* retry schedule (libnatpmp natpmp.c): first wait 250ms, then double per
 * retry, give up after 9 tries (RFC 6886 recommended retransmission) */
#define PMP_RETRY_BASE_MS 250
#define PMP_MAX_TRIES     9

/* openp2p discoverPMP() gives each candidate gateway ~1s before moving on */
#define PMP_PROBE_BUDGET_MS 1000

/* NAT-PMP responses: 8-byte header + payload (4 = public address, 8 = mapping) */
#define PMP_PUBLIC_PAYLOAD   4
#define PMP_MAPPING_PAYLOAD  8

mco_pmp_t* mco_pmp_new(void) {
    return (mco_pmp_t*)calloc(1, sizeof(mco_pmp_t));
}

void mco_pmp_free(mco_pmp_t* pmp) {
    free(pmp);
}

/* ---- request builders (libnatpmp sendpublicaddressrequest /
 * sendnewportmappingrequest, byte-for-byte) ---- */
static void pmp_build_public_address_request(unsigned char* buf) {
    buf[0] = PMP_VERSION;
    buf[1] = PMP_OPCODE_PUBLIC;
}

static void pmp_build_mapping_request(unsigned char* buf, int opcode,
                                      int internal_port, int external_port,
                                      unsigned int lifetime) {
    buf[0]  = PMP_VERSION;
    buf[1]  = (unsigned char)opcode;
    buf[2]  = 0;
    buf[3]  = 0;
    buf[4]  = (unsigned char)(internal_port >> 8);
    buf[5]  = (unsigned char)(internal_port & 0xff);
    buf[6]  = (unsigned char)(external_port >> 8);
    buf[7]  = (unsigned char)(external_port & 0xff);
    buf[8]  = (unsigned char)(lifetime >> 24);
    buf[9]  = (unsigned char)(lifetime >> 16);
    buf[10] = (unsigned char)(lifetime >> 8);
    buf[11] = (unsigned char)(lifetime & 0xff);
}

/* ---- one request/response exchange against a gateway ----
 * Sends the request, then receives with the libnatpmp retry schedule bounded
 * by the caller's deadline. Only packets from the gateway itself, matching
 * version and opcode (req opcode | 0x80) are accepted; anything else is
 * dropped (NATPMP_ERR_WRONGPACKETSOURCE in libnatpmp). On success copies
 * out_payload (>= min_payload bytes guaranteed) and out_epoch.
 * 0 on success; <0 = MCO_ERR_* (server result codes collapse to MCO_ERR_IO). */
static int pmp_exchange(const char* gateway,
                        const unsigned char* req, int req_len,
                        unsigned char* out_payload, int payload_cap, int min_payload,
                        unsigned int* out_epoch, int timeout_ms) {
    mco_socket_t* sock;
    mco_deadline_t dl;
    unsigned char buf[32];
    int delay_ms = PMP_RETRY_BASE_MS;
    int try_number = 1;
    int rc = MCO_ERR_TIMEOUT;

    if (!gateway || gateway[0] == '\0') return MCO_ERR_ADDR;
    if (req_len < 2 || payload_cap < min_payload) return MCO_ERR_ADDR;

    sock = mco_udp_socket();
    if (!sock) return MCO_ERR_IO;
    dl = mco_deadline_after(timeout_ms);

    for (;;) {
        int ms = mco_deadline_ms(dl);
        int n;
        int wait;
        char from[64];
        int from_port = 0;

        if (ms <= 0) { rc = MCO_ERR_TIMEOUT; break; }

        n = mco_udp_sendto(sock, req, req_len, gateway, MCO_PMP_PORT, ms);
        if (n < 0) { rc = n; break; }

        /* wait for the response: this try's backoff, capped by the deadline */
        wait = mco_deadline_ms(dl);
        if (wait > delay_ms) wait = delay_ms;
        n = mco_udp_recvfrom(sock, buf, (int)sizeof(buf), from, (int)sizeof(from),
                             &from_port, wait);
        if (n == MCO_ERR_TIMEOUT) {
            /* libnatpmp: retry with doubled delay, 9 tries max */
            if (try_number >= PMP_MAX_TRIES) {
                rc = MCO_ERR_IO;  /* gateway does not support NAT-PMP */
                break;
            }
            try_number++;
            delay_ms *= 2;
            continue;
        }
        if (n < 0) { rc = n; break; }
        if (n == 0) continue;

        if (strcmp(from, gateway) != 0) continue;
        if (n < 8 || buf[0] != PMP_VERSION) {
            rc = MCO_ERR_IO;  /* unsupported version / malformed */
            break;
        }
        if (buf[1] != (unsigned char)(req[1] | 0x80)) continue;

        {
            unsigned int result = ((unsigned int)buf[2] << 8) | (unsigned int)buf[3];
            int payload_len = n - 8;
            if (result != PMP_RESULT_SUCCESS) {
                rc = MCO_ERR_IO;  /* server refused (auth/resources/...) */
                break;
            }
            if (payload_len < min_payload) {
                rc = MCO_ERR_IO;  /* truncated response */
                break;
            }
            if (payload_len > payload_cap) payload_len = payload_cap;
            if (out_payload)
                memcpy(out_payload, buf + 8, (size_t)payload_len);
            if (out_epoch)
                *out_epoch = ((unsigned int)buf[4] << 24) | ((unsigned int)buf[5] << 16) |
                             ((unsigned int)buf[6] << 8) | (unsigned int)buf[7];
        }
        rc = 0;
        break;
    }

    mco_close(sock);
    mco_socket_destroy(sock);
    return rc;
}

/* ---- public-address request; fills pmp->external_ip / epoch ---- */
static int pmp_get_external_address(mco_pmp_t* pmp, const char* gateway, int timeout_ms) {
    unsigned char req[2];
    unsigned char payload[PMP_PUBLIC_PAYLOAD];
    unsigned int epoch = 0;
    int rc;

    pmp_build_public_address_request(req);
    rc = pmp_exchange(gateway, req, (int)sizeof(req), payload, (int)sizeof(payload),
                      PMP_PUBLIC_PAYLOAD, &epoch, timeout_ms);
    if (rc != 0) return rc;
    pmp->epoch = epoch;
    snprintf(pmp->external_ip, sizeof(pmp->external_ip), "%u.%u.%u.%u",
             payload[0], payload[1], payload[2], payload[3]);
    return 0;
}

int mco_pmp_discover_gateway(mco_pmp_t* pmp, const char* gateway_ip, int timeout_ms) {
    struct in_addr a;
    int rc;
    if (!pmp || !gateway_ip || gateway_ip[0] == '\0') return MCO_ERR_ADDR;
    if (inet_pton(AF_INET, gateway_ip, &a) != 1) return MCO_ERR_ADDR;
    pmp->external_ip[0] = '\0';
    rc = pmp_get_external_address(pmp, gateway_ip, timeout_ms);
    if (rc != 0) return rc;
    hv_strncpy(pmp->gateway, gateway_ip, sizeof(pmp->gateway));
    return 0;
}

/* openp2p potentialGateways(): only private-range addresses are candidates */
static int pmp_is_private_ipv4(in_addr_t ip_net) {
    unsigned int h = ntohl(ip_net);
    unsigned int a = h >> 24;
    unsigned int b = (h >> 16) & 0xff;
    if (a == 10) return 1;
    if (a == 172 && b >= 16 && b <= 31) return 1;
    if (a == 192 && b == 168) return 1;
    return 0;
}

int mco_pmp_discover(mco_pmp_t* pmp, int timeout_ms) {
    mco_deadline_t dl;
    char gw_ip[64];
    char tried[64];
    in_addr_t gw = 0;

    if (!pmp) return MCO_ERR_ADDR;
    dl = mco_deadline_after(timeout_ms);
    gw_ip[0] = '\0';
    tried[0] = '\0';

    /* 1. route table (libnatpmp getdefaultgateway) */
    if (getdefaultgateway(&gw) == 0) {
        struct in_addr a;
        int ms = mco_deadline_ms(dl);
        a.s_addr = gw;
        if (ms > 0 && inet_ntop(AF_INET, &a, gw_ip, (socklen_t)sizeof(gw_ip))) {
            if (mco_pmp_discover_gateway(pmp, gw_ip, ms) == 0)
                return 0;
            hv_strncpy(tried, gw_ip, sizeof(tried));
        }
    }

    /* 2. openp2p potentialGateways() heuristic: for every private IPv4 the
     * candidate gateway is (ip & netmask) | 1 */
    {
        hv_interface_address_t* addrs = NULL;
        int count = 0;
        int i;
        int ok = 0;
        if (hv_get_interface_addresses(&addrs, &count) == 0) {
            for (i = 0; i < count; i++) {
                unsigned int ip, mask, gw_h;
                struct in_addr a;
                int ms = mco_deadline_ms(dl);
                int probe_ms;
                if (ms <= 0) break;
                if (addrs[i].is_internal) continue;
                if (addrs[i].address.sa.sa_family != AF_INET) continue;
                if (addrs[i].netmask.sa.sa_family != AF_INET) continue;
                if (!pmp_is_private_ipv4(addrs[i].address.sin.sin_addr.s_addr)) continue;
                ip = ntohl(addrs[i].address.sin.sin_addr.s_addr);
                mask = ntohl(addrs[i].netmask.sin.sin_addr.s_addr);
                gw_h = (ip & mask) | 1;
                if (gw_h == ip || gw_h == 0) continue;  /* p2p link / no gateway */
                a.s_addr = htonl(gw_h);
                if (!inet_ntop(AF_INET, &a, gw_ip, (socklen_t)sizeof(gw_ip))) continue;
                if (tried[0] != '\0' && strcmp(gw_ip, tried) == 0) continue;
                probe_ms = (ms > PMP_PROBE_BUDGET_MS) ? PMP_PROBE_BUDGET_MS : ms;
                if (mco_pmp_discover_gateway(pmp, gw_ip, probe_ms) == 0) {
                    ok = 1;
                    break;
                }
            }
            hv_free_interface_addresses(addrs, count);
        }
        if (ok) return 0;
    }
    return MCO_ERR_IO;
}

int mco_pmp_get_external_ip(mco_pmp_t* pmp, char* out_ip, int out_ip_len, int timeout_ms) {
    int rc;
    if (!pmp || !out_ip || out_ip_len <= 0) return MCO_ERR_ADDR;
    rc = pmp_get_external_address(pmp, pmp->gateway, timeout_ms);
    if (rc == 0)
        hv_strncpy(out_ip, pmp->external_ip, (size_t)out_ip_len);
    return rc;
}

int mco_pmp_add_port_mapping(mco_pmp_t* pmp, const char* protocol,
                             int external_port, int internal_port,
                             int lifetime_seconds,
                             int* out_mapped_port, int timeout_ms) {
    unsigned char req[12];
    unsigned char payload[PMP_MAPPING_PAYLOAD];
    unsigned int epoch = 0;
    int opcode;
    int rc;

    if (!pmp || !protocol) return MCO_ERR_ADDR;
    /* "UDP"/"TCP"/"udp"/"tcp" — openp2p passes both cases */
    if (strcasecmp(protocol, "udp") == 0) opcode = PMP_OPCODE_UDP;
    else if (strcasecmp(protocol, "tcp") == 0) opcode = PMP_OPCODE_TCP;
    else return MCO_ERR_ADDR;

    if (external_port < 0 || external_port > 65535 ||
        internal_port <= 0 || internal_port > 65535 ||
        lifetime_seconds < 0)
        return MCO_ERR_ADDR;

    pmp_build_mapping_request(req, opcode, internal_port, external_port,
                              (unsigned int)lifetime_seconds);
    rc = pmp_exchange(pmp->gateway, req, (int)sizeof(req), payload, (int)sizeof(payload),
                      PMP_MAPPING_PAYLOAD, &epoch, timeout_ms);
    if (rc != 0) return rc;
    pmp->epoch = epoch;
    /* mapping response payload: [0..1] private port, [2..3] mapped public
     * port, [4..7] lifetime. NAT-PMP may assign a different public port than
     * requested — openp2p reads it back (res.MappedExternalPort). */
    if (out_mapped_port)
        *out_mapped_port = ((int)payload[2] << 8) | (int)payload[3];
    return 0;
}

int mco_pmp_delete_port_mapping(mco_pmp_t* pmp, const char* protocol,
                                int internal_port, int timeout_ms) {
    /* libnatpmp/jackpal: an add request with external port 0 and lifetime 0
     * removes the mapping */
    return mco_pmp_add_port_mapping(pmp, protocol, 0, internal_port, 0, NULL, timeout_ms);
}

#endif /* MINICONET_WITH_PMP */
