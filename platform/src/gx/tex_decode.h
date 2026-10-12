// GX texture formats -> RGBA8.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <vector>

namespace gpu {

struct TexImage;

enum TexFormat : uint32_t {
    TF_I4 = 0x0,
    TF_I8 = 0x1,
    TF_IA4 = 0x2,
    TF_IA8 = 0x3,
    TF_RGB565 = 0x4,
    TF_RGB5A3 = 0x5,
    TF_RGBA8 = 0x6,
    TF_CI4 = 0x8,
    TF_CI8 = 0x9,
    TF_CI14X2 = 0xA,
    TF_CMPR = 0xE,
};

enum TlutFormat : uint32_t { TLUT_IA8 = 0, TLUT_RGB565 = 1, TLUT_RGB5A3 = 2 };

// Bytes used by a (width x height) level in format `fmt`, including tile padding.
size_t texLevelSize(uint32_t fmt, uint32_t width, uint32_t height);

// Decodes one level.  `src` is big-endian texture memory; `dst` receives
// width*height RGBA8 texels (row-major, top row first).  `tlut` points at
// the palette (big-endian u16 entries) for CI formats.
void decodeTexture(uint8_t* dst, const uint8_t* src, uint32_t fmt, uint32_t width, uint32_t height, const uint16_t* tlut, uint32_t tlutFmt);

// Texture decoding off the game thread.  The recorder fills a new image's
// raw fields (TexImage::raw, rawTlut, rawFmt, rawTlutFmt, width, height,
// levels), sets decodeState to 1 and queues it; two decode threads work
// through the queue.  Whoever reads rgba first calls ensureDecoded, which
// decodes the image right there if the decode thread has not got to it yet
// (or waits while it is being decoded), so each image is decoded once.
// With PETARI_TEXDECODE=sync (and with PETARI_FRAMESAVE or PETARI_DUMP,
// which read the pixels on other paths) the recorder decodes at once.
void queueDecode(const std::shared_ptr<TexImage>& img);
// Empties `raw` (a new image's TexImage::raw) for `bytes` bytes to come,
// giving it the buffer of an image decoded earlier if one fits: its memory
// is mapped already, while a fresh one costs the game thread a page fault
// per 4 KB as it is filled.
void takeRawBuffer(std::vector<uint8_t>& raw, size_t bytes);
void ensureDecoded(TexImage& img);
bool decodeOnGameThread();

}  // namespace gpu
