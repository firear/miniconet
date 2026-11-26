/*
 * wintls.c — hssl backend over Schannel (SSPI), Windows only.
 *
 * Layers (bottom-up): 0 constants/helpers, 1 PEM cert+key load (server ctx),
 * 2 ctx lifecycle, 3 raw IO (one syscall, never loops), 4 connection object,
 * 5 reentrant handshake state machine (client/server share it), 6 record layer
 * (decrypt / encrypt+out-queue), 7 close_notify, 8 public API + error mapping.
 *
 * Contract with the coroutine model (must hold):
 *   - Sockets are non-blocking and mco_wait_io waits for readiness. Every
 *     send/recv does exactly one syscall; EWOULDBLOCK becomes HSSL_WANT_READ/
 *     WRITE for the caller to retry. No retry loop (it would spin the vcpu).
 *   - The handshake is reentrant: each hssl_accept/hssl_connect call advances
 *     one step; received ciphertext must stay in the conn object until reused.
 *   - Return semantics: >0 bytes, 0 EOF, <0 via hssl_get_error (see hssl.h).
 */
#include "hssl.h"

#ifdef WITH_WINTLS

#include "hdef.h"
#include "portable/WinCrypt.h"
#include "portable/schannel.h"
#include <windows.h>
#include <wintrust.h>
#include <stdlib.h>
#include <string.h>

#define SECURITY_WIN32
#include <security.h>
#include <sspi.h>

/* ---- 0. constants & helpers ---- */

/* max plaintext of one TLS record; ciphertext adds header/trailer */
#define WINTLS_RECORD_MAX  16384
#define WINTLS_WIRE_BUF    (WINTLS_RECORD_MAX + 1024)   /* ciphertext buffer: socket + EncryptMessage output;
                                                       plaintext uses the same size (struct wintls_s::plain) */

/* raw IO result, kept distinct from byte counts so -1 is not misread as bytes */
#define WINTLS_IO_AGAIN    (-1)   /* EWOULDBLOCK: retry when ready */
#define WINTLS_IO_EOF      (0)    /* peer closed */
#define WINTLS_IO_ERR      (-2)   /* real error */

#ifndef SEC_E_DECRYPT_FAILURE
#define SEC_E_DECRYPT_FAILURE _HRESULT_TYPEDEF_(0x80090330L)
#endif
#ifndef ISC_REQ_MANUAL_CRED_VALIDATION
#define ISC_REQ_MANUAL_CRED_VALIDATION 0x00080000
#endif
#ifndef SCH_USE_STRONG_CRYPTO
#define SCH_USE_STRONG_CRYPTO 0x00400000
#endif
#ifndef SECBUFFER_ALERT
#define SECBUFFER_ALERT 17
#endif
#ifndef SCHANNEL_SHUTDOWN
#define SCHANNEL_SHUTDOWN 0x00000001
#endif

/* server request: STREAM is required so DecryptMessage yields the 4-part output */
#define WINTLS_ASC_REQ  (ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_CONFIDENTIALITY | \
                         ASC_REQ_STREAM | ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT)
#define WINTLS_ISC_REQ  (ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_CONFIDENTIALITY | \
                         ISC_REQ_STREAM | ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT)

static int wintls_min(int a, int b) { return a < b ? a : b; }

static char* wintls_strdup(const char* s)
{
    size_t n;
    char* p;
    if (!s) return NULL;
    n = strlen(s) + 1;
    p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

const char* hssl_backend(void)
{
    return "schannel";
}

/* ---- 1. PEM cert & key load ----
 * Mirrors openssl's cert-chain + key file (test/certs/). Falls back to the
 * system MY store by subject name when PEM load fails. */

/* read a whole file into a malloc'd NUL-terminated buffer */
static char* wintls_read_file_text(const char* path, DWORD* out_len)
{
    HANDLE fh;
    DWORD size, rd;
    char* buf;

    fh = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(fh, NULL);
    if (size == INVALID_FILE_SIZE || size == 0) { CloseHandle(fh); return NULL; }
    buf = (char*)malloc((size_t)size + 1);
    if (!buf) { CloseHandle(fh); return NULL; }
    rd = 0;
    if (!ReadFile(fh, buf, size, &rd, NULL)) { CloseHandle(fh); free(buf); return NULL; }
    CloseHandle(fh);
    buf[rd] = '\0';
    if (out_len) *out_len = rd;
    return buf;
}

/* find the next PEM block labelled `label` in [p, end), base64-decode the body
 * to DER. Returns 1 with malloc'd *der/ *derlen and *next past it, else 0. */
static int wintls_pem_next_block(const char* p, const char* end, const char* label,
                                 const char** next, BYTE** der, DWORD* derlen)
{
    char begin[64], endtag[64];
    const char *b, *e, *body, *q;
    char* b64;
    size_t lb, n, cap;
    DWORD dlen;
    BYTE* out;

    lb = strlen(label);
    if (lb > 48) return 0;
    sprintf(begin, "-----BEGIN %s-----", label);
    sprintf(endtag, "-----END %s-----", label);
    b = strstr(p, begin);
    if (!b) { *next = end; return 0; }
    e = strstr(b, endtag);
    if (!e) { *next = end; return 0; }

    /* keep only base64 chars, drop newlines/whitespace */
    body = b + strlen(begin);
    cap = (size_t)(e - body) + 1;
    b64 = (char*)malloc(cap);
    if (!b64) { *next = e + strlen(endtag); return 0; }
    n = 0;
    for (q = body; q < e; ++q) {
        char c = *q;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=') {
            b64[n++] = c;
        }
    }
    b64[n] = '\0';

    dlen = 0;
    if (!CryptStringToBinaryA(b64, (DWORD)n, CRYPT_STRING_BASE64, NULL, &dlen, NULL, NULL)) {
        free(b64); *next = e + strlen(endtag); return 0;
    }
    out = (BYTE*)malloc(dlen ? dlen : 1);
    if (!out) { free(b64); *next = e + strlen(endtag); return 0; }
    if (!CryptStringToBinaryA(b64, (DWORD)n, CRYPT_STRING_BASE64, out, &dlen, NULL, NULL)) {
        free(b64); free(out); *next = e + strlen(endtag); return 0;
    }
    free(b64);
    *der = out; *derlen = dlen; *next = e + strlen(endtag);
    return 1;
}

