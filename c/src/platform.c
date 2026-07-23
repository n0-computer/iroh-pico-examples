#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef PICO_STD_WIFI
#include "lwip/sockets.h"
#endif
#include "pico/rand.h"
#include "pico/time.h"

typedef void DIR;
struct dirent;

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 1
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 4
#endif

/* Compatibility symbols expected by Rust's espidf std backend. */
void esp_fill_random(void *buffer, size_t length)
{
    uint8_t *out = buffer;
    while (length >= sizeof(uint32_t)) {
        uint32_t value = get_rand_32();
        memcpy(out, &value, sizeof(value));
        out += sizeof(value);
        length -= sizeof(value);
    }
    if (length != 0) {
        uint32_t value = get_rand_32();
        memcpy(out, &value, length);
    }
}

int gethostname(char *name, size_t length)
{
    static const char hostname[] = "presto";
    if (length == 0) {
        errno = EINVAL;
        return -1;
    }
    strncpy(name, hostname, length);
    name[length - 1] = '\0';
    return 0;
}

int clock_gettime(clockid_t clock_id, struct timespec *value)
{
    if (clock_id != CLOCK_MONOTONIC && clock_id != CLOCK_REALTIME) {
        errno = EINVAL;
        return -1;
    }
    uint64_t micros = time_us_64();
    value->tv_sec = (time_t)(micros / 1000000u);
    value->tv_nsec = (long)((micros % 1000000u) * 1000u);
    return 0;
}

#ifdef PICO_STD_WIFI

/* mio uses eventfd to wake its poll loop. Pico SDK has no VFS, so reserve a
 * handful of synthetic descriptors and merge them into lwIP's socket poll. */
#define EVENTFD_BASE 100
#define EVENTFD_COUNT 4
static struct {
    volatile uint32_t active;
    volatile uint64_t value;
} eventfds[EVENTFD_COUNT];

static int eventfd_index(int fd)
{
    int index = fd - EVENTFD_BASE;
    return index >= 0 && index < EVENTFD_COUNT &&
        __atomic_load_n(&eventfds[index].active, __ATOMIC_ACQUIRE) ? index : -1;
}

int eventfd(unsigned int initial_value, int flags)
{
    if (flags != 0) {
        errno = EINVAL;
        return -1;
    }
    for (int i = 0; i < EVENTFD_COUNT; ++i) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&eventfds[i].active, &expected, 1,
                                        false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&eventfds[i].value, initial_value, __ATOMIC_RELEASE);
            return EVENTFD_BASE + i;
        }
    }
    errno = EMFILE;
    return -1;
}

ssize_t __real_read(int fd, void *buffer, size_t length);
ssize_t __real_write(int fd, const void *buffer, size_t length);
int __real_close(int fd);

/* socket2 uses fcntl(F_GETFL/F_SETFL) to put sockets into nonblocking mode.
 * Newlib's bare-metal fcntl is only an ENOSYS stub; lwIP owns the socket
 * descriptors and provides the real implementation under a prefixed name. */
int __wrap_fcntl(int fd, int command, ...)
{
    /* Rust's libc uses Newlib's O_NONBLOCK (0x4000), while lwIP compiles its
     * socket layer with its own O_NONBLOCK value (1). */
    const int lwip_o_nonblock = 1;
    int value = 0;
    if (command != F_GETFL) {
        va_list args;
        va_start(args, command);
        value = va_arg(args, int);
        va_end(args);
    }

    if (command == F_SETFL) {
        if (value & O_NONBLOCK) {
            value = (value & ~O_NONBLOCK) | lwip_o_nonblock;
        }
        return lwip_fcntl(fd, command, value);
    }

    int result = lwip_fcntl(fd, command, value);
    if (command == F_GETFL && result >= 0 && (result & lwip_o_nonblock)) {
        result = (result & ~lwip_o_nonblock) | O_NONBLOCK;
    }
    return result;
}

ssize_t __wrap_read(int fd, void *buffer, size_t length)
{
    int index = eventfd_index(fd);
    if (index < 0) return __real_read(fd, buffer, length);
    if (length != sizeof(uint64_t)) {
        errno = EINVAL;
        return -1;
    }
    uint64_t value = __atomic_exchange_n(&eventfds[index].value, 0, __ATOMIC_ACQ_REL);
    if (value == 0) {
        errno = EAGAIN;
        return -1;
    }
    memcpy(buffer, &value, sizeof(value));
    return sizeof(value);
}

