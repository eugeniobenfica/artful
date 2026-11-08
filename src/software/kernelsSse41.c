/* artful — the kernels with SSE4.1 (this file alone is built with it): x86 without AVX2. */
#define ARTF_KERNEL_SSE41 1
#define KERNELS artfKernelsSse41
#define KERNELS_NAME "SSE4.1"
#include "software/kernels.inc"