/* Decode the DER of a PKCS#1 ("RSA PRIVATE KEY") or PKCS#8 ("PRIVATE KEY") key
 * into a CAPI PRIVATEKEYBLOB. Returns 1 with malloc'd out/outlen. RSA only. */
static int wintls_pemkey_to_blob(const BYTE* der, DWORD dlen, BYTE** out, DWORD* outlen)
{
    BYTE* blob;
    DWORD blen, cb;
    CRYPT_PRIVATE_KEY_INFO* pki;

    blob = NULL;
    blen = 0;
    /* try PKCS#8 first: its PrivateKey field is the PKCS#1 key */
    cb = 0;
    if (CryptDecodeObjectEx(X509_ASN_ENCODING, PKCS_PRIVATE_KEY_INFO, der, dlen, 0, NULL, NULL, &cb) && cb > 0) {
        pki = (CRYPT_PRIVATE_KEY_INFO*)malloc(cb);
        if (pki) {
            if (CryptDecodeObjectEx(X509_ASN_ENCODING, PKCS_PRIVATE_KEY_INFO, der, dlen, 0, NULL, pki, &cb) &&
                wintls_pemkey_to_blob(pki->PrivateKey.pbData, pki->PrivateKey.cbData, &blob, &blen)) {
                free(pki);
                *out = blob; *outlen = blen;
                return 1;
            }
            free(pki);
        }
    }
    /* PKCS#1 RSA: PKCS_RSA_PRIVATE_KEY decodes straight to PRIVATEKEYBLOB */
    cb = 0;
    if (!CryptDecodeObjectEx(X509_ASN_ENCODING, PKCS_RSA_PRIVATE_KEY, der, dlen, 0, NULL, NULL, &cb) || cb == 0)
        return 0;
    blob = (BYTE*)malloc(cb);
    if (!blob) return 0;
    if (CryptDecodeObjectEx(X509_ASN_ENCODING, PKCS_RSA_PRIVATE_KEY, der, dlen, 0, NULL, blob, &cb)) {
        *out = blob; *outlen = cb;
        return 1;
    }
    free(blob);
    return 0;
}

/* Load the full cert chain from a PEM file (first entry is the leaf). Returns a
 * malloc'd array, *out_n is the count; NULL on failure. */
static PCCERT_CONTEXT* wintls_load_certs(const char* path, DWORD* out_n)
{
    DWORD flen, cap, n;
    char* pem;
    const char *p, *end;
    PCCERT_CONTEXT* arr;

    flen = 0;
    pem = wintls_read_file_text(path, &flen);
    if (!pem) return NULL;
    arr = NULL;
    cap = 0; n = 0;
    p = pem;
    end = pem + flen;
    for (;;) {
        BYTE* der;
        DWORD dlen;
        const char* next;
        PCCERT_CONTEXT ctx;

        der = NULL; dlen = 0; next = p;
        if (!wintls_pem_next_block(p, end, "CERTIFICATE", &next, &der, &dlen)) break;
        ctx = CertCreateCertificateContext(X509_ASN_ENCODING, der, dlen);
        free(der);
        if (ctx) {
            if (n >= cap) {
                PCCERT_CONTEXT* tmp;
                cap = cap ? cap * 2 : 4;
                tmp = (PCCERT_CONTEXT*)realloc(arr, (size_t)cap * sizeof(*arr));
                if (!tmp) { CertFreeCertificateContext(ctx); break; }
                arr = tmp;
            }
            arr[n++] = ctx;
        }
        p = next;
    }
    free(pem);
    *out_n = n;
    return arr;
}

/* Load a private key from a PEM file into a named CSP container (kept alive),
 * then attach the container to the leaf cert's CERT_KEY_PROV_INFO_PROP_ID so
 * Schannel can fetch the key at handshake. Returns 1. RSA only. */
