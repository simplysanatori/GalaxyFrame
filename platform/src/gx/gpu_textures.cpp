// Textures released by any thread (a TexImage dies on whichever thread drops
// the last reference), deleted later by the renderer's thread.  Shared by
// the renderer backends: the handles are those of the backend in use.
#include <mutex>
#include <vector>

#include "gx_record.h"
#include "port/heap_routing.h"

namespace gpu {

static std::mutex sDeleteLock;
static std::vector<TexHandle, PortHostAllocator<TexHandle>> sDeleteTextures;  // pushed on any thread, freed on the render one

void queueTextureDelete(TexHandle tex) {
    if (tex) {
        std::lock_guard<std::mutex> lock(sDeleteLock);
        sDeleteTextures.push_back(tex);
    }
}

TexImage::~TexImage() {
    queueTextureDelete(gpuTex);
}

void drainTextureDeletes(void (*fn)(const TexHandle* list, size_t count, void* ctx), void* ctx) {
    std::vector<TexHandle, PortHostAllocator<TexHandle>> list;
    {
        std::lock_guard<std::mutex> lock(sDeleteLock);
        list.swap(sDeleteTextures);
    }
    if (!list.empty()) {
        fn(list.data(), list.size(), ctx);
    }
}

}  // namespace gpu
