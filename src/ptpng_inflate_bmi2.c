/* The general inflater compiled again with BMI2 code generation (see the
 * notes at the top of ptpng_inflate.c). Built only for x86 targets, with
 * -mbmi2 or /arch:AVX2, and selected at run time. */
#define PTPNG_INFLATE_BMI2 1
#include "ptpng_inflate.c"