static int wintls_load_key(const char* path, PCCERT_CONTEXT leaf, HCRYPTPROV* out_prov)
{
    static LONG s_seq = 0;
    char* pem;
    const char* next;
    BYTE *der, *blob;
    DWORD flen, dlen, blen;
    int ok;

    flen = 0;
    pem = wintls_read_file_text(path, &flen);
    if (!pem) return 0;
    ok = 0;
    der = NULL; blob = NULL; dlen = 0; blen = 0; next = pem;
    /* try PKCS#8 ("PRIVATE KEY"), then PKCS#1 ("RSA PRIVATE KEY") */
    if (!wintls_pem_next_block(pem, pem + flen, "PRIVATE KEY", &next, &der, &dlen)) {
        if (!wintls_pem_next_block(pem, pem + flen, "RSA PRIVATE KEY", &next, &der, &dlen)) {
            free(pem);
            return 0;
        }
    }
    if (wintls_pemkey_to_blob(der, dlen, &blob, &blen)) {
        wchar_t name[64];
        HCRYPTPROV hProv;
        HCRYPTKEY hKey;

        wsprintfW(name, L"miniconet_schannel_%ld", InterlockedIncrement(&s_seq));
        hProv = 0;
        /* use the W variants throughout: CryptAcquireContext resolves to the A
         * form here (wants LPCSTR) while CRYPT_KEY_PROV_INFO fields are LPWSTR */
        if (!CryptAcquireContextW(&hProv, name, MS_ENHANCED_PROV_W, PROV_RSA_FULL, CRYPT_NEWKEYSET)) {
            /* container already exists (leftover from a previous run): just open */
            CryptAcquireContextW(&hProv, name, MS_ENHANCED_PROV_W, PROV_RSA_FULL, 0);
        }
        if (hProv) {
            hKey = 0;
            if (CryptImportKey(hProv, blob, blen, 0, 0, &hKey)) {
                wchar_t* cont;
                CRYPT_KEY_PROV_INFO kpi;

                cont = (wchar_t*)malloc(sizeof(name));
                if (cont) {
                    memcpy(cont, name, sizeof(name));
                    memset(&kpi, 0, sizeof(kpi));
                    kpi.pwszContainerName = cont;
                    kpi.pwszProvName = (LPWSTR)MS_ENHANCED_PROV_W;
                    kpi.dwProvType = PROV_RSA_FULL;
                    kpi.dwKeySpec = AT_KEYEXCHANGE;
                    if (CertSetCertificateContextProperty(leaf, CERT_KEY_PROV_INFO_PROP_ID, 0, &kpi)) {
                        *out_prov = hProv;
                        ok = 1;
                    } else {
                        printe("wintls: set key prop failed %x\n", GetLastError());
                    }
                    free(cont);
                }
                /* the key handle is just a reference; the key persists in the
                 * container, which is kept alive with the ctx */
                CryptDestroyKey(hKey);
            } else {
                printe("wintls: CryptImportKey failed %x\n", GetLastError());
            }
            if (!ok) CryptReleaseContext(hProv, 0);
        }
        free(blob);
    }
    free(der);
    free(pem);
    return ok;
}

static void wintls_free_certs(PCCERT_CONTEXT* certs, DWORD n)
{
    DWORD i;
    if (!certs) return;
    for (i = 0; i < n; ++i) CertFreeCertificateContext(certs[i]);
    free(certs);
}

/* ---- 2. ctx lifecycle ---- */

/* wintls ctx: besides the CredHandle, the PEM-loaded cert chain and the key
 * container must stay alive (Schannel may fetch the key mid-handshake). */
typedef struct wintls_ctx {
    CredHandle      cred;
    HCRYPTPROV      hProv;      /* CSP container holding the key (alive until free) */
    PCCERT_CONTEXT* certs;      /* cert chain, referenced by Schannel directly */
    DWORD           ncerts;
    int             verify_peer; /* client: let Schannel do default cert verification */
    int             endpoint;    /* HSSL_SERVER / HSSL_CLIENT */
} wintls_ctx_t;

