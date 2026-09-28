#ifndef HV_THREAD_H_
#define HV_THREAD_H_

#include "hplatform.h"
#include "hexport.h"

typedef enum {
  UV_THREAD_NO_FLAGS = 0x00,
  UV_THREAD_HAS_STACK_SIZE = 0x01
} uv_thread_create_flags;

struct uv_thread_options_s {
  unsigned int flags;
  size_t stack_size;
  /* More fields may be added at any time. */
};

typedef struct uv_thread_options_s uv_thread_options_t;

#ifdef OS_WIN
#define hv_getpid   (long)GetCurrentProcessId
#else
#if defined(__linux__) || (defined(__FreeBSD__) && __FreeBSD_version >= 1301000)
#define UV__CPU_AFFINITY_SUPPORTED 1
#else
#define UV__CPU_AFFINITY_SUPPORTED 0
#endif
#define hv_getpid   (long)getpid
#endif

#ifdef OS_WIN
#define hv_gettid   (long)GetCurrentThreadId
#elif HAVE_GETTID || defined(OS_ANDROID)
#define hv_gettid   (long)gettid
#elif defined(OS_LINUX)
#include <sys/syscall.h>
#define hv_gettid() (long)syscall(SYS_gettid)
#elif defined(OS_DARWIN)
static inline long hv_gettid() {
    uint64_t tid = 0;
/* pthread_threadid_np is not available before 10.6 and in 10.6 for ppc */
#if MAC_OS_X_VERSION_MIN_REQUIRED < 1060 || defined(__POWERPC__)
    tid = pthread_mach_thread_np(pthread_self());
#else
    pthread_threadid_np(NULL, &tid);
#endif
    return tid;
}
#elif HAVE_PTHREAD_H
#define hv_gettid   (long)pthread_self
#else
#define hv_gettid   hv_getpid
#endif

/*
#include "hthread.h"

HTHREAD_ROUTINE(thread_demo) {
    printf("thread[%ld] start\n", hv_gettid());
    hv_delay(3000);
    printf("thread[%ld] end\n", hv_gettid());
    return 0;
}

int main() {
    hthread_t th = hthread_create(thread_demo, NULL);
    hthread_join(th);
    return 0;
}
 */

#ifdef OS_WIN
typedef struct {
  DWORD tls_index;
} hthread_key_t;
typedef HANDLE      hthread_t;
typedef DWORD (WINAPI *hthread_routine)(void*);
#define HTHREAD_RETTYPE DWORD
#define HTHREAD_ROUTINE(fname) DWORD WINAPI fname(void* userdata)
HV_EXPORT hthread_t hthread_create(uv_thread_options_t *params, hthread_routine fn, void* userdata);

static inline int hthread_join(hthread_t th) {
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
#ifdef MemoryBarrier
    MemoryBarrier();  /* For feature parity with pthread_join(). */
#endif    
    return 0;
}

static inline int hthread_detach(hthread_t th) {
    CloseHandle(th);
    return 0;
}

static inline int hthread_key_create(hthread_key_t* key) {
  key->tls_index = TlsAlloc();
  if (key->tls_index == TLS_OUT_OF_INDEXES)
    return -1;
  return 0;
}

static inline void hthread_key_delete(hthread_key_t* key) {
  if (TlsFree(key->tls_index) == FALSE)
    abort();
  key->tls_index = TLS_OUT_OF_INDEXES;
}

static inline void* hthread_key_get(hthread_key_t* key) {
  void* value;

  value = TlsGetValue(key->tls_index);
  if (value == NULL)
    if (GetLastError() != ERROR_SUCCESS)
      abort();

  return value;
}

static inline void hthread_key_set(hthread_key_t* key, void* value) {
  if (TlsSetValue(key->tls_index, value) == FALSE)
    abort();
}
HV_EXPORT hthread_t hthread_self();
#else
typedef pthread_key_t hthread_key_t;
typedef pthread_t   hthread_t;
typedef void* (*hthread_routine)(void*);
#define HTHREAD_RETTYPE void*
#define HTHREAD_ROUTINE(fname) void* fname(void* userdata)
static inline hthread_t hthread_create(uv_thread_options_t *params, hthread_routine fn, void* userdata) {
    pthread_t th;
    pthread_attr_t attr_storage, *attr = NULL;
    size_t pagesize, min_stack_size;
    size_t stack_size = params->flags & UV_THREAD_HAS_STACK_SIZE ? params->stack_size : 0;
    if(stack_size){
        pagesize = (size_t)getpagesize();
        /* Round up to the nearest page boundary. */
        stack_size = (stack_size + pagesize - 1) &~ (pagesize - 1);
        min_stack_size = 8192;
        if (stack_size < min_stack_size)
            stack_size = min_stack_size;
          if (stack_size > 0) {
            attr = &attr_storage;

            if (pthread_attr_init(attr))
                abort();

            if (pthread_attr_setstacksize(attr, stack_size))
                abort();
        }
    }
    

    pthread_create(&th, attr, fn, userdata);
    if (attr != NULL)
        pthread_attr_destroy(attr);
    return th;
}

