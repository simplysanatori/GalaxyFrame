// Preparation of a recorded frame for replay, shared by the renderer
// backends: the recording's register writes are replayed once into batches
// (Item), shader keys and the storage buffer contents the generated shaders
// read (gl_shadergen.h).  Nothing here touches a graphics API; the backends
// map the neutral state below to their own.
#pragma once

#include <stdint.h>

#include <memory>
#include <unordered_map>
#include <vector>

#include "gl_shadergen.h"
#include "gpu.h"
#include "gpu_renderer.h"

namespace gpu {

enum ItemType : u8 { IT_DRAW, IT_COPY, IT_MARKER };

// Primitive topology of a batch (GX quads, strips and fans become triangle
// lists, line strips line lists).
enum class Prim : u8 { Triangles, Lines, Points };

enum class BlendFactor : u8 {
    Zero,
    One,
    SrcColor,
    OneMinusSrcColor,
    DstColor,
    OneMinusDstColor,
    SrcAlpha,
    OneMinusSrcAlpha,
    DstAlpha,
    OneMinusDstAlpha,
    ConstantAlpha,  // the constant's alpha (destination alpha writes)
};

enum class BlendOp : u8 {
    Add,              // src * srcFactor + dst * dstFactor
    ReverseSubtract,  // dst * dstFactor - src * srcFactor
};

enum class CompareFunc : u8 { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };

// Faces culled while Item::cull is set.
enum class CullFace : u8 { Front, Back, FrontAndBack };

// IT_DRAW is a batch: consecutive GX draws with identical render state,
// drawn with one indexed draw.  Per-draw transforms and constants are
// reached through each vertex's draw record.
struct Item {
    ItemType type;
    // Batch
    u32 uid;           // index into Prepared::uids
    u32 program;       // the backend's program / pipeline, resolved by the render thread (0: none yet)
    Prim prim;
    u8 useVr, hud;
    u8 player;         // between the player markers (SpaceWarp stencil tag)
    u32 flags;
    u32 firstWord;     // first vertex, as a word offset into the frame's vertex data
    u32 endWord;       // end of the last merged draw's vertices
    u32 firstOrdinal;  // first vertex ordinal (record index stream)
    u32 idxOffset, idxCount;
    u32 draws;         // GX draws in the batch
    // The batch's runs of draws with one pixel state (Prepared::pixRuns
    // [pixRunFirst, pixRunFirst + pixRunCount)), for a backend that draws
    // each run with its pixel state's base as a constant (Vulkan).
    u32 pixRunFirst, pixRunCount;
    u32 texMask;       // texture units the program samples
    // Render state
    u8 blend, colorMask, depthTest, depthWrite, cull, dstAlpha;
    BlendFactor srcRGB, dstRGB;
    BlendOp eq;
    CompareFunc depthFunc;
    CullFace cullFace;
    float dstAlphaValue;
    float vp[4];  // x, y, w, h in native EFB coordinates (top-left origin)
    int sc[4];    // scissor left, top, right, bottom (native EFB)
    u32 tex[8];   // TexImage index, or 0x80000000 | EFB copy address >> 5; ~0u unused
    u32 texMode0[8], texMode1[8];
    // Copy: BP 0x49..0x52 values plus PE state at that point.
    u32 copy[10];
    u32 copyPixFmt, copyCmode, copyZmode;
    u32 marker;
};

// EFB copies land in textures keyed like texture memory: by destination
// address, one set per render slot (flat view, left eye, right eye), so a
// texture read before this frame's copy to its address still holds the
// previous frame's copy of the same view, as on the console.  The EFB->XFB
// copy (flat view only) has a key of its own.
inline u64 copyKey(u32 slot, u32 addr) { return ((u64)slot << 32) | (addr >> 5); }
const u64 kXfbCopyKey = ~0ull;
// The render slot of a replay: 0 flat view, 1 left eye, 2 right eye.
inline u32 copySlotFor(const EyeView* eye) { return eye ? 1 + (eye->index & 1) : 0; }
// Copies of addresses (or views) not copied to for this many frames are
// dropped: the recorder forgets a copy after 8 frames, and a scene's
// buffers are copied to every frame.
const u64 kCopyKeepFrames = 120;

// A run of a batch's draws with one pixel state: its first index (into
// Prepared::indices; it ends where the next run of the batch starts, or with
// the batch) and the pixel state's base in states[] (vec4s), as the draw
// records' kRecPixel.
struct PixRun {
    u32 idxStart;
    u32 pixBase;
};

// A frame turned into batches and shader data (gl_shadergen.h).  Built from
// a recording on any thread; uploaded and replayed by the render thread.
struct Prepared {
    std::shared_ptr<const Frame> frame;
    u64 number = 0;
    std::vector<Item> items;
    std::vector<ShaderUid> uids;
    std::vector<u32> cutProgs;  // cutaway variants of uids' programs, 0 until resolved
    std::vector<float> rows;    // vec4 matrix rows
    std::vector<u32> recs;      // kRecWords per draw record
    std::vector<float> states;  // vec4 light / projection / pixel states
    std::vector<u32> vtxRec;    // draw record per vertex ordinal
    std::vector<u32> indices;
    std::vector<PixRun> pixRuns;  // see Item::pixRunFirst
    CameraInfo cam;
    u32 clearAR = 0, clearGB = 0, clearZ = 0xFFFFFF;
    u32 nativeW = 640, nativeH = 456;
};

// One line per batch / copy / marker (PETARI_GLITEMS).
void logItems(const Prepared& p);

// Replays a recording's register writes to build a Prepared.
struct Builder {
    std::vector<Item> items;
    std::vector<ShaderUid> uids;
    std::vector<float> rows, states;
    std::vector<u32> recs, vtxRec, indices;
    std::vector<PixRun> pixRuns;
    CameraInfo cam;
    u32 clearAR = 0, clearGB = 0, clearZ = 0xFFFFFF;
    u32 nativeW = 640, nativeH = 456;

