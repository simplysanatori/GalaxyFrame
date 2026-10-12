// GLSL generation for the GX pipeline (XF transform/lighting/texgen as a
// vertex shader, TEV + alpha test + fog as a fragment shader).
#pragma once

#include <stdint.h>
#include <string.h>

#include <string>

namespace gpu {

// Everything that changes the generated code.  Register values are masked
// to the fields that matter; constants (colors, matrices, references) are
// uniforms.
struct ShaderUid {
    uint32_t vtxFlags;
    uint32_t numTexGens;
    uint32_t numChans;
    uint32_t dualTex;
    uint32_t colorCtrl[2];
    uint32_t alphaCtrl[2];
    uint32_t texMtxInfo[8];
    uint32_t postMtxInfo[8];
    uint32_t numTevStages;
    uint32_t numIndStages;
    uint32_t tevOrder[8];
    uint32_t tevColor[16];
    uint32_t tevAlpha[16];
    uint32_t ksel[8];
    uint32_t alphaFunc;   // BP 0xF3 bits 16-23
    uint32_t fogSel;      // BP 0xF1 bits 20-23
    uint32_t indCmd[16];
    uint32_t iref;
    uint32_t indScale[2];
    uint32_t efbHasAlpha;
    uint32_t hud;         // drawn into the HUD layer (no VR transform)
    uint32_t zTex;        // BP 0xF5: Z texture type (bits 0-1) and op (bits 2-3)
    uint32_t cutaway;     // bit 0: VR cutaway variant (see gpu::EyeView), has a discard; bit 1 (Vulkan): the
                          // pixel state from each vertex record, for a batch of many pixel states drawn whole
    uint32_t screenTexGens;  // bit n: texgen n maps positions to their screen position (VR: the eye's)

