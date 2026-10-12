// Choice of the renderer backend (gpu_renderer.h).
#include "gpu_renderer.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <string>

#include "gpu.h"
#include "port/port.h"

namespace gpu {

// Without the Vulkan backend in the build there is none (vk_renderer.cpp
// defines the real one).
__attribute__((weak)) Renderer* createVulkanRenderer() { return nullptr; }

namespace {

std::string sShaderCachePath;
std::string sIniChoice;  // `renderer = ...` from the ini file
bool sCreated = false;
const char* sForcedGles = nullptr;  // forceGles(): why

bool isVulkan(const char* name) { return !strcasecmp(name, "vk") || !strcasecmp(name, "vulkan"); }
bool isGles(const char* name) { return !strcasecmp(name, "gles") || !strcasecmp(name, "gl"); }

// What PETARI_GFX or the ini file asks for, and which of the two said it.
const char* wanted(const char** from) {
    const char* want = debugEnv("PETARI_GFX");
    *from = "PETARI_GFX";
    if (!want && !sIniChoice.empty()) {
        want = sIniChoice.c_str();
        *from = "renderer";
    }
    return want;
}

Renderer* createRenderer() {
    sCreated = true;
    const char* from = nullptr;
    const char* want = wanted(&from);
    if (want && isVulkan(want) && sForcedGles) {
        port_log("gpu: %s = %s, but %s; using GLES", from, want, sForcedGles);
        return createGlesRenderer();
    }
    if (want && isVulkan(want)) {
        if (Renderer* r = createVulkanRenderer()) {
            port_log("gpu: Vulkan renderer (%s = %s)", from, want);
            return r;
        }
        port_log("gpu: %s = %s, but this build has no Vulkan renderer; using GLES", from, want);
    } else if (want && !isGles(want)) {
        port_log("gpu: unknown renderer \"%s\" (%s); using GLES", want, from);
    } else if (want) {
        port_log("gpu: GLES renderer (%s = %s)", from, want);
    }
    return createGlesRenderer();
}

}  // namespace

Renderer& renderer() {
    static Renderer* const r = createRenderer();
    return *r;
}

void selectRenderer(const char* iniPath) {
    if (sCreated) {
        port_log("gpu: selectRenderer after the renderer was created; ignored");
        return;
    }
    sIniChoice.clear();
    FILE* f = iniPath ? fopen(iniPath, "r") : nullptr;
    if (!f) {
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char value[64];
        if (sscanf(line, " renderer = %63[A-Za-z0-9_]", value) == 1) {
            sIniChoice = value;
        }
    }
    fclose(f);
}

Api requestedApi() {
    const char* from = nullptr;
    const char* want = wanted(&from);
    return want && isVulkan(want) ? Api::Vulkan : Api::Gles;
}

void forceGles(const char* why) {
    if (sCreated) {
        port_log("gpu: forceGles after the renderer was created; ignored");
        return;
    }
    sForcedGles = why;
}

void setShaderCachePath(const char* path) { sShaderCachePath = path ? path : ""; }
const char* shaderCachePath() { return sShaderCachePath.c_str(); }

}  // namespace gpu
