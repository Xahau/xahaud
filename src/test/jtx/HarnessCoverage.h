#pragma once
//------------------------------------------------------------------------------
// HarnessCoverage — test-harness vocabulary for Antithesis guideposts.
//
// Keep the SDK gate in one place by routing through the existing
// instrumentation wrapper. The macro is vocabulary only; scenario pass/fail
// stays in BEAST_EXPECT and test assertions.
//------------------------------------------------------------------------------
#include <xrpl/beast/utility/instrumentation.h>

#ifdef ENABLE_VOIDSTAR
#define XRPL_HARNESS_COVER(name, cond, ...) SOMETIMES(cond, name, __VA_ARGS__)
#else
#define XRPL_HARNESS_COVER(name, cond, ...)
#endif
