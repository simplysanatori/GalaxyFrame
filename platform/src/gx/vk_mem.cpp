// Device memory of the Vulkan renderer (vk_mem.h).
#include "vk_mem.h"

#include <stdio.h>
#include <string.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "gpu.h"
#include "port/heap_routing.h"
#include "port/port.h"

namespace gpu {
namespace vk {

namespace {

// The game's reserved window (platform/linux/launcher.c, headless.c).
const uintptr_t kWindowLo = 0x80000000u;
const uintptr_t kWindowHi = 0xE0010000u;

const char* const kCatNames[(int)MemCat::Count] = {"textures", "targets", "copies", "frame data", "scratch", "readback", "other"};

std::atomic<int64_t> sBytes[(int)MemCat::Count];
std::atomic<int64_t> sCount[(int)MemCat::Count];
std::atomic<int64_t> sPeak{0};
std::atomic<int64_t> sTotal{0};
std::atomic<int> sMaps{0};
std::atomic<int> sMapsInWindow{0};
std::atomic<uintptr_t> sMapLo{~(uintptr_t)0}, sMapHi{0};
uint32_t sTypeUpload = UINT32_MAX, sTypeReadback = UINT32_MAX, sTypeDevice = UINT32_MAX;

uint32_t pickType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    const VkPhysicalDeviceMemoryProperties& mp = device().memProps;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) && (f & want) == want && !(f & avoid)) return i;
    }
    return UINT32_MAX;
}

void noteMapping(void* p, VkDeviceSize size, const char* what) {
    uintptr_t lo = (uintptr_t)p, hi = lo + (uintptr_t)size;
    int n = ++sMaps;
    uintptr_t cur = sMapLo.load();
    while (lo < cur && !sMapLo.compare_exchange_weak(cur, lo)) {
    }
    cur = sMapHi.load();
    while (hi > cur && !sMapHi.compare_exchange_weak(cur, hi)) {
    }
    bool inWindow = lo < kWindowHi && hi > kWindowLo;
    if (inWindow) {
        ++sMapsInWindow;
        port_log("vk: MAPPED MEMORY INSIDE THE GAME'S RESERVED WINDOW: %s %llu KB at %p-%p", what, (unsigned long long)(size >> 10), p, (void*)hi);
    } else if (n <= 12) {
        port_log("vk: mapped %s %llu KB at %p-%p (outside the game window 0x%lx-0x%lx)", what, (unsigned long long)(size >> 10), p, (void*)hi,
                 (unsigned long)kWindowLo, (unsigned long)kWindowHi);
    }
}

}  // namespace

