#include "hsocket.h"

#include "hdef.h"

#ifdef OS_WIN
#include "hatomic.h"
static hatomic_flag_t s_wsa_initialized = HATOMIC_FLAG_INIT;
void WSAInit() {
    if (!hatomic_flag_test_and_set(&s_wsa_initialized)) {
        WSADATA wsadata;
        WSAStartup(MAKEWORD(2, 2), &wsadata);
    }
}

void WSADeinit() {
    if (hatomic_flag_test_and_set(&s_wsa_initialized)) {
        hatomic_flag_clear(&s_wsa_initialized);
        WSACleanup();
    }
}
#else
#define pgetaddrinfo getaddrinfo
#define pfreeaddrinfo freeaddrinfo
#endif

static inline int socket_errno_negative(int sockfd) {
    int err = socket_errno();
    if (sockfd >= 0) closesocket(sockfd);
    return err > 0 ? -err : -1;
}

const char* socket_strerror(int err) {
#ifdef OS_WIN
    static char buffer[128];

    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS |
        FORMAT_MESSAGE_MAX_WIDTH_MASK,
        0, ABS(err), 0, buffer, sizeof(buffer), NULL);

    return buffer;
#else
    return strerror(ABS(err));
#endif
}

#if defined(_MSC_VER) && _MSC_VER < 1400 // VS2005
static int inet_ntop4(const unsigned char *src, char *dst, size_t size);
static int inet_ntop6(const unsigned char *src, char *dst, size_t size);
static int inet_pton4(const char *src, unsigned char *dst);
static int inet_pton6(const char *src, unsigned char *dst);

static int inet_ntop4(const unsigned char *src, char *dst, size_t size) {
  static const char fmt[] = "%u.%u.%u.%u";
  char tmp[SOCKADDR_STRLEN];
  int l;

  l = _snprintf(tmp, sizeof(tmp), fmt, src[0], src[1], src[2], src[3]);
  if (l <= 0 || (size_t) l >= size) {
    return ENOSPC;
  }
  strncpy(dst, tmp, size);
  dst[size - 1] = '\0';
  return 0;
}

static int inet_ntop6(const unsigned char *src, char *dst, size_t size) {
  /*
   * Note that int32_t and int16_t need only be "at least" large enough
   * to contain a value of the specified size.  On some systems, like
   * Crays, there is no such thing as an integer variable with 16 bits.
   * Keep this in mind if you think this function should have been coded
   * to use pointer overlays.  All the world's not a VAX.
   */
  char tmp[SOCKADDR_STRLEN], *tp;
  struct { int base, len; } best, cur;
  unsigned int words[sizeof(struct in6_addr) / sizeof(uint16_t)];
  int i;

  /*
   * Preprocess:
   *  Copy the input (bytewise) array into a wordwise array.
   *  Find the longest run of 0x00's in src[] for :: shorthanding.
   */
  memset(words, '\0', sizeof words);
  for (i = 0; i < (int) sizeof(struct in6_addr); i++)
    words[i / 2] |= (src[i] << ((1 - (i % 2)) << 3));
  best.base = -1;
  best.len = 0;
  cur.base = -1;
  cur.len = 0;
  for (i = 0; i < (int) ARRAY_SIZE(words); i++) {
    if (words[i] == 0) {
      if (cur.base == -1)
        cur.base = i, cur.len = 1;
      else
        cur.len++;
    } else {
      if (cur.base != -1) {
        if (best.base == -1 || cur.len > best.len)
          best = cur;
        cur.base = -1;
      }
    }
  }
  if (cur.base != -1) {
    if (best.base == -1 || cur.len > best.len)
      best = cur;
  }
  if (best.base != -1 && best.len < 2)
    best.base = -1;

  /*
   * Format the result.
   */
  tp = tmp;
  for (i = 0; i < (int) ARRAY_SIZE(words); i++) {
    /* Are we inside the best run of 0x00's? */
    if (best.base != -1 && i >= best.base &&
        i < (best.base + best.len)) {
      if (i == best.base)
        *tp++ = ':';
      continue;
    }
    /* Are we following an initial run of 0x00s or any real hex? */
    if (i != 0)
      *tp++ = ':';
    /* Is this address an encapsulated IPv4? */
    if (i == 6 && best.base == 0 && (best.len == 6 ||
        (best.len == 7 && words[7] != 0x0001) ||
        (best.len == 5 && words[5] == 0xffff))) {
      int err = inet_ntop4(src+12, tp, sizeof tmp - (tp - tmp));
      if (err)
        return err;
      tp += strlen(tp);
      break;
    }
    tp += sprintf(tp, "%x", words[i]);
  }
  /* Was it a trailing run of 0x00's? */
  if (best.base != -1 && (best.base + best.len) == ARRAY_SIZE(words))
    *tp++ = ':';
  *tp++ = '\0';

  /*
   * Check for overflow, copy, and we're done.
   */
  if ((size_t)(tp - tmp) > size) {
    return ENOSPC;
  }
  strcpy(dst, tmp);
  return 0;
}


