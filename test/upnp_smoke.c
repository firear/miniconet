/*
 * upnp_smoke.c — UPnP IGD client smoke test, staged like PLAN Step 3/4/5:
 *   Step 3: SSDP response parsing + new/free + discover timeout
 *   Step 4: device description XML fetch + IGD parse (local TCP fake gateway)
 *   Step 5: SOAP request + response parse (same fake gateway gains a SOAP route)
 * No real UDP multicast (CI containers block it, port 1900 is unreliable): SSDP
 * parsing is fed canned text; description/SOAP go over local TCP loopback.
 */
#include "../miniconet.h"
#include "../miniconet_upnp.h"
#include <stdio.h>
#include <string.h>

#define TEST_PORT 17381

/* internal parse helpers (not public API; fed directly to check parsing) */
extern int mco_upnp__parse_ssdp_response(const char* buf, int len,
                                          char* out_location, int out_location_len);
extern int mco_upnp__fetch_description(mco_upnp_t* upnp, const char* desc_url, int timeout_ms);

static int g_parse_ok = 0;
static int g_discover_timeout_ok = 0;
static int g_newfree_ok = 0;
static int g_desc_ok = 0;
static int g_soap_ok = 0;
static int g_server_done = 0;

/* canned SSDP response (LOCATION points at the local fake gateway) */
static const char SSDP_RESP[] =
    "HTTP/1.1 200 OK\r\n"
    "CACHE-CONTROL: max-age=1800\r\n"
    "LOCATION: http://127.0.0.1:17381/gatedesc.xml\r\n"
    "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
    "USN: uuid:fake::urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n";
static const char EXPECTED_LOCATION[] = "http://127.0.0.1:17381/gatedesc.xml";

/* minimal device description XML: one WANIPConnection service, controlURL=/soap,
 * serviceType=urn:schemas-upnp-org:service:WANIPConnection:1。 */
static const char GATE_DESC_XML[] =
    "<?xml version=\"1.0\"?>\n"
    "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\n"
    "  <specVersion><major>1</major><minor>0</minor></specVersion>\n"
    "  <device>\n"
    "    <deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>\n"
    "    <serviceList>\n"
    "      <service>\n"
    "        <serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>\n"
    "        <controlURL>/soap</controlURL>\n"
    "        <eventSubURL>/event</eventSubURL>\n"
    "        <SCPDURL>/scpd.xml</SCPDURL>\n"
    "      </service>\n"
    "    </serviceList>\n"
    "  </device>\n"
    "</root>\n";

/* sample SOAP response (GetExternalIPAddress): upnpreplyparse should yield
 * NewExternalIPAddress=203.0.113.42 */
static const char SOAP_EXTIP_RESP[] =
    "<?xml version=\"1.0\"?>\n"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
    " s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\n"
    "  <s:Body>\n"
    "    <u:GetExternalIPAddressResponse xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\">\n"
    "      <NewExternalIPAddress>203.0.113.42</NewExternalIPAddress>\n"
    "    </u:GetExternalIPAddressResponse>\n"
    "  </s:Body>\n"
    "</s:Envelope>\n";

/* read one HTTP request up to "\r\n\r\n", plus the body if Content-Length is set */
static int read_one_request(mco_socket_t* conn, char* buf, int cap,
                            char* path, int pathcap,
                            char* body_out, int body_cap, int* body_len) {
    int len = 0;
    char* p;
    int content_length = 0;
    for (;;) {
        int r = mco_read(conn, buf + len, cap - len, 3000);
        if (r <= 0) return -1;
        len += r;
        buf[len < cap ? len : cap - 1] = 0;
        if (strstr(buf, "\r\n\r\n")) break;
        if (len >= cap) return -1;
    }
    {
        char* sp1 = strchr(buf, ' ');
        char* sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
        int plen = (sp1 && sp2) ? (int)(sp2 - sp1 - 1) : 0;
        if (plen <= 0 || plen >= pathcap) return -1;
        memcpy(path, sp1 + 1, plen);
        path[plen] = 0;
    }
    p = strstr(buf, "Content-Length:");
    if (p) content_length = atoi(p + strlen("Content-Length:"));
    {
        char* body_start = strstr(buf, "\r\n\r\n") + 4;
        int have = len - (int)(body_start - buf);
        if (body_len) *body_len = 0;
        if (content_length > 0 && body_out) {
            while (have < content_length) {
                int r = mco_read(conn, buf + len, cap - len, 3000);
                if (r <= 0) return -1;
                len += r;
                have += r;
            }
            if (content_length > body_cap - 1) content_length = body_cap - 1;
            memcpy(body_out, body_start, (size_t)content_length);
            body_out[content_length] = 0;
            if (body_len) *body_len = content_length;
        }
    }
    return 0;
}

static void send_response(mco_socket_t* conn, const char* body, int body_len) {
    char header[128];
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\nContent-Length: %d\r\n\r\n",
                        body_len);
    mco_write(conn, header, hlen, 3000);
    mco_write(conn, body, body_len, 3000);
}

