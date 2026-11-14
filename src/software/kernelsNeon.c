/* artful — the kernels with NEON: every 64-bit ARM processor has it. */
#define ARTF_KERNEL_NEON 1
#define KERNELS artfKernelsNeon
#define KERNELS_NAME "NEON"
#define KERNELS_SIMD ARTF_SIMD_128
#include "software/kernels.inc"
