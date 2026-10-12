# xrmock

A mock OpenXR runtime for testing the frame loop of `xr_app.cpp` without a
headset, and a script that compares what two builds do against it.

- `build.sh [out dir]` builds `libxrmock.so` and `runtime.json` (select it with
  `XR_RUNTIME_JSON=<out>/runtime.json`). Needs clang and the OpenXR headers of
  `third_party/openxr`.
- `trace-compare.sh <build A> <build B> [paired|unpaired|warp ...]` runs the
  real `galaxyquest` launcher of each build against the mock in a scratch
  `GALAXYQUEST_HOME` and diffs the traces (swapchain acquire / release, frame
  begin / end with their layers). Use `IGNORE_EXT=XR_KHR_vulkan_enable2` when
  comparing a build from before the extension was enabled.
- `xrmock.c` is the runtime (frame clock, call-order checks, trace, settings
  by `MOCK_XR_*` environment variables, listed in its header comment).
- `xrmock_gfx.h` is the seam to the graphics API; `xrmock_gles.c` is the GLES
  side (swapchain images are textures of the app's EGL context).

## For the Vulkan side (Lane D2)

Add `xrmock_vk.c` with a second `MockGfx` and list it in `kGfxTables`
(`xrmock.c`); `build.sh` gets the new file and `-lvulkan`. It must provide
the `XR_KHR_vulkan_enable2` functions (`xrGetVulkanGraphicsRequirements2KHR`,
`xrCreateVulkanInstanceKHR`, `xrGetVulkanGraphicsDevice2KHR`,
`xrCreateVulkanDeviceKHR`), recognise `XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR`,
list VkFormat numbers, and create VkImages on the app's device for the
swapchains (`XrSwapchainImageVulkan2KHR`). The extension is already offered.
`trace-compare.sh` then runs with `GFX=vk` against a GLES build for the same
trace (a Vulkan session creates the same swapchains in the same order).
