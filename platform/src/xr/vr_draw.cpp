// Choice of the drawing implementation behind the VR presentation
// (vr_draw.h): the one for the graphics API the OpenXR session uses.
#include "vr_draw.h"

#include "xr_gfx.h"

namespace vrdraw {

// Without the Vulkan implementation in the build there is none
// (vr_draw_vk.cpp defines the real one).
__attribute__((weak)) Draw* createVulkanDraw() { return nullptr; }

Draw& draw() {
    static Draw* const d = [] {
        Draw* v = xrgfx::gfx().api() == xrgfx::Api::Vulkan ? createVulkanDraw() : nullptr;
        return v ? v : createGlesDraw();
    }();
    return *d;
}

}  // namespace vrdraw
