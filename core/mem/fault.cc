/*
 * Where a fault goes.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/debug.hh>
#include <osv/mem/fault.hh>
#include <osv/mem/mapping.hh>
#include <osv/mem/vspace.hh>
#include <osv/trace.hh>

#include "exceptions.hh"
#include "dump.hh"
#include "libc/signal.hh"

extern const char text_start[], text_end[];

namespace mem {

TRACEPOINT(trace_vm_fault, "addr=%p, error_code=%x", uintptr_t, unsigned int);
TRACEPOINT(trace_vm_fault_sigsegv, "addr=%p, error_code=%x, %s", uintptr_t, unsigned int, const char*);
TRACEPOINT(trace_vm_fault_ret, "addr=%p, error_code=%x", uintptr_t, unsigned int);

namespace {

void sigsegv(uintptr_t addr, exception_frame *ef)
{
    void *pc = ef->get_pc();
    if (pc >= text_start && pc < text_end) {
        debug_ll("page fault outside application, addr: 0x%016lx\n", addr);
        dump_registers(ef);
        abort();
    }
    osv::handle_mmap_fault(addr, SIGSEGV, ef);
}

bool permitted(unsigned perm, unsigned error_code)
{
    if (mapping::is_page_fault_insn(error_code)) {
        return perm & perm_exec;
    }
    if (mapping::is_page_fault_write(error_code)) {
        return perm & perm_write;
    }
    return perm & perm_read;
}

}

/*
 * A fault is a region lookup and whatever that region wants done about it.
 *
 * The address is passed on to the byte. Rounding it to a page here would be
 * deciding something on the region's behalf, and a region whose units are not
 * pages needs the byte to tell which of them was reached for.
 */
void vm_fault(uintptr_t addr, exception_frame *ef)
{
    unsigned error = ef->get_error();
    trace_vm_fault(addr, error);
    if (mapping::fast_sigsegv_check(addr, ef)) {
        sigsegv(addr, ef);
        trace_vm_fault_sigsegv(addr, error, "fast");
        return;
    }

    // A region without a fault handler has nothing to answer with.
    auto *r = vspace::lookup(addr);
    if (!r || !r->ops || !r->ops->fault || !permitted(r->perm, error) ||
        !r->ops->fault(*r, addr, error)) {
        sigsegv(addr, ef);
        trace_vm_fault_sigsegv(addr, error, "slow");
        return;
    }
    trace_vm_fault_ret(addr, error);
}

}
