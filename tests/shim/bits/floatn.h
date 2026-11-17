/* check-host shim: the host x86_64 glibc floatn.h declares _Float128 via __float128, which the aarch64 target
 * rejects. No _FloatN types are used here, so declare none. */
#pragma once
#define __HAVE_FLOAT128 0
#define __HAVE_DISTINCT_FLOAT128 0
#define __HAVE_FLOAT64X 0
#define __HAVE_FLOAT64X_LONG_DOUBLE 0
#include <bits/floatn-common.h>