/* fake gateway: serves 1 GET /gatedesc.xml + 1 SOAP POST /soap */
static void server_task(void* arg) {
    mco_socket_t* listener = (mco_socket_t*)arg;
    int i;
    (void)arg;
    for (i = 0; i < 2; i++) {  /* 1 desc + 1 soap */
        mco_socket_t* conn = mco_tcp_accept(listener, 5000);
        char reqbuf[4096];
        char path[128];
        char body[2048];
        int body_len = 0;
        if (!conn) continue;
        if (read_one_request(conn, reqbuf, sizeof(reqbuf), path, sizeof(path),
                             body, sizeof(body), &body_len) != 0) {
            mco_close(conn); mco_socket_destroy(conn); continue;
        }
        if (strcmp(path, "/gatedesc.xml") == 0) {
            send_response(conn, GATE_DESC_XML, (int)sizeof(GATE_DESC_XML) - 1);
        } else if (strcmp(path, "/soap") == 0) {
            /* the body must be a SOAP envelope with GetExternalIPAddress */
            if (strstr(body, "GetExternalIPAddress") != NULL &&
                strstr(body, "Envelope") != NULL) {
                send_response(conn, SOAP_EXTIP_RESP, (int)sizeof(SOAP_EXTIP_RESP) - 1);
            } else {
                const char* bad = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
                mco_write(conn, bad, (int)strlen(bad), 3000);
            }
        } else {
            const char* nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
            mco_write(conn, nf, (int)strlen(nf), 3000);
        }
        mco_close(conn);
        mco_socket_destroy(conn);
    }
    mco_close(listener);
    mco_socket_destroy(listener);
    g_server_done = 1;
}

static void client_task(void* arg) {
    mco_upnp_t* upnp;
    char loc[512];
    int rc;
    (void)arg;
    mco_sleep(50);

    /* 1) new/free */
    upnp = mco_upnp_new();
    g_newfree_ok = (upnp != NULL);
    mco_upnp_free(upnp);
    upnp = NULL;

    /* 2) SSDP response parsing */
    rc = mco_upnp__parse_ssdp_response(SSDP_RESP, (int)sizeof(SSDP_RESP) - 1,
                                        loc, (int)sizeof(loc));
    if (rc == 0 && strcmp(loc, EXPECTED_LOCATION) == 0) {
        g_parse_ok = 1;
        printf("[parse] LOCATION = %s\n", loc);
    } else {
        printf("[parse] FAILED rc=%d\n", rc);
    }
    {
        const char non_igd[] = "HTTP/1.1 200 OK\r\nLOCATION: http://x/y\r\n\r\n";
        rc = mco_upnp__parse_ssdp_response(non_igd, (int)sizeof(non_igd) - 1,
                                            loc, (int)sizeof(loc));
        if (rc != 0) printf("[parse] non-IGD correctly rejected\n");
    }

    /* 3) discover timeout with no gateway */
    upnp = mco_upnp_new();
    rc = mco_upnp_discover(upnp, 600);
    if (rc < 0) {
        g_discover_timeout_ok = 1;
        printf("[discover] no gateway rc=%d (expected <0)\n", rc);
    } else {
        printf("[discover] unexpected success rc=%d\n", rc);
    }
    mco_upnp_free(upnp);

    /* 4) description fetch + IGD parse via the local fake gateway */
    upnp = mco_upnp_new();
    rc = mco_upnp__fetch_description(upnp, "http://127.0.0.1:17381/gatedesc.xml", 3000);
    if (rc == 0 &&
        strcmp(upnp->service_type, "urn:schemas-upnp-org:service:WANIPConnection:1") == 0 &&
        strcmp(upnp->control_url, "http://127.0.0.1:17381/soap") == 0) {
        g_desc_ok = 1;
        printf("[desc] service_type=%s control_url=%s\n", upnp->service_type, upnp->control_url);
    } else {
        printf("[desc] FAILED rc=%d st='%s' ctl='%s'\n",
               rc, upnp->service_type, upnp->control_url);
    }

    /* 5) SOAP: reuse the upnp above for get_external_ip */
    if (g_desc_ok) {
        char extip[64];
        rc = mco_upnp_get_external_ip(upnp, extip, sizeof(extip), 3000);
        if (rc == 0 && strcmp(extip, "203.0.113.42") == 0) {
            g_soap_ok = 1;
            printf("[soap] external ip = %s\n", extip);
        } else {
            printf("[soap] FAILED rc=%d extip='%s'\n", rc, rc == 0 ? extip : "(none)");
        }
    }
    mco_upnp_free(upnp);
}

int main(void) {
    mco_loop_t* loop = mco_loop_new();
    mco_socket_t* listener = mco_tcp_listen("0.0.0.0", TEST_PORT);
    int ok;

    if (!listener) { printf("listen failed\n"); return 1; }

    mco_spawn(loop, server_task, listener);
    mco_spawn(loop, client_task, NULL);
    mco_loop_run(loop);
    mco_loop_free(loop);

    printf("newfree=%d parse=%d discover_timeout=%d desc=%d soap=%d server_done=%d\n",
           g_newfree_ok, g_parse_ok, g_discover_timeout_ok, g_desc_ok, g_soap_ok, g_server_done);

    ok = g_newfree_ok && g_parse_ok && g_discover_timeout_ok && g_desc_ok && g_soap_ok && g_server_done;
    printf(ok ? "SMOKE TEST PASSED\n" : "SMOKE TEST FAILED\n");
    return ok ? 0 : 1;
}