static int inet_pton4(const char *src, unsigned char *dst) {
  static const char digits[] = "0123456789";
  int saw_digit, octets, ch;
  unsigned char tmp[sizeof(struct in_addr)], *tp;

  saw_digit = 0;
  octets = 0;
  *(tp = tmp) = 0;
  while ((ch = *src++) != '\0') {
    const char *pch;

    if ((pch = strchr(digits, ch)) != NULL) {
      unsigned int nw = *tp * 10 + (pch - digits);

      if (saw_digit && *tp == 0)
        return EINVAL;
      if (nw > 255)
        return EINVAL;
      *tp = nw;
      if (!saw_digit) {
        if (++octets > 4)
          return EINVAL;
        saw_digit = 1;
      }
    } else if (ch == '.' && saw_digit) {
      if (octets == 4)
        return EINVAL;
      *++tp = 0;
      saw_digit = 0;
    } else
      return EINVAL;
  }
  if (octets < 4)
    return EINVAL;
  memcpy(dst, tmp, sizeof(struct in_addr));
  return 0;
}


static int inet_pton6(const char *src, unsigned char *dst) {
  static const char xdigits_l[] = "0123456789abcdef",
                    xdigits_u[] = "0123456789ABCDEF";
  unsigned char tmp[sizeof(struct in6_addr)], *tp, *endp, *colonp;
  const char *xdigits, *curtok;
  int ch, seen_xdigits;
  unsigned int val;

  memset((tp = tmp), '\0', sizeof tmp);
  endp = tp + sizeof tmp;
  colonp = NULL;
  /* Leading :: requires some special handling. */
  if (*src == ':')
    if (*++src != ':')
      return EINVAL;
  curtok = src;
  seen_xdigits = 0;
  val = 0;
  while ((ch = *src++) != '\0') {
    const char *pch;

    if ((pch = strchr((xdigits = xdigits_l), ch)) == NULL)
      pch = strchr((xdigits = xdigits_u), ch);
    if (pch != NULL) {
      val <<= 4;
      val |= (pch - xdigits);
      if (++seen_xdigits > 4)
        return EINVAL;
      continue;
    }
    if (ch == ':') {
      curtok = src;
      if (!seen_xdigits) {
        if (colonp)
          return EINVAL;
        colonp = tp;
        continue;
      } else if (*src == '\0') {
        return EINVAL;
      }
      if (tp + sizeof(uint16_t) > endp)
        return EINVAL;
      *tp++ = (unsigned char) (val >> 8) & 0xff;
      *tp++ = (unsigned char) val & 0xff;
      seen_xdigits = 0;
      val = 0;
      continue;
    }
    if (ch == '.' && ((tp + sizeof(struct in_addr)) <= endp)) {
      int err = inet_pton4(curtok, tp);
      if (err == 0) {
        tp += sizeof(struct in_addr);
        seen_xdigits = 0;
        break;  /*%< '\\0' was seen by inet_pton4(). */
      }
    }
    return EINVAL;
  }
  if (seen_xdigits) {
    if (tp + sizeof(uint16_t) > endp)
      return EINVAL;
    *tp++ = (unsigned char) (val >> 8) & 0xff;
    *tp++ = (unsigned char) val & 0xff;
  }
  if (colonp != NULL) {
    /*
     * Since some memmove()'s erroneously fail to handle
     * overlapping regions, we'll do the shift by hand.
     */
    const int n = tp - colonp;
    int i;

    if (tp == endp)
      return EINVAL;
    for (i = 1; i <= n; i++) {
      endp[- i] = colonp[n - i];
      colonp[n - i] = 0;
    }
    tp = endp;
  }
  if (tp != endp)
    return EINVAL;
  memcpy(dst, tmp, sizeof tmp);
  return 0;
}

const char * inet_ntop(int family, void*src, char *dst, size_t size)
{
    switch (family)
    {
    case AF_INET:
        inet_ntop4(src, dst, size);
        break;
    case AF_INET6:
        inet_ntop6(src, dst, size);
        break;
    default:
        break;
    }
    return dst;
}

