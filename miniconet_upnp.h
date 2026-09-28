/*
 * miniconet_upnp.h — UPnP IGD client (synchronous, no callbacks). Only
 * implemented when MINICONET_WITH_UPNP_CLIENT=ON; otherwise the functions still
 * link and return NULL/-1. Coroutine-only.
 *   mco_upnp_t* upnp = mco_upnp_new();
 *   if (mco_upnp_discover(upnp, 3000) == 0) { ... }
 *   mco_upnp_free(upnp);
 */
#ifndef MINICONET_UPNP_H
#define MINICONET_UPNP_H

#include "miniconet.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Discovered IGD state: control URL + service type for the SOAP calls. our_ip is
 * the local address (NewInternalClient). Opaque; use new/free. */
typedef struct mco_upnp {
    char our_ip[64];                       /* local IP, textual */
    char control_url[1024];                /* absolute control URL */
    char service_type[256];                /* WANIPConnection/WANPPPConnection */
    char desc_url[512];                    /* LOCATION of the description XML */
    void* _igddatas;                       /* struct IGDdatas*, internal */
} mco_upnp_t;

/* new returns NULL on failure; free(NULL) is safe. */
mco_upnp_t* mco_upnp_new(void);
void        mco_upnp_free(mco_upnp_t* upnp);

/* ---- SSDP discovery ----
 * Multicast M-SEARCH, then fetch and parse the description into control_url /
 * service_type / our_ip. timeout_ms covers the whole process. 0 on success. */
int mco_upnp_discover(mco_upnp_t* upnp, int timeout_ms);

/* ---- GetExternalIPAddress: writes the NUL-terminated IP to out_ip ---- */
int mco_upnp_get_external_ip(mco_upnp_t* upnp, char* out_ip, int out_ip_len, int timeout_ms);

/* ---- GetStatusInfo: any out param may be NULL to skip it ---- */
int mco_upnp_get_status_info(mco_upnp_t* upnp,
                             char* out_status, int out_status_len,
                             int* out_uptime,
                             char* out_last_conn_error, int out_lce_len,
                             int timeout_ms);

/* ---- GetGenericPortMappingEntry by index (0-based); out of range gives <0 ---- */
int mco_upnp_get_generic_port_mapping_entry(mco_upnp_t* upnp, int index,
                                             char* out_protocol, int out_proto_len,
                                             int* out_external_port,
                                             int* out_internal_port,
                                             char* out_internal_client, int out_ic_len,
                                             int* out_enabled,
                                             char* out_description, int out_desc_len,
                                             int* out_lease_time,
                                             int timeout_ms);

/* ---- AddPortMapping: protocol "TCP"/"UDP"; lease_seconds 0 = permanent ---- */
int mco_upnp_add_port_mapping(mco_upnp_t* upnp,
                              const char* protocol, int external_port, int internal_port,
                              const char* description, int lease_seconds, int timeout_ms);

/* ---- DeletePortMapping: protocol "TCP"/"UDP" ---- */
int mco_upnp_delete_port_mapping(mco_upnp_t* upnp,
                                 const char* protocol, int external_port, int timeout_ms);

#ifdef __cplusplus
}
#endif
#endif /* MINICONET_UPNP_H */
