// dkgl: a GLES2-class OpenGL subset (plus the EGL calls SDL makes) implemented on deko3d.
// Lime loads every GL function through eglGetProcAddress, so dkgl only has to export EGL.
#pragma once

#include <switch.h>
#include <deko3d.h>
#define GL_GLEXT_PROTOTYPES 0
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>

#include <stdint.h>
#include <string>
#include <vector>
#include <unordered_map>

namespace dkgl {

void logf(const char* fmt, ...);

constexpr unsigned kFramesInFlight = 3;
constexpr unsigned kMaxTextureUnits = 32;
constexpr unsigned kMaxAttribs = 16;
constexpr unsigned kMaxDescriptors = 4096;

// ---------------------------------------------------------------- memory
struct Block {                    // one DkMemBlock (CPU-visible unless image memory)
	DkMemBlock mem = nullptr;
	uint32_t size = 0;
	void* cpu = nullptr;
	DkGpuAddr gpu = 0;
};
Block blockCreate(uint32_t size, uint32_t flags);
void blockDestroy(Block& b);
void blockDestroyDeferred(Block& b);   // freed once the GPU is done with the current frame

// per-frame linear allocator in CPU-visible memory (uniforms, buffer renames, upload staging)
struct Alloc { void* cpu; DkGpuAddr gpu; };
Alloc ringAlloc(uint32_t size, uint32_t align);

// ---------------------------------------------------------------- objects
struct Buffer {
	Block persist;                // own storage
	void* cpu = nullptr;          // current storage (persist or a ring allocation)
	DkGpuAddr gpu = 0;
	uint32_t size = 0;
	int transientFrame = -1;      // frame whose ring holds the current storage, -1 = persist
	uint64_t lastUsed = 0;        // frame serial of the last draw that read it
};

struct Texture {
	GLenum target = GL_TEXTURE_2D;
	Block mem;
	DkImageLayout layout{};
	DkImage image{};
	bool valid = false;
	uint32_t width = 0, height = 0;
	DkImageFormat dkFormat = DkImageFormat_RGBA8_Unorm;
	DkImageSwizzle swizzle[4] = {DkImageSwizzle_Red, DkImageSwizzle_Green, DkImageSwizzle_Blue, DkImageSwizzle_Alpha};
	uint32_t bpp = 4;
	int desc = -1;                // image + sampler descriptor index
	GLenum minFilter = GL_NEAREST_MIPMAP_LINEAR, magFilter = GL_LINEAR, wrapS = GL_REPEAT, wrapT = GL_REPEAT;
	bool samplerDirty = true;
	uint64_t lastUsed = 0;
	uint32_t mipLevels = 1;       // automatic mip chain for very large textures
	uint32_t div = 1;             // 2 = giant texture stored at half resolution (sampled with 0..1 coords, so it lines up)
	bool useMips = false;
};

struct Renderbuffer {
	Block mem;
	DkImageLayout layout{};
	DkImage image{};
	bool valid = false;
	uint32_t width = 0, height = 0;
	DkImageFormat dkFormat = DkImageFormat_Z24S8;
};

struct Framebuffer {
	GLuint colorTex = 0;          // texture name
	GLuint colorRb = 0;           // or renderbuffer name
	GLuint depthRb = 0;           // depth/stencil renderbuffer name
	GLuint depthTex = 0;
};

struct Uniform {
	std::string name;
	std::string type;             // float, vec2..4, int, ivec2..4, bool, bvec*, mat2..4, sampler2D, samplerCube
	int arraySize = 1;            // 1 = not an array
	uint32_t offset = 0;          // std140 byte offset in the block (non-samplers)
	uint32_t stride = 0;          // std140 array stride
	int location = 0;             // first location (arrays use location..location+arraySize-1)
	int binding = -1;             // samplers: texture binding slot
	int unit = 0;                 // samplers: texture unit set by glUniform1i
};

struct Shader {
	GLenum type = 0;
	std::string source;
	bool compiled = false;
	std::string log;
	bool deletePending = false;
	int attachCount = 0;
};

struct Program {
	std::vector<GLuint> shaders;
	std::unordered_map<std::string, int> boundAttribs;   // glBindAttribLocation
	bool linked = false;
	std::string log;
	// link results
	DkShader vs{}, fs{};
	std::vector<Uniform> uniforms;
	std::unordered_map<std::string, int> attribs;         // name -> location
	std::vector<std::pair<std::string, std::string>> attribTypes; // name, type (for getActiveAttrib)
	std::vector<uint8_t> uboData;
	uint32_t uboSize = 0;
	int numSamplerBindings = 0;
	bool deletePending = false;
	int locationCount = 0;
	int fragScaleOffset = -1;     // dkgl_FragScale in the uniform block (gl_FragCoord correction)
	bool heavy = false;           // big fragment shader: drawn at reduced resolution when the GPU is late
	// invisible-effect skipping (heavy shaders whose output stays fully transparent)
	int invisibleFrames = 0;
	bool skipping = false;
	uint64_t nextProbe = 0;
	uint32_t fsCodeSize = 0;
};

// ---------------------------------------------------------------- shader compile (dkgl_shader.cpp)
struct ShaderBinary { DkShader shader; bool ok; };
bool linkProgram(Program& p, std::string& log);
bool loadDksh(const void* data, size_t size, DkShader& out);
extern uint32_t g_lastDkshCodeSize;
bool compileInternalFragment(const std::string& glsl, DkShader& out);

// ---------------------------------------------------------------- context (dkgl_core.cpp)
struct Context {
	DkDevice device = nullptr;
	DkQueue queue = nullptr;
	DkCmdBuf cmd = nullptr;
	NWindow* window = nullptr;
	uint32_t width = 1280, height = 720;

