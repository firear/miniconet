/* mco_wakefd.c — see mco_wakefd.h. */
#include "mco_wakefd.h"

#if MCO_WAKEFD_USE_EVENTFD
#  include <unistd.h>
#  include <stdint.h>
#elif defined(_WIN32)
#  include <winsock2.h>
#  include "hsocket.h"     /* Socketpair(), nonblocking() */
#else
#  include <unistd.h>
#  include <fcntl.h>
#endif

int mco_wakefd_init(mco_wakefd_t* w) {
    if (!w) return -1;
    w->rfd = -1;
    w->wfd = -1;

#if MCO_WAKEFD_USE_EVENTFD
    {
        /* EFD_NONBLOCK from the start: no fcntl round trip, and drain never
         * blocks. Non-semaphore mode (no EFD_SEMAPHORE): one read returns the
         * accumulated count and resets it, so a burst costs one syscall. */
        int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (efd < 0) return -1;
        w->rfd = efd;
        w->wfd = efd;
        return 0;
    }
#elif defined(_WIN32)
    {
        int fd[2];
        if (Socketpair(AF_INET, SOCK_STREAM, 0, fd) != 0) return -1;
        nonblocking(fd[0]);
        nonblocking(fd[1]);
        w->rfd = fd[0];
        w->wfd = fd[1];
        return 0;
    }
#else
    {
        int fd[2];
        if (pipe(fd) != 0) return -1;
        fcntl(fd[0], F_SETFL, fcntl(fd[0], F_GETFL) | O_NONBLOCK);
        fcntl(fd[1], F_SETFL, fcntl(fd[1], F_GETFL) | O_NONBLOCK);
        w->rfd = fd[0];
        w->wfd = fd[1];
        return 0;
    }
#endif
}

void mco_wakefd_fini(mco_wakefd_t* w) {
    if (!w) return;
#if MCO_WAKEFD_USE_EVENTFD
    if (w->rfd >= 0) close(w->rfd);
#elif defined(_WIN32)
    if (w->rfd >= 0) closesocket((SOCKET)w->rfd);
    if (w->wfd >= 0) closesocket((SOCKET)w->wfd);
#else
    if (w->rfd >= 0) close(w->rfd);
    if (w->wfd >= 0) close(w->wfd);
#endif
    w->rfd = -1;
    w->wfd = -1;
}

void mco_wakefd_signal(mco_wakefd_t* w) {
    if (!w || w->wfd < 0) return;
#if MCO_WAKEFD_USE_EVENTFD
    {
        uint64_t one = 1;
        (void)!write(w->wfd, &one, sizeof(one));
    }
#elif defined(_WIN32)
    {
        char b = 1;
        (void)!send((SOCKET)w->wfd, &b, 1, 0);
    }
#else
    {
        char b = 1;
        (void)!write(w->wfd, &b, 1);
    }
#endif
}

void mco_wakefd_drain(mco_wakefd_t* w) {
    if (!w || w->rfd < 0) return;
#if MCO_WAKEFD_USE_EVENTFD
    {
        uint64_t v;
        (void)!read(w->rfd, &v, sizeof(v));
    }
#elif defined(_WIN32)
    {
        char buf[64];
        while (recv((SOCKET)w->rfd, buf, (int)sizeof(buf), 0) > 0) { /* drain */ }
    }
#else
    {
        char buf[64];
        while (read(w->rfd, buf, sizeof(buf)) > 0) { /* drain */ }
    }
#endif
}