int inet_pton(int family, const char *host, void*addr)
{
    switch (family)
    {
    case AF_INET:
        return inet_pton4(host, (unsigned char *)addr) == 0;
        break;
    case AF_INET6:
        return inet_pton6(host, (unsigned char *)addr) == 0;
        break;
    default:
        break;
    }
    return 0;
}
#endif

bool is_ipv4(const char* host) {
    struct sockaddr_in sin;
    return inet_pton(AF_INET, host, &sin) == 1;
}

bool is_ipv6(const char* host) {
    struct sockaddr_in6 sin6;
    return inet_pton(AF_INET6, host, &sin6) == 1;
}

int ResolveAddr(const char* host, sockaddr_u* addr) {
#ifdef OS_WIN
    WSAInit();
#endif
    if (inet_pton(AF_INET, host, &addr->sin.sin_addr) == 1) {
        addr->sa.sa_family = AF_INET; // host is ipv4, so easy ;)
        return 0;
    }

    if (inet_pton(AF_INET6, host, &addr->sin6.sin6_addr) == 1) {
        addr->sa.sa_family = AF_INET6; // host is ipv6
        return 0;
    }
    {
        struct addrinfo* ais = NULL;
        int ret = pgetaddrinfo(host, NULL, NULL, &ais);
        if (ret != 0 || ais == NULL || ais->ai_addr == NULL || ais->ai_addrlen == 0) {
            printd("unknown host: %s err:%d:%s\n", host, ret, socket_strerror(ret));
            return ret;
        }else{
            struct addrinfo* pai = ais;
            while (pai != NULL) {
                if (pai->ai_family == AF_INET) break;
                pai = pai->ai_next;
            }
            if (pai == NULL) pai = ais;
            memcpy(addr, pai->ai_addr, pai->ai_addrlen);
            pfreeaddrinfo(ais);
            return 0;
        }
    }
}

const char* sockaddr_ip(sockaddr_u* addr, char *ip, int len) {
    if (addr->sa.sa_family == AF_INET) {
        return inet_ntop(AF_INET, &addr->sin.sin_addr, ip, len);
    }
    else if (addr->sa.sa_family == AF_INET6) {
        return inet_ntop(AF_INET6, &addr->sin6.sin6_addr, ip, len);
    }
    return ip;
}

uint16_t sockaddr_port(sockaddr_u* addr) {
    uint16_t port = 0;
    if (addr->sa.sa_family == AF_INET) {
        port = ntohs(addr->sin.sin_port);
    }
    else if (addr->sa.sa_family == AF_INET6) {
        port = ntohs(addr->sin6.sin6_port);
    }
    return port;
}

int sockaddr_set_ip(sockaddr_u* addr, const char* host) {
    if (!host || *host == '\0') {
        addr->sin.sin_family = AF_INET;
        addr->sin.sin_addr.s_addr = htonl(INADDR_ANY);
        return 0;
    }
    return ResolveAddr(host, addr);
}

void sockaddr_set_port(sockaddr_u* addr, int port) {
    if (addr->sa.sa_family == AF_INET) {
        addr->sin.sin_port = htons(port);
    }
    else if (addr->sa.sa_family == AF_INET6) {
        addr->sin6.sin6_port = htons(port);
    }
}

int sockaddr_set_ipport(sockaddr_u* addr, const char* host, int port) {
#if ENABLE_UDS
    if (port < 0) {
        sockaddr_set_path(addr, host);
        return 0;
    }
#endif
    int ret = sockaddr_set_ip(addr, host);
    if (ret != 0) return ret;
    sockaddr_set_port(addr, port);
    // SOCKADDR_PRINT(addr);
    return 0;
}

socklen_t sockaddr_len(sockaddr_u* addr) {
    if (addr->sa.sa_family == AF_INET) {
        return sizeof(struct sockaddr_in);
    }
    else if (addr->sa.sa_family == AF_INET6) {
        return sizeof(struct sockaddr_in6);
    }
#if ENABLE_UDS
    else if (addr->sa.sa_family == AF_UNIX) {
        return sizeof(struct sockaddr_un);
    }
#endif
    return sizeof(sockaddr_u);
}

