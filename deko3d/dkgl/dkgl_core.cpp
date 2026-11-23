// dkgl core: deko3d device, memory, frames, swapchain, descriptors, and the EGL entry points SDL uses.
#include "dkgl_internal.h"

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace dkgl {

Context ctx;

// ---------------------------------------------------------------- logging
static FILE* s_log = nullptr;
void logf(const char* fmt, ...)
{
	if (!s_log) s_log = fopen("sdmc:/switch/dkgl.log", "w");
	if (!s_log) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(s_log, fmt, ap);
	va_end(ap);
	fputc('\n', s_log);
	fflush(s_log);
}

// ---------------------------------------------------------------- memory blocks
static inline uint32_t alignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

Block blockCreate(uint32_t size, uint32_t flags)
{
	Block b;
	DkMemBlockMaker m;
	dkMemBlockMakerDefaults(&m, ctx.device, alignUp(size ? size : 1, DK_MEMBLOCK_ALIGNMENT));
	m.flags = flags;
	if (m.size >= 4u * 1024 * 1024) {
		// deko3d aborts the whole game if it can't get the memory; check first so we can fail softly
		void* probe = memalign(DK_MEMBLOCK_ALIGNMENT, m.size);
		if (!probe) { logf("dkgl: not enough memory for %u KB, skipping it", m.size / 1024); return b; }
		free(probe);
	}
	b.mem = dkMemBlockCreate(&m);
	if (!b.mem) { logf("dkgl: dkMemBlockCreate(%u) failed", size); return b; }
	b.size = m.size;
	b.gpu = dkMemBlockGetGpuAddr(b.mem);
	b.cpu = (flags & DkMemBlockFlags_CpuAccessMask) ? dkMemBlockGetCpuAddr(b.mem) : nullptr;
	return b;
}

void blockDestroy(Block& b)
{
	if (b.mem) dkMemBlockDestroy(b.mem);
	b = Block{};
}

// per-frame data: ring region, command memory, deferred frees
struct FrameData {
	Block ring;
	uint32_t ringUsed = 0;
	std::vector<Block> extraRing;     // overflow blocks for this frame
	Block cmdMem;
	std::vector<Block> extraCmd;
	std::vector<Block> deferred;
};
static FrameData s_frames[kFramesInFlight];
static constexpr uint32_t kRingSize = 32 * 1024 * 1024;
static constexpr uint32_t kCmdSize = 2 * 1024 * 1024;

void blockDestroyDeferred(Block& b)
{
	if (!b.mem) return;
	s_frames[ctx.curFrameIndex()].deferred.push_back(b);
	b = Block{};
}

Alloc ringAlloc(uint32_t size, uint32_t align)
{
	FrameData& f = s_frames[ctx.curFrameIndex()];
	uint32_t off = alignUp(f.ringUsed, align);
	if (off + size <= f.ring.size) {
		f.ringUsed = off + size;
		return { (uint8_t*)f.ring.cpu + off, f.ring.gpu + off };
	}
	// overflow: a dedicated block that lives until this frame is retired
	Block extra = blockCreate(size + align, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
	f.extraRing.push_back(extra);
	return { extra.cpu, extra.gpu };
}

static void cmdAddMem(void* userData, DkCmdBuf cmdbuf, size_t minReqSize)
{
	FrameData& f = s_frames[ctx.curFrameIndex()];
	Block b = blockCreate((uint32_t)(minReqSize > kCmdSize ? minReqSize : kCmdSize), DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
	f.extraCmd.push_back(b);
	dkCmdBufAddMemory(cmdbuf, b.mem, 0, b.size);
}

// implemented in dkgl_gl.cpp: moves buffers whose storage lives in a retired ring back to their own memory
void promoteTransientBuffers(int frameIndex);
void onFrameRetired(int frameIndex);

static void retireFrame(int i)
{
	FrameData& f = s_frames[i];
	onFrameRetired(i);   // reads this frame's GPU results before its ring memory is reused
	promoteTransientBuffers(i);
	for (Block& b : f.deferred) blockDestroy(b);
	f.deferred.clear();
	for (Block& b : f.extraRing) blockDestroy(b);
	f.extraRing.clear();
	for (Block& b : f.extraCmd) blockDestroy(b);
	f.extraCmd.clear();
	f.ringUsed = 0;
}

// ---------------------------------------------------------------- descriptors
int descAlloc()
{
	if (ctx.freeDesc.empty()) { logf("dkgl: out of texture descriptors"); return 0; }
	int d = ctx.freeDesc.back();
	ctx.freeDesc.pop_back();
	return d;
}

void descFree(int d) { if (d > 0) ctx.freeDesc.push_back(d); }

static DkImageDescriptor* imageDescs() { return (DkImageDescriptor*)ctx.descMem.cpu; }
static DkSamplerDescriptor* samplerDescs()
{
	return (DkSamplerDescriptor*)((uint8_t*)ctx.descMem.cpu + kMaxDescriptors * sizeof(DkImageDescriptor));
}

static DkFilter toFilter(GLenum f) { return (f == GL_NEAREST || f == GL_NEAREST_MIPMAP_NEAREST || f == GL_NEAREST_MIPMAP_LINEAR) ? DkFilter_Nearest : DkFilter_Linear; }
static DkWrapMode toWrap(GLenum w)
{
	switch (w) {
		case GL_CLAMP_TO_EDGE: return DkWrapMode_ClampToEdge;
		case GL_MIRRORED_REPEAT: return DkWrapMode_MirroredRepeat;
		default: return DkWrapMode_Repeat;
	}
}

void writeTextureDescriptors(Texture& t)
{
	if (!t.valid || t.desc <= 0) return;
	DkImageView view;
	dkImageViewDefaults(&view, &t.image);
	for (int i = 0; i < 4; i++) view.swizzle[i] = t.swizzle[i];
	dkImageDescriptorInitialize(&imageDescs()[t.desc], &view, false, false);

	DkSampler s;
	dkSamplerDefaults(&s);
	s.minFilter = toFilter(t.minFilter);
	s.magFilter = toFilter(t.magFilter);
	// very large textures get an automatic mip chain (cache-friendly when drawn smaller); others have none
	s.mipFilter = t.useMips ? DkMipFilter_Linear : DkMipFilter_None;
	if (t.useMips) { s.lodClampMin = 0.0f; s.lodClampMax = (float)(t.mipLevels - 1); }
	s.wrapMode[0] = toWrap(t.wrapS);
	s.wrapMode[1] = toWrap(t.wrapT);
	s.wrapMode[2] = DkWrapMode_ClampToEdge;
	dkSamplerDescriptorInitialize(&samplerDescs()[t.desc], &s);
	t.samplerDirty = false;
	ctx.descDirty = true;
}

// ---------------------------------------------------------------- frames / swapchain
static void beginCommandList()
{
	FrameData& f = s_frames[ctx.curFrameIndex()];
	dkCmdBufClear(ctx.cmd);
	dkCmdBufAddMemory(ctx.cmd, f.cmdMem.mem, 0, f.cmdMem.size);
	dkCmdBufBindImageDescriptorSet(ctx.cmd, ctx.descMem.gpu, kMaxDescriptors);
	dkCmdBufBindSamplerDescriptorSet(ctx.cmd, ctx.descMem.gpu + kMaxDescriptors * sizeof(DkImageDescriptor), kMaxDescriptors);
	ctx.descDirty = true;
	if (ctx.tsMem.mem) dkCmdBufReportCounter(ctx.cmd, DkCounter_Timestamp, ctx.tsMem.gpu + ctx.curFrameIndex() * 32);
	onFrameStart();
}

void ensureRecording() {}

DkImage* defaultColorImage() { return &ctx.internalImage; }

// Dynamic resolution: keep the GPU inside the frame budget by rendering fewer pixels when it is late
static void updateDynamicResolution(double gpuMs)
{
	if (gpuMs <= 0.0 || gpuMs > 1000.0) return;
	ctx.gpuMs = ctx.gpuMs <= 0.0f ? (float)gpuMs : ctx.gpuMs * 0.85f + (float)gpuMs * 0.15f;
	float s = ctx.renderScale;
	if (!ctx.drsEnabled) s = 1.0f;
	else if (ctx.gpuMs > 15.0f) s *= 0.94f;
	else if (ctx.gpuMs < 11.5f) s *= 1.03f;
	if (s < ctx.drsMin) s = ctx.drsMin;
	if (s > 1.0f) s = 1.0f;
	s = (float)(int)(s * 40.0f + 0.5f) / 40.0f;   // steps of 2.5% so it settles
	if (s != ctx.renderScale) { ctx.renderScale = s; S_markDirty(); }
}


void flushAndWait()
{
	DkCmdList list = dkCmdBufFinishList(ctx.cmd);
	dkQueueSubmitCommands(ctx.queue, list);
	dkQueueWaitIdle(ctx.queue);
	ctx.completedSerial = ctx.frameSerial - 1;
	// everything finished: command memory of this frame can be reused from the start
	FrameData& f = s_frames[ctx.curFrameIndex()];
	for (Block& b : f.extraCmd) blockDestroy(b);
	f.extraCmd.clear();
	beginCommandList();
}

void swapBuffers()
{
	int i = ctx.curFrameIndex();
	// scale the rendered part of the internal target up to the screen
	ctx.acquiredSlot = dkQueueAcquireImage(ctx.queue, ctx.swapchain);
	dkCmdBufBarrier(ctx.cmd, DkBarrier_Fragments, 0);
	DkImageView src, dst;
	dkImageViewDefaults(&src, &ctx.internalImage);
	dkImageViewDefaults(&dst, &ctx.fbImages[ctx.acquiredSlot]);
	DkImageRect srect = { 0, 0, 0, ctx.scaledW(), ctx.scaledH(), 1 };
	DkImageRect drect = { 0, 0, 0, ctx.width, ctx.height, 1 };
	dkCmdBufBlitImage(ctx.cmd, &src, &srect, &dst, &drect, DkBlitFlag_FilterLinear | DkBlitFlag_ModeBlit, 0);
	if (ctx.tsMem.mem) dkCmdBufReportCounter(ctx.cmd, DkCounter_Timestamp, ctx.tsMem.gpu + i * 32 + 16);
	S_markDirty();
	dkCmdBufSignalFence(ctx.cmd, &ctx.frameFence[i], true);
	DkCmdList list = dkCmdBufFinishList(ctx.cmd);
	dkQueueSubmitCommands(ctx.queue, list);
	dkQueuePresentImage(ctx.queue, ctx.swapchain, ctx.acquiredSlot);
	ctx.acquiredSlot = -1;
	ctx.fenceSerial[i] = ctx.frameSerial;

	ctx.frameSerial++;
	int j = ctx.curFrameIndex();
	if (ctx.fenceSerial[j]) {
		dkFenceWait(&ctx.frameFence[j], -1);
		if (ctx.fenceSerial[j] > ctx.completedSerial) ctx.completedSerial = ctx.fenceSerial[j];
		retireFrame(j);
		if (ctx.tsMem.cpu) {
			const uint64_t* ts = (const uint64_t*)((uint8_t*)ctx.tsMem.cpu + j * 32);
			if (ts[3] > ts[1]) updateDynamicResolution((double)(ts[3] - ts[1]) / 1e6);
		}
		static uint64_t lastLog = 0;
		if (ctx.frameSerial - lastLog >= 300) {
			lastLog = ctx.frameSerial;
			logf("dkgl: GPU %.2f ms/frame, heavy-shader resolution %.0f%%", ctx.gpuMs, ctx.renderScale * 100.0f);
		}
	}
	beginCommandList();
}

static void initImage2D(DkImageLayout& layout, DkImage& image, Block& mem, uint32_t w, uint32_t h, DkImageFormat fmt, uint32_t flags)
{
	DkImageLayoutMaker m;
	dkImageLayoutMakerDefaults(&m, ctx.device);
	m.flags = flags;
	m.format = fmt;
	m.dimensions[0] = w;
	m.dimensions[1] = h;
	dkImageLayoutInitialize(&layout, &m);
	mem = blockCreate((uint32_t)dkImageLayoutGetSize(&layout), DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
	dkImageInitialize(&image, &layout, mem.mem, 0);
}

bool coreInit(NWindow* win)
{
	if (ctx.device) return true;
	ctx.window = win ? win : nwindowGetDefault();

	DkDeviceMaker dm;
	dkDeviceMakerDefaults(&dm);
	dm.cbDebug = [](void*, const char* context, DkResult result, const char* message) {
		logf("dkgl: deko3d error %d in %s: %s", (int)result, context ? context : "?", message ? message : "");
	};
	dm.flags = DkDeviceFlags_DepthMinusOneToOne | DkDeviceFlags_OriginLowerLeft; // GL conventions
	ctx.device = dkDeviceCreate(&dm);
	if (!ctx.device) { logf("dkgl: dkDeviceCreate failed"); return false; }

	DkQueueMaker qm;
	dkQueueMakerDefaults(&qm, ctx.device);
	qm.flags = DkQueueFlags_Graphics;
	ctx.queue = dkQueueCreate(&qm);

	DkCmdBufMaker cm;
	dkCmdBufMakerDefaults(&cm, ctx.device);
	cm.cbAddMem = cmdAddMem;
	ctx.cmd = dkCmdBufCreate(&cm);

	for (unsigned i = 0; i < kFramesInFlight; i++) {
		s_frames[i].ring = blockCreate(kRingSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
		s_frames[i].cmdMem = blockCreate(kCmdSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
	}

	ctx.descMem = blockCreate(kMaxDescriptors * (sizeof(DkImageDescriptor) + sizeof(DkSamplerDescriptor)),
	                          DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
	memset(ctx.descMem.cpu, 0, ctx.descMem.size);
	for (int d = kMaxDescriptors - 1; d >= 1; d--) ctx.freeDesc.push_back(d); // 0 is reserved

	// default framebuffer
	DkImage const* imgs[Context::kSwapImages];
	for (unsigned i = 0; i < Context::kSwapImages; i++) {
		DkImageLayout layout;
		initImage2D(layout, ctx.fbImages[i], ctx.fbMem[i], ctx.width, ctx.height, DkImageFormat_RGBA8_Unorm,
		            DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression);
		imgs[i] = &ctx.fbImages[i];
	}
	{
		DkImageLayout layout;
		initImage2D(layout, ctx.dsImage, ctx.dsMem, ctx.width, ctx.height, DkImageFormat_Z24S8,
		            DkImageFlags_UsageRender | DkImageFlags_HwCompression);
	}
	{
		DkImageLayout layout;
		initImage2D(layout, ctx.internalImage, ctx.internalMem, ctx.width, ctx.height, DkImageFormat_RGBA8_Unorm,
		            DkImageFlags_UsageRender | DkImageFlags_HwCompression);
	}
	ctx.tsMem = blockCreate(kFramesInFlight * 32, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
	if (ctx.tsMem.cpu) memset(ctx.tsMem.cpu, 0, ctx.tsMem.size);
	DkSwapchainMaker sm;
	dkSwapchainMakerDefaults(&sm, ctx.device, ctx.window, imgs, Context::kSwapImages);
	ctx.swapchain = dkSwapchainCreate(&sm);
	if (!ctx.swapchain) { logf("dkgl: dkSwapchainCreate failed"); return false; }

	beginCommandList();
	glInitState();   // records the placeholder texture upload into the first command list
	logf("dkgl: deko3d ready (%ux%u, %u frames in flight)", ctx.width, ctx.height, kFramesInFlight);
	return true;
}

void coreShutdown()
{
	if (!ctx.device) return;
	dkQueueWaitIdle(ctx.queue);
	if (ctx.swapchain) dkSwapchainDestroy(ctx.swapchain);
	ctx.swapchain = nullptr;
	// the process is exiting; the kernel reclaims memory blocks and the device
}

} // namespace dkgl

// ================================================================ EGL (what SDL3's Switch video driver calls)
using namespace dkgl;

static EGLint s_eglError = EGL_SUCCESS;
static int s_dummyDisplay, s_dummyConfig, s_dummyContext, s_dummySurface;
static EGLint s_swapInterval = 1;

extern "C" {

EGLDisplay eglGetDisplay(EGLNativeDisplayType) { return (EGLDisplay)&s_dummyDisplay; }
EGLDisplay eglGetPlatformDisplay(EGLenum, void*, const EGLAttrib*) { return (EGLDisplay)&s_dummyDisplay; }
EGLBoolean eglInitialize(EGLDisplay, EGLint* major, EGLint* minor) { if (major) *major = 1; if (minor) *minor = 5; return EGL_TRUE; }
EGLBoolean eglTerminate(EGLDisplay) { return EGL_TRUE; }
EGLBoolean eglBindAPI(EGLenum) { return EGL_TRUE; }
EGLint eglGetError(void) { EGLint e = s_eglError; s_eglError = EGL_SUCCESS; return e; }

const char* eglQueryString(EGLDisplay, EGLint name)
{
	switch (name) {
		case EGL_VENDOR: return "dkgl";
		case EGL_VERSION: return "1.5 dkgl";
		case EGL_CLIENT_APIS: return "OpenGL OpenGL_ES";
		case EGL_EXTENSIONS: return "EGL_KHR_create_context EGL_KHR_surfaceless_context";
	}
	return "";
}

EGLBoolean eglChooseConfig(EGLDisplay, const EGLint*, EGLConfig* configs, EGLint configSize, EGLint* numConfig)
{
	if (configs && configSize > 0) configs[0] = (EGLConfig)&s_dummyConfig;
	if (numConfig) *numConfig = 1;
	return EGL_TRUE;
}

EGLBoolean eglGetConfigAttrib(EGLDisplay, EGLConfig, EGLint attribute, EGLint* value)
{
	EGLint v = 0;
	switch (attribute) {
		case EGL_RED_SIZE: case EGL_GREEN_SIZE: case EGL_BLUE_SIZE: case EGL_ALPHA_SIZE: v = 8; break;
		case EGL_BUFFER_SIZE: v = 32; break;
		case EGL_DEPTH_SIZE: v = 24; break;
		case EGL_STENCIL_SIZE: v = 8; break;
		case EGL_SAMPLES: case EGL_SAMPLE_BUFFERS: v = 0; break;
		case EGL_RENDERABLE_TYPE: v = EGL_OPENGL_BIT | EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT; break;
		case EGL_SURFACE_TYPE: v = EGL_WINDOW_BIT | EGL_PBUFFER_BIT; break;
		case EGL_CONFIG_ID: v = 1; break;
		case EGL_NATIVE_VISUAL_ID: v = 0; break;
		default: v = 0; break;
	}
	if (value) *value = v;
	return EGL_TRUE;
}

EGLSurface eglCreateWindowSurface(EGLDisplay, EGLConfig, EGLNativeWindowType win, const EGLint*)
{
	if (!coreInit((NWindow*)win)) { s_eglError = EGL_BAD_ALLOC; return EGL_NO_SURFACE; }
	return (EGLSurface)&s_dummySurface;
}
EGLSurface eglCreatePbufferSurface(EGLDisplay, EGLConfig, const EGLint*) { return (EGLSurface)&s_dummySurface; }
EGLBoolean eglDestroySurface(EGLDisplay, EGLSurface) { return EGL_TRUE; }
EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint*)
{
	if (!coreInit(nullptr)) { s_eglError = EGL_BAD_ALLOC; return EGL_NO_CONTEXT; }
	return (EGLContext)&s_dummyContext;
}
EGLBoolean eglDestroyContext(EGLDisplay, EGLContext) { return EGL_TRUE; }
EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext) { return EGL_TRUE; }
EGLBoolean eglSwapInterval(EGLDisplay, EGLint interval) { s_swapInterval = interval; return EGL_TRUE; }
EGLBoolean eglSwapBuffers(EGLDisplay, EGLSurface) { swapBuffers(); return EGL_TRUE; }
EGLBoolean eglWaitGL(void) { return EGL_TRUE; }
EGLBoolean eglWaitNative(EGLint) { return EGL_TRUE; }

__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char* name)
{
	return (__eglMustCastToProperFunctionPointerType)getProc(name);
}

} // extern "C"
