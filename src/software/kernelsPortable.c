/* artful — the kernels for any processor: what the compiler makes of eight lanes in a loop. */
#define KERNELS artfKernelsPortable
#define KERNELS_NAME "portable"
#define KERNELS_SIMD ARTF_SIMD_PORTABLE
#include "software/kernels.inc"
