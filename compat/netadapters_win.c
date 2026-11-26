/* netadapters_win.c — hv_get_interface_addresses 的 Windows 实现，
 * 移植自 libuv src/win/util.c 同名函数（MIT）。
 *
 * GetAdaptersAddresses 经 winapi.h 取用：VC6 走 winapi.c 动态加载的
 * pGetAdaptersAddresses（必要时懒初始化），新编译器直接链接 iphlpapi。
 * FriendlyName 是 UTF-16：libuv 用自带的 wtf8 转换，这里改用
 * WideCharToMultiByte(CP_UTF8) 就地转换，避免依赖可选的 idna 模块。
 */
#include "hbase.h"
#include "netadapters.h"

#ifdef OS_WIN

#ifndef CP_UTF8
#define CP_UTF8  65001
#endif
#ifndef ERROR_ADDRESS_NOT_ASSOCIATED
#define ERROR_ADDRESS_NOT_ASSOCIATED  1228L
#endif
#ifndef IF_TYPE_SOFTWARE_LOOPBACK
#define IF_TYPE_SOFTWARE_LOOPBACK 24
#endif
/* 老 MSVC errno.h 没有 ENOSYS，取 Linux 同名值仅作错误码用 */
#ifndef ENOSYS
#define ENOSYS  38
#endif