hssl_ctx_t hssl_ctx_new(hssl_ctx_opt_t* opt)
{
    wintls_ctx_t* ctx;
    SCHANNEL_CRED credData;
    TimeStamp lifetime;
    SECURITY_STATUS st;
    TCHAR unisp_name[] = UNISP_NAME;
    unsigned long credflag;

    ctx = (wintls_ctx_t*)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    if (opt) {
        if (opt->verify_peer) ctx->verify_peer = 1;
        ctx->endpoint = opt->endpoint;
    }
    memset(&credData, 0, sizeof(credData));
    credData.dwVersion = SCHANNEL_CRED_VERSION;

    if (ctx->endpoint == HSSL_SERVER) {
        /* prefer x509 PEM cert+key (openssl's chain-file + key-file); on failure
         * fall back to the system MY store by subject name (locally deployed cert) */
        if (opt && opt->crt_file && *opt->crt_file && opt->key_file && *opt->key_file) {
            PCCERT_CONTEXT* certs = wintls_load_certs(opt->crt_file, &ctx->ncerts);
            if (certs && ctx->ncerts > 0 &&
                wintls_load_key(opt->key_file, certs[0], &ctx->hProv)) {
                ctx->certs = certs;
                printd("wintls: loaded server cert/key from PEM: %s\n", opt->crt_file);
            } else {
                printe("wintls: PEM cert/key load failed, fallback to MY store.\n");
                if (ctx->ncerts > 0) wintls_free_certs(certs, ctx->ncerts);
                ctx->ncerts = 0;
            }
        }
        if (ctx->ncerts == 0) {
            HCERTSTORE hStore;
            PCCERT_CONTEXT serverCert;

            hStore = CertOpenStore(CERT_STORE_PROV_SYSTEM, X509_ASN_ENCODING, 0,
                                   CERT_SYSTEM_STORE_LOCAL_MACHINE, L"MY");
            if (!hStore) {
                printe("wintls: open MY store failed %x\n", GetLastError());
                free(ctx);
                return NULL;
            }
            serverCert = CertFindCertificateInStore(hStore, X509_ASN_ENCODING, 0,
                                                    CERT_FIND_SUBJECT_STR_A,
                                                    opt ? opt->crt_file : NULL, NULL);
            CertCloseStore(hStore, 0);
            if (!serverCert) {
                printe("wintls: cert not found in MY store %x\n", GetLastError());
                free(ctx);
                return NULL;
            }
            /* MY-store cert private keys are system-managed; Schannel fetches them */
            ctx->certs = (PCCERT_CONTEXT*)malloc(sizeof(PCCERT_CONTEXT));
            if (!ctx->certs) {
                CertFreeCertificateContext(serverCert);
                free(ctx);
                return NULL;
            }
            ctx->certs[0] = serverCert;
            ctx->ncerts = 1;
        }
        credData.cCreds = ctx->ncerts;
        credData.paCred = ctx->certs;
        credflag = SECPKG_CRED_INBOUND;
    } else {
        credflag = SECPKG_CRED_OUTBOUND; /* client needs no local cert */
    }

    st = AcquireCredentialsHandle(NULL, unisp_name, credflag, NULL, &credData,
                                  NULL, NULL, &ctx->cred, &lifetime);
    if (st != SEC_E_OK) {
        printe("wintls: AcquireCredentialsHandle 0x%x\n", st);
        hssl_ctx_free(ctx);
        return NULL;
    }
    return ctx;
}

void hssl_ctx_free(hssl_ctx_t ssl_ctx)
{
    wintls_ctx_t* ctx = (wintls_ctx_t*)ssl_ctx;
    SECURITY_STATUS st;

    if (!ctx) return;
    st = FreeCredentialsHandle(&ctx->cred);
    if (st != SEC_E_OK) printe("wintls: FreeCredentialsHandle 0x%x\n", st);
    wintls_free_certs(ctx->certs, ctx->ncerts);
    if (ctx->hProv) CryptReleaseContext(ctx->hProv, 0);
    free(ctx);
}

/* ---- 3. raw IO: one syscall, never loops ----
 * Sockets are non-blocking; EWOULDBLOCK becomes WANT_* for the caller to retry.
 * No in-place retry (it would spin the vcpu).
 */

static void init_sec_buffer(SecBuffer* b, unsigned long type, unsigned long len, void* buf)
{
    b->BufferType = type;
    b->cbBuffer = len;
    b->pvBuffer = buf;
}

static void init_sec_buffer_desc(SecBufferDesc* d, unsigned long version,
                                 unsigned long n, SecBuffer* bufs)
{
    d->ulVersion = version;
    d->cBuffers = n;
    d->pBuffers = bufs;
}

/* recv once: >0 bytes; WINTLS_IO_EOF peer closed; WINTLS_IO_AGAIN no data;
 * WINTLS_IO_ERR real error */
static int wintls_recv(int fd, unsigned char* buf, int len)
{
    int n;

    if (len <= 0) return WINTLS_IO_ERR;
    for (;;) {
        n = recv(fd, (char*)buf, len, 0);
        if (n >= 0) return n;
        if (WSAGetLastError() == WSAEINTR) continue;
        if (WSAGetLastError() == WSAEWOULDBLOCK) return WINTLS_IO_AGAIN;
        return WINTLS_IO_ERR;
    }
}

/* send once: >=0 bytes sent (maybe partial); WINTLS_IO_AGAIN nothing sent;
 * WINTLS_IO_ERR real error */
static int wintls_send(int fd, const unsigned char* buf, int len)
{
    int n;

    if (len <= 0) return WINTLS_IO_ERR;
    for (;;) {
        n = send(fd, (const char*)buf, len, 0);
        if (n >= 0) return n == 0 ? WINTLS_IO_AGAIN : n;
        if (WSAGetLastError() == WSAEINTR) continue;
        if (WSAGetLastError() == WSAEWOULDBLOCK) return WINTLS_IO_AGAIN;
        return WINTLS_IO_ERR;
    }
}

/* ---- 4. connection object ---- */

typedef enum {
    WHS_INIT = 0,    /* no token sent yet */
    WHS_NEGOTIATING, /* handshake in progress */
    WHS_DONE,
    WHS_FAILED
} wintls_hs_state;

struct wintls_s {
    wintls_ctx_t* ctx;
    int           fd;
    int           is_server;       /* pick AcceptSecurityContext vs InitializeSecurityContext */
    char*         sni;             /* client target name (Schannel uses it for host check) */

