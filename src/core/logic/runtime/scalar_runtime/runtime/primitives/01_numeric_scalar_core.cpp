// Phase5 numeric scalar runtime core split by responsibility.
// Keep include order stable: these fragments form one translation unit.

#include "numeric_scalar_core_parts/01_mpfr_and_cast.cpp"
#include "numeric_scalar_core_parts/02_compare_and_normalize.cpp"
#include "numeric_scalar_core_parts/03_compute_core.cpp"
#include "numeric_scalar_core_parts/04_inplace_and_cache.cpp"
#include "numeric_scalar_core_parts/05_numeric_kind_and_repeat.cpp"
#include "numeric_scalar_core_parts/06_bench_and_tail.cpp"
