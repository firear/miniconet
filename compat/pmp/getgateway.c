/* getgateway.c — 默认网关探测（vendor 自 libnatpmp 1.x getgateway.c，
 * Copyright (c) 2007-2014 Thomas BERNARD，BSD 授权）。
 * 与上游的差异：平台分支由 hplatform.h 的 OS_* 宏驱动（上游用
 * __linux__/WIN32 等编译器宏，MSVC 下 WIN32 未必有定义）；Windows 走
 * GetBestRoute（上游 USE_WIN32_CODE_2），VC6 无 MIB_IPFORWARDROW 声明时
 * 退化为失败（上层启发式兜底）；删去 Cygwin 注册表 / Haiku 分支。 */
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

#include "hplatform.h"
#include "getgateway.h"

/* 专用宏名：避免与外部 SUCCESS/FAILED 定义相撞或被其覆盖 */
#define GETGW_OK      (0)
#define GETGW_FAILED  (-1)

/* There is no portable method to get the default route gateway.
 * Parsing /proc/net/route is for linux.
 * sysctl is the way to access such informations on macOS.
 * BSD systems should provide route information through raw PF_ROUTE sockets.
 * In MS Windows, default gateway is found by using GetBestRoute(). */
#ifdef OS_LINUX
#define USE_PROC_NET_ROUTE
#endif

#if defined(OS_BSD) || defined(__FreeBSD_kernel__)
#define USE_SOCKET_ROUTE
#endif

#ifdef OS_MAC
#define USE_SYSCTL_NET_ROUTE
#endif

#ifdef OS_SOLARIS
#define USE_SOCKET_ROUTE
#endif

#ifdef OS_WIN
#define USE_WIN32_CODE_2
#endif

#ifdef USE_SYSCTL_NET_ROUTE
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <net/route.h>
#endif
#ifdef USE_SOCKET_ROUTE
#include <unistd.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/route.h>
#endif
#ifdef USE_WIN32_CODE_2
#include <iphlpapi.h>
#endif

#ifdef USE_PROC_NET_ROUTE
/*
 parse /proc/net/route which is as follow :

Iface   Destination     Gateway         Flags   RefCnt  Use     Metric  Mask            MTU     Window  IRTT
wlan0   0001A8C0        00000000        0001    0       0       0       00FFFFFF        0       0       0
eth0    0000FEA9        00000000        0001    0       0       0       0000FFFF        0       0       0
wlan0   00000000        0101A8C0        0003    0       0       0       00000000        0       0       0
eth0    00000000        00000000        0001    0       0       1000    00000000        0       0       0

 One header line, and then one line by route by route table entry.
*/
int getdefaultgateway(in_addr_t* addr)
{
    unsigned long d, g;
    char buf[256];
    int line = 0;
    FILE* f;
    char* p;
    f = fopen("/proc/net/route", "r");
    if (!f)
        return GETGW_FAILED;
    while (fgets(buf, sizeof(buf), f)) {
        if (line > 0) { /* skip the first line */
            p = buf;
            /* skip the interface name */
            while (*p && !isspace((unsigned char)*p))
                p++;
            while (*p && isspace((unsigned char)*p))
                p++;
            if (sscanf(p, "%lx%lx", &d, &g) == 2) {
                if (d == 0 && g != 0) { /* default */
                    *addr = (in_addr_t)g;
                    fclose(f);
                    return GETGW_OK;
                }
            }
        }
        line++;
    }
    /* default route not found ! */
    fclose(f);
    return GETGW_FAILED;
}
#endif /* USE_PROC_NET_ROUTE */

#ifdef USE_SYSCTL_NET_ROUTE

#define ROUNDUP(a) \
    ((a) > 0 ? (1 + (((a) - 1) | (sizeof(long) - 1))) : sizeof(long))

int getdefaultgateway(in_addr_t* addr)
{
    /* net.route.0.inet.flags.gateway */
    int mib[] = {CTL_NET, PF_ROUTE, 0, AF_INET,
                 NET_RT_FLAGS, RTF_GATEWAY};
    size_t l;
    char *buf, *p;
    struct rt_msghdr* rt;
    struct sockaddr* sa;
    struct sockaddr* sa_tab[RTAX_MAX];
    int i;
    int r = GETGW_FAILED;
    if (sysctl(mib, sizeof(mib) / sizeof(int), 0, &l, 0, 0) < 0) {
        return GETGW_FAILED;
    }
    if (l > 0) {
        buf = malloc(l);
        if (!buf) return GETGW_FAILED;
        if (sysctl(mib, sizeof(mib) / sizeof(int), buf, &l, 0, 0) < 0) {
            free(buf);
            return GETGW_FAILED;
        }
        for (p = buf; p < buf + l; p += rt->rtm_msglen) {
            rt = (struct rt_msghdr*)p;
            sa = (struct sockaddr*)(rt + 1);
            for (i = 0; i < RTAX_MAX; i++) {
                if (rt->rtm_addrs & (1 << i)) {
                    sa_tab[i] = sa;
                    sa = (struct sockaddr*)((char*)sa + ROUNDUP(sa->sa_len));
                } else {
                    sa_tab[i] = NULL;
                }
            }
            if (((rt->rtm_addrs & (RTA_DST | RTA_GATEWAY)) == (RTA_DST | RTA_GATEWAY))
                && sa_tab[RTAX_DST]->sa_family == AF_INET
                && sa_tab[RTAX_GATEWAY]->sa_family == AF_INET) {
                if (((struct sockaddr_in*)sa_tab[RTAX_DST])->sin_addr.s_addr == 0) {
                    *addr = ((struct sockaddr_in*)(sa_tab[RTAX_GATEWAY]))->sin_addr.s_addr;
                    r = GETGW_OK;
                }
            }
        }
        free(buf);
    }
    return r;
}
#endif /* USE_SYSCTL_NET_ROUTE */

