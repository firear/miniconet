/*
 * miniconet_upnp.c — UPnP IGD client.
 *   1) SSDP discovery: UDP M-SEARCH multicast, parse LOCATION.
 *   2) Device description: mco_http_get_ex() + igd_desc_parse.
 *   3) SOAP control: mco_http_request() POST + upnpreplyparse.
 * Only built with MINICONET_WITH_UPNP_CLIENT=ON.
 */
#include "miniconet_upnp.h"
#include "miniconet_http.h"

#include "igd_desc_parse.h"
#include "upnpreplyparse.h"
#include "minixml.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "hbase.h"      /* hv_parse_url */
#include "hsocket.h"    /* sockaddr_ip / sockaddr_u */

#if !MINICONET_WITH_UPNP_CLIENT

/* ---- stub when the switch is off (keeps linking) ---- */

mco_upnp_t* mco_upnp_new(void) { return NULL; }
void mco_upnp_free(mco_upnp_t* upnp) { (void)upnp; }
int mco_upnp_discover(mco_upnp_t* upnp, int timeout_ms) { (void)upnp; (void)timeout_ms; return MCO_ERR_IO; }
int mco_upnp_get_external_ip(mco_upnp_t* upnp, char* out_ip, int out_ip_len, int timeout_ms) {
    (void)upnp; (void)out_ip; (void)out_ip_len; (void)timeout_ms; return MCO_ERR_IO;
}
int mco_upnp_get_status_info(mco_upnp_t* upnp, char* a, int b, int* c, char* d, int e, int t) {
    (void)upnp; (void)a; (void)b; (void)c; (void)d; (void)e; (void)t; return MCO_ERR_IO;
}
int mco_upnp_get_generic_port_mapping_entry(mco_upnp_t* upnp, int index,
                                             char* a, int b, int* c, int* d, char* e, int f,
                                             int* g, char* h, int i, int* j, int t) {
    (void)upnp; (void)index; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    (void)g; (void)h; (void)i; (void)j; (void)t; return MCO_ERR_IO;
}
int mco_upnp_add_port_mapping(mco_upnp_t* upnp, const char* p, int ep, int ip,
                              const char* d, int l, int t) {
    (void)upnp; (void)p; (void)ep; (void)ip; (void)d; (void)l; (void)t; return MCO_ERR_IO;
}
int mco_upnp_delete_port_mapping(mco_upnp_t* upnp, const char* p, int ep, int t) {
    (void)upnp; (void)p; (void)ep; (void)t; return MCO_ERR_IO;
}

#else /* MINICONET_WITH_UPNP_CLIENT */

#define UPNP_SSDP_ADDR "239.255.255.250"
#define UPNP_SSDP_PORT 1900
#define UPNP_IGD_ST    "urn:schemas-upnp-org:device:InternetGatewayDevice:1"

/* ---- Parse the SSDP response: extract LOCATION ----
 * Match the IGD ST first, then find "\r\nlocation:" case-insensitively.
 * 0 if found, -1 otherwise. Exposed for tests. */
int mco_upnp__parse_ssdp_response(const char* buf, int len,
                                   char* out_location, int out_location_len) {
    const char* needle = "\r\nlocation:";
    int needle_len = (int)strlen(needle);
    int i;
    /* must match the IGD ST */
    {
        const char* p = buf;
        int remaining = len;
        const char* found = NULL;
        /* find UPNP_IGD_ST */
        while (remaining > 0) {
            const char* m = strstr(p, UPNP_IGD_ST);
            if (m) { found = m; break; }
            /* strstr reached the end or NULL */
            break;
        }
        if (!found) return -1;
    }
    /* find "\r\nlocation:" case-insensitively */
    for (i = 0; i + needle_len <= len; i++) {
        int j;
        int match = 1;
        for (j = 0; j < needle_len; j++) {
            if (tolower((unsigned char)buf[i + j]) != needle[j]) { match = 0; break; }
        }
        if (!match) continue;
        {
            const char* val = buf + i + needle_len;
            int val_len;
            const char* eol;
            /* skip leading whitespace */
            while (val < buf + len && isspace((unsigned char)*val)) val++;
            /* find the end of line */
            eol = strstr(val, "\r\n");
            val_len = eol ? (int)(eol - val) : (int)(buf + len - val);
            /* trim trailing whitespace */
            while (val_len > 0 && isspace((unsigned char)val[val_len - 1])) val_len--;
            if (val_len <= 0 || val_len >= out_location_len) return -1;
            memcpy(out_location, val, (size_t)val_len);
            out_location[val_len] = 0;
            return 0;
        }
    }
    return -1;
}