    SecHandle     sechandle;
    int           sechandle_valid; /* first ASC/ISC must pass NULL, not &sechandle */
    SecPkgContext_StreamSizes sizes;
    int           sizes_valid;
    wintls_hs_state state;
    int           hs_need_more;    /* Schannel: record incomplete, recv again first next call */

    /* ciphertext input: bytes recv'd but not yet consumed by Schannel (partial packets) */
    unsigned char in[WINTLS_WIRE_BUF];
    int           in_len;

    /* decrypted plaintext, drained by the caller. One WINTLS_WIRE_BUF holds any
     * single record's plaintext; leftovers stay in in until drained (no 2x). */
    unsigned char plain[WINTLS_WIRE_BUF];
    int           plain_len;
    int           plain_off;

    /* pending out: handshake token or unsent ciphertext (queued on partial write) */
    unsigned char out[WINTLS_WIRE_BUF];
    int           out_len;
    int           out_sent;

    int           peer_closed;     /* peer sent close_notify */
    int           shutdown_sent;   /* we sent close_notify */
};

hssl_t hssl_new(hssl_ctx_t ssl_ctx, int fd, uint8_t is_server)
{
    struct wintls_s* s;

    if (!ssl_ctx) return NULL;
    s = (struct wintls_s*)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->ctx = (wintls_ctx_t*)ssl_ctx;
    s->fd = fd;
    s->is_server = is_server ? 1 : 0;
    s->state = WHS_INIT;
    return s;
}

void hssl_free(hssl_t _ssl)
{
    struct wintls_s* s = (struct wintls_s*)_ssl;
    SECURITY_STATUS st;

    if (!s) return;
    if (s->sechandle_valid) {
        st = DeleteSecurityContext(&s->sechandle);
        if (st != SEC_E_OK) printe("wintls: DeleteSecurityContext 0x%x\n", st);
    }
    free(s->sni);
    free(s);
}

/* Free the output buffers Schannel allocated with ASC_REQ/ISC_REQ_ALLOCATE_MEMORY.
 * Only those it actually filled (pvBuffer != NULL). */
static void free_sec_buffers(SecBufferDesc* desc)
{
    unsigned long i;

    for (i = 0; i < desc->cBuffers; ++i) {
        if (desc->pBuffers[i].pvBuffer) {
            FreeContextBuffer(desc->pBuffers[i].pvBuffer);
            desc->pBuffers[i].pvBuffer = NULL;
            desc->pBuffers[i].cbBuffer = 0;
        }
    }
}

/* ---- 5. handshake state machine ----
 * Each hssl_accept/hssl_connect call advances one step:
 *   HSSL_OK         done
 *   HSSL_WANT_READ  wait for readable (maybe a partial packet is in s->in)
 *   HSSL_WANT_WRITE wait for writable (token not fully sent, in s->out)
 *   HSSL_ERROR      failed
 */

/* Flush out[out_sent..out_len). 0 = done; -1 = leftover (wait writable); -2 = error */
static int wintls_flush_out(struct wintls_s* s)
{
    while (s->out_sent < s->out_len) {
        int n = wintls_send(s->fd, s->out + s->out_sent, s->out_len - s->out_sent);
        if (n == WINTLS_IO_AGAIN) return -1;
        if (n == WINTLS_IO_ERR) return -2;
        s->out_sent += n;
    }
    s->out_len = 0;
    s->out_sent = 0;
    return 0;
}

/* move the unconsumed tail (extra bytes) in `in` to the buffer head */
static void wintls_keep_extra(struct wintls_s* s, unsigned long extra)
{
    if (extra <= 0) { s->in_len = 0; return; }
    if ((int)extra > s->in_len) extra = (unsigned long)s->in_len;
    if ((int)extra < s->in_len)
        memmove(s->in, s->in + (s->in_len - (int)extra), (size_t)extra);
    s->in_len = (int)extra;
}

/* Copy Schannel's output token into the out-queue and flush. Same return as
 * wintls_flush_out; the caller frees the token buffer. */
static int wintls_queue_token(struct wintls_s* s, SecBuffer* token)
{
    if (!token->pvBuffer || token->cbBuffer == 0) return 0;
    if (token->cbBuffer > WINTLS_WIRE_BUF) return -2;
    memcpy(s->out, token->pvBuffer, (size_t)token->cbBuffer);
    s->out_len = (int)token->cbBuffer;
    s->out_sent = 0;
    return wintls_flush_out(s);
}

