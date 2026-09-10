/*
 * llvm-libc stdio seam for the fileless OSv kernel (LLVM_LIBC_PLAN.md,
 * Phase 3). Only built with conf_llvm_libc=1.
 *
 * llvm-libc's baremetal stdio model routes every stream operation
 * through two vendor hooks and three opaque cookie objects: stdin,
 * stdout and stderr are FILE* pointing at the cookies, and the
 * fprintf/fread/... entrypoints in the archive pass the FILE* straight
 * back into __llvm_libc_stdio_read/write. OSv implements the hooks on
 * the console, which preserves the existing fileless model (see
 * libc/io.cc): the three std streams work, unbuffered, and every other
 * FILE operation fails cleanly.
 *
 * The FILE functions llvm-libc does not provide on baremetal (fopen,
 * fclose, fflush, fileno, setvbuf, seek, ungetc, the wide-char trio
 * libstdc++ wants at link time, perror, tmpnam/tempnam) are defined
 * here with the same semantics musl had in this kernel: no-ops where
 * the operation is meaningless on an unbuffered console stream, clean
 * failures everywhere else.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <sys/types.h>

#include <osv/export.h>

#include "drivers/console.hh"

struct __llvm_libc_stdio_cookie {
    char unused; /* identity is the address */
};

extern "C" {

OSV_LIBC_API struct __llvm_libc_stdio_cookie __llvm_libc_stdin_cookie;
OSV_LIBC_API struct __llvm_libc_stdio_cookie __llvm_libc_stdout_cookie;
OSV_LIBC_API struct __llvm_libc_stdio_cookie __llvm_libc_stderr_cookie;

OSV_LIBC_API
ssize_t __llvm_libc_stdio_write(void *cookie, const char *buf, size_t size)
{
    if (cookie == &__llvm_libc_stdout_cookie ||
        cookie == &__llvm_libc_stderr_cookie) {
        console::write(buf, size);
        return size;
    }
    return -EBADF;
}

OSV_LIBC_API
ssize_t __llvm_libc_stdio_read(void *cookie, char *buf, size_t size)
{
    if (cookie == &__llvm_libc_stdin_cookie) {
        return console::read(buf, size);
    }
    return -EBADF;
}

OSV_LIBC_API
int fileno(FILE *f)
{
    if (f == stdin) {
        return 0;
    }
    if (f == stdout) {
        return 1;
    }
    if (f == stderr) {
        return 2;
    }
    errno = EBADF;
    return -1;
}

/* The console streams are unbuffered; there is never anything to flush. */
OSV_LIBC_API
int fflush(FILE *f)
{
    return 0;
}

OSV_LIBC_API
int setvbuf(FILE *f, char *buf, int type, size_t size)
{
    return 0;
}

OSV_LIBC_API
void clearerr(FILE *f)
{
}

/* No FILE beyond the three std streams can ever be created. */
OSV_LIBC_API
FILE *fopen(const char *path, const char *mode)
{
    errno = ENOENT;
    return nullptr;
}

OSV_LIBC_API
FILE *fdopen(int fd, const char *mode)
{
    errno = EBADF;
    return nullptr;
}

OSV_LIBC_API
int fclose(FILE *f)
{
    errno = EBADF;
    return EOF;
}

OSV_LIBC_API
int fseeko(FILE *f, off_t off, int whence)
{
    errno = ESPIPE;
    return -1;
}

OSV_LIBC_API
off_t ftello(FILE *f)
{
    errno = ESPIPE;
    return -1;
}

/* Fail in all cases because the only file descriptors handled
* by the libc are standard streams (non-seekable).
*/
OSV_LIBC_API
int fseek(FILE *f, long off, int whence)
{
    errno = ESPIPE;
    return -1;
}

OSV_LIBC_API
long ftell(FILE *f)
{
    errno = ESPIPE;
    return -1;
}

OSV_LIBC_API
int ungetc(int c, FILE *f)
{
    return EOF;
}

/* llvm-libc 22.1.7 ships baremetal getc.cpp but never registers its
 * CMake target, so the entrypoint cannot be enabled without patching
 * the checkout. Forward to fgetc (which is in the archive) instead. */
OSV_LIBC_API
int getc(FILE *f)
{
    return fgetc(f);
}

OSV_LIBC_API
char *tmpnam(char *s)
{
    return nullptr;
}

OSV_LIBC_API
char *tempnam(const char *dir, const char *pfx)
{
    return nullptr;
}

OSV_LIBC_API
void perror(const char *msg)
{
    const char *err = strerror(errno);
    if (msg && *msg) {
        fprintf(stderr, "%s: %s\n", msg, err);
    } else {
        fprintf(stderr, "%s\n", err);
    }
}

/* Wide-character stream stubs: nothing uses wide streams at runtime,
 * but libstdc++.a is linked --whole-archive and its wide facets
 * reference these. */
OSV_LIBC_API
wint_t getwc(FILE *f)
{
    return WEOF;
}

OSV_LIBC_API
wint_t putwc(wchar_t c, FILE *f)
{
    return WEOF;
}

OSV_LIBC_API
wint_t ungetwc(wint_t c, FILE *f)
{
    return WEOF;
}

} // extern "C"