#ifdef USE_SOCKET_ROUTE
/* Thanks to Darren Kenny for this code */
#define NEXTADDR(w, u)                      \
    if (rtm_addrs & (w)) {                  \
        l = sizeof(struct sockaddr);        \
        memmove(cp, &(u), l);               \
        cp += l;                            \
    }

#define rtm m_rtmsg.m_rtm

static struct {
    struct rt_msghdr m_rtm;
    char m_space[512];
} m_rtmsg;

int getdefaultgateway(in_addr_t* addr)
{
    int s, seq, l, rtm_addrs, i;
    pid_t pid;
    struct sockaddr so_dst, so_mask;
    char* cp = m_rtmsg.m_space;
    struct sockaddr *gate = NULL, *sa;
    struct rt_msghdr* msg_hdr;

    pid = getpid();
    seq = 0;
    rtm_addrs = RTA_DST | RTA_NETMASK;

    memset(&so_dst, 0, sizeof(so_dst));
    memset(&so_mask, 0, sizeof(so_mask));
    memset(&m_rtmsg, 0, sizeof(m_rtmsg));

    rtm.rtm_type = RTM_GET;
    rtm.rtm_flags = RTF_UP | RTF_GATEWAY;
    rtm.rtm_version = RTM_VERSION;
    rtm.rtm_seq = ++seq;
    rtm.rtm_addrs = rtm_addrs;

    so_dst.sa_family = AF_INET;
    so_mask.sa_family = AF_INET;

    NEXTADDR(RTA_DST, so_dst);
    NEXTADDR(RTA_NETMASK, so_mask);

    rtm.rtm_msglen = l = (int)(cp - (char*)&m_rtmsg);

    s = socket(PF_ROUTE, SOCK_RAW, 0);
    if (s < 0) return GETGW_FAILED;

    if (write(s, (char*)&m_rtmsg, l) < 0) {
        close(s);
        return GETGW_FAILED;
    }

    do {
        l = (int)read(s, (char*)&m_rtmsg, sizeof(m_rtmsg));
    } while (l > 0 && (rtm.rtm_seq != seq || rtm.rtm_pid != pid));

    close(s);

    msg_hdr = &rtm;

    cp = (char*)(msg_hdr + 1);
    if (msg_hdr->rtm_addrs) {
        for (i = 1; i; i <<= 1)
            if (i & msg_hdr->rtm_addrs) {
                sa = (struct sockaddr*)cp;
                if (i == RTA_GATEWAY)
                    gate = sa;

                cp += sizeof(struct sockaddr);
            }
    } else {
        return GETGW_FAILED;
    }

    if (gate != NULL) {
        *addr = ((struct sockaddr_in*)gate)->sin_addr.s_addr;
        return GETGW_OK;
    }
    return GETGW_FAILED;
}
#endif /* USE_SOCKET_ROUTE */

#ifdef USE_WIN32_CODE_2
int getdefaultgateway(in_addr_t* addr)
{
#if defined(MSVC_VERSION) && MSVC_VERSION <= 71
    /* VC6 的 vendored 头没有 MIB_IPFORWARDROW/GetBestRoute 声明；
     * 上层 (ip&mask)|1 启发式兜底 */
    (void)addr;
    return GETGW_FAILED;
#else
    MIB_IPFORWARDROW ip_forward;
    memset(&ip_forward, 0, sizeof(ip_forward));
    if (GetBestRoute(inet_addr("0.0.0.0"), 0, &ip_forward) != NO_ERROR)
        return GETGW_FAILED;
    *addr = (in_addr_t)ip_forward.dwForwardNextHop;
    return GETGW_OK;
#endif
}
#endif /* USE_WIN32_CODE_2 */

#if !defined(USE_PROC_NET_ROUTE) && !defined(USE_SOCKET_ROUTE) && !defined(USE_SYSCTL_NET_ROUTE) && !defined(USE_WIN32_CODE_2)
int getdefaultgateway(in_addr_t* addr)
{
    (void)addr;
    return GETGW_FAILED;
}
#endif