mco_upnp_t* mco_upnp_new(void) {
    mco_upnp_t* upnp = (mco_upnp_t*)calloc(1, sizeof(mco_upnp_t));
    return upnp;
}

void mco_upnp_free(mco_upnp_t* upnp) {
    if (!upnp) return;
    free(upnp->_igddatas);
    free(upnp);
}

/* ---- Build the absolute control URL ----
 * A relative controlurl is joined with desc_url's scheme://host[:port]. */
static void build_absolute_control_url(const char* desc_url, const char* controlurl,
                                       char* out, int out_len) {
    /* already absolute: use as-is */
    if (strstr(controlurl, "://") != NULL) {
        strncpy(out, controlurl, out_len - 1);
        out[out_len - 1] = 0;
        return;
    }
    /* take scheme://host[:port] from desc_url */
    {
        const char* scheme_end = strstr(desc_url, "://");
        const char* host_start;
        const char* path_start;
        int prefix_len;
        if (!scheme_end) {
            /* malformed desc_url: fall back to controlurl */
            strncpy(out, controlurl, out_len - 1);
            out[out_len - 1] = 0;
            return;
        }
        host_start = scheme_end + 3;
        /* path starts at the first '/' after the host */
        path_start = strchr(host_start, '/');
        prefix_len = path_start ? (int)(path_start - desc_url) : (int)strlen(desc_url);
        /* out = prefix + controlurl, adding '/' if needed */
        {
            int need_slash = (controlurl[0] != '/') ? 1 : 0;
            int total = prefix_len + need_slash + (int)strlen(controlurl);
            if (total >= out_len) total = out_len - 1;
            memcpy(out, desc_url, (size_t)prefix_len);
            if (need_slash) out[prefix_len++] = '/';
            strncpy(out + prefix_len, controlurl, (size_t)(out_len - 1 - prefix_len));
            out[out_len - 1] = 0;
        }
    }
}

/* ---- Local IP: connect then getsockname. -1 on failure. ---- */
static int fetch_local_ip(const char* desc_url, char* out_ip, int out_ip_len, int timeout_ms) {
    hurl_t u;
    char host[256];
    mco_socket_t* sock;
    int fd;
    struct sockaddr_storage local;
    socklen_t local_len = sizeof(local);
    if (hv_parse_url(&u, desc_url) != 0) return -1;
    {
        int off = u.fields[HV_URL_HOST].off;
        int len = u.fields[HV_URL_HOST].len;
        if (len <= 0 || len >= (int)sizeof(host)) return -1;
        memcpy(host, desc_url + off, (size_t)len);
        host[len] = 0;
    }
    sock = mco_tcp_connect(host, u.port ? u.port : 80, timeout_ms, NULL);
    if (!sock) return -1;
    fd = mco_socket_fd(sock);
    if (getsockname(fd, (struct sockaddr*)&local, &local_len) != 0) {
        mco_close(sock); mco_socket_destroy(sock); return -1;
    }
    sockaddr_ip((sockaddr_u*)&local, out_ip, out_ip_len);
    mco_close(sock);
    mco_socket_destroy(sock);
    return 0;
}