static int wintls_handshake_step(struct wintls_s* s)
{
    SecBuffer in_buf[2], out_buf[3];
    SecBufferDesc in_desc, out_desc;
    ULONG attrs;
    TimeStamp lifetime;
    SECURITY_STATUS st;
    unsigned long ctx_req;
    int first_call, rc, done;

    if (s->state == WHS_DONE) return HSSL_OK;
    if (s->state == WHS_FAILED) return HSSL_ERROR;

    /* 1) flush the previous unsent token first (keep byte order on partial write) */
    rc = wintls_flush_out(s);
    if (rc == -1) return HSSL_WANT_WRITE;
    if (rc == -2) goto fail;

    /* 2) recv input if needed. The client's first packet (ClientHello) needs none;
     *    with a partial packet (hs_need_more) keep receiving, don't re-feed old data. */
    first_call = !s->sechandle_valid;
    if ((!first_call || s->is_server) && (s->in_len == 0 || s->hs_need_more)) {
        int n;
        int space = WINTLS_WIRE_BUF - s->in_len;

        if (space <= 0) goto fail; /* one handshake message larger than the buffer */
        n = wintls_recv(s->fd, s->in + s->in_len, space);
        s->hs_need_more = 0;
        if (n == WINTLS_IO_AGAIN) return HSSL_WANT_READ;
        if (n <= 0) goto fail;    /* 0 = peer closed, -2 = error */
        s->in_len += n;
    }

    /* 3) build sec buffers, step the state machine */
    init_sec_buffer(&in_buf[0], SECBUFFER_TOKEN, (unsigned long)s->in_len, s->in);
    init_sec_buffer(&in_buf[1], SECBUFFER_EMPTY, 0, NULL);
    init_sec_buffer(&out_buf[0], SECBUFFER_TOKEN, 0, NULL);
    init_sec_buffer(&out_buf[1], SECBUFFER_ALERT, 0, NULL);
    init_sec_buffer(&out_buf[2], SECBUFFER_EMPTY, 0, NULL);
    init_sec_buffer_desc(&in_desc, SECBUFFER_VERSION, 2, in_buf);
    init_sec_buffer_desc(&out_desc, SECBUFFER_VERSION, 3, out_buf);
    attrs = 0;
    memset(&lifetime, 0, sizeof(lifetime));

    if (s->is_server) {
        st = AcceptSecurityContext(&s->ctx->cred,
                                   first_call ? NULL : &s->sechandle,
                                   &in_desc, WINTLS_ASC_REQ, 0, &s->sechandle,
                                   &out_desc, &attrs, &lifetime);
    } else {
        /* First call has no input; later calls carry the server token.
         * verify_peer=0 (self-signed / dev) adds MANUAL_CRED_VALIDATION to skip
         * Schannel's default chain+host verification. */
        SecBufferDesc* p_in = (first_call && s->in_len == 0) ? NULL : &in_desc;
        ctx_req = WINTLS_ISC_REQ;
        if (!s->ctx->verify_peer) ctx_req |= ISC_REQ_MANUAL_CRED_VALIDATION;
        st = InitializeSecurityContext(&s->ctx->cred,
                                       first_call ? NULL : &s->sechandle,
                                       s->sni, ctx_req, 0, 0,
                                       p_in, 0, &s->sechandle,
                                       &out_desc, &attrs, &lifetime);
    }
    s->sechandle_valid = 1;

    if (st == SEC_E_INCOMPLETE_MESSAGE || st == SEC_I_INCOMPLETE_CREDENTIALS) {
        free_sec_buffers(&out_desc);
        /* do not drop any buffered bytes; wait for readable, continue receiving */
        s->hs_need_more = 1;
        s->state = WHS_NEGOTIATING;
        return HSSL_WANT_READ;
    }
    if (st == SEC_I_COMPLETE_NEEDED || st == SEC_I_COMPLETE_AND_CONTINUE) {
        SECURITY_STATUS cs = CompleteAuthToken(&s->sechandle, &out_desc);
        if (cs != SEC_E_OK) {
            printe("wintls: CompleteAuthToken 0x%x\n", cs);
            free_sec_buffers(&out_desc);
            goto fail;
        }
        st = (st == SEC_I_COMPLETE_NEEDED) ? SEC_E_OK : SEC_I_CONTINUE_NEEDED;
    }
    if (st != SEC_E_OK && st != SEC_I_CONTINUE_NEEDED) {
        printe("wintls: %sSecurityContext 0x%x\n", s->is_server ? "Accept" : "Initialize", st);
        free_sec_buffers(&out_desc);
        goto fail;
    }

    /* 4) send the token Schannel produced (queue the rest, flush first next time) */
    rc = wintls_queue_token(s, &out_buf[0]);
    free_sec_buffers(&out_desc);
    if (rc == -2) goto fail;
    if (rc == -1) { s->state = WHS_NEGOTIATING; return HSSL_WANT_WRITE; }

    /* 5) consume input: SECBUFFER_EXTRA holds the next message's start */
    wintls_keep_extra(s, (in_buf[1].BufferType == SECBUFFER_EXTRA) ? in_buf[1].cbBuffer : 0);

    done = (st == SEC_E_OK);
    if (done) {
        SECURITY_STATUS qs = QueryContextAttributes(&s->sechandle, SECPKG_ATTR_STREAM_SIZES, &s->sizes);
        if (qs != SEC_E_OK) {
            printe("wintls: QueryContextAttributes(STREAM_SIZES) 0x%x\n", qs);
            goto fail;
        }
        s->sizes_valid = 1;
        s->state = WHS_DONE;
        return HSSL_OK;
    }
    s->state = WHS_NEGOTIATING;
    return HSSL_WANT_READ;

fail:
    s->state = WHS_FAILED;
    return HSSL_ERROR;
}

int hssl_accept(hssl_t ssl)
{
    struct wintls_s* s = (struct wintls_s*)ssl;
    if (!s) return HSSL_ERROR;
    return wintls_handshake_step(s);
}

int hssl_connect(hssl_t ssl)
{
    struct wintls_s* s = (struct wintls_s*)ssl;
    if (!s) return HSSL_ERROR;
    return wintls_handshake_step(s);
}

