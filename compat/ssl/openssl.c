#include "hssl.h"

#ifdef WITH_OPENSSL

#include "openssl/ssl.h"
#include "openssl/err.h"
#ifdef _MSC_VER
//#pragma comment(lib, "libssl.a")
//#pragma comment(lib, "libcrypto.a")
#endif

#define LOG_TAG "openssl"
#include "debug.h"
const char* hssl_backend() {
    return "openssl";
}


static void setopt(SSL_CTX *sslctx)
{
    SSL_CTX_set_options(sslctx, SSL_OP_ALL);
#ifdef SSL_OP_TLS_ROLLBACK_BUG
       SSL_CTX_set_options(sslctx, SSL_OP_TLS_ROLLBACK_BUG);
#endif /* SSL_OP_TLS_ROLLBACK_BUG */
#ifdef SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION
       SSL_CTX_set_options(sslctx, SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION);
#endif /* SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION */
#ifdef SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS
       SSL_CTX_set_options(sslctx, SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS);
#endif /* SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS */
#ifdef SSL_OP_NO_TICKET
       SSL_CTX_set_options(sslctx, SSL_OP_NO_TICKET);
#endif /* SSL_OP_NO_TICKET */

}
static int verify_callback(int preverify_ok, X509_STORE_CTX* x509_store_ctx)
{
    int error_code = preverify_ok ? X509_V_OK : X509_STORE_CTX_get_error(x509_store_ctx);
    if (error_code != X509_V_OK) {
        const char* error_string = X509_verify_cert_error_string(error_code);
        fprintf(stderr, "verify_callback failed[%d]: %s\n", error_code, error_string);
    }
    return preverify_ok;
}
hssl_ctx_t hssl_ctx_new(hssl_ctx_opt_t* param) {
    static int s_initialized = 0;
    const SSL_METHOD* method;
    if (s_initialized == 0) {
#if OPENSSL_VERSION_NUMBER < 0x10100000L
        SSL_library_init();
        SSL_load_error_strings();
#else
        OPENSSL_init_ssl(OPENSSL_INIT_SSL_DEFAULT, NULL);
#endif
        s_initialized = 1;
    }
    if (param && param->endpoint) {
        method = param->endpoint == HSSL_CLIENT ? SSLv23_client_method() : SSLv23_server_method();
    } else {
        method = SSLv23_method();
    }
    SSL_CTX* ctx = SSL_CTX_new(method);
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
	/* If the security level of OpenSSL is set to 2+ in system configuration, 
	 * our forged certificates with 1024-bit RSA key size will be rejected */
	SSL_CTX_set_security_level(ctx, 1);
#endif /* OPENSSL_VERSION_NUMBER >= 0x10100000L */
    if (ctx == NULL) return NULL;
    // setopt(ctx);
    int mode = SSL_VERIFY_NONE;
    const char* ca_file = NULL;
    const char* ca_path = NULL;
    if (param) {
        if (!param->endpoint || param->endpoint == HSSL_CLIENT) {
            if (param->ca_file && *param->ca_file) {
                ca_file = param->ca_file;
            }
            if (param->ca_path && *param->ca_path) {
                ca_path = param->ca_path;
            }
            if (ca_file || ca_path) {
                if (!SSL_CTX_load_verify_locations(ctx, ca_file, ca_path)) {
                    fprintf(stderr, "ssl ca_file/ca_path failed!\n");
                    goto error;
                }
            }
        }
        if (!param->endpoint || param->endpoint == HSSL_SERVER) {
            if (param->crt_file && *param->crt_file) {
                if (!SSL_CTX_use_certificate_chain_file(ctx, param->crt_file)) {
                    fprintf(stderr, "ssl crt_file error!\n");
                    goto error;
                }
            }

            if (param->key_file && *param->key_file) {
                if (!SSL_CTX_use_PrivateKey_file(ctx, param->key_file, SSL_FILETYPE_PEM)) {
                    fprintf(stderr, "ssl key_file error!\n");
                    goto error;
                }
                if (!SSL_CTX_check_private_key(ctx)) {
                    fprintf(stderr, "ssl key_file check failed!\n");
                    goto error;
                }
            }
        }
        if (param->verify_peer) {
            mode = SSL_VERIFY_PEER;
            if (param->endpoint == HSSL_SERVER) {
                mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
            }
        }
    }
    if (mode == SSL_VERIFY_PEER && !ca_file && !ca_path) {
        SSL_CTX_set_default_verify_paths(ctx);
    }

#ifdef SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER
    SSL_CTX_set_mode(ctx, SSL_CTX_get_mode(ctx) | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
#endif
    SSL_CTX_set_verify(ctx, mode, verify_callback);
    return ctx;
error:
    SSL_CTX_free(ctx);
    return NULL;
}

void hssl_ctx_free(hssl_ctx_t ssl_ctx) {
    if (!ssl_ctx) return;
    SSL_CTX_free((SSL_CTX*)ssl_ctx);
}

hssl_t hssl_new(hssl_ctx_t ssl_ctx, int fd, uint8_t is_server) {
    SSL* ssl = SSL_new((SSL_CTX*)ssl_ctx);
    if (ssl == NULL) return NULL;    
    if(fd == -1){
        BIO* read_bio = BIO_new(BIO_s_mem());
        BIO* write_bio = BIO_new(BIO_s_mem());
        SSL_set_bio(ssl, read_bio, write_bio);
    }else{
        SSL_set_fd(ssl, fd);
    }
    is_server ? SSL_set_accept_state(ssl) : SSL_set_connect_state(ssl);
    return ssl;
}

void hssl_free(hssl_t ssl) {
    if (ssl) {
        SSL_free((SSL*)ssl);
        ssl = NULL;
    }
}

int hssl_accept(hssl_t ssl) {
    int ret = SSL_accept((SSL*)ssl);
    if (ret == 1) return 0;

    int err = SSL_get_error((SSL*)ssl, ret);
    if (err == SSL_ERROR_WANT_READ) {
        return HSSL_WANT_READ;
    }
    else if (err == SSL_ERROR_WANT_WRITE) {
        return HSSL_WANT_WRITE;
    }
    else if (err == SSL_ERROR_SYSCALL) {
        LOGE("SSL_accept errno:%d,error:%s", errno, strerror(errno));
    }
    else {
        LOGE("SSL_accept: %s", ERR_error_string(ERR_get_error(), NULL));
    }
    return err;
}

int hssl_connect(hssl_t ssl) {
    int ret = SSL_connect((SSL*)ssl);
    if (ret == 1) return 0;

    int err = SSL_get_error((SSL*)ssl, ret);
    if (err == SSL_ERROR_WANT_READ) {
        return HSSL_WANT_READ;
    }
    else if (err == SSL_ERROR_WANT_WRITE) {
        return HSSL_WANT_WRITE;
    }
    else if (err == SSL_ERROR_SYSCALL) {
        LOGE("SSL_connect errno:%d,error:%s", errno, strerror(errno));
    }
    else {
        LOGE("SSL_connect: %s", ERR_error_string(ERR_get_error(), NULL));
    }
    return err;
}

int hssl_read(hssl_t ssl, void* buf, int len) {
    return SSL_read((SSL*)ssl, buf, len);
}

int hssl_write(hssl_t ssl, const void* buf, int len) {
    return SSL_write((SSL*)ssl, buf, len);
}

int hssl_get_error(hssl_t ssl, int ret) {
    int err;
    if (ret > 0) return HSSL_OK;
    err = SSL_get_error((SSL*)ssl, ret);
    switch (err) {
        case SSL_ERROR_WANT_READ:  return HSSL_WANT_READ;
        case SSL_ERROR_WANT_WRITE: return HSSL_WANT_WRITE;
        case SSL_ERROR_ZERO_RETURN: return HSSL_EOF; /* peer sent close_notify */
        case SSL_ERROR_SYSCALL:
            /* errno==0 usually means the peer dropped the TCP connection without a
             * close_notify; treat as EOF to match mco_read (0 == EOF). */
            if (errno == 0) return HSSL_EOF;
            LOGE("SSL I/O errno:%d,error:%s", errno, strerror(errno));
            return HSSL_ERROR;
        default:
            LOGE("SSL I/O: %s", ERR_error_string(ERR_get_error(), NULL));
            return HSSL_ERROR;
    }
}

int hssl_close(hssl_t ssl) {
    SSL_shutdown((SSL*)ssl);
    return 0;
}

int hssl_set_sni_hostname(hssl_t ssl, const char* hostname) {
#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
    SSL_set_tlsext_host_name((SSL*)ssl, hostname);
#endif
    return 0;
}

#ifdef TLSEXT_TYPE_application_layer_protocol_negotiation
static int hssl_ctx_alpn_select_cb(SSL *ssl,
    const unsigned char **out, unsigned char *outlen,
    const unsigned char *in, unsigned int inlen,
    void *arg) {
    const unsigned char* protos = (unsigned char*)arg;
    unsigned int protos_len = strlen((char*)protos);
    // printf("hssl_ctx_alpn_select_cb(in=%*.s:%u out=%.*s:%u protos=%.*s:%u)\n", inlen, in, inlen, (int)*outlen, (char*)out, (int)*outlen, protos_len, protos, protos_len);
    if (SSL_select_next_proto((unsigned char **) out, outlen, protos, protos_len, in, inlen) != OPENSSL_NPN_NEGOTIATED) {
        fprintf(stderr, "SSL_select_next_proto failed!\n");
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    }
    // printf("SSL_select_next_proto(out=%.*s:%u)\n", (int)*outlen, (char*)*out, (int)*outlen);
    return SSL_TLSEXT_ERR_OK;
}
#endif

int hssl_ctx_set_alpn_protos(hssl_ctx_t ssl_ctx, const unsigned char* protos, unsigned int protos_len) {
    int ret = -1;
    // printf("hssl_ctx_set_alpn_protos(%.*s:%u)\n", protos_len, protos, protos_len);
#ifdef TLSEXT_TYPE_application_layer_protocol_negotiation
    // for HSSL_CLIENT
    // ret = SSL_CTX_set_alpn_protos((SSL_CTX*)ssl_ctx, (const unsigned char*)protos, protos_len);

    // for HSSL_SERVER
    SSL_CTX_set_alpn_select_cb((SSL_CTX*)ssl_ctx, hssl_ctx_alpn_select_cb, (void*)protos);
    ret = 0;
#endif
    return ret;
}

#endif // WITH_OPENSSL
