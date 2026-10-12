// xr_gfx.h on OpenGL ES: the EGL context handed to the runtime (on Linux
// through XR_MNDX_egl_enable, on Android through the OpenGL ES binding), the
// GL swapchains and the GPU fences of the frame loop.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#ifdef __ANDROID__
#include <jni.h>
#endif
#include <stdio.h>
#include <string.h>

#include "xr_gfx.h"

#ifdef __ANDROID__
#define XR_USE_PLATFORM_ANDROID
#else
// Linux (the Steam Frame's own SteamVR runtime): the GLES context is handed
// to the runtime through XR_MNDX_egl_enable.
#define XR_USE_PLATFORM_EGL
#endif
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr_platform.h>

#include "port/port.h"

#ifndef XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME
#define XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME "XR_KHR_vulkan_enable2"
#endif

#define XR_CHECK(call)                                                                                                                               \
    do {                                                                                                                                             \
        XrResult _r = (call);                                                                                                                        \
        if (XR_FAILED(_r)) {                                                                                                                         \
            port_fatal("%s failed: %d (%s:%d)", #call, (int)_r, __FILE__, __LINE__);                                                                 \
        }                                                                                                                                            \
    } while (0)

namespace xrgfx {
namespace {

bool hasExtension(const std::vector<XrExtensionProperties>& exts, const char* name) {
    for (const auto& e : exts) {
        if (strcmp(e.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

class GlesGfx : public Gfx {
public:
    Api api() const override { return Api::Gles; }

    void createContext() override {
#ifdef __ANDROID__
        mDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
#else
        // No window of our own: Mesa's surfaceless platform renders on the GPU's
        // render node whatever the desktop (gamescope, X, Wayland or none).
        mDisplay = EGL_NO_DISPLAY;
        auto getPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        if (getPlatformDisplay) {
            mDisplay = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        }
        if (mDisplay == EGL_NO_DISPLAY) {
            port_log("EGL: no surfaceless platform, using the default display");
            mDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        }
        eglBindAPI(EGL_OPENGL_ES_API);
#endif
        if (!eglInitialize(mDisplay, nullptr, nullptr)) {
            port_fatal("eglInitialize failed: 0x%x", eglGetError());
        }
        const EGLint cfgAttr[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0,
                                  EGL_SAMPLES, 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
        EGLint num = 0;
        if (!eglChooseConfig(mDisplay, cfgAttr, &mConfig, 1, &num) || num == 0) {
            port_fatal("eglChooseConfig failed");
        }
        const EGLint ctxAttr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
        mContext = eglCreateContext(mDisplay, mConfig, EGL_NO_CONTEXT, ctxAttr);
        if (mContext == EGL_NO_CONTEXT) {
            port_fatal("eglCreateContext failed: 0x%x", eglGetError());
        }
        const EGLint pbAttr[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        mSurface = eglCreatePbufferSurface(mDisplay, mConfig, pbAttr);
        if (!eglMakeCurrent(mDisplay, mSurface, mSurface, mContext)) {
            port_fatal("eglMakeCurrent failed: 0x%x", eglGetError());
        }
        port_log("GL: %s / %s / %s", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
    }

    void instanceExtensions(const std::vector<XrExtensionProperties>& offered, std::vector<const char*>* enable) override {
#ifdef __ANDROID__
        enable->push_back(XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME);
#else
        if (!hasExtension(offered, XR_MNDX_EGL_ENABLE_EXTENSION_NAME) || !hasExtension(offered, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME)) {
            port_fatal("the OpenXR runtime has no OpenGL ES through EGL (XR_MNDX_egl_enable + XR_KHR_opengl_es_enable)");
        }
        enable->push_back(XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME);
        enable->push_back(XR_MNDX_EGL_ENABLE_EXTENSION_NAME);
        // The Vulkan binding too when the runtime has it: the API is chosen
        // when the session is made.
        if (hasExtension(offered, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) {
            enable->push_back(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
        }
#endif
    }

    void attach(XrInstance instance, XrSystemId system) override {
        PFN_xrGetOpenGLESGraphicsRequirementsKHR getReq = nullptr;
        XR_CHECK(xrGetInstanceProcAddr(instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&getReq));
        XrGraphicsRequirementsOpenGLESKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
        XR_CHECK(getReq(instance, system, &req));
    }

    const void* sessionBinding() override {
#ifdef __ANDROID__
        mBinding = {XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
#else
        mBinding = {XR_TYPE_GRAPHICS_BINDING_EGL_MNDX};
        mBinding.getProcAddress = (PFN_xrEglGetProcAddressMNDX)eglGetProcAddress;
#endif
        mBinding.display = mDisplay;
        mBinding.config = mConfig;
        mBinding.context = mContext;
        return &mBinding;
    }

    void sessionCreated(XrSession session) override {
        uint32_t fmtCount = 0;
        XR_CHECK(xrEnumerateSwapchainFormats(session, 0, &fmtCount, nullptr));
        mFormats.resize(fmtCount);
        XR_CHECK(xrEnumerateSwapchainFormats(session, fmtCount, &fmtCount, mFormats.data()));
    }

    bool hasFormat(ImageKind kind) const override {
        switch (kind) {
        case ImageKind::Eye:
            return !mFormats.empty();
        case ImageKind::Ui:
            return offers(GL_SRGB8_ALPHA8);
        case ImageKind::Motion:
            return offers(GL_RGBA16F);
        case ImageKind::Depth:
            return offers(GL_DEPTH24_STENCIL8);
        }
        return false;
    }

    bool createChain(XrSession session, ImageKind kind, int width, int height, uint32_t mips, Chain* out) override {
        Chain& sc = *out;
        sc.width = width;
        sc.height = height;
        sc.mips = mips;
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        switch (kind) {
        case ImageKind::Eye: {
            int64_t format = mFormats[0];
            for (int64_t f : mFormats) {
                if (f == GL_SRGB8_ALPHA8) {
                    format = f;
                }
            }
            ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            ci.format = format;
            break;
        }
        case ImageKind::Ui:
            ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            ci.format = GL_SRGB8_ALPHA8;
            break;
        case ImageKind::Motion:
            ci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
            ci.format = GL_RGBA16F;
            break;
        case ImageKind::Depth:
            ci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            ci.format = GL_DEPTH24_STENCIL8;
            break;
        }
        sc.format = ci.format;
        ci.sampleCount = 1;
        ci.width = (uint32_t)width;
        ci.height = (uint32_t)height;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = mips;
        XR_CHECK(xrCreateSwapchain(session, &ci, &sc.handle));
        uint32_t n = 0;
        XR_CHECK(xrEnumerateSwapchainImages(sc.handle, 0, &n, nullptr));
        std::vector<XrSwapchainImageOpenGLESKHR> images(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        XR_CHECK(xrEnumerateSwapchainImages(sc.handle, n, &n, (XrSwapchainImageBaseHeader*)images.data()));
        sc.images.assign(n, gpu::EfbTarget());
        if (kind == ImageKind::Eye || kind == ImageKind::Ui) {
            // Color only: the composite into the swapchain (vr::renderEye)
            // draws without depth.
            std::vector<GLuint> fbos(n);
            glGenFramebuffers((GLsizei)n, fbos.data());
            for (uint32_t i = 0; i < n; i++) {
                glBindFramebuffer(GL_FRAMEBUFFER, fbos[i]);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, images[i].image, 0);
                sc.images[i].fbo = fbos[i];
            }
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        for (uint32_t i = 0; i < n; i++) {
            gpu::EfbTarget& t = sc.images[i];
            (kind == ImageKind::Depth ? t.depth : t.color) = images[i].image;
            t.width = width;
            t.height = height;
        }
        return true;
    }

    uint32_t acquire(Chain& chain) override { return acquireImage(chain.handle); }
    void wait(Chain& chain) override { waitImage(chain.handle); }
    void release(Chain& chain) override { releaseImage(chain.handle); }

    Fence submit() override {
        Fence f = (Fence)glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();
        return f;
    }

    bool waitFence(Fence fence, int64_t timeoutNs) override {
        return glClientWaitSync((GLsync)fence, GL_SYNC_FLUSH_COMMANDS_BIT, (GLuint64)timeoutNs) != GL_TIMEOUT_EXPIRED;
    }

    void destroyFence(Fence fence) override { glDeleteSync((GLsync)fence); }

    void finish() override { glFinish(); }

    gpu::EfbTarget createImage(ImageKind kind, int width, int height, uint32_t mips) override {
        gpu::EfbTarget t;
        t.width = width;
        t.height = height;
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        switch (kind) {
        case ImageKind::Eye:
        case ImageKind::Ui:
            glTexStorage2D(GL_TEXTURE_2D, (GLsizei)mips, GL_SRGB8_ALPHA8, width, height);  // like the swapchain
            if (kind == ImageKind::Ui) {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            }
            glGenFramebuffers(1, &t.fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            t.color = tex;
            break;
        case ImageKind::Motion:
            glTexStorage2D(GL_TEXTURE_2D, (GLsizei)mips, GL_RGBA16F, width, height);
            t.color = tex;
            break;
        case ImageKind::Depth:
            glTexStorage2D(GL_TEXTURE_2D, (GLsizei)mips, GL_DEPTH24_STENCIL8, width, height);
            t.depth = tex;
            break;
        }
        return t;
    }

private:
    bool offers(int64_t format) const {
        for (int64_t f : mFormats) {
            if (f == format) {
                return true;
            }
        }
        return false;
    }

    EGLDisplay mDisplay = EGL_NO_DISPLAY;
    EGLConfig mConfig = nullptr;
    EGLContext mContext = EGL_NO_CONTEXT;
    EGLSurface mSurface = EGL_NO_SURFACE;
#ifdef __ANDROID__
    XrGraphicsBindingOpenGLESAndroidKHR mBinding{};
#else
    XrGraphicsBindingEGLMNDX mBinding{};
#endif
    std::vector<int64_t> mFormats;  // the swapchain formats the runtime offers
};

}  // namespace

Gfx* createGlesGfx() { return new GlesGfx(); }

}  // namespace xrgfx