    bool operator==(const ShaderUid& o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
};

struct ShaderUidHash {
    size_t operator()(const ShaderUid& u) const {
        const uint32_t* w = (const uint32_t*)&u;
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < sizeof(u) / 4; i++) {
            h = (h ^ w[i]) * 1099511628211ull;
        }
        return (size_t)h;
    }
};

// Builds the UID from the current BP/XF register state and the draw's
// vertex layout.
// PETARI_VKLEGACY=1: the Vulkan replay as before the perf work (A/B timing,
// and a fallback): every optimisation below is off.
bool vkLegacy();
// Vulkan: the fragment shader reads the pixel state at the draw's push
// constant (kVkPushPixBase) rather than a flat varying from the draw record
// (each batch is drawn per run of one pixel state, Item::pixRunFirst).
bool vkPixelStatePushed();

ShaderUid makeShaderUid(const uint32_t* bp, const uint32_t* xfRegs, uint32_t vtxFlags, bool hud);

// Which GLSL the generator writes.  GLES is what the GL renderer compiles
// (GLSL ES 3.20) and stays byte-identical per uid.  VK is for the Vulkan
// backend, compiled to SPIR-V at run time (vk_spirv.h): GLSL 4.50 with the
// layout below.
enum class Dialect { GLES, VK };

std::string genVertexShader(const ShaderUid& uid, Dialect dialect = Dialect::GLES);
std::string genFragmentShader(const ShaderUid& uid, Dialect dialect = Dialect::GLES);

// The fixed EFB copy program (12 modes, selected by u_mode / the push
// constant copyMode), a fullscreen triangle drawn with 3 vertices.  The GLES
// text is the same as the one kCopyVs / kCopyFs hold in gl_renderer.cpp.
std::string genCopyVertexShader(Dialect dialect = Dialect::GLES);
std::string genCopyFragmentShader(Dialect dialect = Dialect::GLES);

// Vulkan dialect interface, shared by the generated shaders and the fixed
// copy program, so one pipeline layout fits them all:
//   set 0 (per frame, buffers)   binding 0 rows[] SSBO       VS (FS declares it too)
//                                binding 1 recs[] SSBO       VS, FS
//                                binding 2 st[] SSBO         VS, FS
//                                binding 3 vtxRec[] SSBO     VS
//                                binding 4 EyeBlock UBO      VS, FS (GLES: binding 2)
//   set 1 (push descriptors)     bindings 0-7 combined image samplers s_tex0..7, FS
//                                (the copy program reads s_color on 0, s_depth on 1)
//   push constants, 32 bytes, stages VERTEX | FRAGMENT:
//                                offset 0  uint u_firstOrdinal  (GX draws)
//                                offset 4  int  u_copyMode      (copy program)
//                                offset 8  uint pad[2]
//                                offset 16 vec4 u_copyRect      (copy program)
// Varyings carry explicit locations (see the kLoc* values in the .cpp), the
// fragment output is location 0.  Vertex attribute locations are as in GLES.
//
// Vertex index contract: the shaders read vtxRec[u_firstOrdinal + gl_VertexIndex].
// Draw with vkCmdDrawIndexed(vertexOffset = 0, firstInstance = 0), the vertex
// buffer bound at the batch's first word (offset firstWord * 4) and the index
// buffer offset to the batch's first index; the 32-bit indices of a batch start
// at 0, so gl_VertexIndex is the index read from the index buffer, exactly
// what gl_VertexID is for glDrawElements, and u_firstOrdinal is the batch's
// firstOrdinal.  A nonzero vertexOffset would be added to gl_VertexIndex and
// break the lookup: do not use one.  The copy program is vkCmdDraw(3, 1, 0, 0).
enum : uint32_t {
    kVkSetBuffers = 0,
    kVkSetTextures = 1,
    kVkBindRows = 0,
    kVkBindRecs = 1,
    kVkBindState = 2,
    kVkBindVtxRec = 3,
    kVkBindEye = 4,
    kVkNumSamplers = 8,
    kVkPushSize = 32,
    kVkPushFirstOrdinal = 0,
    kVkPushCopyMode = 4,
    kVkPushPixBase = 8,  // draws: the pixel state's base in st[] (fragment shader)
    kVkPushCopyRect = 16,
};

// Per-frame shader data (see gl_renderer.cpp).  Draws are batched, so
// everything that varies per draw lives in storage buffers:
//   rows[]  (binding 0)  matrix rows (vec4) snapshotted from XF memory
//   recs[]  (binding 1)  one DrawRecord (kRecWords uints) per distinct draw state
//   st[]    (binding 2)  light / projection / pixel states (vec4s)
//   vtxRec[] (binding 3) draw record per vertex ordinal
// A batch's indices start at 0, so vtxRec[u_firstOrdinal + gl_VertexID].
enum : uint32_t {
    kRecPosSlots = 0,    // 22 slots: rows 0..65 of the position/texture matrix memory, 3 rows each
    kRecNrmSlots = 22,   // 11 slots: normal matrix rows 0..32
    kRecPost = 33,       // 8: post-transform matrix per texgen
    kRecLights = 41,     // base of the light state in st[]
    kRecProj = 42,       // base of the projection state in st[]
    kRecPixel = 43,      // base of the pixel state in st[]
    kRecMtxIdxA = 44,    // XF 0x18 (default matrix indices)
    kRecMtxIdxB = 45,    // XF 0x19
    kRecFlags = 46,      // bit 0: VR camera, bit 1: a sky around the game camera, bit 2: the pointer's cursor
    kRecWords = 48,
};

// Light state: 8 lights x (color, cos atten, dist atten, position, direction),
// material colors, ambient colors.
struct LightState {
    float lights[8][5][4];
    float matColor[2][4];
    float ambColor[2][4];
};

// Projection state: GX projection rows, depth params (zScale, zOffset).
struct ProjState {
    float proj[4][4];
    float depthParams[4];
};

struct PixelState {
    float tevReg[4][4];      // PREV, REG0, REG1, REG2
    float konst[4][4];       // K0..K3
    float alphaRef[4];       // ref0, ref1, Z texture bias (24-bit units)
    float fogColor[4];
    float fogParams[4];      // a, b, c, 0
    float fogRange[4];
    float texSize[8][4];     // LOD bias, height, 1/width, 1/height
    float indMtx[3][2][4];   // indirect matrices (rows, scaled)
    float efbSize[4];        // efb width/height for indirect/fog helpers
};

struct alignas(16) EyeBlock {
    float vrView[4][4];      // game view space -> eye space (rows)
    float vrProj[4][4];      // eye space -> clip (rows)
    int32_t vrFlags[4];      // x: 1 rendering for a VR eye, 2 one picture of the flat screen's stereo pair
    float vrFocus[4];        // cutaway target (eye space), w > 0: enabled
    float vrCut[4];          // cutaway shape (see gpu::EyeView)
    float vrEyePos[4];       // the eye in game view space (lights at the camera move there)
    float vrStereo[4];       // the stereo pair's shift (see gpu::EyeView::stereo)
    float vrStereo2[4];      // x: the pointer cursor's shift, y: 1 = the cursor goes with the HUD (gpu::EyeView::pointer)
};

}  // namespace gpu
