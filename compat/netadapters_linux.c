/* netadapters_linux.c — hv_get_interface_addresses 的 UNIX 实现，
 * 移植自 libuv src/unix/core.c 同名函数（MIT，覆盖 Linux/macOS/BSD 等 UNIX）。
 * 无 ifaddrs.h 的平台（由 cmake/PlatformConfig.cmake 探测 HAVE_IFADDRS_H）
 * 编译为返回 ENOSYS 的桩。 */
#include "hbase.h"
#include "netadapters.h"

#ifdef OS_UNIX

#if HAVE_IFADDRS_H
#include <ifaddrs.h>
#endif
#include <net/if.h>     /* IFF_UP / IFF_RUNNING / IFF_LOOPBACK / IFF_BROADCAST */
#ifdef __linux__
#include <linux/if_packet.h>    /* struct sockaddr_ll（MAC 地址来源） */
#endif

#if HAVE_IFADDRS_H
static int ifaddr_exclude(struct ifaddrs* ent, int exclude_type)
{
    if (!((ent->ifa_flags & IFF_UP) && (ent->ifa_flags & IFF_RUNNING)))
        return 1;
    if (ent->ifa_addr == NULL)
        return 1;
    /*
     * On Linux getifaddrs returns information related to the raw underlying
     * devices. We're not interested in this information yet.
     */
#ifdef PF_PACKET
    if (ent->ifa_addr->sa_family == PF_PACKET)
        return exclude_type;
#endif
    return !exclude_type;
}
#endif /* HAVE_IFADDRS_H */

int hv_get_interface_addresses(hv_interface_address_t** addresses, int* count)
{
#if !HAVE_IFADDRS_H
    *addresses = NULL;
    *count = 0;
    return ENOSYS;
#else
    struct ifaddrs *addrs, *ent;
    hv_interface_address_t* address;

    *addresses = NULL;
    *count = 0;

    if (getifaddrs(&addrs))
        return errno;

    /* Count the number of interfaces */
    for (ent = addrs; ent != NULL; ent = ent->ifa_next) {
        if (ifaddr_exclude(ent, HV__EXCLUDE_IFADDR))
            continue;

        (*count)++;
    }

    if (*count == 0) {
        freeifaddrs(addrs);
        return 0;
    }

    /* Make sure the memory is initialized to zero using calloc() */
    *addresses = hv_calloc(*count, sizeof(**addresses));
    address = *addresses;

    for (ent = addrs; ent != NULL; ent = ent->ifa_next) {
        if (ifaddr_exclude(ent, HV__EXCLUDE_IFADDR))
            continue;

        if (ent->ifa_addr->sa_family == AF_INET6) {
            address->address.sin6 = *((struct sockaddr_in6*)ent->ifa_addr);
        } else {
            address->address.sin = *((struct sockaddr_in*)ent->ifa_addr);
        }

        /* ifa_netmask is not guaranteed by POSIX; keep zeros when absent */
        if (ent->ifa_netmask != NULL) {
            if (ent->ifa_netmask->sa_family == AF_INET6) {
                address->netmask.sin6 = *((struct sockaddr_in6*)ent->ifa_netmask);
            } else {
                address->netmask.sin = *((struct sockaddr_in*)ent->ifa_netmask);
            }
        }

        if ((ent->ifa_flags & IFF_BROADCAST) && ent->ifa_broadaddr != NULL &&
            ent->ifa_broadaddr->sa_family == AF_INET) {
            address->broadcast.broadcast4 = *((struct sockaddr_in*)ent->ifa_broadaddr);
        }

        address->is_internal = !!(ent->ifa_flags & IFF_LOOPBACK);
        address->name = hv_strdup(ent->ifa_name);

        address++;
    }

#ifdef PF_PACKET
    /* Fill in physical addresses for each interface */
    for (ent = addrs; ent != NULL; ent = ent->ifa_next) {
        int i;
        struct sockaddr_ll* sll;
        size_t namelen;

        if (ifaddr_exclude(ent, HV__EXCLUDE_IFPHYS))
            continue;

        sll = (struct sockaddr_ll*)ent->ifa_addr;
        namelen = strlen(ent->ifa_name);
        address = *addresses;

        for (i = 0; i < (*count); i++) {
            /* Alias interfaces share the same physical address */
            if (address->name != NULL &&
                strncmp(address->name, ent->ifa_name, namelen) == 0 &&
                (address->name[namelen] == 0 || address->name[namelen] == ':')) {
                memcpy(address->phys_addr, sll->sll_addr, sizeof(address->phys_addr));
            }
            address++;
        }
    }
#endif

    freeifaddrs(addrs);
    return 0;
#endif /* HAVE_IFADDRS_H */
}

void hv_free_interface_addresses(hv_interface_address_t* addresses, int count)
{
    int i;
    if (!addresses) return;
    for (i = 0; i < count; i++) {
        hv_free(addresses[i].name);
    }
    hv_free(addresses);
}
#endif /* OS_UNIX */
