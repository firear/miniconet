/*
 * debug module smoke tests. We avoid asserting on rendered log text (the async
 * writer thread makes output non-deterministic) and instead verify the API
 * contract: init/destroy do not crash, level setters validate their range, and
 * d_log_enabled() reflects the configured stdout/file levels.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "debug.h"

#include <stdio.h>
#include <stdlib.h>

/* CHECK() 在 NDEBUG（RelWithDebInfo / Release）下展开为空，这些断言就全被
 * 编译掉、测试变成空转。改用始终求值的 CHECK()。 */
static void tclog_check_fail(const char* file, int line, const char* expr)
{
    fprintf(stderr, "CHECK FAILED %s:%d: %s\n", file, line, expr);
    abort();
}
#define CHECK(cond) \
    do { if (!(cond)) tclog_check_fail(__FILE__, __LINE__, #cond); } while (0)

static void test_log_enabled_before_init(void)
{
    /* No init: level_cache starts at -1, so everything is enabled. */
    CHECK(d_log_enabled(DEBUG_VERBOSE) == 1);
    CHECK(d_log_enabled(DEBUG_NONE) == 1);
    printf("ok: log enabled before init (cache=-1)\n");
}

static void test_init_destroy_smoke(void)
{
    d_init_debug(NULL, 0, 0, DEBUG_VERBOSE, DEBUG_VERBOSE, "tclog_test");
    LOGI("info message from tclog standalone test");
    LOGE("error message from tclog standalone test");
    LOGD("debug message");
    LOGV("verbose message");
    d_debug_bin_buffer("abc", 3);
    d_destory_debug();
    printf("ok: init/destroy smoke\n");
}

static void test_set_levels(void)
{
    CHECK(d_set_debug_level(DEBUG_INFO) == 0);
    CHECK(d_set_debug_level(99) == -1);          /* out of range */
    CHECK(d_set_log_level(DEBUG_WARNING) == 0);
    CHECK(d_set_log_level(-1) == -1);            /* out of range */

    /* level_cache = max(stdout=INFO, file=WARNING) = INFO */
    CHECK(d_log_enabled(DEBUG_ERROR) == 1);
    CHECK(d_log_enabled(DEBUG_INFO) == 1);
    CHECK(d_log_enabled(DEBUG_DEBUG) == 0);      /* DEBUG > INFO -> off */
    CHECK(d_log_enabled(DEBUG_VERBOSE) == 0);

    d_destory_debug();
    printf("ok: set levels + filtering\n");
}

int main(void)
{
    test_log_enabled_before_init();
    test_init_destroy_smoke();
    test_set_levels();
    printf("ALL DEBUG TESTS PASSED\n");
    return 0;
}
