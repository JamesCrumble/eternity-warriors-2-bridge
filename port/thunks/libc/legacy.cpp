#include <math.h>
#include "platform.h"

/*
 * bionic numbers the FP_* classes as bit flags (FP_INFINITE 0x01, FP_NAN 0x02,
 * FP_NORMAL 0x04, FP_SUBNORMAL 0x08, FP_ZERO 0x10); glibc counts them
 * (FP_NAN 0, FP_INFINITE 1, FP_ZERO 2, FP_SUBNORMAL 3, FP_NORMAL 4). The guest
 * tests the bits - Unity 4.5's PhysX does `tst r0, #3` for "NaN or infinite" -
 * so a glibc answer turns every 0.0f into an invalid value.
 */
static int bionic_fp_class(int c) {
  switch (c) {
  case FP_INFINITE:  return 0x01;
  case FP_NAN:       return 0x02;
  case FP_NORMAL:    return 0x04;
  case FP_SUBNORMAL: return 0x08;
  default:           return 0x10;   /* FP_ZERO */
  }
}

extern "C" ABI_ATTR int __fpclassifyd_impl(double d) {
  return bionic_fp_class(fpclassify(d));
}

extern "C" ABI_ATTR int __fpclassifyf_impl(float f) {
  return bionic_fp_class(fpclassify(f));
}

extern "C" ABI_ATTR int __isinf_impl(double d) {
  return isinf(d);
}

extern "C" ABI_ATTR int __isinff_impl(float f) {
  return isinf(f);
}

extern "C" ABI_ATTR int __isnan_impl(double d) {
  return isnan(d);
}

extern "C" ABI_ATTR int __isnanf_impl(float f) {
  return isnan(f);
}

extern "C" ABI_ATTR int __isfinite_impl(double d) {
  return isfinite(d);
}

extern "C" ABI_ATTR int __isfinitef_impl(float f) {
  return isfinite(f);
}

extern "C" ABI_ATTR int __isnormal(double d) {
  return isnormal(d);
}

extern "C" ABI_ATTR int __isnormalf_impl(float f) {
  return isnormal(f);
}

extern "C" ABI_ATTR int __fpclassifyl_impl(long double ld) {
  return bionic_fp_class(fpclassify(ld));
}

extern "C" ABI_ATTR int __isinfl_impl(long double ld) {
  return isinf(ld);
}

extern "C" ABI_ATTR int __isnanl_impl(long double ld) {
  return isnan(ld);
}

extern "C" ABI_ATTR int __isfinitel_impl(long double ld) {
  return isfinite(ld);
}

extern "C" ABI_ATTR int __isnormall_impl(long double ld) {
  return isnormal(ld);
}

extern "C" ABI_ATTR int __isnormal_impl(double __x) {
    return __builtin_isnormal(__x);
}

extern "C" ABI_ATTR int __fpclassify_impl(double f) __attribute__((alias("__fpclassifyd_impl")));
extern "C" ABI_ATTR int isinf_impl(double d) __attribute__((alias("__isinf_impl")));
extern "C" ABI_ATTR int isinff_impl(float f) __attribute__((alias("__isinff_impl")));
extern "C" ABI_ATTR int isnan_impl(double d) __attribute__((alias("__isnan_impl")));
extern "C" ABI_ATTR int isnanf_impl(float f) __attribute__((alias("__isnanf_impl")));
extern "C" ABI_ATTR int isfinite_impl(double d) __attribute__((alias("__isfinite_impl")));
extern "C" ABI_ATTR int isfinitef_impl(float f) __attribute__((alias("__isfinitef_impl")));
extern "C" ABI_ATTR int isnormal_impl(double f) __attribute__((alias("__isnormal_impl")));
extern "C" ABI_ATTR int isnormalf_impl(float f) __attribute__((alias("__isnormalf_impl")));
extern "C" ABI_ATTR int isinfl_impl(long double ld) __attribute__((alias("__isinfl_impl")));
extern "C" ABI_ATTR int isnanl_impl(long double ld) __attribute__((alias("__isnanl_impl")));
extern "C" ABI_ATTR int isfinitel_impl(long double ld) __attribute__((alias("__isfinitel_impl")));
extern "C" ABI_ATTR int isnormall_impl(long double ld) __attribute__((alias("__isnormall_impl")));
