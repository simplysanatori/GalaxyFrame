// Device memory of the Vulkan renderer: allocations (the GPU shares the
// system memory, so every type is host visible), buffers with their own
// memory, usage statistics, and the check that mapped memory stays out of
// the game's reserved 32-bit address window (0x80000000-0xE0010000, see
// platform/linux/launcher.c).
#pragma once

#include "vk_device.h"

namespace gpu {
namespace vk {

// How the CPU uses the memory.
enum class MemUse {
    Upload,    // written by the CPU, read by the GPU: coherent, uncached (write-combined)
    Readback,  // written by the GPU, read by the CPU: cached
    Device,    // the GPU only (images)
};

// What the memory is for (statistics).
enum class MemCat { Texture, Target, Copy, FrameData, Scratch, Readback, Misc, Count };

struct Allocation {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    uint8_t* ptr = nullptr;  // mapped, or null
    MemCat cat = MemCat::Misc;
};

bool memInit();
uint32_t memoryType(uint32_t typeBits, MemUse use);
// Allocates (and maps, with `map`) memory for `req`.  Thread safe.
bool allocateMemory(const VkMemoryRequirements& req, MemUse use, MemCat cat, bool map, Allocation* out, const char* what);
void freeMemory(Allocation& a);

// A buffer with memory of its own, mapped unless `use` is Device.
struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    Allocation mem;
    VkDeviceSize size = 0;
    uint8_t* ptr = nullptr;
};
bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, MemUse use, Buffer* out, MemCat cat, const char* what);
void destroyBuffer(Buffer& b);

// Bytes and allocations per category, and the driver's view of the heap
// (VK_EXT_memory_budget), as one log line.
void logMemory(const char* why);
int64_t memoryInUse();  // bytes allocated through allocateMemory

// Logs what is mapped inside the game's reserved address window
// (/proc/self/maps) and whether any Vulkan mapping landed there.
void checkReservedWindow(const char* when);

// Validation messages so far (PETARI_VKVALIDATE).
int validationErrors();
int validationWarnings();

}  // namespace vk
}  // namespace gpu
