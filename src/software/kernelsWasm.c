/* artful — the kernels with WebAssembly's SIMD (this file alone is built with -msimd128): what browsers have had since
 * 2021. */
#define ARTF_KERNEL_WASM 1
#define KERNELS artfKernelsWasm
#define KERNELS_NAME "WebAssembly SIMD"
#define KERNELS_SIMD ARTF_SIMD_128
#include "software/kernels.inc"