/* ---- Fetch the description XML and parse it; fills control_url/service_type. ---- */
static int fetch_and_parse_description(mco_upnp_t* upnp, int timeout_ms) {
    mco_http_response_t* resp;
    struct IGDdatas* igd;
    struct xmlparser parser;
    if (!upnp->_igddatas) {
        upnp->_igddatas = calloc(1, sizeof(struct IGDdatas));
        if (!upnp->_igddatas) return MCO_ERR_IO;
    }
    igd = (struct IGDdatas*)upnp->_igddatas;
    memset(igd, 0, sizeof(*igd));

    resp = mco_http_get_ex(upnp->desc_url, NULL, timeout_ms);
    if (!resp || resp->body.size <= 0) {
        if (resp) mco_http_response_free(resp);
        return MCO_ERR_IO;
    }
    memset(&parser, 0, sizeof(parser));
    parser.xmlstart = chars_t_cstr(&resp->body);
    parser.xmlsize = (int)resp->body.size;
    parser.data = igd;
    parser.starteltfunc = IGDstartelt;
    parser.endeltfunc = IGDendelt;
    parser.datafunc = IGDdata;
    parsexml(&parser);
    mco_http_response_free(resp);

    /* the WANIPConnection/WANPPPConnection service */
    if (igd->first.servicetype[0] == '\0') {
        return MCO_ERR_IO;  /* no WAN service in the description */
    }
    strncpy(upnp->service_type, igd->first.servicetype, sizeof(upnp->service_type) - 1);
    upnp->service_type[sizeof(upnp->service_type) - 1] = 0;
    build_absolute_control_url(upnp->desc_url, igd->first.controlurl,
                               upnp->control_url, (int)sizeof(upnp->control_url));
    return 0;
}

/* internal entry for tests: set desc_url first */
int mco_upnp__fetch_description(mco_upnp_t* upnp, const char* desc_url, int timeout_ms) {
    if (!upnp || !desc_url) return MCO_ERR_IO;
    strncpy(upnp->desc_url, desc_url, sizeof(upnp->desc_url) - 1);
    upnp->desc_url[sizeof(upnp->desc_url) - 1] = 0;
    return fetch_and_parse_description(upnp, timeout_ms);
}

/* ---- SOAP request: POST the envelope to control_url, parse the reply XML.
 * 0 on success with *out_pdata (caller clears it); <0 on failure. ---- */
static int upnp_soap_request(mco_upnp_t* upnp, const char* action,
                             const char* params, /* may be NULL/"" */
                             struct NameValueParserData* out_pdata,
                             int timeout_ms) {
    static const char envelope_tmpl[] =
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body>"
        "<u:%s xmlns:u=\"%s\">%s</u:%s>"
        "</s:Body></s:Envelope>";
    char body[2048];
    int body_len;
    char soap_action[256];
    mco_http_header_t extra;
    mco_http_options_t opt;
    mco_http_response_t* resp;
    int rc;

    if (!upnp || !action || !out_pdata) return MCO_ERR_IO;
    if (upnp->control_url[0] == 0 || upnp->service_type[0] == 0) return MCO_ERR_IO;

    body_len = snprintf(body, sizeof(body), envelope_tmpl,
                        action, upnp->service_type,
                        params ? params : "", action);
    if (body_len < 0 || body_len >= (int)sizeof(body)) return MCO_ERR_IO;

    snprintf(soap_action, sizeof(soap_action), "\"%s#%s\"",
             upnp->service_type, action);
    extra.name = "SOAPAction";
    extra.value = soap_action;

    memset(&opt, 0, sizeof(opt));
    opt.method = "POST";
    opt.url = upnp->control_url;
    opt.body = body;
    opt.body_len = body_len;
    opt.content_type = "text/xml; charset=\"utf-8\"";
    opt.timeout_ms = timeout_ms;
    opt.extra_headers = &extra;
    opt.num_extra_headers = 1;

    resp = mco_http_request(&opt);
    if (!resp || resp->body.size <= 0) {
        if (resp) mco_http_response_free(resp);
        return MCO_ERR_IO;
    }
    ParseNameValue(chars_t_cstr(&resp->body), (int)resp->body.size, out_pdata);
    rc = 0;
    mco_http_response_free(resp);
    return rc;
}

/* ---- public: GetExternalIPAddress ---- */
int mco_upnp_get_external_ip(mco_upnp_t* upnp, char* out_ip, int out_ip_len, int timeout_ms) {
    struct NameValueParserData pdata;
    int rc;
    if (!upnp || !out_ip || out_ip_len <= 0) return MCO_ERR_IO;
    rc = upnp_soap_request(upnp, "GetExternalIPAddress", "", &pdata, timeout_ms);
    if (rc != 0) return rc;
    {
        char* v = GetValueFromNameValueList(&pdata, "NewExternalIPAddress");
        if (v) {
            strncpy(out_ip, v, out_ip_len - 1);
            out_ip[out_ip_len - 1] = 0;
            rc = 0;
        } else {
            rc = MCO_ERR_IO;
        }
    }
    ClearNameValueList(&pdata);
    return rc;
}

