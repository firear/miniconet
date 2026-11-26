/* netif_smoke.c — hv_get_interface_addresses（compat/netadapters）冒烟测试。
 * 只做确定性断言：枚举成功、每个条目有名字、环回 127.0.0.1 以 internal 条目
 * 出现、free 可正常回收（ASan 下验证无泄漏）。
 * 「存在非环回网卡」依赖环境，只打印不判定。 */
#include "../compat/netadapters.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    hv_interface_address_t* addrs = NULL;
    int count = 0;
    int i;
    int r;
    int ok = 1;
    int have_lo4 = 0;
    int have_nic = 0;

    r = hv_get_interface_addresses(&addrs, &count);
    if (r != 0) {
        printf("get_interface_addresses failed: %d\n", r);
        return 1;
    }
    printf("interface addresses: %d\n", count);

    for (i = 0; i < count; i++) {
        char ip[64] = {0};
        char mac[24] = "00:00:00:00:00:00";
        const unsigned char* m;

        if (addrs[i].name == NULL) {
            printf("[%d] missing name\n", i);
            ok = 0;
            continue;
        }
        sockaddr_ip(&addrs[i].address, ip, sizeof(ip));
        m = (const unsigned char*)addrs[i].phys_addr;
        if (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]) {
            sprintf(mac, "%02x:%02x:%02x:%02x:%02x:%02x",
                    m[0], m[1], m[2], m[3], m[4], m[5]);
        }
        printf("  %-16s %-40s internal=%d mac=%s\n",
               addrs[i].name, ip, addrs[i].is_internal, mac);

        if (addrs[i].address.sa.sa_family == AF_INET &&
            strcmp(ip, "127.0.0.1") == 0 &&
            addrs[i].is_internal)
            have_lo4 = 1;
        if (!addrs[i].is_internal)
            have_nic = 1;
    }
    hv_free_interface_addresses(addrs, count);

    if (!have_lo4) {
        printf("loopback 127.0.0.1 was not enumerated as internal\n");
        ok = 0;
    }
    printf("non-loopback interfaces found: %d\n", have_nic);
    printf(ok ? "NETIF TEST PASSED\n" : "NETIF TEST FAILED\n");
    return ok ? 0 : 1;
}
