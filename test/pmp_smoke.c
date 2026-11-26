/*
 * pmp_smoke.c — NAT-PMP client smoke test against a local mock gateway.
 * The mock binds 127.0.0.1:5351 (the real NAT-PMP port) and answers:
 *   public address  -> 203.0.113.42
 *   port mapping    -> echoes private/lifetime; mapped port = requested,
 *                      except requested 9999 -> 40001 (NAT-PMP remaps when
 *                      the port is taken; openp2p reads MappedExternalPort)
 * Stages:
 *   1. new/free
 *   2. discover_gateway(127.0.0.1) -> external ip
 *   3. get_external_ip
 *   4. add udp mapping (echo path)
 *   5. add tcp mapping with a taken port (alternate-port path)
 *   6. delete mapping (add with ext=0, lifetime=0)
 *   7. discover against a blackhole gateway -> timeout <0
 */
#include "../miniconet.h"
#include "../miniconet_pmp.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MOCK_EPOCH     123456u
#define N_REQUESTS     5       /* discover + extip + add-udp + add-tcp + delete */

static int g_newfree_ok = 0;
static int g_discover_ok = 0;
static int g_extip_ok = 0;
static int g_add_udp_ok = 0;
static int g_add_tcp_alt_ok = 0;
static int g_delete_ok = 0;
static int g_blackhole_ok = 0;
static int g_server_done = 0;

static void put16(unsigned char* p, unsigned int v) {
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)(v & 0xff);
}

static unsigned int get16(const unsigned char* p) {
    return ((unsigned int)p[0] << 8) | p[1];
}

/* mock gateway: answer N requests, then stop */
static void mock_gateway_task(void* arg) {
    mco_socket_t* sock = (mco_socket_t*)arg;
    int served = 0;
    while (served < N_REQUESTS) {
        unsigned char buf[32];
        char from[64];
        int from_port = 0;
        int n = mco_udp_recvfrom(sock, buf, (int)sizeof(buf),
                                 from, (int)sizeof(from), &from_port, 10000);
        if (n < 0) break;                        /* socket error */
        if (n < 2 || buf[0] != 0) continue;  /* malformed: ignore */
        {
            unsigned char rsp[16];
            int rsp_len;
            rsp[0] = 0;
            rsp[1] = (unsigned char)(buf[1] | 0x80);
            rsp[2] = 0; rsp[3] = 0;  /* result code: success */
            rsp[4] = (unsigned char)(MOCK_EPOCH >> 24);
            rsp[5] = (unsigned char)(MOCK_EPOCH >> 16);
            rsp[6] = (unsigned char)(MOCK_EPOCH >> 8);
            rsp[7] = (unsigned char)(MOCK_EPOCH & 0xff);
            if (buf[1] == 0) {  /* public address */
                rsp[8] = 203; rsp[9] = 0; rsp[10] = 113; rsp[11] = 42;
                rsp_len = 12;
            } else {            /* port mapping */
                unsigned int private_port = get16(buf + 4);
                unsigned int requested = get16(buf + 6);
                unsigned int mapped = (requested == 9999) ? 40001 : requested;
                put16(rsp + 8, private_port);
                put16(rsp + 10, mapped);
                rsp[12] = buf[8]; rsp[13] = buf[9];    /* lifetime: echo */
                rsp[14] = buf[10]; rsp[15] = buf[11];
                rsp_len = 16;
            }
            mco_udp_sendto(sock, rsp, rsp_len, from, from_port, 3000);
        }
        served++;
    }
    mco_close(sock);
    mco_socket_destroy(sock);
    g_server_done = 1;
}

