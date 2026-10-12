// libgq_glsl.so: GLSL to SPIR-V through glslang's C API (see gq_glsl.h).
#include "gq_glsl.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

#define EXPORT __attribute__((visibility("default")))

static pthread_once_t sOnce = PTHREAD_ONCE_INIT;
static int sInitOk;

static void initProcess(void) {
    sInitOk = glslang_initialize_process();
}

static char* dupLog(const char* a, const char* b, const char* c) {
    size_t n = (a ? strlen(a) : 0) + (b ? strlen(b) : 0) + (c ? strlen(c) : 0);
    char* s = (char*)malloc(n + 1);
    if (!s) return NULL;
    s[0] = 0;
    if (a) strcat(s, a);
    if (b) strcat(s, b);
    if (c) strcat(s, c);
    return s;
}

EXPORT int gq_glsl_version(void) {
    return GQ_GLSL_VERSION;
}

EXPORT void gq_glsl_free(void* p) {
    free(p);
}

EXPORT int gq_glsl_compile(int stage, const char* source, size_t len, uint32_t** out_words, size_t* out_count, char** out_log) {
    if (out_log) *out_log = NULL;
    if (!source || !out_words || !out_count) {
        if (out_log) *out_log = dupLog("bad arguments", NULL, NULL);
        return 1;
    }
    pthread_once(&sOnce, initProcess);
    if (!sInitOk) {
        if (out_log) *out_log = dupLog("glslang did not initialise", NULL, NULL);
        return 1;
    }
    glslang_stage_t gs = stage == GQ_GLSL_VERTEX ? GLSLANG_STAGE_VERTEX : stage == GQ_GLSL_FRAGMENT ? GLSLANG_STAGE_FRAGMENT : GLSLANG_STAGE_COMPUTE;

    char* text = (char*)malloc(len + 1);
    if (!text) {
        if (out_log) *out_log = dupLog("out of memory", NULL, NULL);
        return 1;
    }
    memcpy(text, source, len);
    text[len] = 0;

    const int messages = GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT;
    glslang_input_t in;
    memset(&in, 0, sizeof(in));
    in.language = GLSLANG_SOURCE_GLSL;
    in.stage = gs;
    in.client = GLSLANG_CLIENT_VULKAN;
    in.client_version = GLSLANG_TARGET_VULKAN_1_2;
    in.target_language = GLSLANG_TARGET_SPV;
    in.target_language_version = GLSLANG_TARGET_SPV_1_5;
    in.code = text;
    in.default_version = 450;
    in.default_profile = GLSLANG_NO_PROFILE;
    in.messages = messages;
    in.resource = glslang_default_resource();

    int rc = 1;
    glslang_shader_t* shader = glslang_shader_create(&in);
    glslang_program_t* program = NULL;
    if (!shader) {
        if (out_log) *out_log = dupLog("glslang_shader_create failed", NULL, NULL);
        goto done;
    }
    // parse works on the preprocessed text, so this step is not optional
    if (!glslang_shader_preprocess(shader, &in)) {
        if (out_log) *out_log = dupLog(glslang_shader_get_info_log(shader), glslang_shader_get_info_debug_log(shader), NULL);
        goto done;
    }
    if (!glslang_shader_parse(shader, &in)) {
        if (out_log) *out_log = dupLog(glslang_shader_get_info_log(shader), glslang_shader_get_info_debug_log(shader), NULL);
        goto done;
    }
    program = glslang_program_create();
    glslang_program_add_shader(program, shader);
    if (!glslang_program_link(program, messages)) {
        if (out_log) *out_log = dupLog(glslang_program_get_info_log(program), glslang_program_get_info_debug_log(program), NULL);
        goto done;
    }
    glslang_spv_options_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.disable_optimizer = true;  // the driver's compiler optimises; keeps SPIRV-Tools out of the picture
    glslang_program_SPIRV_generate_with_options(program, gs, &opts);
    size_t n = glslang_program_SPIRV_get_size(program);
    if (!n) {
        const char* m = glslang_program_SPIRV_get_messages(program);
        if (out_log) *out_log = dupLog("no SPIR-V generated: ", m, NULL);
        goto done;
    }
    uint32_t* words = (uint32_t*)malloc(n * 4);
    if (!words) {
        if (out_log) *out_log = dupLog("out of memory", NULL, NULL);
        goto done;
    }
    glslang_program_SPIRV_get(program, words);
    *out_words = words;
    *out_count = n;
    if (out_log) {
        const char* m = glslang_program_SPIRV_get_messages(program);
        *out_log = dupLog(glslang_shader_get_info_log(shader), m, NULL);
    }
    rc = 0;
done:
    if (program) glslang_program_delete(program);
    if (shader) glslang_shader_delete(shader);
    free(text);
    return rc;
}