/* ---- public: GetStatusInfo ---- */
int mco_upnp_get_status_info(mco_upnp_t* upnp,
                             char* out_status, int out_status_len,
                             int* out_uptime,
                             char* out_last_conn_error, int out_lce_len,
                             int timeout_ms) {
    struct NameValueParserData pdata;
    int rc;
    if (!upnp) return MCO_ERR_IO;
    rc = upnp_soap_request(upnp, "GetStatusInfo", "", &pdata, timeout_ms);
    if (rc != 0) return rc;
    if (out_status && out_status_len > 0) {
        char* v = GetValueFromNameValueList(&pdata, "NewConnectionStatus");
        if (v) { strncpy(out_status, v, out_status_len - 1); out_status[out_status_len - 1] = 0; }
        else out_status[0] = 0;
    }
    if (out_uptime) {
        char* v = GetValueFromNameValueList(&pdata, "NewUptime");
        *out_uptime = v ? atoi(v) : 0;
    }
    if (out_last_conn_error && out_lce_len > 0) {
        char* v = GetValueFromNameValueList(&pdata, "NewLastConnectionError");
        if (v) { strncpy(out_last_conn_error, v, out_lce_len - 1); out_last_conn_error[out_lce_len - 1] = 0; }
        else out_last_conn_error[0] = 0;
    }
    ClearNameValueList(&pdata);
    return 0;
}

/* ---- public: GetGenericPortMappingEntry ---- */
int mco_upnp_get_generic_port_mapping_entry(mco_upnp_t* upnp, int index,
                                             char* out_protocol, int out_proto_len,
                                             int* out_external_port,
                                             int* out_internal_port,
                                             char* out_internal_client, int out_ic_len,
                                             int* out_enabled,
                                             char* out_description, int out_desc_len,
                                             int* out_lease_time,
                                             int timeout_ms) {
    char params[128];
    struct NameValueParserData pdata;
    int rc;
    if (!upnp) return MCO_ERR_IO;
    snprintf(params, sizeof(params), "<NewPortMappingIndex>%d</NewPortMappingIndex>", index);
    rc = upnp_soap_request(upnp, "GetGenericPortMappingEntry", params, &pdata, timeout_ms);
    if (rc != 0) return rc;
    if (out_protocol && out_proto_len > 0) {
        char* v = GetValueFromNameValueList(&pdata, "NewProtocol");
        if (v) { strncpy(out_protocol, v, out_proto_len - 1); out_protocol[out_proto_len - 1] = 0; }
        else out_protocol[0] = 0;
    }
    if (out_external_port) {
        char* v = GetValueFromNameValueList(&pdata, "NewExternalPort");
        *out_external_port = v ? atoi(v) : 0;
    }
    if (out_internal_port) {
        char* v = GetValueFromNameValueList(&pdata, "NewInternalPort");
        *out_internal_port = v ? atoi(v) : 0;
    }
    if (out_internal_client && out_ic_len > 0) {
        char* v = GetValueFromNameValueList(&pdata, "NewInternalClient");
        if (v) { strncpy(out_internal_client, v, out_ic_len - 1); out_internal_client[out_ic_len - 1] = 0; }
        else out_internal_client[0] = 0;
    }
    if (out_enabled) {
        char* v = GetValueFromNameValueList(&pdata, "NewEnabled");
        *out_enabled = v ? atoi(v) : 0;
    }
    if (out_description && out_desc_len > 0) {
        char* v = GetValueFromNameValueList(&pdata, "NewPortMappingDescription");
        if (v) { strncpy(out_description, v, out_desc_len - 1); out_description[out_desc_len - 1] = 0; }
        else out_description[0] = 0;
    }
    if (out_lease_time) {
        char* v = GetValueFromNameValueList(&pdata, "NewLeaseDuration");
        *out_lease_time = v ? atoi(v) : 0;
    }
    ClearNameValueList(&pdata);
    return 0;
}