const char* sockaddr_str(sockaddr_u* addr, char* buf, int len) {
    char ip[SOCKADDR_STRLEN] = {0};
    uint16_t port = 0;
    if (addr->sa.sa_family == AF_INET) {
        inet_ntop(AF_INET, &addr->sin.sin_addr, ip, len);
        port = ntohs(addr->sin.sin_port);
        snprintf(buf, len, "%s:%d", ip, port);
    }
    else if (addr->sa.sa_family == AF_INET6) {
        inet_ntop(AF_INET6, &addr->sin6.sin6_addr, ip, len);
        port = ntohs(addr->sin6.sin6_port);
        snprintf(buf, len, "[%s]:%d", ip, port);
    }
#if ENABLE_UDS
    else if (addr->sa.sa_family == AF_UNIX) {
        snprintf(buf, len, "%s", addr->sun.sun_path);
    }
#endif
    return buf;
}

int sockaddr_compare(const sockaddr_u* addr1, const sockaddr_u* addr2) {
    if (addr1->sa.sa_family != addr2->sa.sa_family)
        return addr1->sa.sa_family - addr2->sa.sa_family;
    if (addr1->sa.sa_family == AF_INET) {
        if (addr1->sin.sin_family != addr2->sin.sin_family)
            return addr1->sin.sin_family - addr2->sin.sin_family;
        if (addr1->sin.sin_port != addr2->sin.sin_port)
            return addr1->sin.sin_port - addr2->sin.sin_port;
        return memcmp(&addr1->sin.sin_addr, &addr2->sin.sin_addr, sizeof(struct in_addr));
    }
    else if (addr1->sa.sa_family == AF_INET6) {
        if (addr1->sin6.sin6_family != addr2->sin6.sin6_family)
            return addr1->sin6.sin6_family - addr2->sin6.sin6_family;
        if (addr1->sin6.sin6_port != addr2->sin6.sin6_port)
            return addr1->sin6.sin6_port - addr2->sin6.sin6_port;
        return memcmp(&addr1->sin6.sin6_addr, &addr2->sin6.sin6_addr, sizeof(struct in_addr));
    }
    return memcmp(addr1, addr2, sizeof(sockaddr_u));
}

static int sockaddr_bind(sockaddr_u* localaddr, int type) {
    // socket -> setsockopt -> bind
#ifdef SOCK_CLOEXEC
    type |= SOCK_CLOEXEC;
#endif
    int sockfd = hsocket(localaddr->sa.sa_family, type, 0);
    if (sockfd < 0) {
        perror("socket");
        goto error;
    }

#ifdef OS_UNIX
    so_reuseaddr(sockfd, 1);
    // so_reuseport(sockfd, 1);
#endif

    if (localaddr->sa.sa_family == AF_INET6) {
        ip_v6only(sockfd, 0);
    }

    if (bind(sockfd, &localaddr->sa, sockaddr_len(localaddr)) < 0) {
        perror("bind");
        goto error;
    }

    return sockfd;
error:
    return socket_errno_negative(sockfd);
}