/* ---- 6. record layer ---- */

/* Decrypt complete records from in into plain; leave undecryptable partial
 * packets in in. Returns 0 = ok, -1 = error. */
static int wintls_decrypt_pending(struct wintls_s* s)
{
    /* stop when plain has no room for one more record; caller drains plain first */
    while (s->in_len > 0 &&
           s->plain_len + WINTLS_RECORD_MAX <= (int)sizeof(s->plain)) {
        SecBuffer b[4];
        SecBufferDesc desc;
        SECURITY_STATUS st;
        unsigned long extra;
        int consumed, data_len;

        init_sec_buffer(&b[0], SECBUFFER_DATA, (unsigned long)s->in_len, s->in);
        init_sec_buffer(&b[1], SECBUFFER_EMPTY, 0, NULL);
        init_sec_buffer(&b[2], SECBUFFER_EMPTY, 0, NULL);
        init_sec_buffer(&b[3], SECBUFFER_EMPTY, 0, NULL);
        init_sec_buffer_desc(&desc, SECBUFFER_VERSION, 4, b);

        st = DecryptMessage(&s->sechandle, &desc, 0, NULL);
        if (st == SEC_E_INCOMPLETE_MESSAGE) return 0; /* partial packet, need more */
        if (st == SEC_I_CONTEXT_EXPIRED || st == SEC_E_CONTEXT_EXPIRED) {
            /* peer sent close_notify (INFO-level on modern, ERROR-level on old
             * platforms); hand out already-decrypted plaintext, then EOF */
            s->peer_closed = 1;
            if (b[1].pvBuffer && b[1].cbBuffer > 0 &&
                s->plain_len + (int)b[1].cbBuffer <= (int)sizeof(s->plain)) {
                memcpy(s->plain + s->plain_len, b[1].pvBuffer, (size_t)b[1].cbBuffer);
                s->plain_len += (int)b[1].cbBuffer;
            }
            s->in_len = 0;
            return 0;
        }
        if (st != SEC_E_OK) {
            printe("wintls: DecryptMessage 0x%x\n", st);
            return -1;
        }

        data_len = (int)b[1].cbBuffer;
        if (data_len > 0) {
            if (s->plain_len + data_len > (int)sizeof(s->plain)) return -1; /* plain full, caller drains */
            memcpy(s->plain + s->plain_len, b[1].pvBuffer, (size_t)data_len);
            s->plain_len += data_len;
        }
        extra = (b[3].BufferType == SECBUFFER_EXTRA) ? b[3].cbBuffer : 0;
        consumed = s->in_len - (int)extra;
        wintls_keep_extra(s, extra);
        /* no plaintext and no input consumed: cannot progress, stop (avoid loop) */
        if (data_len <= 0 && consumed <= 0) return 0;
    }
    return 0;
}

int hssl_read(hssl_t _ssl, void* buf, int len)
{
    struct wintls_s* s = (struct wintls_s*)_ssl;
    int n;

    if (!s || !buf || len <= 0) return 0;
    if (!s->sechandle_valid) return -1;

    for (;;) {
        /* 1) hand out buffered plaintext first (a recv may decrypt several) */
        if (s->plain_off < s->plain_len) {
            n = wintls_min(len, s->plain_len - s->plain_off);
            memcpy(buf, s->plain + s->plain_off, (size_t)n);
            s->plain_off += n;
            if (s->plain_off >= s->plain_len) { s->plain_off = 0; s->plain_len = 0; }
            return n;
        }
        /* 2) in may still hold an unfinished record; try decrypting */
        if (s->in_len > 0) {
            if (wintls_decrypt_pending(s) < 0) return -1;
            if (s->plain_off < s->plain_len) continue;
        }
        /* 3) close_notify seen with no leftover plaintext = clean TLS close */
        if (s->peer_closed) return 0;

        /* 4) receive more ciphertext; a full buffer means one record too big */
        if (s->in_len >= WINTLS_WIRE_BUF) return -1;
        n = wintls_recv(s->fd, s->in + s->in_len, WINTLS_WIRE_BUF - s->in_len);
        if (n == WINTLS_IO_AGAIN) return HSSL_WANT_READ;
        if (n == WINTLS_IO_EOF) return 0;      /* TCP FIN (EOF even without close_notify) */
        if (n == WINTLS_IO_ERR) return -1;
        s->in_len += n;
    }
}

