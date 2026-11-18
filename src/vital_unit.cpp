// The vendored Vital DSP (src/vital_dsp) for the plugin builder, which compiles only the top-level src/*.cpp.
// The 1.x build (vst/vst.json) compiles the same files one by one. zig's arm32 target has NEON: use vital's 32-bit NEON path there.
#if defined(__arm__) && (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(NEON_ARM32)
#define NEON_ARM32 1
#endif
#include "vital_dsp/compressor.cpp"
#include "vital_dsp/linkwitz_riley_filter.cpp"
#include "vital_dsp/framework/processor.cpp"
#include "vital_dsp/framework/value.cpp"
#include "vital_dsp/framework/utils.cpp"
#include "vital_dsp/framework/operators.cpp"
#include "vital_dsp/utilities/smooth_value.cpp"