    // Shadows of the GPU registers.
    u32 bp[256];
    u32 tevReg[8], tevKonst[8];
    u32 xfRegs[256];
    float xfMem[0x1000];
    u32 boundTex[8];
    bool inHud = false;
    bool inSky = false;     // between sky markers: drawn around the game camera
    bool inPlayer = false;  // between player markers
    bool inPointer = false; // between pointer markers: the pointer's 2D cursor
    // The shader key of the last draw, reused while nothing it depends on
    // changed (hashing a key for each of ~40,000 draws was most of the
    // preparation time).
    bool uidDirty = true;
    u32 lastUid = 0, lastUidFlags = ~0u;
    bool lastUidHud = false;
    // Draws in a row with no command in between (the strips of a model's
    // display list) share all state: a draw after one of the same format
    // joins its batch without the state being worked out again.
    bool drawStateDirty = true;
    bool lastDrawReusable = false;  // not with a shader key per draw
    u32 lastDrawFlags = 0, lastDrawRec = 0;
    Prim lastDrawPrim = Prim::Triangles;

    std::unordered_map<u64, std::vector<u32>> stateIndex;  // content hash -> bases
    std::unordered_map<ShaderUid, u32, ShaderUidHash> uidIndex;
    // Snapshot bases in rows[] of each 3-row matrix slot (~0u: changed since).
    u32 posSlot[22], nrmSlot[11], postSlot[64];
    u32 lightBase = ~0u, projBase = ~0u, pixBase = ~0u;

    void build(std::shared_ptr<const Frame> frame, Prepared& out);
    u32 screenTexGens(const Frame& f, const Item& it, u32 first, const ShaderUid& uid);
    u32 uidFor(const ShaderUid& uid);
    u32 appendRows(const float* src, int n, int stride);
    u32 addState(const float* data, size_t vec4s);
    void invalidateXf(u32 a0, u32 a1);
    u32 drawRecord(bool useVr, bool sky, bool pointer, bool dualTex, u32 numTexGens);
    void appendIndices(u32 kind, u32 base, u32 count);
    void notePixRun(Item& b, u32 rec);
    void fillLights(LightState& b);
    void fillProj(ProjState& b);
    void fillPix(PixelState& b);
};

// ---------------------------------------------------------------------------
// Replay helpers: what a backend does for an item, worked out the same way
// for every backend.  Rectangles are in target pixels with GL's bottom-left
// origin (the Vulkan backend uses a standard viewport, so its images have
// the same memory layout and the same rectangles apply).
// ---------------------------------------------------------------------------

// The per-render camera block (EyeBlock, uniform binding 2 under GLES) for
// a flat replay (eye null), a VR eye or one picture of a stereo pair.
void fillEyeBlock(const EyeView* eye, EyeBlock& eb);

// Viewport and scissor of a batch drawn into a w x h target.
struct DrawRect {
    int vp[4];     // x, y, width, height
    bool scissor;  // false: no scissor test (VR eye draws cover the target)
    int sc[4];     // x, y, width, height
};
void drawRect(const Item& it, int targetW, int targetH, u32 nativeW, u32 nativeH, const EyeView* eye, bool vrEye, DrawRect& out);

// Blend state of a batch.  The HUD target keeps premultiplied coverage in
// alpha for the compositor; destination alpha writes the constant alpha.
struct BlendState {
    bool on;
    BlendFactor srcRGB, dstRGB, srcA, dstA;
    BlendOp eqRGB, eqA;
    float constAlpha;  // used where a factor is ConstantAlpha
    int colorMask;     // bit 0 R, 1 G, 2 B, 3 A
};
void blendState(const Item& it, bool toHudTarget, BlendState& out);

// An EFB copy (IT_COPY) from a srcW x srcH target.
struct CopyOp {
    u32 id;
    bool toXfb, clear;
    int sx0, sy0, sx1, sy1;  // source rectangle (also the clear's rectangle)
    int dw, dh;              // destination size (texture copies: at least 1, halved for half-size copies)
    u32 fmt;                 // texture copy format (BP 0x52 bits 3-6)
    int mode;                // copy shader mode (kCopyFsBody), 11 = Z copy
    u64 key;                 // copyKey() of the destination (texture copies)
    // Clear after the copy: which parts, and the values.
    bool colorUpd, alphaUpd, zUpd;
    float clearColor[4];
    float clearDepth;
};
void decodeCopy(const Item& it, int srcW, int srcH, u32 nativeW, u32 nativeH, u32 copySlot, CopyOp& op);

// The EFB copy program: a fullscreen quad (4 vertices, triangle strip,
// vertex index only) reading the source color (unit / binding 0) and depth
// (1) at v_uv, with u_rect = source rectangle in target UV (x0, y0, x1, y1)
// and u_mode = CopyOp::mode.  Shader text = dialect header + body; the
// bodies use VERTEX_ID for the vertex index.
extern const char* const kCopyVsBody;
extern const char* const kCopyFsBody;
extern const char* const kCopyVsHeaderGles;
extern const char* const kCopyFsHeaderGles;
// Vulkan GLSL (#version 450) headers: a draft for the Vulkan backend
// (u_rect and u_mode as push constants, samplers at set 0 bindings 0/1);
// not compiled by anything yet.
extern const char* const kCopyVsHeaderVk;
extern const char* const kCopyFsHeaderVk;

}  // namespace gpu
