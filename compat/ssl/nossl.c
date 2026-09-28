#include "hssl.h"

#ifdef HV_WITHOUT_SSL

const char* hssl_backend() {
    return "nossl";
}

hssl_ctx_t hssl_ctx_new(hssl_ctx_opt_t* opt) {
    fprintf(stderr, "Please recompile WITH_SSL.\n");
    return NULL;
}

void hssl_ctx_free(hssl_ctx_t ssl_ctx) {
}

hssl_t hssl_new(hssl_ctx_t ssl_ctx, int fd, uint8_t is_server) {
    return (void*)(intptr_t)fd;
}

void hssl_free(hssl_t ssl) {
}

int hssl_accept(hssl_t ssl) {
    return 0;
}

int hssl_connect(hssl_t ssl) {
    return 0;
}

int hssl_read(hssl_t ssl, void* buf, int len) {
    int fd = (intptr_t)ssl;
    return read(fd, buf, len);
}

int hssl_write(hssl_t ssl, const void* buf, int len) {
    int fd = (intptr_t)ssl;
    return write(fd, buf, len);
}

int hssl_get_error(hssl_t ssl, int ret) {
    /* Under nossl, hssl_read/write are plain read()/write(), translated by raw
     * fd errno. Callers must not use nossl for real encryption: mco_ssl_ctx_new_*
     * rejects "nossl" rather than silently downgrading TLS to plaintext. This
     * keeps the API complete and compilable across backends. */
    (void)ssl;
    if (ret > 0) return HSSL_OK;
    if (ret == 0) return HSSL_EOF;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return HSSL_WANT_READ;
    return HSSL_ERROR;
}

int hssl_close(hssl_t ssl) {
    return 0;
}

int hssl_set_sni_hostname(hssl_t ssl, const char* hostname) {
    return 0;
}

#endif // HV_WITHOUT_SSL