int hssl_write(hssl_t _ssl, const void* buf, int len)
{
    struct wintls_s* s = (struct wintls_s*)_ssl;
    SecBuffer b[4];
    SecBufferDesc desc;
    SECURITY_STATUS st;
    int total, rc;

    if (!s || !buf || len <= 0) return 0;
    if (!s->sechandle_valid || !s->sizes_valid) return -1;

    /* 1) flush leftover ciphertext first: a partial write must be resumed in
     * order with the same buf (the caller retries on WANT_WRITE, no re-encrypt). */
    rc = wintls_flush_out(s);
    if (rc == -1) return HSSL_WANT_WRITE;
    if (rc == -2) return -1;

    /* 2) at most one record per call; the caller writes the rest again */
    if (len > (int)s->sizes.cbMaximumMessage) len = (int)s->sizes.cbMaximumMessage;
    total = (int)(s->sizes.cbHeader + (unsigned long)len + s->sizes.cbTrailer);
    if (total > WINTLS_WIRE_BUF) return -1;

    init_sec_buffer(&b[0], SECBUFFER_STREAM_HEADER, s->sizes.cbHeader, s->out);
    init_sec_buffer(&b[1], SECBUFFER_DATA, (unsigned long)len, s->out + s->sizes.cbHeader);
    init_sec_buffer(&b[2], SECBUFFER_STREAM_TRAILER, s->sizes.cbTrailer,
                    s->out + s->sizes.cbHeader + len);
    init_sec_buffer(&b[3], SECBUFFER_EMPTY, 0, NULL);
    init_sec_buffer_desc(&desc, SECBUFFER_VERSION, 4, b);
    memcpy(b[1].pvBuffer, buf, (size_t)len);

    st = EncryptMessage(&s->sechandle, 0, &desc, 0);
    if (st != SEC_E_OK) {
        printe("wintls: EncryptMessage 0x%x\n", st);
        return -1;
    }

    /* 3) send it; queue the remainder (flush first next time) and return the
     * encrypted plaintext bytes, matching OpenSSL SSL_write semantics. */
    s->out_len = (int)(b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer);
    s->out_sent = 0;
    rc = wintls_flush_out(s);
    if (rc == -2) return -1;
    return (int)b[1].cbBuffer;
}

/* ---- 7. close ---- */

int hssl_close(hssl_t _ssl)
{
    struct wintls_s* s = (struct wintls_s*)_ssl;
    SecBuffer token_buf;
    SecBufferDesc token_desc;
    SecBuffer out_buf[3];
    SecBufferDesc out_desc;
    ULONG attrs;
    TimeStamp lifetime;
    SECURITY_STATUS st;
    DWORD shutdown_type;

    if (!s || !s->sechandle_valid || s->shutdown_sent) return 0;
    s->shutdown_sent = 1;

    /* 1) tell Schannel we are closing, 2) step the state machine for the token */
    shutdown_type = SCHANNEL_SHUTDOWN;
    init_sec_buffer(&token_buf, SECBUFFER_TOKEN, sizeof(shutdown_type), &shutdown_type);
    init_sec_buffer_desc(&token_desc, SECBUFFER_VERSION, 1, &token_buf);
    if (ApplyControlToken(&s->sechandle, &token_desc) != SEC_E_OK) return 0;

    init_sec_buffer(&out_buf[0], SECBUFFER_TOKEN, 0, NULL);
    init_sec_buffer(&out_buf[1], SECBUFFER_ALERT, 0, NULL);
    init_sec_buffer(&out_buf[2], SECBUFFER_EMPTY, 0, NULL);
    init_sec_buffer_desc(&out_desc, SECBUFFER_VERSION, 3, out_buf);
    attrs = 0;
    memset(&lifetime, 0, sizeof(lifetime));

    if (s->is_server) {
        st = AcceptSecurityContext(&s->ctx->cred, &s->sechandle, NULL,
                                   WINTLS_ASC_REQ, 0, &s->sechandle,
                                   &out_desc, &attrs, &lifetime);
    } else {
        st = InitializeSecurityContext(&s->ctx->cred, &s->sechandle, s->sni,
                                       WINTLS_ISC_REQ, 0, 0, NULL, 0, &s->sechandle,
                                       &out_desc, &attrs, &lifetime);
    }
    /* best-effort send: no wait for confirmation, failures ignored */
    if (st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED || st == SEC_I_COMPLETE_NEEDED) {
        if (out_buf[0].pvBuffer && out_buf[0].cbBuffer > 0)
            (void)wintls_send(s->fd, (const unsigned char*)out_buf[0].pvBuffer,
                              (int)out_buf[0].cbBuffer);
    }
    free_sec_buffers(&out_desc);
    return 0;
}

/* ---- 8. public API: SNI / error mapping ---- */

int hssl_set_sni_hostname(hssl_t _ssl, const char* hostname)
{
    struct wintls_s* s = (struct wintls_s*)_ssl;
    char* dup;

    if (!s || !hostname) return -1;
    dup = wintls_strdup(hostname);
    if (!dup) return -1;
    if (s->sni) free(s->sni);
    s->sni = dup;
    return 0;
}

int hssl_get_error(hssl_t ssl, int ret)
{
    (void)ssl;
    /* This backend returns raw semantics from hssl_read/hssl_write:
     *   ret > 0                -> ok, ret bytes
     *   ret == 0               -> peer closed (close_notify or TCP FIN)
     *   ret == HSSL_WANT_READ  -> wait for readable
     *   ret == HSSL_WANT_WRITE -> wait for writable
     * Note: renegotiation (Read required during Write) is unsupported -> HSSL_ERROR. */
    if (ret > 0) return HSSL_OK;
    if (ret == 0) return HSSL_EOF;
    if (ret == HSSL_WANT_READ) return HSSL_WANT_READ;
    if (ret == HSSL_WANT_WRITE) return HSSL_WANT_WRITE;
    return HSSL_ERROR;
}

#endif /* WITH_WINTLS */
