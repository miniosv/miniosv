/*
 * Copyright (C) 2024 OSv
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

/*
 * Minimal "fileless" libc I/O.
 *
 * This kernel has no filesystem and no file-descriptor table. The only streams
 * that exist are the three standard ones (stdin/stdout/stderr), and stdout and
 * stderr are backed directly by the console. Everything else fails cleanly with
 * EBADF/ENOSYS so the C library and applications get well-defined errors instead
 * of dereferencing a file table that no longer exists.
 */

#include <unistd.h>
#include <errno.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <osv/export.h>

#include "drivers/console.hh"

static bool is_stdout(int fd) { return fd == 1 || fd == 2; }

extern "C" OSV_LIBC_API
ssize_t write(int fd, const void *buf, size_t count)
{
    if (is_stdout(fd)) {
        console::write(static_cast<const char*>(buf), count);
        return count;
    }
    errno = EBADF;
    return -1;
}

extern "C" OSV_LIBC_API
ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
    if (!is_stdout(fd)) {
        errno = EBADF;
        return -1;
    }
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len) {
            console::write(static_cast<const char*>(iov[i].iov_base), iov[i].iov_len);
            total += iov[i].iov_len;
        }
    }
    return total;
}

extern "C" OSV_LIBC_API
ssize_t read(int fd, void *, size_t)
{
    if (fd == 0) {
        return 0; // no console input: report EOF
    }
    errno = EBADF;
    return -1;
}

extern "C" OSV_LIBC_API
ssize_t readv(int fd, const struct iovec *, int)
{
    if (fd == 0) {
        return 0;
    }
    errno = EBADF;
    return -1;
}

extern "C" OSV_LIBC_API
int close(int fd)
{
    if (fd >= 0 && fd <= 2) {
        return 0;
    }
    errno = EBADF;
    return -1;
}

extern "C" OSV_LIBC_API
off_t lseek(int, off_t, int)
{
    errno = ESPIPE;
    return -1;
}

extern "C" OSV_LIBC_API
int isatty(int fd)
{
    if (fd >= 0 && fd <= 2) {
        return 1;
    }
    errno = ENOTTY;
    return 0;
}

extern "C" OSV_LIBC_API int fsync(int) { return 0; }
extern "C" OSV_LIBC_API int fdatasync(int) { return 0; }

// ---------------------------------------------------------------------------
// Everything below operates on files, directories or the file-descriptor
// table, none of which exist. They are provided so the C library, libstdc++
// (e.g. std::filesystem) and applications link and get a clean error at
// runtime rather than calling into a filesystem that is not there.
// ---------------------------------------------------------------------------

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <dirent.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/shm.h>

