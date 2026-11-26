/*
 * miniconet_pmp.h — NAT-PMP client (RFC 6886, coroutine-synchronous style).
 * Protocol/retry schedule aligned with libnatpmp (compat/pmp/getgateway is
 * vendored from it); usage mirrors official openp2p's setUPNP() via
 * jackpal/go-nat-pmp. Only implemented when MINICONET_WITH_PMP=ON; otherwise
 * the functions still link and return <0. Coroutine-only.
 *
 *   mco_pmp_t* pmp = mco_pmp_new();
 *   if (mco_pmp_discover(pmp, 2000) == 0) {
 *       int mapped = 0;
 *       mco_pmp_add_port_mapping(pmp, "udp", port, port, 604800, &mapped, 3000);
 *   }
 *   mco_pmp_free(pmp);
 */
#ifndef MINICONET_PMP_H
#define MINICONET_PMP_H

#include "miniconet.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NAT-PMP server port on the gateway (fixed by the RFC). */
#define MCO_PMP_PORT 5351

/* Discovered gateway state. gateway is textual IPv4; external_ip is filled by
 * discover()/get_external_ip(); epoch is the gateway's seconds-since-boot
 * counter from the last response (RFC 6886 mapping-maintenance hint). */
typedef struct mco_pmp {
    char gateway[64];
    char external_ip[64];
    unsigned int epoch;
} mco_pmp_t;

/* new returns NULL on failure; free(NULL) is safe. */
mco_pmp_t* mco_pmp_new(void);
void       mco_pmp_free(mco_pmp_t* pmp);

/* ---- Discovery ----
 * Find the default gateway (route table via getdefaultgateway; fallback:
 * openp2p's "(private ip & netmask) | 1" heuristic over the interface list),
 * then probe it with a public-address request. On success fills gateway and
 * external_ip. timeout_ms covers the whole process. 0 on success. */
int mco_pmp_discover(mco_pmp_t* pmp, int timeout_ms);

/* Probe one specific gateway (openp2p's "pmp:192.168.0.1" spec; also what the
 * tests use against a mock gateway). Same semantics as discover(). */
int mco_pmp_discover_gateway(mco_pmp_t* pmp, const char* gateway_ip, int timeout_ms);

/* ---- GetExternalAddress: writes the NUL-terminated IP to out_ip ---- */
int mco_pmp_get_external_ip(mco_pmp_t* pmp, char* out_ip, int out_ip_len, int timeout_ms);

/* ---- AddPortMapping ----
 * protocol "UDP"/"TCP" (case-insensitive, as openp2p passes both). Requests
 * external_port for internal_port with the given lifetime in seconds.
 * out_mapped_port (may be NULL) receives the ACTUAL mapped external port:
 * NAT-PMP silently assigns a different one when the requested port is taken,
 * and openp2p relies on reading it back. 0 on success. */
int mco_pmp_add_port_mapping(mco_pmp_t* pmp, const char* protocol,
                             int external_port, int internal_port,
                             int lifetime_seconds,
                             int* out_mapped_port, int timeout_ms);

/* ---- DeletePortMapping ----
 * libnatpmp/jackpal semantics: an add request with external port 0 and
 * lifetime 0 removes the mapping for internal_port. */
int mco_pmp_delete_port_mapping(mco_pmp_t* pmp, const char* protocol,
                                int internal_port, int timeout_ms);

#ifdef __cplusplus
}
#endif
#endif /* MINICONET_PMP_H */
