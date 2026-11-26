/* netadapters.h — 网卡/接口地址枚举（libuv uv_get_interface_addresses 的移植，
 * 与 ref/openp2p_chv/chv/util/netadapters* 同源）。
 *
 * 每个处于 UP 状态接口的每个单播地址返回一个条目：
 *   name        接口名（UNIX: "eth0" 等；Windows: FriendlyName，UTF-8）
 *   phys_addr   6 字节 MAC（尽力而为：无对应条目时保持全 0）
 *   is_internal 环回接口非 0（UNIX: IFF_LOOPBACK；Windows: IF_TYPE_SOFTWARE_LOOPBACK）
 *   address     单播地址（AF_INET / AF_INET6）
 *   netmask     掩码（Windows 由 OnLinkPrefixLength 推导）
 *   broadcast   IPv4 广播地址（仅 AF_INET 条目有效）
 *
 * 平台实现：netadapters_linux.c（UNIX getifaddrs，无 ifaddrs.h 时返回 ENOSYS）、
 * netadapters_win.c（GetAdaptersAddresses）。纯阻塞 libc/系统调用，耗时短，
 * 可在协程内直接调用；无内部加锁，多线程请自行串行化。
 */
#ifndef __NETADAPTERS_H__
#define __NETADAPTERS_H__

#include "hexport.h"
#include "hsocket.h"

enum {
    HV__EXCLUDE_IFPHYS,
    HV__EXCLUDE_IFADDR
};

typedef struct hv_interface_address_s {
    char* name;
    char phys_addr[6];
    int is_internal;
    sockaddr_u address;
    sockaddr_u netmask;
    union {
        struct sockaddr_in broadcast4;
    } broadcast;
} hv_interface_address_t;

BEGIN_EXTERN_C

/* 0 成功：*addresses 与 *count 填好（数组本身 calloc 清零，name 需单独释放）。
 * 无任何地址时 *count=0，*addresses 可能为 NULL（UNIX）或非 NULL 占位（Windows），
 * 两种情况 hv_free_interface_addresses 都安全。
 * 失败返回 errno 风格错误码；平台无实现时返回 ENOSYS。 */
HV_EXPORT int hv_get_interface_addresses(hv_interface_address_t** addresses, int* count);
/* 释放上面返回的数组（含各 name 与块本身；NULL 安全）。 */
HV_EXPORT void hv_free_interface_addresses(hv_interface_address_t* addresses, int count);

END_EXTERN_C
#endif