ssize_t __wrap_write(int fd, const void *buffer, size_t length)
{
    int index = eventfd_index(fd);
    if (index < 0) return __real_write(fd, buffer, length);
    if (length != sizeof(uint64_t)) {
        errno = EINVAL;
        return -1;
    }
    uint64_t value;
    memcpy(&value, buffer, sizeof(value));
    if (value == UINT64_MAX) {
        errno = EINVAL;
        return -1;
    }
    __atomic_fetch_add(&eventfds[index].value, value, __ATOMIC_ACQ_REL);
    return sizeof(value);
}

int __wrap_close(int fd)
{
    int index = eventfd_index(fd);
    if (index < 0) return __real_close(fd);
    __atomic_store_n(&eventfds[index].value, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&eventfds[index].active, 0, __ATOMIC_RELEASE);
    return 0;
}

/* lwIP implements poll under its prefixed name. Synthetic eventfds are checked
 * between short lwIP waits, which is sufficient to wake a current-thread Tokio
 * runtime without requiring an ESP-IDF-style VFS layer.
 *
 * Rust libc uses ESP-IDF's poll flag values while lwIP has its own table. Only
 * POLLIN happens to match. Translate both directions or mio's POLLOUT (0x08)
 * is ignored by lwIP, leaving every UDP send permanently pending. */
#define RUST_POLLIN       0x01
#define RUST_POLLRDNORM   0x02
#define RUST_POLLRDBAND   0x04
#define RUST_POLLPRI      RUST_POLLRDBAND
#define RUST_POLLOUT      0x08
#define RUST_POLLWRBAND   0x10
#define RUST_POLLERR      0x20
#define RUST_POLLNVAL     0x20
#define RUST_POLLHUP      0x40

static short poll_events_to_lwip(short events)
{
    short result = 0;
    if (events & (RUST_POLLIN | RUST_POLLRDNORM)) result |= POLLIN;
    if (events & RUST_POLLOUT) result |= POLLOUT;
    if (events & RUST_POLLPRI) result |= POLLPRI;
    if (events & RUST_POLLRDBAND) result |= POLLRDBAND;
    if (events & RUST_POLLWRBAND) result |= POLLWRBAND;
    return result;
}

static short poll_events_from_lwip(short events)
{
    short result = 0;
    if (events & POLLIN) result |= RUST_POLLIN;
    if (events & POLLOUT) result |= RUST_POLLOUT;
    if (events & POLLERR) result |= RUST_POLLERR;
    if (events & POLLNVAL) result |= RUST_POLLNVAL;
    if (events & POLLHUP) result |= RUST_POLLHUP;
    if (events & POLLPRI) result |= RUST_POLLPRI;
    if (events & POLLRDBAND) result |= RUST_POLLRDBAND;
    if (events & POLLWRBAND) result |= RUST_POLLWRBAND;
    return result;
}

#undef poll
int poll(struct pollfd *fds, nfds_t count, int timeout)
{
    const int quantum_ms = 10;
    int elapsed = 0;
    for (;;) {
        struct pollfd sockets[count];
        int ready = 0;
        for (nfds_t i = 0; i < count; ++i) {
            sockets[i] = fds[i];
            fds[i].revents = 0;
            int index = eventfd_index(fds[i].fd);
            if (index >= 0) {
                sockets[i].fd = -1;
                if ((fds[i].events & RUST_POLLIN) &&
                    __atomic_load_n(&eventfds[index].value, __ATOMIC_ACQUIRE) != 0) {
                    fds[i].revents = RUST_POLLIN;
                    ++ready;
                }
            } else {
                sockets[i].events = poll_events_to_lwip(fds[i].events);
            }
        }
        if (ready != 0 || timeout == 0) return ready;

        int wait = timeout < 0 ? quantum_ms : timeout - elapsed;
        if (wait > quantum_ms) wait = quantum_ms;
        int socket_ready = lwip_poll(sockets, count, wait);
        if (socket_ready < 0) return socket_ready;
        for (nfds_t i = 0; i < count; ++i) {
            if (sockets[i].fd >= 0 && sockets[i].revents != 0) {
                fds[i].revents = poll_events_from_lwip(sockets[i].revents);
                ++ready;
            }
        }
        if (ready != 0) return ready;
        if (timeout >= 0) {
            elapsed += wait;
            if (elapsed >= timeout) return 0;
        }
    }
}

#endif /* PICO_STD_WIFI */

/* There is no filesystem. These symbols are referenced by std's directory
 * iterator even though the direct-only example never opens a directory. */
DIR *opendir(const char *path)
{
    (void)path;
    errno = ENOENT;
    return NULL;
}

int closedir(DIR *directory)
{
    (void)directory;
    errno = EBADF;
    return -1;
}

int readdir_r(DIR *directory, struct dirent *entry, struct dirent **result)
{
    (void)directory;
    (void)entry;
    *result = NULL;
    return EBADF;
}
