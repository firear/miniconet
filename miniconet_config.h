/*
 * miniconet_config.h — fallback defaults for the feature switches (not a cmake
 * artifact). Only included when MINICONET_HAVE_GENERATED_CONFIG is undefined,
 * i.e. compiling straight against this source tree with -I. cmake builds use the
 * generated header.
 * Everything optional defaults to 0, so these may not match how the library was
 * actually built; use the `cmake --install` headers for an exact match.
 * Each macro is #ifndef-guarded, so -D overrides it.
 */
#ifndef MINICONET_CONFIG_H_
#define MINICONET_CONFIG_H_

#ifndef ENABLE_UDS
#define ENABLE_UDS 0
#endif

#ifndef MINICONET_WITH_IO_URING
#define MINICONET_WITH_IO_URING 0
#endif

#ifndef MINICONET_WITH_IOCP
#define MINICONET_WITH_IOCP 0
#endif

#ifndef MINICONET_WITH_WORKPOOL
#define MINICONET_WITH_WORKPOOL 0
#endif

#ifndef MINICONET_WITH_PROTOCOL_CLIENTS
#define MINICONET_WITH_PROTOCOL_CLIENTS 0
#endif
#ifndef MINICONET_WITH_HTTP_PARSER
#define MINICONET_WITH_HTTP_PARSER 0
#endif
#ifndef MINICONET_WITH_MQTT_PROTOCOL
#define MINICONET_WITH_MQTT_PROTOCOL 0
#endif
#ifndef MINICONET_WITH_MINIUPNP
#define MINICONET_WITH_MINIUPNP 0
#endif
#ifndef MINICONET_WITH_KCP
#define MINICONET_WITH_KCP 0
#endif
#ifndef MINICONET_WITH_CRYPTO_UTILS
#define MINICONET_WITH_CRYPTO_UTILS 0
#endif
#ifndef MINICONET_WITH_IDNA
#define MINICONET_WITH_IDNA 0
#endif
#ifndef MINICONET_WITH_WRAPPER_UTILS
#define MINICONET_WITH_WRAPPER_UTILS 0
#endif
#ifndef MINICONET_WITH_UPNP_CLIENT
#define MINICONET_WITH_UPNP_CLIENT 0
#endif

#ifndef MINICONET_TEST_HOOKS
#define MINICONET_TEST_HOOKS 0
#endif

/* Whether a real TLS backend is present (openssl/mbedtls/gnutls/wintls) */
#ifndef MINICONET_HAVE_SSL
#define MINICONET_HAVE_SSL 0
#endif

#endif /* MINICONET_CONFIG_H_ */
