/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef EXECINFO_HH_
#define EXECINFO_HH_

namespace osv {
  // walks DWARF via compiler's _Unwind_Backtrace, fills `pc` with up to 
  // `nr` return addresses, starting at the direct caller
  // returns how many it wrote
  // [note] if `cfa` is non-null it also receives a canoncial frame address of
  // frame (useful for recursion, stack overflow)
  int unwind(void** pc, unsigned long* cfa, int nr); 
}

#endif /* EXECINFO_HH_ */
