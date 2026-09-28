/*
 * d_init_debug(LOGDIR, 0, 0, DEBUG_VERBOSE, DEBUG_VERBOSE, APPNAME);
 * d_destory_debug();
 *
*/

#ifndef _DEBUG_H_
#define _DEBUG_H_    1
#include <stdarg.h>

#ifndef HV_EXPORT
#  if defined(HV_STATICLIB) || defined(HV_SOURCE)
#    define HV_EXPORT
#  elif defined(_MSC_VER)
#    if defined(HV_DYNAMICLIB) || defined(HV_EXPORTS) || defined(tclog_EXPORTS)
#      define HV_EXPORT __declspec(dllexport)
#    else
#      define HV_EXPORT __declspec(dllimport)
#    endif
#  elif defined(__GNUC__)
#    define HV_EXPORT __attribute__((visibility("default")))
#  else
#    define HV_EXPORT
#  endif
#endif

#ifndef LOG_TAG
#define LOG_TAG ""
#endif

#ifndef DEBUG_SHOW_DEBUG
#define DEBUG_SHOW_DEBUG    1
#endif

#if DEBUG_SHOW_DEBUG
    #if defined(__ANDROID__)
        #define LOGINIT(d,s,c,lp,lf,n)
        #define LOGDESTROY()
        #include <android/log.h>
        #define LOGF(fmt,...)  __android_log_print(ANDROID_LOG_FATAL,LOG_TAG,fmt,##__VA_ARGS__); printf(fmt"\n",##__VA_ARGS__);abort()
        #define LOGE(fmt,...)  __android_log_print(ANDROID_LOG_ERROR,LOG_TAG,fmt,##__VA_ARGS__); printf(fmt"\n",##__VA_ARGS__)
        #define LOGW(fmt,...)  __android_log_print(ANDROID_LOG_WARN,LOG_TAG,fmt,##__VA_ARGS__); printf(fmt"\n",##__VA_ARGS__)
        #define LOGI(fmt,...)  __android_log_print(ANDROID_LOG_INFO,LOG_TAG,fmt,##__VA_ARGS__); printf(fmt"\n",##__VA_ARGS__)
        #define LOGD(fmt,...)  __android_log_print(ANDROID_LOG_DEBUG,LOG_TAG,fmt,##__VA_ARGS__); printf(fmt"\n",##__VA_ARGS__)
        #define LOGV(fmt,...)  __android_log_print(ANDROID_LOG_VERBOSE,LOG_TAG,fmt,##__VA_ARGS__); printf(fmt"\n",##__VA_ARGS__)

    #else //!__ANDROID__

        #define DEBUG_NONE          0
        #define DEBUG_FATAL         1
        #define DEBUG_ERROR         2
        #define DEBUG_WARNING       3
        #define DEBUG_INFO          4
        #define DEBUG_DEBUG         5
        #define DEBUG_VERBOSE       6
        #define DEBUG_MIN           DEBUG_NONE
        #define DEBUG_MAX           DEBUG_VERBOSE

        #include <stdlib.h>

        /* Define used colors */
        extern const char *g_colortable[];
        enum COLOR{
            NONE,
            KGRN,
            KCYN,
            KRED,
            KYEL,
            KBLU,
            KCYN_L,
            KBRN,
            RESET
        };
         #if defined(__cplusplus)
        extern "C" {
        #endif

        HV_EXPORT unsigned long _gettid();
        /* debug.c */
        HV_EXPORT void d_debug_bin_buffer(const void*data, size_t len);
        /*
        logdir: path to logfile dir,must end with /
        logfilesize: max Kbyte size of a logfile default not limited
        maxlogfilenumber:max number of logfile default 10
        level: stdout log level
        log:    file log level
        binname:file prefix
        */
        HV_EXPORT void d_init_debug(const char *logdir, size_t logfilesize, size_t maxlogfilenumber, int level, int log, const char *binname);
        HV_EXPORT int  d_set_debug_level(int level);
        HV_EXPORT int  d_set_log_level(int level);
        /* 切同步/异步输出。async 默认开；置 0 走同步分支（直接写 stderr/文件，
         * 不经队列）。原先没有 setter 时同步分支是不可达代码。 */
        HV_EXPORT int  d_set_async(int on);
        HV_EXPORT void d_destory_debug();
        HV_EXPORT void __d_debug(int level, const char* tag, const unsigned long pid, const enum COLOR color, const char* format,...);
        HV_EXPORT void d_log_flush();
        HV_EXPORT int  d_log_enabled(int level);
        #if defined(__cplusplus)
        }
        #endif

        #define LOGINIT(d,s,c,lp,lf,n) d_init_debug(d,s,c,lp,lf,n)
        #define LOGDESTROY() d_destory_debug()
#if defined(_MSC_VER) && _MSC_VER < 1400 // pre-VS2005 TODO
        HV_EXPORT void LOGF(const char* format, ...);
        HV_EXPORT void LOGE(const char* format, ...);
        HV_EXPORT void LOGW(const char* format, ...);
        HV_EXPORT void LOGI(const char* format, ...);
        HV_EXPORT void LOGD(const char* format, ...);
        HV_EXPORT void LOGV(const char* format, ...);
#else
        #define __LOG_AT(lvl, color, format, ...) \
            do { if (d_log_enabled(lvl)) \
                     __d_debug(lvl, LOG_TAG, _gettid(), color, format "\n", ##__VA_ARGS__); \
            } while (0)
        #define LOGF(format, ...)   do { __LOG_AT(DEBUG_FATAL,   KRED, format, ##__VA_ARGS__); abort(); } while (0)
        #define LOGE(format, ...)   __LOG_AT(DEBUG_ERROR,   KRED, format, ##__VA_ARGS__)
        #define LOGW(format, ...)   __LOG_AT(DEBUG_WARNING, KBRN, format, ##__VA_ARGS__)
        #define LOGI(format, ...)   __LOG_AT(DEBUG_INFO,    KGRN, format, ##__VA_ARGS__)
        #define LOGD(format, ...)   __LOG_AT(DEBUG_DEBUG,   KBLU, format, ##__VA_ARGS__)
        #define LOGV(format, ...)   __LOG_AT(DEBUG_VERBOSE, KCYN, format, ##__VA_ARGS__)
#endif
        #define LOGBIN(data, len)   d_debug_bin_buffer(data, len)
        #define STOPHERE            {LOGD("STOP HERE"); while(true) sleep(1);}
    #endif //__ANDROID
#else //DEBUG_SHOW_DEBUG
    #define LOGINIT
    #define LOGF(format, ...)   do {} while (0)
    #define LOGE(format, ...)   do {} while (0)
    #define LOGW(format, ...)   do {} while (0)
    #define LOGI(format, ...)   do {} while (0)
    #define LOGD(format, ...)   do {} while (0)
    #define LOGV(format, ...)   do {} while (0)
    #define LOGBIN(data, len)   do {} while (0)
    #define STOPHERE
#endif  //DEBUG_SHOW_DEBUG

#endif  // #ifndef _DEBUG_H_
