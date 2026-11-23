// dkgl GL entry points: object management, state tracking, uploads, uniforms and draws on deko3d.
#include "dkgl_internal.h"

#include <algorithm>
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace dkgl {

// ---------------------------------------------------------------- object tables
std::unordered_map<GLuint, Shader> g_shaders;
static std::unordered_map<GLuint, Program> s_programs;
static std::unordered_map<GLuint, Texture> s_textures;
static std::unordered_map<GLuint, Buffer> s_buffers;
static std::unordered_map<GLuint, std::vector<uint8_t>> s_shadow;   // CPU copy of each buffer
static std::unordered_map<GLuint, Framebuffer> s_fbos;
static std::unordered_map<GLuint, Renderbuffer> s_rbos;
static GLuint s_nextName = 1;
static std::vector<GLuint> s_promote[kFramesInFlight];   // buffers renamed into the ring, per frame

struct Attrib {
	bool enabled = false;
	GLint size = 4;
	GLenum type = GL_FLOAT;
	bool normalized = false;
	GLsizei stride = 0;
	uintptr_t pointer = 0;
	GLuint buffer = 0;
	float value[4] = {0, 0, 0, 1};
};

static struct State {
	GLenum activeUnit = 0;
	GLuint tex2D[kMaxTextureUnits] = {};
	GLuint arrayBuffer = 0, elementBuffer = 0;
	GLuint fbo = 0, rbo = 0, program = 0;
	Attrib attribs[kMaxAttribs];
	GLint vp[4] = {0, 0, 1280, 720};
	GLint sc[4] = {0, 0, 1280, 720};
	bool scissorTest = false, blend = false, depthTest = false, stencilTest = false, cull = false;
	GLenum blendSrcRGB = GL_ONE, blendDstRGB = GL_ZERO, blendSrcA = GL_ONE, blendDstA = GL_ZERO;
	GLenum blendEqRGB = GL_FUNC_ADD, blendEqA = GL_FUNC_ADD;
	float blendColor[4] = {0, 0, 0, 0};
	bool colorMask[4] = {true, true, true, true};
	bool depthMask = true;
	GLenum depthFunc = GL_LESS;
	GLenum stencilFunc[2] = {GL_ALWAYS, GL_ALWAYS};
	GLint stencilRef[2] = {0, 0};
	GLuint stencilValueMask[2] = {0xFF, 0xFF}, stencilWriteMask[2] = {0xFF, 0xFF};
	GLenum stencilFail[2] = {GL_KEEP, GL_KEEP}, stencilZFail[2] = {GL_KEEP, GL_KEEP}, stencilZPass[2] = {GL_KEEP, GL_KEEP};
	GLenum cullMode = GL_BACK, frontFace = GL_CCW;
	float clearColor[4] = {0, 0, 0, 0};
	float clearDepth = 1.0f;
	GLint clearStencil = 0;
	GLint unpackAlign = 4, packAlign = 4;
	GLenum error = GL_NO_ERROR;
	// dirty tracking
	bool targetsDirty = true, viewportDirty = true, stateDirty = true, shadersDirty = true;
	uint32_t targetW = 1280, targetH = 720;
	bool targetIsDefault = true;
} S;

static GLuint s_whiteTex = 0;
void S_markDirty() { S.targetsDirty = S.viewportDirty = S.stateDirty = true; }

static void setError(GLenum e) { if (S.error == GL_NO_ERROR) S.error = e; }

// ---------------------------------------------------------------- frame hooks
void onFrameStart()
{
	S.targetsDirty = S.viewportDirty = S.stateDirty = S.shadersDirty = true;
	// buffers whose data was written into last frame's ring: copy it back into their own memory on the GPU
	int prev = (int)((ctx.frameSerial - 1) % kFramesInFlight);
	bool any = false;
	for (GLuint id : s_promote[prev]) {
		auto it = s_buffers.find(id);
		if (it == s_buffers.end()) continue;
		Buffer& b = it->second;
		if (b.transientFrame != prev || !b.persist.mem) continue;
		if (!any) { dkCmdBufBarrier(ctx.cmd, DkBarrier_Full, 0); any = true; }
		dkCmdBufCopyBuffer(ctx.cmd, b.gpu, b.persist.gpu, b.size);
		b.cpu = b.persist.cpu;
		b.gpu = b.persist.gpu;
		b.transientFrame = -1;
		b.lastUsed = ctx.frameSerial;   // the copy writes it during this frame
	}
	if (any) dkCmdBufBarrier(ctx.cmd, DkBarrier_Full, DkInvalidateFlags_L2Cache);
	s_promote[prev].clear();
}

void promoteTransientBuffers(int frameIndex)
{
	// storage still pointing into a retired ring is copied back at the start of the following frame;
	// by the time a frame is retired that has already happened, so nothing is left to do here
	(void)frameIndex;
}

// ---------------------------------------------------------------- buffers
static void bufferWrite(GLuint id, Buffer& b, uint32_t off, const void* data, uint32_t len, bool whole)
{
	std::vector<uint8_t>& sh = s_shadow[id];
	if (sh.size() < b.size) sh.resize(b.size);
	if (data) memcpy(sh.data() + off, data, len);
	else if (whole) memset(sh.data(), 0, b.size);

	// still read by queued GPU work, or living in per-frame memory: write a fresh copy instead
	bool busy = b.lastUsed > ctx.completedSerial || b.transientFrame >= 0;
	if (busy) {
		Alloc a = ringAlloc(b.size, 256);
		memcpy(a.cpu, sh.data(), b.size);
		b.cpu = a.cpu;
		b.gpu = a.gpu;
		int fi = ctx.curFrameIndex();
		if (b.transientFrame != fi) s_promote[fi].push_back(id);
		b.transientFrame = fi;
		b.lastUsed = 0;
		return;
	}
	if (whole) memcpy(b.cpu, sh.data(), b.size);
	else if (data) memcpy((uint8_t*)b.cpu + off, data, len);
}

// ---------------------------------------------------------------- textures
struct FmtInfo { DkImageFormat dk; uint32_t bpp; DkImageSwizzle sw[4]; bool expandRGB; };

static bool texFormat(GLenum format, GLenum type, FmtInfo& f)
{
	const DkImageSwizzle R = DkImageSwizzle_Red, G = DkImageSwizzle_Green, B = DkImageSwizzle_Blue,
	                     A = DkImageSwizzle_Alpha, Z = DkImageSwizzle_Zero, O = DkImageSwizzle_One;
	f.expandRGB = false;
	auto set = [&](DkImageFormat dk, uint32_t bpp, DkImageSwizzle a, DkImageSwizzle b, DkImageSwizzle c, DkImageSwizzle d) {
		f.dk = dk; f.bpp = bpp; f.sw[0] = a; f.sw[1] = b; f.sw[2] = c; f.sw[3] = d;
	};
	if (type == GL_UNSIGNED_BYTE) {
		switch (format) {
			case GL_RGBA: set(DkImageFormat_RGBA8_Unorm, 4, R, G, B, A); return true;
			case GL_BGRA_EXT: set(DkImageFormat_BGRA8_Unorm, 4, R, G, B, A); return true;
			case GL_RGB: set(DkImageFormat_RGBA8_Unorm, 4, R, G, B, O); f.expandRGB = true; return true;
			case GL_ALPHA: set(DkImageFormat_R8_Unorm, 1, Z, Z, Z, R); return true;
			case GL_LUMINANCE: set(DkImageFormat_R8_Unorm, 1, R, R, R, O); return true;
			case GL_LUMINANCE_ALPHA: set(DkImageFormat_RG8_Unorm, 2, R, R, R, G); return true;
			case GL_RED: set(DkImageFormat_R8_Unorm, 1, R, Z, Z, O); return true;
			case GL_RG: set(DkImageFormat_RG8_Unorm, 2, R, G, Z, O); return true;
		}
	} else if (type == GL_FLOAT && format == GL_RGBA) { set(DkImageFormat_RGBA32_Float, 16, R, G, B, A); return true; }
	else if ((type == GL_HALF_FLOAT || type == GL_HALF_FLOAT_OES) && format == GL_RGBA) { set(DkImageFormat_RGBA16_Float, 8, R, G, B, A); return true; }
	return false;
}

static Texture* boundTexture(GLenum target)
{
	if (target != GL_TEXTURE_2D) return nullptr;
	GLuint id = S.tex2D[S.activeUnit];
	auto it = s_textures.find(id);
	return it == s_textures.end() ? nullptr : &it->second;
}

// a mip chain only for very large 8-bit colour textures (backgrounds/atlases drawn much smaller than their size)
extern "C" void cne_mem_info(unsigned long long* used, unsigned long long* total) __attribute__((weak));

static uint32_t autoMipLevels(uint32_t w, uint32_t h, const FmtInfo& f)
{
	uint32_t m = w > h ? w : h;
	if (m < 4096 || f.bpp != 4) return 1;
	if (cne_mem_info) {   // the extra third of memory only when there is room for it
		unsigned long long used = 0, total = 0;
		cne_mem_info(&used, &total);
		if (used > 1800ull * 1024 * 1024) return 1;
	}
	uint32_t levels = 1;
	while ((m >> levels) >= 1 && levels < 6) levels++;
	return levels;
}

static void generateMips(Texture& t)
{
	if (!t.valid || t.mipLevels <= 1 || !t.useMips) return;
	for (uint32_t i = 1; i < t.mipLevels; i++) {
		DkImageView sv, dv;
		dkImageViewDefaults(&sv, &t.image); sv.mipLevelOffset = i - 1; sv.mipLevelCount = 1;
		dkImageViewDefaults(&dv, &t.image); dv.mipLevelOffset = i;     dv.mipLevelCount = 1;
		uint32_t sw = t.width >> (i - 1), sh = t.height >> (i - 1), dw = t.width >> i, dh = t.height >> i;
		DkImageRect sr = { 0, 0, 0, sw ? sw : 1, sh ? sh : 1, 1 };
		DkImageRect dr = { 0, 0, 0, dw ? dw : 1, dh ? dh : 1, 1 };
		dkCmdBufBarrier(ctx.cmd, DkBarrier_Primitives, DkInvalidateFlags_Image);
		dkCmdBufBlitImage(ctx.cmd, &sv, &sr, &dv, &dr, DkBlitFlag_FilterLinear | DkBlitFlag_ModeBlit, 0);
	}
	dkCmdBufBarrier(ctx.cmd, DkBarrier_Primitives, DkInvalidateFlags_Image);
}

static void textureAllocate(Texture& t, uint32_t lw, uint32_t lh, const FmtInfo& f, uint32_t div = 1)
{
	uint32_t w = (lw + div - 1) / div, h = (lh + div - 1) / div;   // storage size
	uint32_t levels = autoMipLevels(w, h, f);
	bool same = t.valid && t.width == w && t.height == h && t.dkFormat == f.dk && t.mipLevels == levels && t.div == div;
	if (!same) {
		if (t.valid) blockDestroyDeferred(t.mem);
		DkImageLayoutMaker m;
		dkImageLayoutMakerDefaults(&m, ctx.device);
		m.flags = DkImageFlags_UsageRender;   // no HwCompression: it pads/aligns every texture heavily
		m.format = f.dk;
		m.dimensions[0] = w ? w : 1;
		m.dimensions[1] = h ? h : 1;
		m.mipLevels = levels;
		dkImageLayoutInitialize(&t.layout, &m);
		t.mem = blockCreate((uint32_t)dkImageLayoutGetSize(&t.layout), DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
		dkImageInitialize(&t.image, &t.layout, t.mem.mem, 0);
		t.valid = t.mem.mem != nullptr;
		t.width = w; t.height = h;
		t.dkFormat = f.dk;
		t.mipLevels = levels;
		t.useMips = levels > 1;
		t.div = div;
	}
	t.bpp = f.bpp;
	for (int i = 0; i < 4; i++) t.swizzle[i] = f.sw[i];
	if (t.desc <= 0) t.desc = descAlloc();
	writeTextureDescriptors(t);
}

// writes output rows [r0, r0+rows) as tightly packed rows; div 2 averages 2x2 source pixels (half resolution)
static void packRows(uint8_t* dst, const uint8_t* src, int w, int h, int r0, int rows, uint32_t srcPitch, uint32_t dstRow,
                     bool expandRGB, uint32_t bpp, uint32_t div)
{
	uint32_t sb = expandRGB ? 3 : bpp;
	if (div == 1) {
		for (int r = 0; r < rows; r++) {
			const uint8_t* s = src + (size_t)(r0 + r) * srcPitch;
			uint8_t* d = dst + (size_t)r * dstRow;
			if (expandRGB) { for (int c = 0; c < w; c++) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; d += 4; s += 3; } }
			else memcpy(d, s, dstRow);
		}
		return;
	}
	int ow = (w + 1) / 2;
	for (int r = 0; r < rows; r++) {
		int sy0 = (r0 + r) * 2, sy1 = sy0 + 1 < h ? sy0 + 1 : sy0;
		const uint8_t* a = src + (size_t)sy0 * srcPitch;
		const uint8_t* b = src + (size_t)sy1 * srcPitch;
		uint8_t* d = dst + (size_t)r * dstRow;
		for (int c = 0; c < ow; c++) {
			int sx0 = c * 2, sx1 = sx0 + 1 < w ? sx0 + 1 : sx0;
			const uint8_t *p0 = a + sx0 * sb, *p1 = a + sx1 * sb, *p2 = b + sx0 * sb, *p3 = b + sx1 * sb;
			for (int ch = 0; ch < 3; ch++) d[ch] = (uint8_t)((p0[ch] + p1[ch] + p2[ch] + p3[ch] + 2) >> 2);
			d[3] = expandRGB ? 255 : (uint8_t)((p0[3] + p1[3] + p2[3] + p3[3] + 2) >> 2);
			d += 4;
		}
	}
}

static void copyBand(Texture& t, DkGpuAddr from, int x, int y, int w, int rows)
{
	DkImageView view;
	dkImageViewDefaults(&view, &t.image);
	view.mipLevelOffset = 0; view.mipLevelCount = 1;
	DkCopyBuf cb = { from, 0, 0 };
	DkImageRect rect = { (uint32_t)x, (uint32_t)y, 0, (uint32_t)w, (uint32_t)rows, 1 };
	dkCmdBufBarrier(ctx.cmd, DkBarrier_Fragments, 0);
	dkCmdBufCopyBufferToImage(ctx.cmd, &cb, &view, &rect, 0);
	dkCmdBufBarrier(ctx.cmd, DkBarrier_None, DkInvalidateFlags_Image);
}

static void textureUpload(Texture& t, int x, int y, int w, int h, GLenum format, const FmtInfo& f, const void* pixels)
{
	if (!pixels || w <= 0 || h <= 0 || !t.valid) return;
	uint32_t div = t.div;
	uint32_t srcBpp = f.expandRGB ? 3 : f.bpp;
	uint32_t srcRow = (uint32_t)w * srcBpp;
	uint32_t srcPitch = (srcRow + S.unpackAlign - 1) / S.unpackAlign * S.unpackAlign;
	int ow = div == 2 ? (w + 1) / 2 : w, oh = div == 2 ? (h + 1) / 2 : h;   // what gets stored
	int ox = x / (int)div, oy = y / (int)div;
	if (ox + ow > (int)t.width) ow = (int)t.width - ox;
	if (oy + oh > (int)t.height) oh = (int)t.height - oy;
	if (ow <= 0 || oh <= 0) return;
	uint32_t dstRow = (uint32_t)ow * f.bpp;
	const uint8_t* src = (const uint8_t*)pixels;
	uint64_t total = (uint64_t)dstRow * oh;

	// small and medium uploads (incl. 1080p video frames) go through the per-frame ring as one copy
	static constexpr uint32_t kBand = 16u * 1024 * 1024;
	if (total <= 24u * 1024 * 1024) {
		Alloc st = ringAlloc((uint32_t)total, 256);
		packRows((uint8_t*)st.cpu, src, w, h, 0, oh, srcPitch, dstRow, f.expandRGB, f.bpp, div);
		copyBand(t, st.gpu, ox, oy, ow, oh);
	} else {
		// huge textures: stream through one reusable 16 MB buffer, band by band, instead of a second full copy
		static Block band;
		if (!band.mem) band = blockCreate(kBand, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
		if (!band.mem) { logf("dkgl: no memory for the upload buffer"); return; }
		int rowsPer = (int)(kBand / dstRow);
		if (rowsPer < 1) rowsPer = 1;
		for (int r0 = 0; r0 < oh; r0 += rowsPer) {
			int rows = (oh - r0) < rowsPer ? (oh - r0) : rowsPer;
			packRows((uint8_t*)band.cpu, src, w, h, r0, rows, srcPitch, dstRow, f.expandRGB, f.bpp, div);
			copyBand(t, band.gpu, ox, oy + r0, ow, rows);
			flushAndWait();   // the band buffer is reused for the next rows
		}
		S.viewportDirty = S.stateDirty = S.shadersDirty = true;
	}
	generateMips(t);       // keep the automatic mip chain in step with level 0
	S.targetsDirty = true; // copies may disturb the bound render targets
}

static void createWhiteTexture()
{
	s_whiteTex = s_nextName++;
	Texture& t = s_textures[s_whiteTex];
	FmtInfo f;
	texFormat(GL_RGBA, GL_UNSIGNED_BYTE, f);
	textureAllocate(t, 1, 1, f);
	uint32_t white = 0xFFFFFFFF;
	textureUpload(t, 0, 0, 1, 1, GL_RGBA, f, &white);
}

// ---------------------------------------------------------------- render targets
static DkImage* fboColorImage(Framebuffer& fb, uint32_t& w, uint32_t& h)
{
	if (fb.colorTex) {
		auto it = s_textures.find(fb.colorTex);
		if (it != s_textures.end() && it->second.valid) { w = it->second.width; h = it->second.height; it->second.lastUsed = ctx.frameSerial; return &it->second.image; }
	}
	if (fb.colorRb) {
		auto it = s_rbos.find(fb.colorRb);
		if (it != s_rbos.end() && it->second.valid) { w = it->second.width; h = it->second.height; return &it->second.image; }
	}
	return nullptr;
}

static void bindTargets()
{
	if (!S.targetsDirty) return;
	DkImage* color = nullptr;
	DkImage* depth = nullptr;
	uint32_t w = ctx.width, h = ctx.height;
	if (S.fbo == 0) {
		color = defaultColorImage();
		depth = &ctx.dsImage;
		S.targetIsDefault = true;
	} else {
		Framebuffer& fb = s_fbos[S.fbo];
		color = fboColorImage(fb, w, h);
		if (fb.depthRb) {
			auto it = s_rbos.find(fb.depthRb);
			if (it != s_rbos.end() && it->second.valid) depth = &it->second.image;
		}
		S.targetIsDefault = false;
	}
	dkCmdBufBarrier(ctx.cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
	DkImageView cv, dv;
	if (color) dkImageViewDefaults(&cv, color);
	if (depth) dkImageViewDefaults(&dv, depth);
	DkImageView const* cvs[1] = { &cv };
	dkCmdBufBindRenderTargets(ctx.cmd, color ? cvs : nullptr, color ? 1 : 0, depth ? &dv : nullptr);
	S.targetW = w; S.targetH = h;
	S.targetsDirty = false;
	S.viewportDirty = true;
}

// heavy shader pass: the draw is rendered into a smaller layer texture first (see drawCall)
static struct HeavyPass { bool active = false; float k = 1.0f; int vx = 0, vy = 0; uint32_t tw = 0, th = 0; } HP;

static void applyViewport()
{
	if (!S.viewportDirty) return;
	if (HP.active) {
		DkViewport hv = { 0.0f, 0.0f, S.vp[2] * HP.k, S.vp[3] * HP.k, 0.0f, 1.0f };
		dkCmdBufSetViewports(ctx.cmd, 0, &hv, 1);
		int hx = 0, hy = 0, hw = (int)HP.tw, hh = (int)HP.th;
		if (S.scissorTest) {
			hx = (int)floorf((S.sc[0] - HP.vx) * HP.k); hy = (int)floorf((S.sc[1] - HP.vy) * HP.k);
			hw = (int)ceilf((S.sc[0] + S.sc[2] - HP.vx) * HP.k) - hx; hh = (int)ceilf((S.sc[1] + S.sc[3] - HP.vy) * HP.k) - hy;
			if (hx < 0) { hw += hx; hx = 0; }
			if (hy < 0) { hh += hy; hy = 0; }
			if (hw < 0) hw = 0;
			if (hh < 0) hh = 0;
		}
		DkScissor hs = { (uint32_t)hx, (uint32_t)hy, (uint32_t)hw, (uint32_t)hh };
		dkCmdBufSetScissors(ctx.cmd, 0, &hs, 1);
		S.viewportDirty = false;
		return;
	}
	float k = S.targetIsDefault ? ctx.fbScale : 1.0f;
	DkViewport vp;
	vp.x = S.vp[0] * k;
	vp.y = S.vp[1] * k;
	vp.width = S.vp[2] * k;
	vp.height = S.vp[3] * k;
	vp.near = 0.0f; vp.far = 1.0f;
	dkCmdBufSetViewports(ctx.cmd, 0, &vp, 1);

	int x = 0, y = 0, w = (int)S.targetW, h = (int)S.targetH;
	if (S.scissorTest) {
		x = (int)floorf(S.sc[0] * k); y = (int)floorf(S.sc[1] * k);
		w = (int)ceilf((S.sc[0] + S.sc[2]) * k) - x; h = (int)ceilf((S.sc[1] + S.sc[3]) * k) - y;
	}
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (w < 0) w = 0;
	if (h < 0) h = 0;
	DkScissor sc = { (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h };
	dkCmdBufSetScissors(ctx.cmd, 0, &sc, 1);
	S.viewportDirty = false;
}

// ---------------------------------------------------------------- state translation
static DkBlendFactor blendFactor(GLenum f)
{
	switch (f) {
		case GL_ZERO: return DkBlendFactor_Zero;
		case GL_ONE: return DkBlendFactor_One;
		case GL_SRC_COLOR: return DkBlendFactor_SrcColor;
		case GL_ONE_MINUS_SRC_COLOR: return DkBlendFactor_InvSrcColor;
		case GL_SRC_ALPHA: return DkBlendFactor_SrcAlpha;
		case GL_ONE_MINUS_SRC_ALPHA: return DkBlendFactor_InvSrcAlpha;
		case GL_DST_ALPHA: return DkBlendFactor_DstAlpha;
		case GL_ONE_MINUS_DST_ALPHA: return DkBlendFactor_InvDstAlpha;
		case GL_DST_COLOR: return DkBlendFactor_DstColor;
		case GL_ONE_MINUS_DST_COLOR: return DkBlendFactor_InvDstColor;
		case GL_SRC_ALPHA_SATURATE: return DkBlendFactor_SrcAlphaSaturate;
		case GL_CONSTANT_COLOR: return DkBlendFactor_ConstColor;
		case GL_ONE_MINUS_CONSTANT_COLOR: return DkBlendFactor_InvConstColor;
		case GL_CONSTANT_ALPHA: return DkBlendFactor_ConstAlpha;
		case GL_ONE_MINUS_CONSTANT_ALPHA: return DkBlendFactor_InvConstAlpha;
	}
	return DkBlendFactor_One;
}
static DkBlendOp blendOp(GLenum e)
{
	switch (e) {
		case GL_FUNC_SUBTRACT: return DkBlendOp_Sub;
		case GL_FUNC_REVERSE_SUBTRACT: return DkBlendOp_RevSub;
		case GL_MIN: return DkBlendOp_Min;
		case GL_MAX: return DkBlendOp_Max;
	}
	return DkBlendOp_Add;
}
static DkCompareOp compareOp(GLenum f)
{
	switch (f) {
		case GL_NEVER: return DkCompareOp_Never;
		case GL_LESS: return DkCompareOp_Less;
		case GL_EQUAL: return DkCompareOp_Equal;
		case GL_LEQUAL: return DkCompareOp_Lequal;
		case GL_GREATER: return DkCompareOp_Greater;
		case GL_NOTEQUAL: return DkCompareOp_NotEqual;
		case GL_GEQUAL: return DkCompareOp_Gequal;
	}
	return DkCompareOp_Always;
}
static DkStencilOp stencilOp(GLenum o)
{
	switch (o) {
		case GL_ZERO: return DkStencilOp_Zero;
		case GL_REPLACE: return DkStencilOp_Replace;
		case GL_INCR: return DkStencilOp_Incr;
		case GL_DECR: return DkStencilOp_Decr;
		case GL_INVERT: return DkStencilOp_Invert;
		case GL_INCR_WRAP: return DkStencilOp_IncrWrap;
		case GL_DECR_WRAP: return DkStencilOp_DecrWrap;
	}
	return DkStencilOp_Keep;
}

static void applyState()
{
	if (!S.stateDirty) return;
	bool flip = false;
	DkRasterizerState rs;
	dkRasterizerStateDefaults(&rs);
	rs.cullMode = !S.cull ? DkFace_None : S.cullMode == GL_FRONT ? DkFace_Front : S.cullMode == GL_BACK ? DkFace_Back : DkFace_FrontAndBack;
	bool ccw = S.frontFace == GL_CCW;
	if (flip) ccw = !ccw;
	rs.frontFace = ccw ? DkFrontFace_CCW : DkFrontFace_CW;
	dkCmdBufBindRasterizerState(ctx.cmd, &rs);

	DkColorState cs;
	dkColorStateDefaults(&cs);
	dkColorStateSetBlendEnable(&cs, 0, S.blend);
	dkCmdBufBindColorState(ctx.cmd, &cs);

	DkBlendState bs;
	dkBlendStateDefaults(&bs);
	dkBlendStateSetOps(&bs, blendOp(S.blendEqRGB), blendOp(S.blendEqA));
	dkBlendStateSetFactors(&bs, blendFactor(S.blendSrcRGB), blendFactor(S.blendDstRGB), blendFactor(S.blendSrcA), blendFactor(S.blendDstA));
	dkCmdBufBindBlendStates(ctx.cmd, 0, &bs, 1);
	dkCmdBufSetBlendConst(ctx.cmd, S.blendColor[0], S.blendColor[1], S.blendColor[2], S.blendColor[3]);

	DkColorWriteState cw;
	dkColorWriteStateDefaults(&cw);
	uint32_t mask = (S.colorMask[0] ? DkColorMask_R : 0) | (S.colorMask[1] ? DkColorMask_G : 0) |
	                (S.colorMask[2] ? DkColorMask_B : 0) | (S.colorMask[3] ? DkColorMask_A : 0);
	dkColorWriteStateSetMask(&cw, 0, mask);
	dkCmdBufBindColorWriteState(ctx.cmd, &cw);

	DkDepthStencilState ds;
	dkDepthStencilStateDefaults(&ds);
	ds.depthTestEnable = S.depthTest;
	ds.depthWriteEnable = S.depthTest && S.depthMask;
	ds.depthCompareOp = compareOp(S.depthFunc);
	ds.stencilTestEnable = S.stencilTest;
	ds.stencilFrontFailOp = stencilOp(S.stencilFail[0]);
	ds.stencilFrontDepthFailOp = stencilOp(S.stencilZFail[0]);
	ds.stencilFrontPassOp = stencilOp(S.stencilZPass[0]);
	ds.stencilFrontCompareOp = compareOp(S.stencilFunc[0]);
	ds.stencilBackFailOp = stencilOp(S.stencilFail[1]);
	ds.stencilBackDepthFailOp = stencilOp(S.stencilZFail[1]);
	ds.stencilBackPassOp = stencilOp(S.stencilZPass[1]);
	ds.stencilBackCompareOp = compareOp(S.stencilFunc[1]);
	dkCmdBufBindDepthStencilState(ctx.cmd, &ds);
	dkCmdBufSetStencil(ctx.cmd, DkFace_Front, S.stencilWriteMask[0], S.stencilRef[0], S.stencilValueMask[0]);
	dkCmdBufSetStencil(ctx.cmd, DkFace_Back, S.stencilWriteMask[1], S.stencilRef[1], S.stencilValueMask[1]);
	S.stateDirty = false;
}

// ---------------------------------------------------------------- draws
static bool attribFormat(const Attrib& a, DkVtxAttribSize& size, DkVtxAttribType& type, uint32_t& bytes)
{
	int n = a.size;
	if (n < 1 || n > 4) return false;
	switch (a.type) {
		case GL_FLOAT: {
			static const DkVtxAttribSize s[4] = {DkVtxAttribSize_1x32, DkVtxAttribSize_2x32, DkVtxAttribSize_3x32, DkVtxAttribSize_4x32};
			size = s[n - 1]; type = DkVtxAttribType_Float; bytes = 4 * n; return true;
		}
		case GL_UNSIGNED_BYTE: case GL_BYTE: {
			static const DkVtxAttribSize s[4] = {DkVtxAttribSize_1x8, DkVtxAttribSize_2x8, DkVtxAttribSize_3x8, DkVtxAttribSize_4x8};
			size = s[n - 1];
			type = a.normalized ? (a.type == GL_BYTE ? DkVtxAttribType_Snorm : DkVtxAttribType_Unorm)
			                    : (a.type == GL_BYTE ? DkVtxAttribType_Sscaled : DkVtxAttribType_Uscaled);
			bytes = n; return true;
		}
		case GL_UNSIGNED_SHORT: case GL_SHORT: {
			static const DkVtxAttribSize s[4] = {DkVtxAttribSize_1x16, DkVtxAttribSize_2x16, DkVtxAttribSize_3x16, DkVtxAttribSize_4x16};
			size = s[n - 1];
			type = a.normalized ? (a.type == GL_SHORT ? DkVtxAttribType_Snorm : DkVtxAttribType_Unorm)
			                    : (a.type == GL_SHORT ? DkVtxAttribType_Sscaled : DkVtxAttribType_Uscaled);
			bytes = 2 * n; return true;
		}
	}
	return false;
}

static bool prepareDraw(GLsizei vertexCountHint)
{
	auto pit = s_programs.find(S.program);
	if (pit == s_programs.end() || !pit->second.linked) return false;
	Program& p = pit->second;

	bindTargets();
	applyViewport();
	applyState();
	if (S.shadersDirty) {
		DkShader const* sh[2] = { &p.vs, &p.fs };
		dkCmdBufBindShaders(ctx.cmd, DkStageFlag_GraphicsMask, sh, 2);
		S.shadersDirty = false;
	}

	// textures
	for (Uniform& u : p.uniforms) {
		if (u.binding < 0) continue;
		for (int i = 0; i < u.arraySize; i++) {
			int unit = (i == 0) ? u.unit : u.unit + i;
			GLuint id = (unit >= 0 && unit < (int)kMaxTextureUnits) ? S.tex2D[unit] : 0;
			auto it = s_textures.find(id);
			Texture* t = (it != s_textures.end() && it->second.valid) ? &it->second : &s_textures[s_whiteTex];
			if (t->samplerDirty) writeTextureDescriptors(*t);
			t->lastUsed = ctx.frameSerial;
			DkResHandle h = dkMakeTextureHandle(t->desc, t->desc);
			dkCmdBufBindTextures(ctx.cmd, DkStage_Vertex, u.binding + i, &h, 1);
			dkCmdBufBindTextures(ctx.cmd, DkStage_Fragment, u.binding + i, &h, 1);
		}
	}
	if (ctx.descDirty) { dkCmdBufBarrier(ctx.cmd, DkBarrier_None, DkInvalidateFlags_Descriptors); ctx.descDirty = false; }

	// uniforms
	if (p.fragScaleOffset >= 0) {   // map gl_FragCoord back to GL pixel units (heavy pass / scaled framebuffer)
		float v[4] = { 1.0f, 1.0f, 0.0f, 0.0f };
		if (HP.active) { v[0] = v[1] = 1.0f / HP.k; v[2] = (float)HP.vx; v[3] = (float)HP.vy; }
		else if (S.targetIsDefault && ctx.fbScale != 1.0f) v[0] = v[1] = 1.0f / ctx.fbScale;
		memcpy(p.uboData.data() + p.fragScaleOffset, v, sizeof v);
	}
	if (p.uboSize) {
		Alloc a = ringAlloc(p.uboSize, DK_UNIFORM_BUF_ALIGNMENT);
		memcpy(a.cpu, p.uboData.data(), p.uboSize);
		DkBufExtents e = { a.gpu, p.uboSize };
		dkCmdBufBindUniformBuffers(ctx.cmd, DkStage_Vertex, 0, &e, 1);
		dkCmdBufBindUniformBuffers(ctx.cmd, DkStage_Fragment, 0, &e, 1);
	}

	// vertex inputs: one vertex buffer slot per enabled attribute
	DkVtxAttribState as[kMaxAttribs];
	DkVtxBufferState bs[kMaxAttribs];
	DkBufExtents be[kMaxAttribs];
	int maxLoc = -1;
	for (auto& at : p.attribs) if (at.second > maxLoc && at.second < (int)kMaxAttribs) maxLoc = at.second;
	int slots = 0;
	for (int loc = 0; loc <= maxLoc; loc++) {
		memset(&as[loc], 0, sizeof(as[loc]));
		const Attrib& a = S.attribs[loc];
		DkVtxAttribSize size; DkVtxAttribType type; uint32_t bytes;
		if (!a.enabled || !attribFormat(a, size, type, bytes)) {
			// constant attribute (glVertexAttrib*): OpenFL sets single-value inputs like alpha this way.
			// One vec4 in memory, read per instance (divisor 1) so every vertex of the draw sees it.
			Alloc c = ringAlloc(16, 16);
			memcpy(c.cpu, a.value, 16);
			as[loc].bufferId = slots; as[loc].offset = 0;
			as[loc].size = DkVtxAttribSize_4x32; as[loc].type = DkVtxAttribType_Float;
			bs[slots] = { 16, 1 };
			be[slots] = { c.gpu, 16 };
			slots++;
			continue;
		}
		uint32_t stride = a.stride ? a.stride : bytes;
		DkGpuAddr addr;
		uint32_t avail;
		if (a.buffer) {
			auto bit = s_buffers.find(a.buffer);
			if (bit == s_buffers.end() || !bit->second.gpu) { as[loc].isFixed = 1; continue; }
			Buffer& b = bit->second;
			b.lastUsed = ctx.frameSerial;
			addr = b.gpu + a.pointer;
			avail = a.pointer < b.size ? b.size - (uint32_t)a.pointer : 0;
		} else {
			// client-side array: copy what the draw can read
			uint32_t n = (uint32_t)vertexCountHint * stride;
			Alloc c = ringAlloc(n ? n : 16, 16);
			if (a.pointer && n) memcpy(c.cpu, (const void*)a.pointer, n);
			addr = c.gpu; avail = n;
		}
		as[loc].bufferId = slots; as[loc].offset = 0; as[loc].size = size; as[loc].type = type;
		bs[slots] = { stride, 0 };
		be[slots] = { addr, avail };
		slots++;
	}
	if (maxLoc >= 0) dkCmdBufBindVtxAttribState(ctx.cmd, as, maxLoc + 1);
	if (slots) {
		dkCmdBufBindVtxBufferState(ctx.cmd, bs, slots);
		dkCmdBufBindVtxBuffers(ctx.cmd, 0, be, slots);
	}
	return true;
}

static DkPrimitive primitive(GLenum mode)
{
	switch (mode) {
		case GL_POINTS: return DkPrimitive_Points;
		case GL_LINES: return DkPrimitive_Lines;
		case GL_LINE_LOOP: return DkPrimitive_LineLoop;
		case GL_LINE_STRIP: return DkPrimitive_LineStrip;
		case GL_TRIANGLE_STRIP: return DkPrimitive_TriangleStrip;
		case GL_TRIANGLE_FAN: return DkPrimitive_TriangleFan;
	}
	return DkPrimitive_Triangles;
}

// ================================================================ GL functions
#define GLF static

// --- errors / info
GLF GLenum gl_GetError() { GLenum e = S.error; S.error = GL_NO_ERROR; return e; }
static std::vector<const char*> s_extensions = {"GL_EXT_texture_format_BGRA8888", "GL_OES_standard_derivatives",
                                                "GL_EXT_shader_texture_lod", "GL_OES_element_index_uint"};
GLF const GLubyte* gl_GetString(GLenum name)
{
	static std::string ext;
	switch (name) {
		case GL_VENDOR: return (const GLubyte*)"dkgl";
		case GL_RENDERER: return (const GLubyte*)"deko3d (Tegra X1) via dkgl";
		case GL_VERSION: return (const GLubyte*)"3.3.0 dkgl";
		case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte*)"3.30 dkgl";
		case GL_EXTENSIONS:
			if (ext.empty()) for (const char* e : s_extensions) { ext += e; ext += ' '; }
			return (const GLubyte*)ext.c_str();
	}
	return (const GLubyte*)"";
}
GLF const GLubyte* gl_GetStringi(GLenum name, GLuint i)
{
	if (name == GL_EXTENSIONS && i < s_extensions.size()) return (const GLubyte*)s_extensions[i];
	return nullptr;
}
GLF void gl_GetIntegerv(GLenum p, GLint* v)
{
	if (!v) return;
	switch (p) {
		case GL_MAX_TEXTURE_SIZE: case GL_MAX_RENDERBUFFER_SIZE: *v = 16384; return;
		case GL_MAX_VIEWPORT_DIMS: v[0] = v[1] = 16384; return;
		case GL_VIEWPORT: memcpy(v, S.vp, sizeof S.vp); return;
		case GL_SCISSOR_BOX: memcpy(v, S.sc, sizeof S.sc); return;
		case GL_FRAMEBUFFER_BINDING: *v = S.fbo; return;
		case GL_RENDERBUFFER_BINDING: *v = S.rbo; return;
		case GL_CURRENT_PROGRAM: *v = S.program; return;
		case GL_ACTIVE_TEXTURE: *v = GL_TEXTURE0 + S.activeUnit; return;
		case GL_TEXTURE_BINDING_2D: *v = S.tex2D[S.activeUnit]; return;
		case GL_ARRAY_BUFFER_BINDING: *v = S.arrayBuffer; return;
		case GL_ELEMENT_ARRAY_BUFFER_BINDING: *v = S.elementBuffer; return;
		case GL_MAX_VERTEX_ATTRIBS: *v = kMaxAttribs; return;
		case GL_MAX_TEXTURE_IMAGE_UNITS: case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS: *v = kMaxTextureUnits; return;
		case GL_MAX_VERTEX_UNIFORM_VECTORS: case GL_MAX_FRAGMENT_UNIFORM_VECTORS: *v = 1024; return;
		case GL_MAX_VARYING_VECTORS: *v = 32; return;
		case GL_NUM_EXTENSIONS: *v = (GLint)s_extensions.size(); return;
		case GL_MAJOR_VERSION: *v = 3; return;
		case GL_MINOR_VERSION: *v = 3; return;
		case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: case GL_ALPHA_BITS: case GL_STENCIL_BITS: *v = 8; return;
		case GL_DEPTH_BITS: *v = 24; return;
		case GL_UNPACK_ALIGNMENT: *v = S.unpackAlign; return;
		case GL_PACK_ALIGNMENT: *v = S.packAlign; return;
		case GL_NUM_PROGRAM_BINARY_FORMATS: case GL_NUM_SHADER_BINARY_FORMATS: case GL_SAMPLES: case GL_SAMPLE_BUFFERS: *v = 0; return;
		case GL_MAX_SAMPLES: *v = 1; return;
		case GL_IMPLEMENTATION_COLOR_READ_FORMAT: *v = GL_RGBA; return;
		case GL_IMPLEMENTATION_COLOR_READ_TYPE: *v = GL_UNSIGNED_BYTE; return;
		case GL_STENCIL_REF: *v = S.stencilRef[0]; return;
		case GL_STENCIL_WRITEMASK: *v = S.stencilWriteMask[0]; return;
		case GL_BLEND_SRC_RGB: *v = S.blendSrcRGB; return;
		case GL_BLEND_DST_RGB: *v = S.blendDstRGB; return;
	}
	*v = 0;
}
GLF void gl_GetFloatv(GLenum p, GLfloat* v)
{
	if (!v) return;
	if (p == GL_COLOR_CLEAR_VALUE) { memcpy(v, S.clearColor, sizeof S.clearColor); return; }
	if (p == GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT) { *v = 1.0f; return; }
	GLint iv[4] = {0};
	gl_GetIntegerv(p, iv);
	*v = (GLfloat)iv[0];
}
GLF void gl_GetBooleanv(GLenum p, GLboolean* v)
{
	if (!v) return;
	if (p == GL_COLOR_WRITEMASK) { for (int i = 0; i < 4; i++) v[i] = S.colorMask[i]; return; }
	if (p == GL_DEPTH_WRITEMASK) { *v = S.depthMask; return; }
	GLint iv[4] = {0};
	gl_GetIntegerv(p, iv);
	*v = iv[0] != 0;
}
GLF GLboolean gl_IsEnabled(GLenum cap)
{
	switch (cap) {
		case GL_BLEND: return S.blend;
		case GL_SCISSOR_TEST: return S.scissorTest;
		case GL_DEPTH_TEST: return S.depthTest;
		case GL_STENCIL_TEST: return S.stencilTest;
		case GL_CULL_FACE: return S.cull;
	}
	return GL_FALSE;
}
GLF void gl_Flush() {}
GLF void gl_Finish() { flushAndWait(); }
GLF void gl_Hint(GLenum, GLenum) {}
GLF void gl_PixelStorei(GLenum p, GLint v)
{
	if (p == GL_UNPACK_ALIGNMENT) S.unpackAlign = v > 0 ? v : 1;
	else if (p == GL_PACK_ALIGNMENT) S.packAlign = v > 0 ? v : 1;
}

// --- capabilities and fixed-function state
GLF void setCap(GLenum cap, bool on)
{
	switch (cap) {
		case GL_BLEND: S.blend = on; S.stateDirty = true; break;
		case GL_SCISSOR_TEST: S.scissorTest = on; S.viewportDirty = true; break;
		case GL_DEPTH_TEST: S.depthTest = on; S.stateDirty = true; break;
		case GL_STENCIL_TEST: S.stencilTest = on; S.stateDirty = true; break;
		case GL_CULL_FACE: S.cull = on; S.stateDirty = true; break;
	}
}
GLF void gl_Enable(GLenum cap) { setCap(cap, true); }
GLF void gl_Disable(GLenum cap) { setCap(cap, false); }
GLF void gl_Viewport(GLint x, GLint y, GLsizei w, GLsizei h) { S.vp[0] = x; S.vp[1] = y; S.vp[2] = w; S.vp[3] = h; S.viewportDirty = true; }
GLF void gl_Scissor(GLint x, GLint y, GLsizei w, GLsizei h) { S.sc[0] = x; S.sc[1] = y; S.sc[2] = w; S.sc[3] = h; S.viewportDirty = true; }
GLF void gl_BlendFunc(GLenum s, GLenum d) { S.blendSrcRGB = S.blendSrcA = s; S.blendDstRGB = S.blendDstA = d; S.stateDirty = true; }
GLF void gl_BlendFuncSeparate(GLenum sr, GLenum dr, GLenum sa, GLenum da) { S.blendSrcRGB = sr; S.blendDstRGB = dr; S.blendSrcA = sa; S.blendDstA = da; S.stateDirty = true; }
GLF void gl_BlendEquation(GLenum e) { S.blendEqRGB = S.blendEqA = e; S.stateDirty = true; }
GLF void gl_BlendEquationSeparate(GLenum r, GLenum a) { S.blendEqRGB = r; S.blendEqA = a; S.stateDirty = true; }
GLF void gl_BlendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { S.blendColor[0] = r; S.blendColor[1] = g; S.blendColor[2] = b; S.blendColor[3] = a; S.stateDirty = true; }
GLF void gl_BlendBarrier() { dkCmdBufBarrier(ctx.cmd, DkBarrier_Fragments, DkInvalidateFlags_Image); }
GLF void gl_ColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { S.colorMask[0] = r; S.colorMask[1] = g; S.colorMask[2] = b; S.colorMask[3] = a; S.stateDirty = true; }
GLF void gl_DepthMask(GLboolean m) { S.depthMask = m; S.stateDirty = true; }
GLF void gl_DepthFunc(GLenum f) { S.depthFunc = f; S.stateDirty = true; }
GLF void gl_DepthRangef(GLfloat, GLfloat) {}
GLF void gl_ClearDepthf(GLfloat d) { S.clearDepth = d; }
GLF void gl_ClearDepth(double d) { S.clearDepth = (float)d; }
GLF void gl_StencilFuncSeparate(GLenum face, GLenum f, GLint ref, GLuint mask)
{
	for (int i = 0; i < 2; i++)
		if (face == GL_FRONT_AND_BACK || (i == 0 && face == GL_FRONT) || (i == 1 && face == GL_BACK)) { S.stencilFunc[i] = f; S.stencilRef[i] = ref; S.stencilValueMask[i] = mask; }
	S.stateDirty = true;
}
GLF void gl_StencilFunc(GLenum f, GLint ref, GLuint mask) { gl_StencilFuncSeparate(GL_FRONT_AND_BACK, f, ref, mask); }
GLF void gl_StencilOpSeparate(GLenum face, GLenum sf, GLenum zf, GLenum zp)
{
	for (int i = 0; i < 2; i++)
		if (face == GL_FRONT_AND_BACK || (i == 0 && face == GL_FRONT) || (i == 1 && face == GL_BACK)) { S.stencilFail[i] = sf; S.stencilZFail[i] = zf; S.stencilZPass[i] = zp; }
	S.stateDirty = true;
}
GLF void gl_StencilOp(GLenum sf, GLenum zf, GLenum zp) { gl_StencilOpSeparate(GL_FRONT_AND_BACK, sf, zf, zp); }
GLF void gl_StencilMaskSeparate(GLenum face, GLuint m)
{
	for (int i = 0; i < 2; i++)
		if (face == GL_FRONT_AND_BACK || (i == 0 && face == GL_FRONT) || (i == 1 && face == GL_BACK)) S.stencilWriteMask[i] = m;
	S.stateDirty = true;
}
GLF void gl_StencilMask(GLuint m) { gl_StencilMaskSeparate(GL_FRONT_AND_BACK, m); }
GLF void gl_CullFace(GLenum m) { S.cullMode = m; S.stateDirty = true; }
GLF void gl_FrontFace(GLenum m) { S.frontFace = m; S.stateDirty = true; }
GLF void gl_LineWidth(GLfloat w) { dkCmdBufSetLineWidth(ctx.cmd, w); }
GLF void gl_PolygonOffset(GLfloat, GLfloat) {}
GLF void gl_SampleCoverage(GLfloat, GLboolean) {}
GLF void gl_ClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { S.clearColor[0] = r; S.clearColor[1] = g; S.clearColor[2] = b; S.clearColor[3] = a; }
GLF void gl_ClearStencil(GLint s) { S.clearStencil = s; }
GLF void gl_Clear(GLbitfield mask)
{
	bindTargets();
	applyViewport();  // clears honour the scissor rectangle like GL
	if (mask & GL_COLOR_BUFFER_BIT) {
		uint32_t cm = (S.colorMask[0] ? DkColorMask_R : 0) | (S.colorMask[1] ? DkColorMask_G : 0) |
		              (S.colorMask[2] ? DkColorMask_B : 0) | (S.colorMask[3] ? DkColorMask_A : 0);
		dkCmdBufClearColor(ctx.cmd, 0, cm, S.clearColor);
	}
	if (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT))
		dkCmdBufClearDepthStencil(ctx.cmd, (mask & GL_DEPTH_BUFFER_BIT) != 0, S.clearDepth,
		                          (mask & GL_STENCIL_BUFFER_BIT) ? S.stencilWriteMask[0] : 0, (uint8_t)S.clearStencil);
}

// --- names
template <typename M> static void genNames(GLsizei n, GLuint* out, M& map)
{
	for (GLsizei i = 0; i < n; i++) { GLuint id = s_nextName++; map[id]; out[i] = id; }
}
GLF void gl_GenTextures(GLsizei n, GLuint* t) { genNames(n, t, s_textures); }
GLF void gl_GenBuffers(GLsizei n, GLuint* b) { genNames(n, b, s_buffers); }
GLF void gl_GenFramebuffers(GLsizei n, GLuint* f) { genNames(n, f, s_fbos); }
GLF void gl_GenRenderbuffers(GLsizei n, GLuint* r) { genNames(n, r, s_rbos); }
GLF void gl_GenVertexArrays(GLsizei n, GLuint* a) { for (GLsizei i = 0; i < n; i++) a[i] = s_nextName++; }
GLF void gl_BindVertexArray(GLuint) {}
GLF void gl_DeleteVertexArrays(GLsizei, const GLuint*) {}
GLF GLboolean gl_IsTexture(GLuint t) { return s_textures.count(t) ? GL_TRUE : GL_FALSE; }
GLF GLboolean gl_IsBuffer(GLuint b) { return s_buffers.count(b) ? GL_TRUE : GL_FALSE; }
GLF GLboolean gl_IsFramebuffer(GLuint f) { return s_fbos.count(f) ? GL_TRUE : GL_FALSE; }
GLF GLboolean gl_IsRenderbuffer(GLuint r) { return s_rbos.count(r) ? GL_TRUE : GL_FALSE; }

GLF void gl_DeleteTextures(GLsizei n, const GLuint* ids)
{
	for (GLsizei i = 0; i < n; i++) {
		auto it = s_textures.find(ids[i]);
		if (it == s_textures.end() || ids[i] == s_whiteTex) continue;
		if (it->second.valid) blockDestroyDeferred(it->second.mem);
		descFree(it->second.desc);
		for (GLuint& u : S.tex2D) if (u == ids[i]) u = 0;
		s_textures.erase(it);
	}
}
GLF void gl_DeleteBuffers(GLsizei n, const GLuint* ids)
{
	for (GLsizei i = 0; i < n; i++) {
		auto it = s_buffers.find(ids[i]);
		if (it == s_buffers.end()) continue;
		blockDestroyDeferred(it->second.persist);
		s_buffers.erase(it);
		s_shadow.erase(ids[i]);
		if (S.arrayBuffer == ids[i]) S.arrayBuffer = 0;
		if (S.elementBuffer == ids[i]) S.elementBuffer = 0;
	}
}
GLF void gl_DeleteFramebuffers(GLsizei n, const GLuint* ids)
{
	for (GLsizei i = 0; i < n; i++) { s_fbos.erase(ids[i]); if (S.fbo == ids[i]) { S.fbo = 0; S.targetsDirty = true; } }
}
GLF void gl_DeleteRenderbuffers(GLsizei n, const GLuint* ids)
{
	for (GLsizei i = 0; i < n; i++) {
		auto it = s_rbos.find(ids[i]);
		if (it == s_rbos.end()) continue;
		if (it->second.valid) blockDestroyDeferred(it->second.mem);
		s_rbos.erase(it);
	}
}

// --- textures
GLF void gl_ActiveTexture(GLenum t) { S.activeUnit = (t - GL_TEXTURE0) < kMaxTextureUnits ? t - GL_TEXTURE0 : 0; }
GLF void gl_BindTexture(GLenum target, GLuint id)
{
	if (target != GL_TEXTURE_2D) return;
	if (id && !s_textures.count(id)) s_textures[id];
	S.tex2D[S.activeUnit] = id;
}
GLF void gl_TexParameteri(GLenum target, GLenum p, GLint v)
{
	Texture* t = boundTexture(target);
	if (!t) return;
	switch (p) {
		case GL_TEXTURE_MIN_FILTER: t->minFilter = v; break;
		case GL_TEXTURE_MAG_FILTER: t->magFilter = v; break;
		case GL_TEXTURE_WRAP_S: t->wrapS = v; break;
		case GL_TEXTURE_WRAP_T: t->wrapT = v; break;
		default: return;
	}
	t->samplerDirty = true;
}
GLF void gl_TexParameterf(GLenum target, GLenum p, GLfloat v) { gl_TexParameteri(target, p, (GLint)v); }
GLF void gl_TexParameteriv(GLenum target, GLenum p, const GLint* v) { if (v) gl_TexParameteri(target, p, v[0]); }
GLF void gl_TexParameterfv(GLenum target, GLenum p, const GLfloat* v) { if (v) gl_TexParameteri(target, p, (GLint)v[0]); }
GLF void gl_TexImage2D(GLenum target, GLint level, GLint, GLsizei w, GLsizei h, GLint, GLenum format, GLenum type, const void* pixels)
{
	Texture* t = boundTexture(target);
	if (!t || level != 0) return;   // mip levels other than 0 are ignored
	FmtInfo f;
	if (!texFormat(format, type, f)) { logf("dkgl: unsupported texture format 0x%x/0x%x", format, type); setError(GL_INVALID_ENUM); return; }
	// giant images (6144+ px, e.g. 8192x8192 character sheets) are stored at half resolution: 4x less memory
	// and upload work. Render targets (no pixel data) keep their full size.
	uint32_t div = (pixels && f.bpp == 4 && (w >= 6144 || h >= 6144)) ? 2 : 1;
	if (div == 2 && (t->div != 2 || t->width != (uint32_t)(w + 1) / 2)) logf("dkgl: %dx%d texture stored at half resolution", w, h);
	textureAllocate(*t, w, h, f, div);
	textureUpload(*t, 0, 0, w, h, format, f, pixels);
}
GLF void gl_TexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void* pixels)
{
	Texture* t = boundTexture(target);
	if (!t || level != 0 || !t->valid) return;
	FmtInfo f;
	if (!texFormat(format, type, f)) return;
	textureUpload(*t, x, y, w, h, format, f, pixels);
}
GLF void gl_CompressedTexImage2D(GLenum, GLint, GLenum fmt, GLsizei, GLsizei, GLint, GLsizei, const void*)
{
	logf("dkgl: compressed textures not supported yet (0x%x)", fmt);
	setError(GL_INVALID_ENUM);
}
GLF void gl_GenerateMipmap(GLenum) {}
GLF void gl_CopyTexSubImage2D(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei) { logf("dkgl: glCopyTexSubImage2D not implemented"); }
GLF void gl_CopyTexImage2D(GLenum, GLint, GLenum, GLint, GLint, GLsizei, GLsizei, GLint) { logf("dkgl: glCopyTexImage2D not implemented"); }

// --- buffers
GLF void gl_BindBuffer(GLenum target, GLuint id)
{
	if (id && !s_buffers.count(id)) s_buffers[id];
	if (target == GL_ARRAY_BUFFER) S.arrayBuffer = id;
	else if (target == GL_ELEMENT_ARRAY_BUFFER) S.elementBuffer = id;
}
GLF void gl_BufferData(GLenum target, GLsizeiptr size, const void* data, GLenum)
{
	GLuint id = target == GL_ARRAY_BUFFER ? S.arrayBuffer : target == GL_ELEMENT_ARRAY_BUFFER ? S.elementBuffer : 0;
	auto it = s_buffers.find(id);
	if (it == s_buffers.end() || size < 0) return;
	Buffer& b = it->second;
	uint32_t sz = (uint32_t)size;
	if (!b.persist.mem || b.persist.size < sz) {
		if (b.persist.mem) blockDestroyDeferred(b.persist);
		b.persist = blockCreate(sz ? sz : 16, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
		if (b.transientFrame < 0) { b.cpu = b.persist.cpu; b.gpu = b.persist.gpu; b.lastUsed = 0; }
	}
	b.size = sz;
	if (!b.cpu) { b.cpu = b.persist.cpu; b.gpu = b.persist.gpu; }
	s_shadow[id].assign(sz, 0);
	bufferWrite(id, b, 0, data, sz, true);
}
GLF void gl_BufferSubData(GLenum target, GLintptr off, GLsizeiptr size, const void* data)
{
	GLuint id = target == GL_ARRAY_BUFFER ? S.arrayBuffer : target == GL_ELEMENT_ARRAY_BUFFER ? S.elementBuffer : 0;
	auto it = s_buffers.find(id);
	if (it == s_buffers.end() || off < 0 || size < 0 || (uint32_t)(off + size) > it->second.size) { setError(GL_INVALID_VALUE); return; }
	bufferWrite(id, it->second, (uint32_t)off, data, (uint32_t)size, false);
}

// --- vertex attributes
GLF void gl_VertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void* ptr)
{
	if (i >= kMaxAttribs) return;
	Attrib& a = S.attribs[i];
	a.size = size; a.type = type; a.normalized = norm; a.stride = stride; a.pointer = (uintptr_t)ptr; a.buffer = S.arrayBuffer;
}
GLF void gl_EnableVertexAttribArray(GLuint i) { if (i < kMaxAttribs) S.attribs[i].enabled = true; }
GLF void gl_DisableVertexAttribArray(GLuint i) { if (i < kMaxAttribs) S.attribs[i].enabled = false; }
GLF void gl_VertexAttrib4f(GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { if (i < kMaxAttribs) { float* v = S.attribs[i].value; v[0] = x; v[1] = y; v[2] = z; v[3] = w; } }
GLF void gl_VertexAttrib1f(GLuint i, GLfloat x) { gl_VertexAttrib4f(i, x, 0, 0, 1); }
GLF void gl_VertexAttrib2f(GLuint i, GLfloat x, GLfloat y) { gl_VertexAttrib4f(i, x, y, 0, 1); }
GLF void gl_VertexAttrib3f(GLuint i, GLfloat x, GLfloat y, GLfloat z) { gl_VertexAttrib4f(i, x, y, z, 1); }
GLF void gl_VertexAttrib1fv(GLuint i, const GLfloat* v) { gl_VertexAttrib4f(i, v[0], 0, 0, 1); }
GLF void gl_VertexAttrib2fv(GLuint i, const GLfloat* v) { gl_VertexAttrib4f(i, v[0], v[1], 0, 1); }
GLF void gl_VertexAttrib3fv(GLuint i, const GLfloat* v) { gl_VertexAttrib4f(i, v[0], v[1], v[2], 1); }
GLF void gl_VertexAttrib4fv(GLuint i, const GLfloat* v) { gl_VertexAttrib4f(i, v[0], v[1], v[2], v[3]); }
GLF void gl_VertexAttribDivisor(GLuint, GLuint) {}

// --- shaders and programs
GLF GLuint gl_CreateShader(GLenum type) { GLuint id = s_nextName++; g_shaders[id].type = type; return id; }
GLF void gl_ShaderSource(GLuint id, GLsizei count, const GLchar* const* str, const GLint* len)
{
	auto it = g_shaders.find(id);
	if (it == g_shaders.end()) return;
	std::string s;
	for (GLsizei i = 0; i < count; i++) {
		if (!str[i]) continue;
		if (len && len[i] >= 0) s.append(str[i], len[i]); else s += str[i];
	}
	it->second.source = s;
}
GLF void gl_CompileShader(GLuint id)
{
	auto it = g_shaders.find(id);
	if (it != g_shaders.end()) { it->second.compiled = true; it->second.log.clear(); } // real compile happens at link
}
GLF void gl_DeleteShader(GLuint id) { g_shaders.erase(id); }
GLF GLboolean gl_IsShader(GLuint id) { return g_shaders.count(id) ? GL_TRUE : GL_FALSE; }
GLF void gl_GetShaderiv(GLuint id, GLenum p, GLint* v)
{
	auto it = g_shaders.find(id);
	if (!v) return;
	if (it == g_shaders.end()) { *v = 0; setError(GL_INVALID_VALUE); return; }
	switch (p) {
		case GL_COMPILE_STATUS: *v = it->second.compiled; return;
		case GL_SHADER_TYPE: *v = it->second.type; return;
		case GL_INFO_LOG_LENGTH: *v = (GLint)it->second.log.size() + 1; return;
		case GL_SHADER_SOURCE_LENGTH: *v = (GLint)it->second.source.size() + 1; return;
		case GL_DELETE_STATUS: *v = 0; return;
	}
	*v = 0;
}
static void copyLog(const std::string& s, GLsizei max, GLsizei* len, GLchar* out)
{
	if (!out || max <= 0) { if (len) *len = 0; return; }
	GLsizei n = (GLsizei)s.size() < max - 1 ? (GLsizei)s.size() : max - 1;
	memcpy(out, s.data(), n);
	out[n] = 0;
	if (len) *len = n;
}
GLF void gl_GetShaderInfoLog(GLuint id, GLsizei max, GLsizei* len, GLchar* out)
{
	auto it = g_shaders.find(id);
	copyLog(it == g_shaders.end() ? "" : it->second.log, max, len, out);
}
GLF void gl_GetShaderSource(GLuint id, GLsizei max, GLsizei* len, GLchar* out)
{
	auto it = g_shaders.find(id);
	copyLog(it == g_shaders.end() ? "" : it->second.source, max, len, out);
}
GLF void gl_ShaderBinary(GLsizei, const GLuint*, GLenum, const void*, GLsizei) { setError(GL_INVALID_ENUM); }
GLF void gl_ReleaseShaderCompiler() {}
GLF GLuint gl_CreateProgram() { GLuint id = s_nextName++; s_programs[id]; return id; }
GLF void gl_DeleteProgram(GLuint id) { s_programs.erase(id); if (S.program == id) S.program = 0; }
GLF GLboolean gl_IsProgram(GLuint id) { return s_programs.count(id) ? GL_TRUE : GL_FALSE; }
GLF void gl_AttachShader(GLuint p, GLuint sh) { auto it = s_programs.find(p); if (it != s_programs.end()) it->second.shaders.push_back(sh); }
GLF void gl_DetachShader(GLuint p, GLuint sh)
{
	auto it = s_programs.find(p);
	if (it == s_programs.end()) return;
	auto& v = it->second.shaders;
	v.erase(std::remove(v.begin(), v.end(), sh), v.end());
}
GLF void gl_BindAttribLocation(GLuint p, GLuint idx, const GLchar* name) { auto it = s_programs.find(p); if (it != s_programs.end() && name) it->second.boundAttribs[name] = idx; }
GLF void gl_LinkProgram(GLuint id)
{
	auto it = s_programs.find(id);
	if (it == s_programs.end()) return;
	Program& p = it->second;
	p.log.clear();
	p.linked = linkProgram(p, p.log);
	if (S.program == id) S.shadersDirty = true;
}
GLF void gl_ValidateProgram(GLuint) {}
GLF void gl_UseProgram(GLuint id) { if (S.program != id) { S.program = id; S.shadersDirty = true; } }
static Program* programOf(GLuint id) { auto it = s_programs.find(id); return it == s_programs.end() ? nullptr : &it->second; }
GLF void gl_GetProgramiv(GLuint id, GLenum pn, GLint* v)
{
	Program* p = programOf(id);
	if (!v) return;
	if (!p) { *v = 0; setError(GL_INVALID_VALUE); return; }
	switch (pn) {
		case GL_LINK_STATUS: *v = p->linked; return;
		case GL_VALIDATE_STATUS: *v = p->linked; return;
		case GL_INFO_LOG_LENGTH: *v = (GLint)p->log.size() + 1; return;
		case GL_ATTACHED_SHADERS: *v = (GLint)p->shaders.size(); return;
		case GL_ACTIVE_UNIFORMS: *v = (GLint)p->uniforms.size(); return;
		case GL_ACTIVE_ATTRIBUTES: *v = (GLint)p->attribTypes.size(); return;
		case GL_ACTIVE_UNIFORM_MAX_LENGTH: case GL_ACTIVE_ATTRIBUTE_MAX_LENGTH: *v = 256; return;
		case GL_DELETE_STATUS: *v = 0; return;
	}
	*v = 0;
}
GLF void gl_GetProgramInfoLog(GLuint id, GLsizei max, GLsizei* len, GLchar* out)
{
	Program* p = programOf(id);
	copyLog(p ? p->log : "", max, len, out);
}
static GLenum glslTypeEnum(const std::string& t)
{
	static const std::unordered_map<std::string, GLenum> m = {
		{"float", GL_FLOAT}, {"vec2", GL_FLOAT_VEC2}, {"vec3", GL_FLOAT_VEC3}, {"vec4", GL_FLOAT_VEC4},
		{"int", GL_INT}, {"ivec2", GL_INT_VEC2}, {"ivec3", GL_INT_VEC3}, {"ivec4", GL_INT_VEC4},
		{"bool", GL_BOOL}, {"bvec2", GL_BOOL_VEC2}, {"bvec3", GL_BOOL_VEC3}, {"bvec4", GL_BOOL_VEC4},
		{"mat2", GL_FLOAT_MAT2}, {"mat3", GL_FLOAT_MAT3}, {"mat4", GL_FLOAT_MAT4},
		{"sampler2D", GL_SAMPLER_2D}, {"samplerCube", GL_SAMPLER_CUBE}};
	auto it = m.find(t);
	return it == m.end() ? GL_FLOAT : it->second;
}
GLF void gl_GetActiveUniform(GLuint id, GLuint i, GLsizei max, GLsizei* len, GLint* size, GLenum* type, GLchar* name)
{
	Program* p = programOf(id);
	if (!p || i >= p->uniforms.size()) { setError(GL_INVALID_VALUE); return; }
	const Uniform& u = p->uniforms[i];
	if (size) *size = u.arraySize;
	if (type) *type = glslTypeEnum(u.type);
	copyLog(u.arraySize > 1 ? u.name + "[0]" : u.name, max, len, name);
}
GLF void gl_GetActiveAttrib(GLuint id, GLuint i, GLsizei max, GLsizei* len, GLint* size, GLenum* type, GLchar* name)
{
	Program* p = programOf(id);
	if (!p || i >= p->attribTypes.size()) { setError(GL_INVALID_VALUE); return; }
	if (size) *size = 1;
	if (type) *type = glslTypeEnum(p->attribTypes[i].second);
	copyLog(p->attribTypes[i].first, max, len, name);
}
GLF GLint gl_GetAttribLocation(GLuint id, const GLchar* name)
{
	Program* p = programOf(id);
	if (!p || !name) return -1;
	auto it = p->attribs.find(name);
	return it == p->attribs.end() ? -1 : it->second;
}
GLF GLint gl_GetUniformLocation(GLuint id, const GLchar* nameIn)
{
	Program* p = programOf(id);
	if (!p || !nameIn) return -1;
	std::string name = nameIn;
	int index = 0;
	size_t br = name.find('[');
	if (br != std::string::npos) { index = atoi(name.c_str() + br + 1); name = name.substr(0, br); }
	for (const Uniform& u : p->uniforms)
		if (u.name == name) return index < u.arraySize ? u.location + index : -1;
	return -1;
}

// uniform writes: kind 0 = float, 1 = int; comps per element; count elements
static void setUniform(GLint loc, int kind, int comps, GLsizei count, const void* data, int matCols = 0, bool transpose = false)
{
	Program* p = programOf(S.program);
	if (!p || loc < 0 || !data) return;
	for (Uniform& u : p->uniforms) {
		if (loc < u.location || loc >= u.location + u.arraySize) continue;
		int first = loc - u.location;
		int n = count;
		if (first + n > u.arraySize) n = u.arraySize - first;
		if (u.binding >= 0) { // sampler: texture unit
			if (kind == 1) { const GLint* iv = (const GLint*)data; u.unit = iv[0] - first; }
			return;
		}
		bool targetInt = u.type[0] == 'i' || u.type[0] == 'b' || u.type == "int" || u.type == "bool";
		for (int e = 0; e < n; e++) {
			uint8_t* dst = p->uboData.data() + u.offset + (first + e) * u.stride;
			if (matCols) {
				const float* m = (const float*)data + e * matCols * matCols;
				for (int c = 0; c < matCols; c++)
					for (int r = 0; r < matCols; r++) {
						float v = transpose ? m[r * matCols + c] : m[c * matCols + r];
						memcpy(dst + c * 16 + r * 4, &v, 4);
					}
				continue;
			}
			for (int c = 0; c < comps; c++) {
				if (kind == 0) {
					float f = ((const float*)data)[e * comps + c];
					if (targetInt) { int32_t iv = (int32_t)f; memcpy(dst + c * 4, &iv, 4); }
					else memcpy(dst + c * 4, &f, 4);
				} else {
					int32_t iv = ((const GLint*)data)[e * comps + c];
					if (targetInt) memcpy(dst + c * 4, &iv, 4);
					else { float f = (float)iv; memcpy(dst + c * 4, &f, 4); }
				}
			}
		}
		return;
	}
}
GLF void gl_Uniform1f(GLint l, GLfloat a) { setUniform(l, 0, 1, 1, &a); }
GLF void gl_Uniform2f(GLint l, GLfloat a, GLfloat b) { GLfloat v[2] = {a, b}; setUniform(l, 0, 2, 1, v); }
GLF void gl_Uniform3f(GLint l, GLfloat a, GLfloat b, GLfloat c) { GLfloat v[3] = {a, b, c}; setUniform(l, 0, 3, 1, v); }
GLF void gl_Uniform4f(GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d) { GLfloat v[4] = {a, b, c, d}; setUniform(l, 0, 4, 1, v); }
GLF void gl_Uniform1i(GLint l, GLint a) { setUniform(l, 1, 1, 1, &a); }
GLF void gl_Uniform2i(GLint l, GLint a, GLint b) { GLint v[2] = {a, b}; setUniform(l, 1, 2, 1, v); }
GLF void gl_Uniform3i(GLint l, GLint a, GLint b, GLint c) { GLint v[3] = {a, b, c}; setUniform(l, 1, 3, 1, v); }
GLF void gl_Uniform4i(GLint l, GLint a, GLint b, GLint c, GLint d) { GLint v[4] = {a, b, c, d}; setUniform(l, 1, 4, 1, v); }
GLF void gl_Uniform1fv(GLint l, GLsizei n, const GLfloat* v) { setUniform(l, 0, 1, n, v); }
GLF void gl_Uniform2fv(GLint l, GLsizei n, const GLfloat* v) { setUniform(l, 0, 2, n, v); }
GLF void gl_Uniform3fv(GLint l, GLsizei n, const GLfloat* v) { setUniform(l, 0, 3, n, v); }
GLF void gl_Uniform4fv(GLint l, GLsizei n, const GLfloat* v) { setUniform(l, 0, 4, n, v); }
GLF void gl_Uniform1iv(GLint l, GLsizei n, const GLint* v) { setUniform(l, 1, 1, n, v); }
GLF void gl_Uniform2iv(GLint l, GLsizei n, const GLint* v) { setUniform(l, 1, 2, n, v); }
GLF void gl_Uniform3iv(GLint l, GLsizei n, const GLint* v) { setUniform(l, 1, 3, n, v); }
GLF void gl_Uniform4iv(GLint l, GLsizei n, const GLint* v) { setUniform(l, 1, 4, n, v); }
GLF void gl_UniformMatrix2fv(GLint l, GLsizei n, GLboolean t, const GLfloat* v) { setUniform(l, 0, 0, n, v, 2, t); }
GLF void gl_UniformMatrix3fv(GLint l, GLsizei n, GLboolean t, const GLfloat* v) { setUniform(l, 0, 0, n, v, 3, t); }
GLF void gl_UniformMatrix4fv(GLint l, GLsizei n, GLboolean t, const GLfloat* v) { setUniform(l, 0, 0, n, v, 4, t); }
GLF void gl_GetUniformfv(GLuint, GLint, GLfloat* v) { if (v) *v = 0; }
GLF void gl_GetUniformiv(GLuint, GLint, GLint* v) { if (v) *v = 0; }
GLF void gl_GetUniformuiv(GLuint, GLint, GLuint* v) { if (v) *v = 0; }

// --- framebuffers / renderbuffers
GLF void gl_BindFramebuffer(GLenum, GLuint id)
{
	if (id && !s_fbos.count(id)) s_fbos[id];
	if (S.fbo != id) { S.fbo = id; S.targetsDirty = true; S.stateDirty = true; }
}
GLF void gl_FramebufferTexture2D(GLenum, GLenum attach, GLenum, GLuint tex, GLint)
{
	if (!S.fbo) return;
	Framebuffer& fb = s_fbos[S.fbo];
	if (attach == GL_COLOR_ATTACHMENT0) {
		fb.colorTex = tex; fb.colorRb = 0;
		auto it = s_textures.find(tex);   // rendered-to textures can't keep a mip chain in step
		if (it != s_textures.end() && it->second.useMips) { it->second.useMips = false; writeTextureDescriptors(it->second); }
	}
	else fb.depthTex = tex;
	S.targetsDirty = true;
}
GLF void gl_FramebufferRenderbuffer(GLenum, GLenum attach, GLenum, GLuint rb)
{
	if (!S.fbo) return;
	Framebuffer& fb = s_fbos[S.fbo];
	if (attach == GL_COLOR_ATTACHMENT0) { fb.colorRb = rb; fb.colorTex = 0; }
	else fb.depthRb = rb;   // depth, stencil and depth_stencil all share one Z24S8 renderbuffer
	S.targetsDirty = true;
}
GLF GLenum gl_CheckFramebufferStatus(GLenum)
{
	if (!S.fbo) return GL_FRAMEBUFFER_COMPLETE;
	uint32_t w, h;
	return fboColorImage(s_fbos[S.fbo], w, h) ? GL_FRAMEBUFFER_COMPLETE : GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
}
GLF void gl_BindRenderbuffer(GLenum, GLuint id) { if (id && !s_rbos.count(id)) s_rbos[id]; S.rbo = id; }
GLF void gl_RenderbufferStorage(GLenum, GLenum fmt, GLsizei w, GLsizei h)
{
	auto it = s_rbos.find(S.rbo);
	if (it == s_rbos.end()) return;
	Renderbuffer& r = it->second;
	if (r.valid) blockDestroyDeferred(r.mem);
	bool color = fmt == GL_RGBA8 || fmt == GL_RGBA || fmt == GL_RGBA4 || fmt == GL_RGB565 || fmt == GL_RGB5_A1;
	r.dkFormat = color ? DkImageFormat_RGBA8_Unorm : DkImageFormat_Z24S8;
	DkImageLayoutMaker m;
	dkImageLayoutMakerDefaults(&m, ctx.device);
	m.flags = DkImageFlags_UsageRender | DkImageFlags_HwCompression;
	m.format = r.dkFormat;
	m.dimensions[0] = w ? w : 1;
	m.dimensions[1] = h ? h : 1;
	dkImageLayoutInitialize(&r.layout, &m);
	r.mem = blockCreate((uint32_t)dkImageLayoutGetSize(&r.layout), DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
	dkImageInitialize(&r.image, &r.layout, r.mem.mem, 0);
	r.valid = r.mem.mem != nullptr;
	r.width = w; r.height = h;
	S.targetsDirty = true;
}
GLF void gl_RenderbufferStorageMultisample(GLenum t, GLsizei, GLenum fmt, GLsizei w, GLsizei h) { gl_RenderbufferStorage(t, fmt, w, h); }
GLF void gl_GetFramebufferAttachmentParameteriv(GLenum, GLenum, GLenum, GLint* v) { if (v) *v = 0; }
GLF void gl_GetRenderbufferParameteriv(GLenum, GLenum, GLint* v) { if (v) *v = 0; }
GLF void gl_DrawBuffers(GLsizei, const GLenum*) {}
GLF void gl_ReadBuffer(GLenum) {}
GLF void gl_BlitFramebuffer(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum) { logf("dkgl: glBlitFramebuffer not implemented"); }

GLF void gl_ReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* out)
{
	if (!out || w <= 0 || h <= 0 || type != GL_UNSIGNED_BYTE || (format != GL_RGBA && format != GL_BGRA_EXT)) return;
	bindTargets();
	DkImage* img = nullptr;
	DkImageFormat srcFmt = DkImageFormat_RGBA8_Unorm;
	if (S.fbo == 0) img = defaultColorImage();
	else {
		uint32_t fw, fh;
		img = fboColorImage(s_fbos[S.fbo], fw, fh);
		Texture* t = nullptr;
		auto it = s_textures.find(s_fbos[S.fbo].colorTex);
		if (it != s_textures.end()) { t = &it->second; srcFmt = t->dkFormat; }
	}
	if (!img) return;
	// framebuffer 0 may be rendered smaller (dynamic resolution): read that area and scale it back
	float k = S.fbo == 0 ? ctx.fbScale : 1.0f;
	int rx = (int)floorf(x * k), ry = (int)floorf(y * k);
	int rw = (int)ceilf(w * k), rh = (int)ceilf(h * k);
	if (rw < 1) rw = 1;
	if (rh < 1) rh = 1;
	uint32_t row = (uint32_t)rw * 4;
	Block tmp = blockCreate(row * rh, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
	DkImageView view;
	dkImageViewDefaults(&view, img);
	DkImageRect rect = { (uint32_t)rx, (uint32_t)ry, 0, (uint32_t)rw, (uint32_t)rh, 1 };
	DkCopyBuf cb = { tmp.gpu, 0, 0 };
	dkCmdBufBarrier(ctx.cmd, DkBarrier_Full, 0);
	dkCmdBufCopyImageToBuffer(ctx.cmd, &view, &rect, &cb, 0);
	flushAndWait();
	S.targetsDirty = S.viewportDirty = S.stateDirty = S.shadersDirty = true;
	uint32_t outRow = (uint32_t)w * 4;
	uint32_t outPitch = (outRow + S.packAlign - 1) / S.packAlign * S.packAlign;
	bool swapRB = (srcFmt == DkImageFormat_BGRA8_Unorm) != (format == GL_BGRA_EXT);
	for (int r = 0; r < h; r++) {
		int sr = (rh == h) ? r : (int)((int64_t)r * rh / h);
		const uint8_t* srow = (const uint8_t*)tmp.cpu + sr * row;
		uint8_t* d = (uint8_t*)out + r * outPitch;
		for (int c = 0; c < w; c++) {
			int sc = (rw == w) ? c : (int)((int64_t)c * rw / w);
			const uint8_t* sp = srow + sc * 4;
			if (swapRB) { d[0] = sp[2]; d[1] = sp[1]; d[2] = sp[0]; d[3] = sp[3]; }
			else { d[0] = sp[0]; d[1] = sp[1]; d[2] = sp[2]; d[3] = sp[3]; }
			d += 4;
		}
	}
	blockDestroy(tmp);
}

// --- draws
struct DrawArgs { bool indexed; DkPrimitive prim; uint32_t count, first; DkIdxFormat fmt; DkGpuAddr idx; };

static void issueDraw(const DrawArgs& a)
{
	if (!a.indexed) { dkCmdBufDraw(ctx.cmd, a.prim, a.count, 1, a.first, 0); return; }
	dkCmdBufBindIdxBuffer(ctx.cmd, a.fmt, a.idx);
	dkCmdBufDrawIndexed(ctx.cmd, a.prim, a.count, 1, 0, 0, 0);
}

static Texture s_layer;          // reduced-resolution target for heavy draws
static DkShader s_compositeFs;
static int s_compositeState = 0; // 0 = not tried, 1 = ready, -1 = failed

static DkShader s_compositeDiscardFs;   // same, but drops empty pixels so they can be counted
static int s_discardState = 0;

static bool ensureComposite()
{
	if (s_compositeState) return s_compositeState > 0;
	static const char* src =
		"#version 460\n"
		"layout(location = 0) out vec4 dkgl_out;\n"
		"layout(binding = 0) uniform sampler2D dkgl_layer;\n"
		"layout(std140, binding = 0) uniform DkglComposite { vec4 p; vec4 sz; };\n"
		"void main() { dkgl_out = texture(dkgl_layer, (gl_FragCoord.xy - p.yz) * p.x / sz.xy); }\n";
	static const char* srcDiscard =
		"#version 460\n"
		"layout(location = 0) out vec4 dkgl_out;\n"
		"layout(binding = 0) uniform sampler2D dkgl_layer;\n"
		"layout(std140, binding = 0) uniform DkglComposite { vec4 p; vec4 sz; };\n"
		"void main() {\n"
		"    vec4 c = texture(dkgl_layer, (gl_FragCoord.xy - p.yz) * p.x / sz.xy);\n"
		"    if (max(max(c.r, c.g), max(c.b, c.a)) < 0.002) discard;\n"
		"    dkgl_out = c;\n"
		"}\n";
	s_compositeState = compileInternalFragment(src, s_compositeFs) ? 1 : -1;
	if (s_compositeState < 0) logf("dkgl: composite shader failed; heavy shaders stay at full resolution");
	if (s_compositeState > 0) s_discardState = compileInternalFragment(srcDiscard, s_compositeDiscardFs) ? 1 : -1;
	return s_compositeState > 0;
}

// visible-pixel counts of heavy draws, read back once the frame has finished on the GPU
struct VisQuery { GLuint program; const uint64_t* report; };
static std::vector<VisQuery> s_visQueries[kFramesInFlight];

// skipping empty pixels only leaves the picture unchanged when a zero source keeps the destination as is
static bool zeroSourceKeepsDestination()
{
	if (!S.blend) return false;
	auto keeps = [](GLenum d) { return d == GL_ONE || d == GL_ONE_MINUS_SRC_ALPHA || d == GL_ONE_MINUS_SRC_COLOR; };
	return keeps(S.blendDstRGB) && keeps(S.blendDstA) && S.blendEqRGB == GL_FUNC_ADD && S.blendEqA == GL_FUNC_ADD;
}

void onFrameRetired(int frameIndex)
{
	std::unordered_map<GLuint, uint64_t> visible;
	for (const VisQuery& q : s_visQueries[frameIndex]) {
		uint64_t before = q.report[0], after = q.report[2];
		visible[q.program] += after >= before ? after - before : 1;
	}
	s_visQueries[frameIndex].clear();
	for (auto& v : visible) {
		auto it = s_programs.find(v.first);
		if (it == s_programs.end()) continue;
		Program& p = it->second;
		if (v.second > 0) {
			if (p.skipping) logf("dkgl: heavy shader %u visible again, drawing it every frame", v.first);
			p.skipping = false;
			p.invisibleFrames = 0;
		} else if (!p.skipping && ++p.invisibleFrames >= 30) {
			p.skipping = true;
			p.nextProbe = ctx.frameSerial + 15;
			logf("dkgl: heavy shader %u has drawn nothing visible for 30 frames, skipping it (re-checked every 15 frames)", v.first);
		}
	}
}

static bool ensureLayer(uint32_t w, uint32_t h)
{
	if (s_layer.valid && s_layer.width >= w && s_layer.height >= h) return true;
	FmtInfo f;
	texFormat(GL_RGBA, GL_UNSIGNED_BYTE, f);
	s_layer.minFilter = s_layer.magFilter = GL_LINEAR;
	s_layer.wrapS = s_layer.wrapT = GL_CLAMP_TO_EDGE;
	textureAllocate(s_layer, w > s_layer.width ? w : s_layer.width, h > s_layer.height ? h : s_layer.height, f);
	return s_layer.valid;
}

// Draws normally, or for heavy shaders while the GPU is over budget: shader at reduced resolution into
// a layer (no blending), then the same geometry again with a cheap shader that samples the layer using
// the original blending/stencil/scissor, so coverage and blending stay exact.
static void drawCall(const DrawArgs& a, GLsizei vertexHint)
{
	auto pit = s_programs.find(S.program);
	// heavy shaders are never shaded at more than ~one screen of pixels (OpenFL can render a shaded sprite into
	// a texture as big as the sprite, e.g. 5520x1700), and less when the GPU is late (ctx.renderScale)
	float cap = 1.0f;
	if (S.vp[2] > 0 && S.vp[3] > 0) {
		float area = (float)S.vp[2] * (float)S.vp[3];
		float budget = (float)ctx.width * (float)ctx.height;
		if (area > budget) cap = sqrtf(budget / area);
	}
	float hk = ctx.renderScale < cap ? ctx.renderScale : cap;
	bool heavy = pit != s_programs.end() && pit->second.linked && pit->second.heavy && hk < 0.999f &&
	             S.vp[2] > 0 && S.vp[3] > 0 && ensureComposite();
	if (!heavy) {
		if (prepareDraw(vertexHint)) issueDraw(a);
		return;
	}
	Program& p = pit->second;
	if (p.skipping) {   // invisible effect: only run it now and then to see whether it has appeared
		if (ctx.frameSerial < p.nextProbe) return;
		p.nextProbe = ctx.frameSerial + 15;
	}
	float k = hk;
	uint32_t tw = (uint32_t)ceilf(S.vp[2] * k), th = (uint32_t)ceilf(S.vp[3] * k);
	if (!ensureLayer(tw, th)) { if (prepareDraw(vertexHint)) issueDraw(a); return; }

	// pass 1: the heavy shader, into the layer
	bindTargets();   // make sure the real target is known before switching away
	dkCmdBufBarrier(ctx.cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
	DkImageView lv;
	dkImageViewDefaults(&lv, &s_layer.image);
	DkImageView const* lvs[1] = { &lv };
	dkCmdBufBindRenderTargets(ctx.cmd, lvs, 1, nullptr);
	DkScissor full = { 0, 0, tw, th };
	dkCmdBufSetScissors(ctx.cmd, 0, &full, 1);
	float zero[4] = { 0, 0, 0, 0 };
	dkCmdBufClearColor(ctx.cmd, 0, DkColorMask_RGBA, zero);

	bool sBlend = S.blend, sDepth = S.depthTest, sStencil = S.stencilTest, sDefault = S.targetIsDefault;
	bool sMask[4] = { S.colorMask[0], S.colorMask[1], S.colorMask[2], S.colorMask[3] };
	S.blend = S.depthTest = S.stencilTest = false;
	S.colorMask[0] = S.colorMask[1] = S.colorMask[2] = S.colorMask[3] = true;
	S.targetIsDefault = false;
	HP = { true, k, S.vp[0], S.vp[1], tw, th };
	S.viewportDirty = S.stateDirty = true;
	bool ok = prepareDraw(vertexHint);
	if (ok) issueDraw(a);
	HP.active = false;
	S.blend = sBlend; S.depthTest = sDepth; S.stencilTest = sStencil; S.targetIsDefault = sDefault;
	for (int i = 0; i < 4; i++) S.colorMask[i] = sMask[i];
	S.targetsDirty = S.viewportDirty = S.stateDirty = true;
	if (!ok) return;
	s_layer.lastUsed = ctx.frameSerial;

	// pass 2: same geometry, sample the layer, original state
	bindTargets();
	applyViewport();
	applyState();
	bool measure = s_discardState > 0 && zeroSourceKeepsDestination();
	DkShader const* sh[2] = { &p.vs, measure ? &s_compositeDiscardFs : &s_compositeFs };
	dkCmdBufBindShaders(ctx.cmd, DkStageFlag_GraphicsMask, sh, 2);
	S.shadersDirty = true;   // the next normal draw rebinds the program's own fragment shader
	Alloc u = ringAlloc(32, DK_UNIFORM_BUF_ALIGNMENT);
	float params[8] = { k, (float)HP.vx, (float)HP.vy, 0.0f, (float)s_layer.width, (float)s_layer.height, 0.0f, 0.0f };
	memcpy(u.cpu, params, sizeof params);
	DkBufExtents ue = { u.gpu, 32 };
	dkCmdBufBindUniformBuffers(ctx.cmd, DkStage_Fragment, 0, &ue, 1);
	if (ctx.descDirty) { dkCmdBufBarrier(ctx.cmd, DkBarrier_None, DkInvalidateFlags_Descriptors); ctx.descDirty = false; }
	DkResHandle h = dkMakeTextureHandle(s_layer.desc, s_layer.desc);
	dkCmdBufBindTextures(ctx.cmd, DkStage_Fragment, 0, &h, 1);
	if (measure) {
		Alloc q = ringAlloc(32, 16);
		memset(q.cpu, 0, 32);
		dkCmdBufReportCounter(ctx.cmd, DkCounter_SamplesPassed, q.gpu);
		issueDraw(a);
		dkCmdBufReportCounter(ctx.cmd, DkCounter_SamplesPassed, q.gpu + 16);
		s_visQueries[ctx.curFrameIndex()].push_back({ S.program, (const uint64_t*)q.cpu });
	} else issueDraw(a);
}

GLF void gl_DrawArrays(GLenum mode, GLint first, GLsizei count)
{
	if (count <= 0) return;
	DrawArgs a = { false, primitive(mode), (uint32_t)count, (uint32_t)first, DkIdxFormat_Uint16, 0 };
	drawCall(a, first + count);
}
GLF void gl_DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices)
{
	if (count <= 0) return;
	uint32_t isz = type == GL_UNSIGNED_INT ? 4 : type == GL_UNSIGNED_BYTE ? 1 : 2;
	DkIdxFormat fmt = type == GL_UNSIGNED_INT ? DkIdxFormat_Uint32 : type == GL_UNSIGNED_BYTE ? DkIdxFormat_Uint8 : DkIdxFormat_Uint16;
	// client-side vertex arrays need the highest index to know how much to copy
	GLsizei maxVertex = 0;
	DkGpuAddr addr = 0;
	if (S.elementBuffer) {
		auto it = s_buffers.find(S.elementBuffer);
		if (it == s_buffers.end() || !it->second.gpu) return;
		it->second.lastUsed = ctx.frameSerial;
		addr = it->second.gpu + (uintptr_t)indices;
		const uint8_t* src = s_shadow[S.elementBuffer].data() + (uintptr_t)indices;
		for (GLsizei i = 0; i < count; i++) {
			uint32_t v = isz == 4 ? ((const uint32_t*)src)[i] : isz == 2 ? ((const uint16_t*)src)[i] : src[i];
			if ((GLsizei)v + 1 > maxVertex) maxVertex = v + 1;
		}
	} else {
		Alloc a = ringAlloc(count * isz, 16);
		memcpy(a.cpu, indices, count * isz);
		addr = a.gpu;
		for (GLsizei i = 0; i < count; i++) {
			uint32_t v = isz == 4 ? ((const uint32_t*)indices)[i] : isz == 2 ? ((const uint16_t*)indices)[i] : ((const uint8_t*)indices)[i];
			if ((GLsizei)v + 1 > maxVertex) maxVertex = v + 1;
		}
	}
	DrawArgs a = { true, primitive(mode), (uint32_t)count, 0, fmt, addr };
	drawCall(a, maxVertex);
}

// ================================================================ init + proc table
void glInitState()
{
	S = State{};
	S.vp[2] = S.sc[2] = (GLint)ctx.width;
	S.vp[3] = S.sc[3] = (GLint)ctx.height;
	createWhiteTexture();
}

// unknown functions: a named stub per slot, so the first call is logged with the function's name
static const char* s_unknownNames[512];
static int s_unknownCount = 0;
template <int I> static uintptr_t unknownStub()
{
	static bool once = false;
	if (!once) { once = true; logf("dkgl: called unimplemented %s", s_unknownNames[I] ? s_unknownNames[I] : "?"); }
	return 0;
}
template <int... Is> struct StubTable { static void** get() { static void* t[] = { (void*)&unknownStub<Is>... }; return t; } };
template <int N, int... Is> struct MakeStubs : MakeStubs<N - 1, N - 1, Is...> {};
template <int... Is> struct MakeStubs<0, Is...> { using type = StubTable<Is...>; };
using Stubs = MakeStubs<512>::type;

#define P(fn) { "gl" #fn, (void*)&gl_##fn }
static const std::unordered_map<std::string, void*> s_procs = {
	P(GetError), P(GetString), P(GetStringi), P(GetIntegerv), P(GetFloatv), P(GetBooleanv), P(IsEnabled), P(Flush), P(Finish),
	P(Hint), P(PixelStorei), P(Enable), P(Disable), P(Viewport), P(Scissor), P(BlendFunc), P(BlendFuncSeparate), P(BlendEquation),
	P(BlendEquationSeparate), P(BlendColor), P(BlendBarrier), P(ColorMask), P(DepthMask), P(DepthFunc), P(DepthRangef),
	P(ClearDepthf), P(ClearDepth), P(StencilFunc), P(StencilFuncSeparate), P(StencilOp), P(StencilOpSeparate), P(StencilMask),
	P(StencilMaskSeparate), P(CullFace), P(FrontFace), P(LineWidth), P(PolygonOffset), P(SampleCoverage), P(ClearColor),
	P(ClearStencil), P(Clear), P(GenTextures), P(GenBuffers), P(GenFramebuffers), P(GenRenderbuffers), P(GenVertexArrays),
	P(BindVertexArray), P(DeleteVertexArrays), P(IsTexture), P(IsBuffer), P(IsFramebuffer), P(IsRenderbuffer), P(DeleteTextures),
	P(DeleteBuffers), P(DeleteFramebuffers), P(DeleteRenderbuffers), P(ActiveTexture), P(BindTexture), P(TexParameteri),
	P(TexParameterf), P(TexParameteriv), P(TexParameterfv), P(TexImage2D), P(TexSubImage2D), P(CompressedTexImage2D),
	P(GenerateMipmap), P(CopyTexSubImage2D), P(CopyTexImage2D), P(BindBuffer), P(BufferData), P(BufferSubData),
	P(VertexAttribPointer), P(EnableVertexAttribArray), P(DisableVertexAttribArray), P(VertexAttrib1f), P(VertexAttrib2f),
	P(VertexAttrib3f), P(VertexAttrib4f), P(VertexAttrib1fv), P(VertexAttrib2fv), P(VertexAttrib3fv), P(VertexAttrib4fv),
	P(VertexAttribDivisor), P(CreateShader), P(ShaderSource), P(CompileShader), P(DeleteShader), P(IsShader), P(GetShaderiv),
	P(GetShaderInfoLog), P(GetShaderSource), P(ShaderBinary), P(ReleaseShaderCompiler), P(CreateProgram), P(DeleteProgram),
	P(IsProgram), P(AttachShader), P(DetachShader), P(BindAttribLocation), P(LinkProgram), P(ValidateProgram), P(UseProgram),
	P(GetProgramiv), P(GetProgramInfoLog), P(GetActiveUniform), P(GetActiveAttrib), P(GetAttribLocation), P(GetUniformLocation),
	P(Uniform1f), P(Uniform2f), P(Uniform3f), P(Uniform4f), P(Uniform1i), P(Uniform2i), P(Uniform3i), P(Uniform4i),
	P(Uniform1fv), P(Uniform2fv), P(Uniform3fv), P(Uniform4fv), P(Uniform1iv), P(Uniform2iv), P(Uniform3iv), P(Uniform4iv),
	P(UniformMatrix2fv), P(UniformMatrix3fv), P(UniformMatrix4fv), P(GetUniformfv), P(GetUniformiv), P(GetUniformuiv),
	P(BindFramebuffer), P(FramebufferTexture2D), P(FramebufferRenderbuffer), P(CheckFramebufferStatus), P(BindRenderbuffer),
	P(RenderbufferStorage), P(RenderbufferStorageMultisample), P(GetFramebufferAttachmentParameteriv),
	P(GetRenderbufferParameteriv), P(DrawBuffers), P(ReadBuffer), P(BlitFramebuffer), P(ReadPixels), P(DrawArrays), P(DrawElements),
};
#undef P

void* getProc(const char* name)
{
	if (!name) return nullptr;
	auto it = s_procs.find(name);
	if (it != s_procs.end()) return it->second;
	// also accept the ARB/EXT/OES-suffixed names of functions dkgl has
	std::string n = name;
	for (const char* suf : {"ARB", "EXT", "OES", "KHR"}) {
		size_t l = strlen(suf);
		if (n.size() > l && n.compare(n.size() - l, l, suf) == 0) {
			auto it2 = s_procs.find(n.substr(0, n.size() - l));
			if (it2 != s_procs.end()) return it2->second;
		}
	}
	if (n.compare(0, 2, "gl") != 0) return nullptr;
	if (s_unknownCount >= 512) return nullptr;
	s_unknownNames[s_unknownCount] = strdup(name);
	return Stubs::get()[s_unknownCount++];
}

} // namespace dkgl

extern "C" void dkgl_set_flip_y(int) {}
extern "C" void dkgl_set_dynamic_resolution(int enabled, float minScale)
{
	dkgl::ctx.drsEnabled = enabled != 0;
	dkgl::ctx.drsMin = minScale < 0.25f ? 0.25f : minScale > 1.0f ? 1.0f : minScale;
	if (!dkgl::ctx.drsEnabled) { dkgl::ctx.renderScale = 1.0f; dkgl::S_markDirty(); }
}
extern "C" float dkgl_get_render_scale() { return dkgl::ctx.renderScale; }
extern "C" float dkgl_get_gpu_ms() { return dkgl::ctx.gpuMs; }