/* ---- public: AddPortMapping ---- */
int mco_upnp_add_port_mapping(mco_upnp_t* upnp,
                              const char* protocol, int external_port, int internal_port,
                              const char* description, int lease_seconds, int timeout_ms) {
    char params[512];
    struct NameValueParserData pdata;
    int rc;
    if (!upnp || !protocol) return MCO_ERR_IO;
    snprintf(params, sizeof(params),
             "<NewRemoteHost></NewRemoteHost>"
             "<NewExternalPort>%d</NewExternalPort>"
             "<NewProtocol>%s</NewProtocol>"
             "<NewInternalPort>%d</NewInternalPort>"
             "<NewInternalClient>%s</NewInternalClient>"
             "<NewEnabled>1</NewEnabled>"
             "<NewPortMappingDescription>%s</NewPortMappingDescription>"
             "<NewLeaseDuration>%d</NewLeaseDuration>",
             external_port, protocol, internal_port,
             upnp->our_ip, description ? description : "", lease_seconds);
    rc = upnp_soap_request(upnp, "AddPortMapping", params, &pdata, timeout_ms);
    if (rc != 0) return rc;
    ClearNameValueList(&pdata);
    return 0;
}

/* ---- public: DeletePortMapping ---- */
int mco_upnp_delete_port_mapping(mco_upnp_t* upnp,
                                 const char* protocol, int external_port, int timeout_ms) {
    char params[256];
    struct NameValueParserData pdata;
    int rc;
    if (!upnp || !protocol) return MCO_ERR_IO;
    snprintf(params, sizeof(params),
             "<NewRemoteHost></NewRemoteHost>"
             "<NewExternalPort>%d</NewExternalPort>"
             "<NewProtocol>%s</NewProtocol>",
             external_port, protocol);
    rc = upnp_soap_request(upnp, "DeletePortMapping", params, &pdata, timeout_ms);
    if (rc != 0) return rc;
    ClearNameValueList(&pdata);
    return 0;
}

/* ---- mco_upnp_discover：SSDP M-SEARCH ----
 * Loop until timeout or an IGD match, store LOCATION in desc_url, then fetch and
 * parse the description (reusing the remaining budget). 0 on success. */
int mco_upnp_discover(mco_upnp_t* upnp, int timeout_ms) {
    static const char msg[] =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "ST: ssdp:all\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 2\r\n\r\n";
    mco_socket_t* sock;
    mco_deadline_t dl;
    int n;
    char buf[2048];

    if (!upnp) return MCO_ERR_IO;
    dl = mco_deadline_after(timeout_ms);

    sock = mco_udp_socket();
    if (!sock) return MCO_ERR_IO;

    /* send M-SEARCH to the SSDP multicast group */
    n = mco_udp_sendto(sock, msg, (int)sizeof(msg) - 1,
                       UPNP_SSDP_ADDR, UPNP_SSDP_PORT, mco_deadline_ms(dl));
    if (n < 0) { mco_close(sock); mco_socket_destroy(sock); return n; }

    /* loop until an IGD match or timeout */
    for (;;) {
        int ms = mco_deadline_ms(dl);
        if (ms <= 0) { mco_close(sock); mco_socket_destroy(sock); return MCO_ERR_TIMEOUT; }
        n = mco_udp_recvfrom(sock, buf, (int)sizeof(buf) - 1, NULL, 0, NULL, ms);
        if (n == MCO_ERR_TIMEOUT) { mco_close(sock); mco_socket_destroy(sock); return MCO_ERR_TIMEOUT; }
        if (n < 0) { mco_close(sock); mco_socket_destroy(sock); return n; }
        if (n == 0) continue;
        buf[n] = 0;
        {
            char loc[512];
            if (mco_upnp__parse_ssdp_response(buf, n, loc, (int)sizeof(loc)) == 0) {
                int rc;
                strncpy(upnp->desc_url, loc, sizeof(upnp->desc_url) - 1);
                upnp->desc_url[sizeof(upnp->desc_url) - 1] = 0;
                mco_close(sock);
                mco_socket_destroy(sock);
                /* discovery done: fetch and parse the description */
                rc = fetch_and_parse_description(upnp, mco_deadline_ms(dl));
                if (rc != 0) return rc;
                fetch_local_ip(upnp->desc_url, upnp->our_ip,
                               (int)sizeof(upnp->our_ip), mco_deadline_ms(dl));
                /* our_ip is not fatal: some routers fill it from the source */
                return 0;
            }
        }
        /* other UPnP device: keep waiting */
    }
}

#endif /* MINICONET_WITH_UPNP_CLIENT */
