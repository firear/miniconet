#include "hthread.h"
#include "hmutex.h"
#include <errno.h>

#if _WIN32

static hthread_key_t uv__current_thread_key;
static honce_t uv__current_thread_init_guard = HONCE_INIT;

int uv_cpumask_size(void)
{
    return (int)(sizeof(void*) * 8);
}

static void uv__init_current_thread_key(void)
{
    if (hthread_key_create(&uv__current_thread_key))
        abort();
}

struct thread_ctx {
    hthread_routine entry;
    void* arg;
    hthread_t self;
};

static UINT __stdcall uv__thread_start(void* arg)
{
    struct thread_ctx* ctx_p;
    struct thread_ctx ctx;

    ctx_p = arg;
    ctx = *ctx_p;
    free(ctx_p);

    honce(&uv__current_thread_init_guard, uv__init_current_thread_key);
    hthread_key_set(&uv__current_thread_key, ctx.self);

    ctx.entry(ctx.arg);

    return 0;
}

hthread_t hthread_create(uv_thread_options_t* params, hthread_routine fn, void* arg)
{
    // return CreateThread(NULL, 0, fn, userdata, 0, NULL);
    hthread_t thread;
    struct thread_ctx* ctx;
    size_t pagesize;
    size_t stack_size = params->flags & UV_THREAD_HAS_STACK_SIZE ? params->stack_size : 0;
    if (stack_size != 0) {
        SYSTEM_INFO sysinfo;
        pGetNativeSystemInfo(&sysinfo);
        pagesize = (size_t)sysinfo.dwPageSize;
        /* Round up to the nearest page boundary. */
        stack_size = (stack_size + pagesize - 1) & ~(pagesize - 1);

        if ((unsigned)stack_size != stack_size)
            stack_size = 0;
    }

    ctx = malloc(sizeof(*ctx));
    if (ctx == NULL)
        return NULL;

    ctx->entry = fn;
    ctx->arg = arg;
    thread = (HANDLE)_beginthreadex(NULL, (unsigned)stack_size, uv__thread_start, ctx, CREATE_SUSPENDED, NULL); // instead of win32 CreateThread witch maybe cause tiddata memory leak
    if (thread == NULL) {
        free(ctx);
    } else {
        ctx->self = thread;
        ResumeThread(thread);
    }
    return thread;
}

hthread_t hthread_self(void)
{
    hthread_t key;
    honce(&uv__current_thread_init_guard, uv__init_current_thread_key);
    key = hthread_key_get(&uv__current_thread_key);
    if (key == NULL) {
        /* If the thread wasn't started by uv_thread_create (such as the main
         * thread), we assign an id to it now. */
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                GetCurrentProcess(), &key, 0,
                FALSE, DUPLICATE_SAME_ACCESS)) {
            abort();
        }
        hthread_key_set(&uv__current_thread_key, key);
    }
    return key;
}

int hthread_setaffinity(hthread_t tid, char* cpumask, char* oldmask, size_t mask_size)
{
    int i;
    HANDLE hproc;
    DWORD_PTR procmask;
    DWORD_PTR sysmask;
    DWORD_PTR threadmask;
    DWORD_PTR oldthreadmask;
    int cpumasksize;

    cpumasksize = uv_cpumask_size();
    assert(cpumasksize > 0);
    if (mask_size < (size_t)cpumasksize)
        return EINVAL;

    hproc = GetCurrentProcess();
    if (!GetProcessAffinityMask(hproc, &procmask, &sysmask))
        return GetLastError();

    threadmask = 0;
    for (i = 0; i < cpumasksize; i++) {
        if (cpumask[i]) {
            if (procmask & (1ul << i))
                threadmask |= 1ul << i;
            else
                return EINVAL;
        }
    }

    oldthreadmask = SetThreadAffinityMask(tid, threadmask);
    if (oldthreadmask == 0)
        return GetLastError();

    if (oldmask != NULL) {
        for (i = 0; i < cpumasksize; i++)
            oldmask[i] = (oldthreadmask >> i) & 1;
    }

    return 0;
}

int hthread_getaffinity(hthread_t tid, char* cpumask, size_t mask_size)
{
    int i;
    HANDLE hproc;
    DWORD_PTR procmask;
    DWORD_PTR sysmask;
    DWORD_PTR threadmask;
    int cpumasksize;

    cpumasksize = uv_cpumask_size();
    assert(cpumasksize > 0);
    if (mask_size < (size_t)cpumasksize)
        return EINVAL;

    hproc = GetCurrentProcess();
    if (!GetProcessAffinityMask(hproc, &procmask, &sysmask))
        return GetLastError();

    threadmask = SetThreadAffinityMask(tid, procmask);
    if (threadmask == 0 || SetThreadAffinityMask(tid, threadmask) == 0)
        return GetLastError();

    for (i = 0; i < cpumasksize; i++)
        cpumask[i] = (threadmask >> i) & 1;

    return 0;
}