static void client_task(void* arg) {
    mco_pmp_t* pmp;
    int rc;
    int mapped = 0;
    (void)arg;
    mco_sleep(50);

    /* 1) new/free */
    pmp = mco_pmp_new();
    g_newfree_ok = (pmp != NULL);
    mco_pmp_free(pmp);

    /* 2) discover a specific gateway (mock) */
    pmp = mco_pmp_new();
    rc = mco_pmp_discover_gateway(pmp, "127.0.0.1", 3000);
    if (rc == 0 && strcmp(pmp->external_ip, "203.0.113.42") == 0 &&
        pmp->epoch == MOCK_EPOCH) {
        g_discover_ok = 1;
        printf("[discover] gateway=%s external_ip=%s epoch=%u\n",
               pmp->gateway, pmp->external_ip, pmp->epoch);
    } else {
        printf("[discover] FAILED rc=%d extip='%s'\n", rc, pmp->external_ip);
    }

    /* 3) get_external_ip again */
    {
        char ext[64];
        rc = mco_pmp_get_external_ip(pmp, ext, (int)sizeof(ext), 3000);
        if (rc == 0 && strcmp(ext, "203.0.113.42") == 0) g_extip_ok = 1;
        else printf("[extip] FAILED rc=%d ext='%s'\n", rc, ext);
    }

    /* 4) add udp mapping: requested == mapped (echo path) */
    rc = mco_pmp_add_port_mapping(pmp, "udp", 12345, 12345, 7200, &mapped, 3000);
    if (rc == 0 && mapped == 12345) {
        g_add_udp_ok = 1;
        printf("[add-udp] mapped %d -> %d\n", 12345, mapped);
    } else {
        printf("[add-udp] FAILED rc=%d mapped=%d\n", rc, mapped);
    }

    /* 5) add tcp mapping on a taken port: gateway remaps 9999 -> 40001;
     * openp2p relies on reading back the actual port */
    mapped = 0;
    rc = mco_pmp_add_port_mapping(pmp, "TCP", 9999, 9999, 7200, &mapped, 3000);
    if (rc == 0 && mapped == 40001) {
        g_add_tcp_alt_ok = 1;
        printf("[add-tcp] requested 9999, gateway mapped %d\n", mapped);
    } else {
        printf("[add-tcp] FAILED rc=%d mapped=%d\n", rc, mapped);
    }

    /* 6) delete: add with ext=0 lifetime=0 must succeed */
    rc = mco_pmp_delete_port_mapping(pmp, "udp", 12345, 3000);
    if (rc == 0) g_delete_ok = 1;
    else printf("[delete] FAILED rc=%d\n", rc);

    mco_pmp_free(pmp);

    /* 7) blackhole gateway (TEST-NET-1, never answers) -> timeout */
    pmp = mco_pmp_new();
    rc = mco_pmp_discover_gateway(pmp, "192.0.2.1", 700);
    if (rc < 0) {
        g_blackhole_ok = 1;
        printf("[blackhole] rc=%d (expected <0)\n", rc);
    } else {
        printf("[blackhole] unexpected success rc=%d\n", rc);
    }
    mco_pmp_free(pmp);
}

int main(void) {
    mco_loop_t* loop;
    mco_socket_t* mock;
    int ok;

    mock = mco_udp_bind("127.0.0.1", MCO_PMP_PORT);
    if (!mock) { printf("mock gateway bind 127.0.0.1:5351 failed\n"); return 1; }

    loop = mco_loop_new();
    mco_spawn(loop, mock_gateway_task, mock);
    mco_spawn(loop, client_task, NULL);
    mco_loop_run(loop);
    mco_loop_free(loop);

    printf("newfree=%d discover=%d extip=%d add_udp=%d add_tcp_alt=%d delete=%d "
           "blackhole=%d server_done=%d\n",
           g_newfree_ok, g_discover_ok, g_extip_ok, g_add_udp_ok,
           g_add_tcp_alt_ok, g_delete_ok, g_blackhole_ok, g_server_done);

    ok = g_newfree_ok && g_discover_ok && g_extip_ok && g_add_udp_ok &&
         g_add_tcp_alt_ok && g_delete_ok && g_blackhole_ok && g_server_done;
    printf(ok ? "PMP TEST PASSED\n" : "PMP TEST FAILED\n");
    return ok ? 0 : 1;
}