extern "C" {

OSV_LIBC_API ssize_t pread(int, void *, size_t, off_t) { errno = EBADF; return -1; }
OSV_LIBC_API ssize_t pwrite(int, const void *, size_t, off_t) { errno = EBADF; return -1; }
OSV_LIBC_API ssize_t pread64(int, void *, size_t, off_t) { errno = EBADF; return -1; }
OSV_LIBC_API ssize_t pwrite64(int, const void *, size_t, off_t) { errno = EBADF; return -1; }
OSV_LIBC_API ssize_t preadv(int, const struct iovec *, int, off_t) { errno = EBADF; return -1; }
OSV_LIBC_API ssize_t pwritev(int, const struct iovec *, int, off_t) { errno = EBADF; return -1; }
OSV_LIBC_API ssize_t sendfile(int, int, off_t *, size_t) { errno = ENOSYS; return -1; }

OSV_LIBC_API int open(const char *, int, ...) { errno = ENOENT; return -1; }
OSV_LIBC_API int openat(int, const char *, int, ...) { errno = ENOENT; return -1; }
OSV_LIBC_API int __openat_2(int, const char *, int) { errno = ENOENT; return -1; }
OSV_LIBC_API int creat(const char *, mode_t) { errno = ENOSYS; return -1; }

OSV_LIBC_API int stat(const char *, struct stat *) { errno = ENOENT; return -1; }
OSV_LIBC_API int lstat(const char *, struct stat *) { errno = ENOENT; return -1; }
OSV_LIBC_API int fstat(int, struct stat *) { errno = EBADF; return -1; }
OSV_LIBC_API int __fstat(int, struct stat *) { errno = EBADF; return -1; }
OSV_LIBC_API int fstatat(int, const char *, struct stat *, int) { errno = ENOENT; return -1; }
OSV_LIBC_API int statx(int, const char *, int, unsigned, struct statx *) { errno = ENOENT; return -1; }
OSV_LIBC_API int statfs(const char *, struct statfs *) { errno = ENOSYS; return -1; }
OSV_LIBC_API int fstatfs(int, struct statfs *) { errno = ENOSYS; return -1; }
OSV_LIBC_API int statvfs(const char *, struct statvfs *) { errno = ENOSYS; return -1; }
OSV_LIBC_API int fstatvfs(int, struct statvfs *) { errno = ENOSYS; return -1; }

OSV_LIBC_API int access(const char *, int) { errno = ENOENT; return -1; }
OSV_LIBC_API int faccessat(int, const char *, int, int) { errno = ENOENT; return -1; }
OSV_LIBC_API int chdir(const char *) { errno = ENOENT; return -1; }
OSV_LIBC_API int fchdir(int) { errno = EBADF; return -1; }
OSV_LIBC_API int chmod(const char *, mode_t) { errno = ENOENT; return -1; }
OSV_LIBC_API int fchmod(int, mode_t) { errno = EBADF; return -1; }
OSV_LIBC_API int fchmodat(int, const char *, mode_t, int) { errno = ENOENT; return -1; }
OSV_LIBC_API int fcntl(int, int, ...) { errno = EBADF; return -1; }
// No ioctl(): what an application needs from the kernel it calls directly.
OSV_LIBC_API int flock(int, int) { errno = EBADF; return -1; }
OSV_LIBC_API int ftruncate(int, off_t) { errno = EBADF; return -1; }
OSV_LIBC_API int truncate(const char *, off_t) { errno = ENOENT; return -1; }
OSV_LIBC_API mode_t umask(mode_t) { return 0; }
OSV_LIBC_API int utimensat(int, const char *, const struct timespec *, int) { errno = ENOENT; return -1; }

OSV_LIBC_API int dup(int) { errno = EBADF; return -1; }
OSV_LIBC_API int dup2(int, int) { errno = EBADF; return -1; }
OSV_LIBC_API int dup3(int, int, int) { errno = EBADF; return -1; }
OSV_LIBC_API int __dup3(int, int, int) { errno = EBADF; return -1; }

OSV_LIBC_API int mkdir(const char *, mode_t) { errno = ENOSYS; return -1; }
OSV_LIBC_API int mkdirat(int, const char *, mode_t) { errno = ENOSYS; return -1; }
OSV_LIBC_API int mknodat(int, const char *, mode_t, dev_t) { errno = ENOSYS; return -1; }
OSV_LIBC_API int rmdir(const char *) { errno = ENOENT; return -1; }
OSV_LIBC_API int unlink(const char *) { errno = ENOENT; return -1; }
OSV_LIBC_API int unlinkat(int, const char *, int) { errno = ENOENT; return -1; }
OSV_LIBC_API int rename(const char *, const char *) { errno = ENOENT; return -1; }
OSV_LIBC_API int renameat(int, const char *, int, const char *) { errno = ENOENT; return -1; }
OSV_LIBC_API int link(const char *, const char *) { errno = ENOSYS; return -1; }
OSV_LIBC_API int symlink(const char *, const char *) { errno = ENOSYS; return -1; }
OSV_LIBC_API int symlinkat(const char *, int, const char *) { errno = ENOSYS; return -1; }
OSV_LIBC_API ssize_t readlink(const char *, char *, size_t) { errno = ENOENT; return -1; }
OSV_LIBC_API ssize_t readlinkat(int, const char *, char *, size_t) { errno = ENOENT; return -1; }

OSV_LIBC_API char *getcwd(char *buf, size_t size)
{
    if (buf && size >= 2) { buf[0] = '/'; buf[1] = '\0'; return buf; }
    errno = ERANGE;
    return nullptr;
}

OSV_LIBC_API DIR *opendir(const char *) { errno = ENOENT; return nullptr; }
OSV_LIBC_API DIR *fdopendir(int) { errno = EBADF; return nullptr; }
OSV_LIBC_API struct dirent *readdir(DIR *) { errno = EBADF; return nullptr; }
OSV_LIBC_API int closedir(DIR *) { errno = EBADF; return -1; }
OSV_LIBC_API int dirfd(DIR *) { errno = EINVAL; return -1; }

OSV_LIBC_API int poll(struct pollfd *, nfds_t, int) { errno = ENOSYS; return -1; }
OSV_LIBC_API int ppoll(struct pollfd *, nfds_t, const struct timespec *, const sigset_t *) { errno = ENOSYS; return -1; }
OSV_LIBC_API int select(int, fd_set *, fd_set *, fd_set *, struct timeval *) { errno = ENOSYS; return -1; }
OSV_LIBC_API int epoll_create(int) { errno = ENOSYS; return -1; }
OSV_LIBC_API int epoll_create1(int) { errno = ENOSYS; return -1; }
OSV_LIBC_API int epoll_ctl(int, int, int, struct epoll_event *) { errno = ENOSYS; return -1; }
OSV_LIBC_API int epoll_wait(int, struct epoll_event *, int, int) { errno = ENOSYS; return -1; }
OSV_LIBC_API int epoll_pwait(int, struct epoll_event *, int, int, const sigset_t *) { errno = ENOSYS; return -1; }
OSV_LIBC_API int eventfd2(unsigned, int) { errno = ENOSYS; return -1; }
OSV_LIBC_API int pipe(int [2]) { errno = ENOSYS; return -1; }
OSV_LIBC_API int pipe2(int [2], int) { errno = ENOSYS; return -1; }
OSV_LIBC_API int timerfd_create(int, int) { errno = ENOSYS; return -1; }
OSV_LIBC_API int timerfd_settime(int, int, const struct itimerspec *, struct itimerspec *) { errno = ENOSYS; return -1; }
OSV_LIBC_API int timerfd_gettime(int, struct itimerspec *) { errno = ENOSYS; return -1; }

OSV_LIBC_API int shmget(key_t, size_t, int) { errno = ENOSYS; return -1; }
OSV_LIBC_API void *shmat(int, const void *, int) { errno = ENOSYS; return (void *)-1; }
OSV_LIBC_API int shmdt(const void *) { errno = ENOSYS; return -1; }
OSV_LIBC_API int shmctl(int, int, struct shmid_ds *) { errno = ENOSYS; return -1; }

// Referenced by the Linux syscall table (linux.cc); there is no filesystem.
OSV_LIBC_API ssize_t sys_getdents64(int, void *, size_t) { errno = ENOSYS; return -1; }

}

// utimensat4() is declared with C++ linkage in linux.cc's syscall table.
int utimensat4(int, const char *, const struct timespec *, int) { errno = ENOSYS; return -1; }