int hthread_getcpu(void)
{
    return pGetCurrentProcessorNumber();
}

#else

#if defined(__linux__)
#include <sched.h>
#define uv__cpu_set_t cpu_set_t
#elif defined(__FreeBSD__)
#include <pthread_np.h>
#include <sys/cpuset.h>
#include <sys/param.h>
#define uv__cpu_set_t cpuset_t
#endif

int uv_cpumask_size(void)
{
#if UV__CPU_AFFINITY_SUPPORTED
    return CPU_SETSIZE;
#else
    return UV_ENOTSUP;
#endif
}

#if defined(__ANDROID_API__) && __ANDROID_API__ < 21 // min sdk ver
#include <dlfcn.h>
static pid_t pthread_gettid_np(pthread_t __pthread)
{
    char api_level_str[5];
    __system_property_get("ro.build.version.sdk", api_level_str);
    int api_level = atoi(api_level_str);
    if(api_level < 21){ //<= android 4.4
        typedef struct pthread_internal_t {
            struct pthread_internal_t* next;
            struct pthread_internal_t* prev;
            pthread_attr_t              attr;
            pid_t tid;
        }dummy;
        return ((dummy *)__pthread)->tid;
    }else{
        static pid_t (*pthread_gettid_np_fun)(pthread_t) = NULL;
        if(!pthread_gettid_np_fun){
            void *h = dlopen("libc.so", RTLD_LAZY);
            if(h) {
                pthread_gettid_np_fun = dlsym(h, "pthread_gettid_np");
                dlclose(h);
            }else{
                printf("%s dlopen failed %s\n", __func__, dlerror());
                h = (void *)-1;
            }
        }
        if(pthread_gettid_np_fun != (void *)-1 && pthread_gettid_np_fun){
//            printf("%s:ret=%d\n", __func__, pthread_gettid_np_fun(__pthread));
            return pthread_gettid_np_fun(__pthread);
        }else{
            printf("%s dlsym failed %s\n", __func__, dlerror());
        }
    }
    return -1;
}
#endif /* __ANDROID_API__ >= 21 */

int hthread_setaffinity(hthread_t tid, char* cpumask, char* oldmask, size_t mask_size)
{
    int i;
    int r;
    uv__cpu_set_t cpuset;
    int cpumasksize;

    cpumasksize = uv_cpumask_size();
    if (cpumasksize < 0)
        return cpumasksize;
    if (mask_size < (size_t)cpumasksize)
        return EINVAL;

    if (oldmask != NULL) {
        r = hthread_getaffinity(tid, oldmask, mask_size);
        if (r < 0)
            return r;
    }

    CPU_ZERO(&cpuset);
    for (i = 0; i < cpumasksize; i++)
        if (cpumask[i])
            CPU_SET(i, &cpuset);

#if defined(__ANDROID__)
    if (sched_setaffinity(pthread_gettid_np(tid), sizeof(cpuset), &cpuset))
        r = errno;
    else
        r = 0;
#else
    r = pthread_setaffinity_np(tid, sizeof(cpuset), &cpuset);
#endif

    return r;
}

int hthread_getaffinity(hthread_t tid, char* cpumask, size_t mask_size)
{
    int r;
    int i;
    uv__cpu_set_t cpuset;
    int cpumasksize;

    cpumasksize = uv_cpumask_size();
        if (cpumasksize < 0)
        return cpumasksize;
    if (mask_size < (size_t)cpumasksize)
        return EINVAL;

    CPU_ZERO(&cpuset);
    #if defined(__ANDROID__)
    if (sched_getaffinity(pthread_gettid_np(tid), sizeof(cpuset), &cpuset))
        r = errno;
    else
        r = 0;
#else
    r = pthread_getaffinity_np(tid, sizeof(cpuset), &cpuset);
#endif
    if (r)
        return r;
    for (i = 0; i < cpumasksize; i++)
        cpumask[i] = !!CPU_ISSET(i, &cpuset);

    return 0;
}

int hthread_getcpu(void)
{
#if UV__CPU_AFFINITY_SUPPORTED && HAVE_SCHED_GETCPU
    int cpu;
    cpu = sched_getcpu();
    if (cpu < 0)
        return errno;

    return cpu;
#else
    return ENOTSUP;
#endif
}
#endif