static int sockaddr_connect(sockaddr_u* peeraddr, int nonblock) {
    // socket -> nonblocking -> connect
    int ret = 0;
    int connfd = hsocket(peeraddr->sa.sa_family, SOCK_STREAM, 0);
    if (connfd < 0) {
        perror("socket");
        goto error;
    }

    if (nonblock) {
        nonblocking(connfd);
    }

    ret = connect(connfd, &peeraddr->sa, sockaddr_len(peeraddr));
#ifdef OS_WIN
    if (ret < 0 && socket_errno() != WSAEWOULDBLOCK) {
#else
    if (ret < 0 && socket_errno() != EINPROGRESS) {
#endif
        // perror("connect");
        goto error;
    }

    return connfd;
error:
    return socket_errno_negative(connfd);
}

static int ListenFD(int sockfd) {
    if (sockfd < 0) return sockfd;
    if (listen(sockfd, SOMAXCONN) < 0) {
        perror("listen");
        return socket_errno_negative(sockfd);
    }
    return sockfd;
}

static int ConnectFDTimeout(int connfd, int ms) {
    int err,ret;
    socklen_t optlen = sizeof(err);
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    fd_set writefds;
    FD_ZERO(&writefds);
    FD_SET(connfd, &writefds);
    ret = select(connfd+1, 0, &writefds, 0, &tv);
    if (ret < 0) {
        perror("select");
        goto error;
    }
    if (ret == 0) {
        errno = ETIMEDOUT;
        goto error;
    }
    if (getsockopt(connfd, SOL_SOCKET, SO_ERROR, (char*)&err, &optlen) < 0 || err != 0) {
        if (err != 0) errno = err;
        goto error;
    }
    blocking(connfd);
    return connfd;
error:
    return socket_errno_negative(connfd);
}

int Bind(int port, const char* host, int type) {
    sockaddr_u localaddr;
    int ret;
#ifdef OS_WIN
    WSAInit();
#endif
    memset(&localaddr, 0, sizeof(localaddr));
    ret = sockaddr_set_ipport(&localaddr, host, port);
    if (ret != 0) {
        return NABS(ret);
    }
    return sockaddr_bind(&localaddr, type);
}

int Listen(int port, const char* host) {
    int sockfd = Bind(port, host, SOCK_STREAM);
    if (sockfd < 0) return sockfd;
    return ListenFD(sockfd);
}

int Connect(const char* host, int port, int nonblock) {
    int ret;
    sockaddr_u peeraddr;
#ifdef OS_WIN
    WSAInit();
#endif
    memset(&peeraddr, 0, sizeof(peeraddr));
    ret = sockaddr_set_ipport(&peeraddr, host, port);
    if (ret != 0) {
        return NABS(ret);
    }
    return sockaddr_connect(&peeraddr, nonblock);
}

int ConnectNonblock(const char* host, int port) {
    return Connect(host, port, 1);
}

int ConnectTimeout(const char* host, int port, int ms) {
    int connfd = Connect(host, port, 1);
    if (connfd < 0) return connfd;
    return ConnectFDTimeout(connfd, ms);
}

#if ENABLE_UDS
int BindUnix(const char* path, int type) {
    sockaddr_u localaddr;
    memset(&localaddr, 0, sizeof(localaddr));
    sockaddr_set_path(&localaddr, path);
    return sockaddr_bind(&localaddr, type);
}

int ListenUnix(const char* path) {
    int sockfd = BindUnix(path, SOCK_STREAM);
    if (sockfd < 0) return sockfd;
    return ListenFD(sockfd);
}

int ConnectUnix(const char* path, int nonblock) {
    sockaddr_u peeraddr;
    memset(&peeraddr, 0, sizeof(peeraddr));
    sockaddr_set_path(&peeraddr, path);
    return sockaddr_connect(&peeraddr, nonblock);
}

int ConnectUnixNonblock(const char* path) {
    return ConnectUnix(path, 1);
}

int ConnectUnixTimeout(const char* path, int ms) {
    int connfd = ConnectUnix(path, 1);
    if (connfd < 0) return connfd;
    return ConnectFDTimeout(connfd, ms);
}
#endif

int Socketpair(int family, int type, int protocol, int sv[2]) {
#if defined(OS_UNIX) && HAVE_SOCKETPAIR
    return socketpair(AF_LOCAL, type, protocol, sv);
#endif
    int listenfd, connfd, acceptfd;
    struct sockaddr_in localaddr;
    socklen_t addrlen;
    /* Platforms without a native socketpair (Windows) emulate it with a loopback
     * TCP pair. AF_UNIX/AF_LOCAL requests are accepted too: the caller just wants
     * a pair of bidirectional streams, address family aside. */
    if (type != SOCK_STREAM) return -1;
#if defined(AF_UNIX)
    if (family != AF_INET && family != AF_UNIX) return -1;
#else
    if (family != AF_INET) return -1;
#endif
#ifdef OS_WIN
    WSAInit();
#endif
    listenfd = connfd = acceptfd = -1;
    addrlen = sizeof(localaddr);
    memset(&localaddr, 0, addrlen);
    localaddr.sin_family = AF_INET;
    localaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    localaddr.sin_port = 0;
    // listener
    listenfd = hsocket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        perror("socket");
        goto error;
    }
    if (bind(listenfd, (struct sockaddr*)&localaddr, addrlen) < 0) {
        perror("bind");
        goto error;
    }
    if (listen(listenfd, 1) < 0) {
        perror("listen");
        goto error;
    }
    if (getsockname(listenfd, (struct sockaddr*)&localaddr, &addrlen) < 0) {
        perror("getsockname");
        goto error;
    }
    // connector
    connfd = hsocket(AF_INET, SOCK_STREAM, 0);
    if (connfd < 0) {
        perror("socket");
        goto error;
    }
    if (connect(connfd, (struct sockaddr*)&localaddr, addrlen) < 0) {
        perror("connect");
        goto error;
    }
    // acceptor
    acceptfd = accept(listenfd, (struct sockaddr*)&localaddr, &addrlen);
    if (acceptfd < 0) {
        perror("accept");
        goto error;
    }

    closesocket(listenfd);
    sv[0] = connfd;
    sv[1] = acceptfd;
    return 0;
error:
    if (listenfd != -1) {
        closesocket(listenfd);
    }
    if (connfd != -1) {
        closesocket(connfd);
    }
    if (acceptfd != -1) {
        closesocket(acceptfd);
    }
    return -1;
}
