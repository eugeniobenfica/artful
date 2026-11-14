/* artful — a black hole's light with SSE4.1 (this file alone is built with it): x86 without AVX2. */
#define ARTF_KERNEL_SSE41 1
#define LENS_ROWS artfHoleRowsSse41
#include "lens.inc"
