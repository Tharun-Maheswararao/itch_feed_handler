// Allocation counter: proves a code path performs no heap allocation.
//
// Linking src/alloc_counter.cpp (CMake target fh_alloc_counter) replaces
// every global operator new and delete (plain, array, sized, aligned,
// nothrow) with versions that bump a counter. Read it before and after the
// section under test:
//
//     const uint64_t before = fh::allocation_count();
//     replay();                                   // must not allocate
//     assert(fh::allocation_count() == before);
//
// Only operator new is counted (not raw malloc), which is what C++ containers
// and `new` expressions use.
#pragma once

#include <cstdint>

namespace fh {

uint64_t allocation_count() noexcept;  // total operator new calls so far
uint64_t allocated_bytes() noexcept;   // total bytes requested so far

}  // namespace fh
