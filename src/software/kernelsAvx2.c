/* artful — the kernels with AVX2 (this file alone is built with -mavx2). */
#define ARTF_KERNEL_AVX2 1
#define KERNELS artfKernelsAvx2
#define KERNELS_NAME "AVX2"
#include "software/kernels.inc"