int hv_get_interface_addresses(hv_interface_address_t** addresses_ptr, int* count_ptr)
{
    IP_ADAPTER_ADDRESSES* win_address_buf;
    ULONG win_address_buf_size;
    IP_ADAPTER_ADDRESSES* adapter;

    hv_interface_address_t* uv_address_buf;
    char* name_buf;
    size_t uv_address_buf_size;
    hv_interface_address_t* uv_address;

    int count;
    ULONG flags;

    *addresses_ptr = NULL;
    *count_ptr = 0;

#if defined(MSVC_VERSION) && MSVC_VERSION <= 71
    /* VC6：函数指针由 winapi.c 的 uv_winapi_init() 动态加载（幂等），这里兜底懒初始化 */
    if (pGetAdaptersAddresses == NULL)
        uv_winapi_init();
    if (pGetAdaptersAddresses == NULL)
        return ENOSYS;  /* iphlpapi.dll 缺失或过老 */
#endif

    flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;

    /* Fetch the size of the adapters reported by windows, and then get the list
     * itself. */
    win_address_buf_size = 0;
    win_address_buf = NULL;

    for (;;) {
        ULONG r;

        /* If win_address_buf is 0, then GetAdaptersAddresses will fail with
         * ERROR_BUFFER_OVERFLOW, and the required buffer size will be stored in
         * win_address_buf_size. */
        r = pGetAdaptersAddresses(AF_UNSPEC,
            flags,
            NULL,
            win_address_buf,
            &win_address_buf_size);

        if (r == ERROR_SUCCESS)
            break;

        hv_free(win_address_buf);

        switch (r) {
        case ERROR_BUFFER_OVERFLOW:
            /* This happens when win_address_buf is NULL or too small to hold all
             * adapters. */
            win_address_buf = hv_malloc(win_address_buf_size);
            continue;

        case ERROR_NO_DATA: {
            /* No adapters were found. Return a non-NULL dummy like libuv. */
            uv_address_buf = hv_malloc(1);

            *count_ptr = 0;
            *addresses_ptr = uv_address_buf;

            return 0;
        }

        case ERROR_ADDRESS_NOT_ASSOCIATED:
            /* The network stack is not ready yet (DHCP in progress etc.) */
            return EAGAIN;

        case ERROR_INVALID_PARAMETER:
            /* MSDN says the remaining cause is adapter data greater than
             * ULONG_MAX; libuv maps this to ENOBUFS, which old MSVC errno.h
             * lacks — ENOMEM carries the same meaning here. */
            return ENOMEM;

        default:
            /* Other (unspecified) errors can happen, but we don't have any special
             * meaning for them. */
            return (int)r;
        }
    }

    /* Count the number of enabled interfaces and compute how much space is
     * needed to store their info. */
    count = 0;
    uv_address_buf_size = 0;

    for (adapter = win_address_buf;
         adapter != NULL;
         adapter = adapter->Next) {
        IP_ADAPTER_UNICAST_ADDRESS* unicast_address;
        int name_size;

        /* Interfaces that are not 'up' should not be reported. Also skip
         * interfaces that have no associated unicast address, as to avoid
         * allocating space for the name for this interface. */
        if (adapter->OperStatus != IfOperStatusUp || adapter->FirstUnicastAddress == NULL)
            continue;

        /* Compute the size of the interface name (UTF-8, NUL included). */
        name_size = WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1,
                                        NULL, 0, NULL, NULL);
        if (name_size <= 0)
            name_size = 1;  /* conversion failed: keep an empty-name slot */
        uv_address_buf_size += (size_t)name_size;

        /* Count the number of addresses associated with this interface, and
         * compute the size. */
        for (unicast_address = adapter->FirstUnicastAddress;
             unicast_address != NULL;
             unicast_address = unicast_address->Next) {
            count++;
            uv_address_buf_size += sizeof(hv_interface_address_t);
        }
    }

    /* Allocate space to store interface data plus adapter names.
     * hv_malloc() aborts the process on failure, so never hand it a 0 size
     * (malloc(0) may return NULL). */
    uv_address_buf = hv_malloc(uv_address_buf_size ? uv_address_buf_size : 1);

    /* Compute the start of the hv_interface_address_t array, and the place in
     * the buffer where the interface names will be stored. */
    uv_address = uv_address_buf;
    name_buf = (char*)(uv_address_buf + count);

    /* Fill out the output buffer. */
    for (adapter = win_address_buf;
         adapter != NULL;
         adapter = adapter->Next) {
        IP_ADAPTER_UNICAST_ADDRESS* unicast_address;
        int name_size;

        if (adapter->OperStatus != IfOperStatusUp || adapter->FirstUnicastAddress == NULL)
            continue;

        /* Convert the interface name to UTF8. */
        name_size = (int)((char*)uv_address_buf + uv_address_buf_size - name_buf);
        if (WideCharToMultiByte(CP_UTF8, 0, adapter->FriendlyName, -1,
                                name_buf, name_size, NULL, NULL) == 0)
            name_buf[0] = '\0';  /* conversion failed: empty name */
        name_size = strlen(name_buf) + 1;

        /* Add an hv_interface_address_t element for every unicast address. */
        for (unicast_address = adapter->FirstUnicastAddress;
             unicast_address != NULL;
             unicast_address = unicast_address->Next) {
            struct sockaddr* sa;
            ULONG prefix_len;

            sa = unicast_address->Address.lpSockaddr;

            prefix_len = ((IP_ADAPTER_UNICAST_ADDRESS_LH*)unicast_address)->OnLinkPrefixLength;

            memset(uv_address, 0, sizeof *uv_address);

            uv_address->name = name_buf;

            if (adapter->PhysicalAddressLength == sizeof(uv_address->phys_addr)) {
                memcpy(uv_address->phys_addr,
                    adapter->PhysicalAddress,
                    sizeof(uv_address->phys_addr));
            }

            uv_address->is_internal = (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK);

            if (sa->sa_family == AF_INET6) {
                /* clamp: a bogus OnLinkPrefixLength must not write past
                 * s6_addr nor shift by a negative count below */
                if (prefix_len > 128) prefix_len = 128;

                uv_address->address.sin6 = *((struct sockaddr_in6*)sa);

                uv_address->netmask.sin6.sin6_family = AF_INET6;
                memset(uv_address->netmask.sin6.sin6_addr.s6_addr, 0xff, prefix_len >> 3);
                /* This check ensures that we don't write past the size of the data. */
                if (prefix_len % 8) {
                    uv_address->netmask.sin6.sin6_addr.s6_addr[prefix_len >> 3] = 0xff << (8 - prefix_len % 8);
                }

            } else {
                if (prefix_len > 32) prefix_len = 32;

                uv_address->address.sin = *((struct sockaddr_in*)sa);

                uv_address->netmask.sin.sin_family = AF_INET;
                uv_address->netmask.sin.sin_addr.s_addr = (prefix_len > 0) ? htonl(0xffffffff << (32 - prefix_len)) : 0;

                /*
                 * This assumes contiguous bits in the netmask, which is asserted
                 * in the calculation of the netmask above.
                 */
                uv_address->broadcast.broadcast4.sin_family = AF_INET;
                uv_address->broadcast.broadcast4.sin_addr.s_addr = uv_address->address.sin.sin_addr.s_addr | ~uv_address->netmask.sin.sin_addr.s_addr;
            }

            uv_address++;
        }

        name_buf += name_size;
    }

    hv_free(win_address_buf);

    *addresses_ptr = uv_address_buf;
    *count_ptr = count;

    return 0;
}

void hv_free_interface_addresses(hv_interface_address_t* addresses, int count)
{
    (void)count;
    hv_free(addresses);
}

#endif /* OS_WIN */