	// swapchain / default framebuffer
	static constexpr unsigned kSwapImages = 3;
	Block fbMem[kSwapImages];
	DkImage fbImages[kSwapImages];
	Block dsMem;
	DkImage dsImage;
	DkSwapchain swapchain = nullptr;
	int acquiredSlot = -1;
	// internal render target for the default framebuffer (dynamic resolution renders into part of it)
	Block internalMem;
	DkImage internalImage;
	float renderScale = 1.0f;     // resolution used for heavy shader draws (dynamic, from GPU time)
	float fbScale = 1.0f;         // framebuffer 0 stays at full resolution
	bool drsEnabled = true;
	float drsMin = 0.25f;
	float gpuMs = 0.0f;           // smoothed GPU time per frame
	Block tsMem;                  // GPU timestamps, 32 bytes per frame in flight
	uint32_t scaledW() const { uint32_t v = (uint32_t)(width * fbScale + 0.5f); return v ? v : 1; }
	uint32_t scaledH() const { uint32_t v = (uint32_t)(height * fbScale + 0.5f); return v ? v : 1; }
	bool flipY = false;           // set if the default framebuffer shows upside down

	// frames
	uint64_t frameSerial = 1;     // increments every swap; 0 = never used
	uint64_t completedSerial = 0; // all GPU work up to this serial is finished
	DkFence frameFence[kFramesInFlight];
	uint64_t fenceSerial[kFramesInFlight] = {};

	// descriptors
	Block descMem;                // image descriptors then sampler descriptors
	std::vector<int> freeDesc;
	bool descDirty = true;

	int curFrameIndex() const { return (int)(frameSerial % kFramesInFlight); }
};
extern Context ctx;

bool coreInit(NWindow* win);
void coreShutdown();
void ensureRecording();          // command memory available
void flushAndWait();             // submit everything recorded and wait for the GPU
void swapBuffers();
int descAlloc();
void descFree(int d);
void writeTextureDescriptors(Texture& t);
DkImage* defaultColorImage();    // the internal render target behind framebuffer 0

// ---------------------------------------------------------------- GL entry (dkgl_gl.cpp)
void* getProc(const char* name);
void glInitState();
void onFrameStart();
void S_markDirty();

} // namespace dkgl