bool memInit() {
    const VkPhysicalDeviceMemoryProperties& mp = device().memProps;
    sTypeUpload = pickType(~0u, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (sTypeUpload == UINT32_MAX) sTypeUpload = pickType(~0u, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    sTypeReadback = pickType(~0u, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    if (sTypeReadback == UINT32_MAX) sTypeReadback = sTypeUpload;
    sTypeDevice = pickType(~0u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT);
    std::string types;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        char b[48];
        snprintf(b, sizeof(b), "%s%u:%x", i ? " " : "", i, mp.memoryTypes[i].propertyFlags);
        types += b;
    }
    port_log("vk: %u memory heaps (%.1f GB), types %s; upload %u, readback %u, device %u", mp.memoryHeapCount,
             mp.memoryHeapCount ? mp.memoryHeaps[0].size / 1073741824.0 : 0.0, types.c_str(), sTypeUpload, sTypeReadback, sTypeDevice);
    if (sTypeUpload == UINT32_MAX || sTypeDevice == UINT32_MAX) {
        port_log("vk: no host-visible coherent memory type");
        return false;
    }
    return true;
}

uint32_t memoryType(uint32_t typeBits, MemUse use) {
    VkMemoryPropertyFlags want = 0, avoid = 0;
    switch (use) {
    case MemUse::Upload:
        if (typeBits & (1u << sTypeUpload)) return sTypeUpload;
        want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        break;
    case MemUse::Readback:
        if (typeBits & (1u << sTypeReadback)) return sTypeReadback;
        want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        break;
    case MemUse::Device:
        if (typeBits & (1u << sTypeDevice)) return sTypeDevice;
        want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        avoid = VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
        break;
    }
    uint32_t t = pickType(typeBits, want, avoid);
    return t != UINT32_MAX ? t : pickType(typeBits, 0, VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT);
}

bool allocateMemory(const VkMemoryRequirements& req, MemUse use, MemCat cat, bool map, Allocation* out, const char* what) {
    *out = Allocation();
    uint32_t type = memoryType(req.memoryTypeBits, use);
    if (type == UINT32_MAX) {
        port_log("vk: no memory type for %s (bits %x)", what, req.memoryTypeBits);
        return false;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    VkResult r = vkAllocateMemory(device().device, &ai, nullptr, &out->memory);
    if (r != VK_SUCCESS) {
        port_log("vk: cannot allocate %llu KB for %s (%d)", (unsigned long long)(req.size >> 10), what, (int)r);
        out->memory = VK_NULL_HANDLE;
        return false;
    }
    out->size = req.size;
    out->cat = cat;
    if (map) {
        void* p = nullptr;
        r = vkMapMemory(device().device, out->memory, 0, VK_WHOLE_SIZE, 0, &p);
        if (r != VK_SUCCESS) {
            port_log("vk: cannot map %s (%d)", what, (int)r);
            vkFreeMemory(device().device, out->memory, nullptr);
            *out = Allocation();
            return false;
        }
        out->ptr = (uint8_t*)p;
        noteMapping(p, req.size, what);
    }
    sBytes[(int)cat] += (int64_t)req.size;
    sCount[(int)cat]++;
    int64_t total = (sTotal += (int64_t)req.size);
    int64_t peak = sPeak.load();
    while (total > peak && !sPeak.compare_exchange_weak(peak, total)) {
    }
    return true;
}

void freeMemory(Allocation& a) {
    if (!a.memory) return;
    // Freeing unmaps.
    vkFreeMemory(device().device, a.memory, nullptr);
    sBytes[(int)a.cat] -= (int64_t)a.size;
    sCount[(int)a.cat]--;
    sTotal -= (int64_t)a.size;
    a = Allocation();
}

bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, MemUse use, Buffer* out, MemCat cat, const char* what) {
    *out = Buffer();
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size ? size : 16;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult r = vkCreateBuffer(device().device, &bi, nullptr, &out->buffer);
    if (r != VK_SUCCESS) {
        port_log("vk: cannot create a %llu KB buffer for %s (%d)", (unsigned long long)(size >> 10), what, (int)r);
        out->buffer = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device().device, out->buffer, &req);
    if (!allocateMemory(req, use, cat, use != MemUse::Device, &out->mem, what) ||
        vkBindBufferMemory(device().device, out->buffer, out->mem.memory, 0) != VK_SUCCESS) {
        vkDestroyBuffer(device().device, out->buffer, nullptr);
        freeMemory(out->mem);
        *out = Buffer();
        return false;
    }
    out->size = bi.size;
    out->ptr = out->mem.ptr;
    setName(VK_OBJECT_TYPE_BUFFER, (uint64_t)out->buffer, what);
    return true;
}

void destroyBuffer(Buffer& b) {
    if (b.buffer) vkDestroyBuffer(device().device, b.buffer, nullptr);
    freeMemory(b.mem);
    b = Buffer();
}

int64_t memoryInUse() { return sTotal.load(); }

void logMemory(const char* why) {
    char line[512];
    int n = snprintf(line, sizeof(line), "vk: memory (%s): %.1f MB in use (peak %.1f MB):", why, sTotal.load() / 1048576.0, sPeak.load() / 1048576.0);
    for (int c = 0; c < (int)MemCat::Count && n < (int)sizeof(line); c++) {
        n += snprintf(line + n, sizeof(line) - n, " %s %.1f MB/%lld", kCatNames[c], sBytes[c].load() / 1048576.0, (long long)sCount[c].load());
    }
    if (device().memoryBudget && vkGetPhysicalDeviceMemoryProperties2 && n < (int)sizeof(line)) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 mp2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
        mp2.pNext = &budget;
        vkGetPhysicalDeviceMemoryProperties2(device().phys, &mp2);
        snprintf(line + n, sizeof(line) - n, "; driver: heap usage %.1f MB of %.1f MB budget", budget.heapUsage[0] / 1048576.0,
                 budget.heapBudget[0] / 1048576.0);
    }
    port_log("%s", line);
}

void checkReservedWindow(const char* when) {
    PortHostAllocScope hostAlloc;
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return;
    struct Group {
        std::string name;
        int count;
        uint64_t bytes;
    };
    std::vector<Group> groups;
    char line[512];
    int foreign = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned long lo = 0, hi = 0;
        char perms[8] = {}, name[400] = {};
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %399[^\n]", &lo, &hi, perms, name) < 3) continue;
        if (!(lo < kWindowHi && hi > kWindowLo)) continue;
        std::string n = name[0] ? name : "[anonymous]";
        // Only the game's own mappings belong here: the reservation, the
        // emulated memory, libgame.so.
        bool own = n.find("petari") != std::string::npos || n.find("libgame") != std::string::npos || n == "[anonymous]" ||
                   n.find("memfd:") != std::string::npos || n.find("/dev/shm") != std::string::npos;
        if (!own || n.find("/dev/dri") != std::string::npos || n.find("kgsl") != std::string::npos) foreign++;
        bool merged = false;
        for (Group& g : groups) {
            if (g.name == n) {
                g.count++;
                g.bytes += hi - lo;
                merged = true;
            }
        }
        if (!merged) groups.push_back(Group{n, 1, hi - lo});
    }
    fclose(f);
    std::string text;
    for (Group& g : groups) {
        char b[512];
        snprintf(b, sizeof(b), "%s%s x%d %.0f MB", text.empty() ? "" : "; ", g.name.c_str(), g.count, g.bytes / 1048576.0);
        text += b;
    }
    uintptr_t lo = sMapLo.load(), hi = sMapHi.load();
    port_log("vk: reserved window 0x%lx-0x%lx (%s): %s; %d other mappings there; %d Vulkan maps so far at %p-%p, %d inside the window", (unsigned long)kWindowLo,
             (unsigned long)kWindowHi, when, text.c_str(), foreign, sMaps.load(), (void*)(lo == ~(uintptr_t)0 ? 0 : lo), (void*)hi, sMapsInWindow.load());
}

}  // namespace vk
}  // namespace gpu
