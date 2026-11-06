/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/execinfo.h>
#include <osv/execinfo.hh>

#include <unwind.h>

struct worker_arg {
    void **buffer;
    unsigned long *cfas;  // optional out-array for canonical frame addresses
    int size;
    int pos;
    unsigned long prevcfa;
};

static _Unwind_Reason_Code worker (struct _Unwind_Context *ctx, void *data)
{
    struct worker_arg *arg = (struct worker_arg *) data;
    if (arg->pos >= 0) {
        arg->buffer[arg->pos] = (void *)_Unwind_GetIP(ctx);
        unsigned long cfa = _Unwind_GetCFA(ctx);
        if (arg->cfas) {
            arg->cfas[arg->pos] = cfa;
        }
        if (arg->pos > 0 && arg->buffer[arg->pos-1] == arg->buffer[arg->pos]
                         && arg->prevcfa == cfa) {
            return _URC_END_OF_STACK;
        }
        arg->prevcfa = cfa;
    }
    if (++arg->pos == arg->size) {
        return _URC_END_OF_STACK;
    }
    return _URC_NO_REASON;
}

// pos starts at -1 so the callback skips the wrapper's own frame; the
// returned buffer starts at the direct caller. _Unwind_Backtrace may leave
// a trailing null "top-most caller" that we trim.
int osv::unwind(void **pc, unsigned long *cfas, int nr)
{
    worker_arg arg { pc, cfas, nr, -1, 0 };
    _Unwind_Backtrace(worker, &arg);
    if (arg.pos > 0 && pc[arg.pos-1] == nullptr) {
        arg.pos--;
    }
    return arg.pos > 0 ? arg.pos : 0;
}

#include <osv/demangle.hh>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

void backtrace_symbols_fd(void *const *addrs, int len, int fd)
{
    for (int i = 0; i < len; i++) {
        char name[1024];
        // FIXME: I think Linux also shows the name of the object
        osv::lookup_name_demangled(addrs[i], name, sizeof(name));
        auto remain = strlen(name);
        snprintf(name + remain, sizeof(name) - remain, " [%p]\n", addrs[i]);
        remain = strlen(name);
        while (remain > 0) {
            auto n = write(fd, name, remain);
            if (n < 0) {
                return; // write error, nothing better we can do...
            }
            remain -= n;
        }
    }
}
char **backtrace_symbols(void *const *addrs, int len)
{
    // We need to return a newly allocated char **, i.e., an array of len
    // pointers. We put the strings we point to after this array, allocated
    // together, so when the user free()s the array, the strings will also
    // be freed.
    size_t used = len * sizeof(char*);
    size_t bufsize = used + 1;
    char *buf = (char*)malloc(bufsize);
    char **ret = (char **)buf;
    for (int i = 0; i < len; i++) {
        char name[1024];
        // FIXME: I think Linux also shows the name of the object
        osv::lookup_name_demangled(addrs[i], name, sizeof(name));
        auto remain = strlen(name);
        snprintf(name + remain, sizeof(name) - remain, " [%p]", addrs[i]);
        remain = strlen(name);
        if (remain >= (bufsize - used)) {
            bufsize = bufsize*2 + remain + 1;
            buf = (char*)realloc(buf, bufsize);
            ret = (char **)buf;
        }
        ret[i] = buf + used;
        strcpy(buf + used, name);
        used += remain + 1;
    }
    return ret;
}