static inline int hthread_join(hthread_t th) {
    return pthread_join(th, NULL);
}

static inline int hthread_detach(hthread_t th) {
    return pthread_detach(th);
}

static inline int hthread_key_create(hthread_key_t* key) {
  return pthread_key_create(key, NULL);
}

static inline void hthread_key_delete(hthread_key_t* key) {
  if (pthread_key_delete(*key))
    abort();
}

static inline void* hthread_key_get(hthread_key_t* key) {
  return pthread_getspecific(*key);
}

static inline void hthread_key_set(hthread_key_t* key, void* value) {
  if (pthread_setspecific(*key, value))
    abort();
}
#define hthread_self pthread_self
#endif

HV_EXPORT int hthread_setaffinity(hthread_t tid, char* cpumask, char* oldmask, size_t mask_size);
HV_EXPORT int hthread_getaffinity(hthread_t tid, char* cpumask, size_t mask_size);
HV_EXPORT int hthread_getcpu(void);

#ifdef __cplusplus
/************************************************
 * HThread
 * Status: STOP,RUNNING,PAUSE
 * Control: start,stop,pause,resume
 * first-level virtual: doTask
 * second-level virtual: run
************************************************/
#include <thread>
#include <atomic>
#include <chrono>

class HThread {
public:
    enum Status {
        STOP,
        RUNNING,
        PAUSE,
    };

    enum SleepPolicy {
        YIELD,
        SLEEP_FOR,
        SLEEP_UNTIL,
        NO_SLEEP,
    };

    HThread() {
        status = STOP;
        status_changed = false;
        dotask_cnt = 0;
        sleep_policy = YIELD;
        sleep_ms = 0;
    }

    virtual ~HThread() {}

    void setStatus(Status stat) {
        status_changed = true;
        status = stat;
    }

    void setSleepPolicy(SleepPolicy policy, uint32_t ms = 0) {
        sleep_policy = policy;
        sleep_ms = ms;
        setStatus(status);
    }

    virtual int start() {
        if (status == STOP) {
            thread = std::thread([this] {
                if (!doPrepare()) return;
                setStatus(RUNNING);
                run();
                setStatus(STOP);
                if (!doFinish()) return;
            });
        }
        return 0;
    }

    virtual int stop() {
        if (status != STOP) {
            setStatus(STOP);
        }
        if (thread.joinable()) {
            thread.join();  // wait thread exit
        }
        return 0;
    }

    virtual int pause() {
        if (status == RUNNING) {
            setStatus(PAUSE);
        }
        return 0;
    }

    virtual int resume() {
        if (status == PAUSE) {
            setStatus(RUNNING);
        }
        return 0;
    }

    virtual void run() {
        while (status != STOP) {
            while (status == PAUSE) {
                std::this_thread::yield();
            }

            doTask();
            ++dotask_cnt;

            HThread::sleep();
        }
    }

    virtual bool doPrepare() {return true;}
    virtual void doTask() {}
    virtual bool doFinish() {return true;}

    std::thread thread;
    std::atomic<Status> status;
    uint32_t dotask_cnt;
protected:
    void sleep() {
        switch (sleep_policy) {
        case YIELD:
            std::this_thread::yield();
            break;
        case SLEEP_FOR:
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
            break;
        case SLEEP_UNTIL: {
            if (status_changed) {
                status_changed = false;
                base_tp = std::chrono::steady_clock::now();
            }
            base_tp += std::chrono::milliseconds(sleep_ms);
            std::this_thread::sleep_until(base_tp);
        }
            break;
        default:    // donothing, go all out.
            break;
        }
    }

    SleepPolicy sleep_policy;
    uint32_t    sleep_ms;
    // for SLEEP_UNTIL
    std::atomic<bool> status_changed;
    std::chrono::steady_clock::time_point base_tp;
};
#endif

#endif // HV_THREAD_H_
