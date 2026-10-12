// libgq_glsl.so: runtime GLSL to SPIR-V compiler for the Vulkan backend.
//
// A thin wrapper over glslang's C API, linked statically into a separate
// shared library built with ordinary flags, so the game library keeps its own
// (no exceptions, no RTTI, fixed image base).  The game dlopens it
// (platform/src/gx/vk_spirv.cpp) and falls back to GLES when it is missing.
#ifndef GQ_GLSL_H
#define GQ_GLSL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped when the compile options (and so the SPIR-V produced for the same
// text) change; vk_spirv.cpp folds it into its cache keys.
#define GQ_GLSL_VERSION 1

enum {
    GQ_GLSL_VERTEX = 0,
    GQ_GLSL_FRAGMENT = 1,
    GQ_GLSL_COMPUTE = 2,
};

// Compiles `source` (len bytes, need not be NUL terminated; it carries its own
// #version) for Vulkan 1.2 / SPIR-V 1.5, no optimisation.  Returns 0 on
// success and stores a malloc'ed word array in *out_words / *out_count.  On
// failure returns nonzero and the words are not set.  *out_log (optional)
// always receives a malloc'ed NUL terminated message (empty when there is
// none).  Free all of them with gq_glsl_free.  Thread safe.
int gq_glsl_compile(int stage, const char* source, size_t len, uint32_t** out_words, size_t* out_count, char** out_log);

void gq_glsl_free(void* p);

int gq_glsl_version(void);

#ifdef __cplusplus
}
#endif

#endif
