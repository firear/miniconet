/* getgateway.h — 默认网关探测（vendor 自 libnatpmp getgateway.h，Thomas BERNARD，
 * BSD 授权；平台分支改用 hplatform.h 的 OS_* 宏驱动）。
 *
 * getdefaultgateway():
 *   返回 0 成功（*addr = 网关 IPv4，网络字节序）；-1 失败。
 *   Linux: /proc/net/route；macOS: sysctl(NET_RT_FLAGS)；
 *   BSD/Solaris: PF_ROUTE socket；Windows: GetBestRoute（iphlpapi）。
 *   未覆盖的平台返回 -1，由上层启发式（(ip&mask)|1）兜底。 */
#ifndef __GETGATEWAY_H__
#define __GETGATEWAY_H__

#include "hplatform.h"

#ifdef OS_WIN
/* winsock 头没有 in_addr_t（libnatpmp 同款处理） */
#ifndef in_addr_t
#define in_addr_t uint32_t
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

int getdefaultgateway(in_addr_t* addr);

#ifdef __cplusplus
}
#endif
#endif /* __GETGATEWAY_H__ */
