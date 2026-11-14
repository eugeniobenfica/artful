/* artful — a black hole's light with AVX2 (this file alone is built with -mavx2). */
#define ARTF_KERNEL_AVX2 1
#define LENS_ROWS artfHoleRowsAvx2
#include "lens.inc"
