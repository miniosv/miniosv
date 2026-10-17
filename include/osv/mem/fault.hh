/*
 * Fault handling.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef OSV_MEM_FAULT_HH
#define OSV_MEM_FAULT_HH

#include <osv/mem/types.hh>

struct exception_frame;

namespace mem {

// Hand a fault to whichever region owns the address, or raise SIGSEGV.
void vm_fault(uintptr_t addr, exception_frame *ef);

}

#endif /* OSV_MEM_FAULT_HH */
