// dkgl shaders: rewrites GL-style GLSL (any version, ES or desktop) into deko3d-ready GLSL 460,
// compiles it with UAM on the console and caches the DKSH on the SD card.
#include "dkgl_internal.h"
#include "uam_lib.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <algorithm>
#include <set>

namespace dkgl {

const char* g_shaderCacheDir = "sdmc:/switch/CodenameEngine/shadercache";

// ---------------------------------------------------------------- code memory arena
static std::vector<Block> s_codeBlocks;
static uint32_t s_codeUsed = 0;
static constexpr uint32_t kCodeBlock = 4 * 1024 * 1024;

static bool codeAlloc(uint32_t size, DkMemBlock& mem, uint32_t& offset)
{
	uint32_t off = (s_codeUsed + DK_SHADER_CODE_ALIGNMENT - 1) & ~(DK_SHADER_CODE_ALIGNMENT - 1);
	if (s_codeBlocks.empty() || off + size > s_codeBlocks.back().size) {
		Block b = blockCreate(size > kCodeBlock ? size : kCodeBlock,
		                      DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code);
		if (!b.mem) return false;
		s_codeBlocks.push_back(b);
		off = 0;
	}
	mem = s_codeBlocks.back().mem;
	offset = off;
	s_codeUsed = off + size;
	return true;
}

struct DkshHeader {
	uint32_t magic, header_sz, control_sz, code_sz, programs_off, num_programs;
};

uint32_t g_lastDkshCodeSize = 0;

bool loadDksh(const void* data, size_t size, DkShader& out)
{
	if (size < sizeof(DkshHeader)) return false;
	const DkshHeader* hdr = (const DkshHeader*)data;
	g_lastDkshCodeSize = hdr->code_sz;
	if (size < (size_t)hdr->control_sz + hdr->code_sz) return false;
	DkMemBlock mem;
	uint32_t off;
	if (!codeAlloc(hdr->code_sz, mem, off)) return false;
	memcpy((uint8_t*)dkMemBlockGetCpuAddr(mem) + off, (const uint8_t*)data + hdr->control_sz, hdr->code_sz);
	DkShaderMaker m;
	dkShaderMakerDefaults(&m, mem, off);
	m.control = data;
	m.programId = 0;
	dkShaderInitialize(&out, &m);
	return true;
}

// ---------------------------------------------------------------- text helpers
static std::string stripComments(const std::string& s)
{
	std::string o;
	o.reserve(s.size());
	for (size_t i = 0; i < s.size();) {
		if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') {
			while (i < s.size() && s[i] != '\n') i++;
		} else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*') {
			i += 2;
			while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) { if (s[i] == '\n') o += '\n'; i++; }
			i += 2;
			o += ' ';
		} else o += s[i++];
	}
	return o;
}

static std::string trim(const std::string& s)
{
	size_t a = 0, b = s.size();
	while (a < b && isspace((unsigned char)s[a])) a++;
	while (b > a && isspace((unsigned char)s[b - 1])) b--;
	return s.substr(a, b - a);
}

static bool isIdent(char c) { return isalnum((unsigned char)c) || c == '_'; }

static std::vector<std::string> words(const std::string& s)
{
	std::vector<std::string> w;
	std::string cur;
	for (char c : s) {
		if (isIdent(c)) cur += c;
		else { if (!cur.empty()) w.push_back(cur); cur.clear(); }
	}
	if (!cur.empty()) w.push_back(cur);
	return w;
}

static void replaceIdent(std::string& s, const std::string& from, const std::string& to)
{
	std::string o;
	for (size_t i = 0; i < s.size();) {
		if (isIdent(s[i]) && (i == 0 || !isIdent(s[i - 1])) && s.compare(i, from.size(), from) == 0 &&
		    (i + from.size() >= s.size() || !isIdent(s[i + from.size()]))) {
			o += to;
			i += from.size();
		} else o += s[i++];
	}
	s.swap(o);
}

// ---------------------------------------------------------------- top-level statement split
struct Stmt {
	std::string text;     // without the trailing ';'
	bool directive;       // preprocessor line
	bool decl;            // ended with ';' at depth 0 (declaration candidate)
};

static std::vector<Stmt> splitTopLevel(const std::string& src)
{
	std::vector<Stmt> out;
	std::string cur;
	int depth = 0;
	bool lineStart = true;
	for (size_t i = 0; i < src.size(); i++) {
		char c = src[i];
		if (lineStart && depth == 0 && trim(cur).empty()) {
			size_t j = i;
			while (j < src.size() && (src[j] == ' ' || src[j] == '\t')) j++;
			if (j < src.size() && src[j] == '#') {
				if (!cur.empty()) { out.push_back({cur, false, false}); cur.clear(); }
				std::string line;
				for (i = j; i < src.size(); i++) {
					if (src[i] == '\\' && i + 1 < src.size() && src[i + 1] == '\n') { i++; continue; }
					if (src[i] == '\n') break;
					line += src[i];
				}
				out.push_back({line, true, false});
				lineStart = true;
				continue;
			}
		}
		lineStart = (c == '\n');
		cur += c;
		if (c == '{') depth++;
		else if (c == '}') {
			depth--;
			if (depth == 0) {
				// "}" ends a function body; "};" ends a struct or block declaration
				size_t j = i + 1;
				while (j < src.size() && isspace((unsigned char)src[j])) j++;
				if (j < src.size() && src[j] == ';') continue;
				out.push_back({cur, false, false});
				cur.clear();
			}
		} else if (c == ';' && depth == 0) {
			cur.pop_back();
			out.push_back({cur, false, true});
			cur.clear();
		}
	}
	if (!trim(cur).empty()) out.push_back({cur, false, false});
	return out;
}

// ---------------------------------------------------------------- declarations
struct Var { std::string name; int arraySize = 1; std::string init; };
struct Decl {
	bool ok = false;
	std::string layout;           // existing layout(...) contents
	std::vector<std::string> quals; // non-storage qualifiers (flat, highp...)
	std::string storage;          // uniform/attribute/varying/in/out
	std::string type;
	std::vector<Var> vars;
};

static const std::set<std::string> kStorage = {"uniform", "attribute", "varying", "in", "out"};
static const std::set<std::string> kQual = {"flat", "smooth", "noperspective", "centroid", "invariant", "highp", "mediump", "lowp", "const"};

static Decl parseDecl(const std::string& stmtIn)
{
	Decl d;
	std::string s = trim(stmtIn);
	if (s.find('{') != std::string::npos || s.find('(') != std::string::npos) {
		// layout(...) is the only allowed parenthesis before the type
		if (s.compare(0, 6, "layout") == 0) {
			size_t p = s.find('('), q = s.find(')');
			if (p == std::string::npos || q == std::string::npos) return d;
			d.layout = trim(s.substr(p + 1, q - p - 1));
			s = trim(s.substr(q + 1));
		}
		if (s.find('{') != std::string::npos) return d;
		// an initializer like vec2(1.0) is fine after '='
		size_t eq = s.find('='), par = s.find('(');
		if (par != std::string::npos && (eq == std::string::npos || par < eq)) return d;
	}
	// qualifiers, storage, type
	size_t pos = 0;
	auto nextWord = [&](std::string& w) {
		while (pos < s.size() && isspace((unsigned char)s[pos])) pos++;
		size_t st = pos;
		while (pos < s.size() && isIdent(s[pos])) pos++;
		w = s.substr(st, pos - st);
		return !w.empty();
	};
	std::string w;
	while (nextWord(w)) {
		if (kStorage.count(w)) { d.storage = w; continue; }
		if (kQual.count(w)) { d.quals.push_back(w); continue; }
		d.type = w;
		break;
	}
	if (d.storage.empty() || d.type.empty()) return d;
	// declarators
	std::string rest = s.substr(pos);
	std::vector<std::string> parts;
	int depth = 0;
	std::string cur;
	for (char c : rest) {
		if (c == '(' || c == '[') depth++;
		if (c == ')' || c == ']') depth--;
		if (c == ',' && depth == 0) { parts.push_back(cur); cur.clear(); continue; }
		cur += c;
	}
	parts.push_back(cur);
	for (std::string p : parts) {
		Var v;
		size_t eq = p.find('=');
		if (eq != std::string::npos) { v.init = trim(p.substr(eq + 1)); p = p.substr(0, eq); }
		p = trim(p);
		size_t br = p.find('[');
		if (br != std::string::npos) {
			v.arraySize = atoi(p.substr(br + 1).c_str());
			if (v.arraySize <= 0) v.arraySize = 1;
			p = trim(p.substr(0, br));
		}
		if (p.empty()) return Decl{};
		v.name = p;
		d.vars.push_back(v);
	}
	d.ok = true;
	return d;
}

// std140 size/alignment of a scalar/vector/matrix type; returns false for unsupported types
static bool std140(const std::string& t, uint32_t& size, uint32_t& align, int& locs)
{
	locs = 1;
	if (t == "float" || t == "int" || t == "uint" || t == "bool") { size = 4; align = 4; return true; }
	if (t == "vec2" || t == "ivec2" || t == "uvec2" || t == "bvec2") { size = 8; align = 8; return true; }
	if (t == "vec3" || t == "ivec3" || t == "uvec3" || t == "bvec3") { size = 12; align = 16; return true; }
	if (t == "vec4" || t == "ivec4" || t == "uvec4" || t == "bvec4") { size = 16; align = 16; return true; }
	if (t == "mat2") { size = 32; align = 16; locs = 2; return true; }
	if (t == "mat3") { size = 48; align = 16; locs = 3; return true; }
	if (t == "mat4") { size = 64; align = 16; locs = 4; return true; }
	return false;
}

static bool isSampler(const std::string& t) { return t.compare(0, 7, "sampler") == 0; }

static int varyingSlots(const std::string& t)
{
	if (t == "mat2") return 2;
	if (t == "mat3") return 3;
	if (t == "mat4") return 4;
	return 1;
}

// identifiers that are fine in old GLSL but reserved or built in for GLSL 460
static const char* kCollide[] = {"texture", "sample", "filter", "input", "output", "buffer", "shared", "common",
                                 "partition", "active", "resource", "patch", "subroutine", "image"};

static const char* kDropExtensions[] = {"GL_OES_standard_derivatives", "GL_EXT_shader_texture_lod", "GL_EXT_frag_depth",
                                        "GL_EXT_draw_buffers", "GL_OES_texture_3D", "GL_ARB_shader_texture_lod",
                                        "GL_ARB_explicit_attrib_location", "GL_ARB_separate_shader_objects"};

struct LinkTables {
	std::unordered_map<std::string, int> varyingLoc;
	int nextVarying = 0;
	std::vector<Uniform>* uniforms;
	std::unordered_map<std::string, int> uniformIndex;
	int nextBinding = 0;
	std::unordered_map<std::string, int>* attribs;
	std::vector<std::pair<std::string, std::string>>* attribTypes;
	const std::unordered_map<std::string, int>* boundAttribs;
	std::string error;
};

// rewrites one stage; uniforms are collected into tables (the block is inserted later)
static bool rewriteStage(const std::string& srcIn, bool vertex, LinkTables& T, std::string& out)
{
	std::string src = stripComments(srcIn);

	// rename user identifiers that collide with GLSL 460 built-ins
	std::vector<std::string> w = words(src);
	std::set<std::string> declared;
	for (size_t i = 0; i + 1 < w.size(); i++) {
		const std::string& t = w[i];
		if (t == "float" || t == "int" || t == "bool" || t.compare(0, 3, "vec") == 0 || t.compare(0, 4, "ivec") == 0 ||
		    t.compare(0, 3, "mat") == 0 || isSampler(t))
			declared.insert(w[i + 1]);
	}
	for (const char* c : kCollide)
		if (declared.count(c)) replaceIdent(src, c, std::string("dkgl_") + c);

	std::vector<Stmt> stmts = splitTopLevel(src);
	std::string body, extensions;
	int ifDepth = 0;
	int fragOutLoc = 0;

	for (Stmt& st : stmts) {
		if (st.directive) {
			std::string t = trim(st.text);
			std::vector<std::string> dw = words(t);
			std::string kw = dw.empty() ? "" : dw[0];
			if (kw == "version") continue;
			if (kw == "extension") {
				bool drop = false;
				for (const char* e : kDropExtensions) if (t.find(e) != std::string::npos) drop = true;
				if (!drop) extensions += t + "\n";
				continue;
			}
			if (kw == "define" && dw.size() > 1) {
				static const std::set<std::string> ours = {"texture2D", "texture2DProj", "texture2DLod", "textureCube", "texture2DLodEXT",
				                                           "texture2DGradEXT", "texture2DProjLod", "shadow2D", "gl_FragDepthEXT"};
				if (ours.count(dw[1])) continue;   // the header already defines it
			}
			if (kw == "if" || kw == "ifdef" || kw == "ifndef") ifDepth++;
			if (kw == "endif") ifDepth--;
			body += t + "\n";
			continue;
		}
		if (!st.decl) { body += st.text; continue; }

		Decl d = parseDecl(st.text);
		if (!d.ok) { body += st.text + ";"; continue; }

		std::string quals;
		for (auto& q : d.quals) if (q != "highp" && q != "mediump" && q != "lowp") quals += q + " ";

		if (d.storage == "uniform") {
			if (isSampler(d.type)) {
				for (Var& v : d.vars) {
					int idx;
					auto it = T.uniformIndex.find(v.name);
					if (it == T.uniformIndex.end()) {
						Uniform u;
						u.name = v.name; u.type = d.type; u.arraySize = v.arraySize;
						u.binding = T.nextBinding;
						T.nextBinding += v.arraySize;
						idx = (int)T.uniforms->size();
						T.uniformIndex[v.name] = idx;
						T.uniforms->push_back(u);
					} else idx = it->second;
					const Uniform& u = (*T.uniforms)[idx];
					char buf[256];
					snprintf(buf, sizeof buf, "layout(binding = %d) uniform %s %s%s;", u.binding, d.type.c_str(), v.name.c_str(),
					         v.arraySize > 1 ? ("[" + std::to_string(v.arraySize) + "]").c_str() : "");
					body += buf;
				}
			} else {
				uint32_t sz, al; int locs;
				if (!std140(d.type, sz, al, locs)) {
					T.error = "unsupported uniform type '" + d.type + "' (" + d.vars[0].name + ")";
					return false;
				}
				for (Var& v : d.vars) {
					if (T.uniformIndex.count(v.name)) continue;
					Uniform u;
					u.name = v.name; u.type = d.type; u.arraySize = v.arraySize;
					T.uniformIndex[v.name] = (int)T.uniforms->size();
					T.uniforms->push_back(u);
					// initializers ("uniform float x = 1.0;") are not allowed in blocks; dkgl ignores them
				}
			}
			continue;
		}

		std::string storage = d.storage;
		if (vertex && (storage == "attribute" || storage == "in")) {
			for (Var& v : d.vars) {
				int loc;
				auto b = T.boundAttribs->find(v.name);
				if (b != T.boundAttribs->end()) loc = b->second;
				else {
					std::set<int> used;
					for (auto& a : *T.attribs) used.insert(a.second);
					for (auto& a : *T.boundAttribs) used.insert(a.second);
					loc = 0;
					while (used.count(loc)) loc++;
				}
				(*T.attribs)[v.name] = loc;
				T.attribTypes->push_back({v.name, d.type});
				body += "layout(location = " + std::to_string(loc) + ") " + quals + "in " + d.type + " " + v.name +
				        (v.arraySize > 1 ? "[" + std::to_string(v.arraySize) + "]" : "") + ";";
			}
			continue;
		}
		bool isVarying = (storage == "varying") || (vertex && storage == "out") || (!vertex && storage == "in");
		if (isVarying) {
			for (Var& v : d.vars) {
				int loc;
				auto it = T.varyingLoc.find(v.name);
				if (it != T.varyingLoc.end()) loc = it->second;
				else { loc = T.nextVarying; T.varyingLoc[v.name] = loc; T.nextVarying += varyingSlots(d.type) * v.arraySize; }
				body += "layout(location = " + std::to_string(loc) + ") " + quals + (vertex ? "out " : "in ") + d.type + " " +
				        v.name + (v.arraySize > 1 ? "[" + std::to_string(v.arraySize) + "]" : "") + ";";
			}
			continue;
		}
		if (!vertex && storage == "out") { // fragment outputs (GLSL 1.30+ style)
			for (Var& v : d.vars) {
				int loc = fragOutLoc;
				fragOutLoc += v.arraySize;
				body += "layout(location = " + std::to_string(loc) + ") out " + d.type + " " + v.name +
				        (v.arraySize > 1 ? "[" + std::to_string(v.arraySize) + "]" : "") + ";";
			}
			continue;
		}
		body += st.text + ";";
	}

	std::string head = "#version 460\n" + extensions + "#define DKGL 1\n"
	                   "#define texture2D texture\n#define texture2DProj textureProj\n#define texture2DLod textureLod\n"
	                   "#define textureCube texture\n#define texture2DLodEXT textureLod\n#define texture2DGradEXT textureGrad\n"
	                   "#define texture2DProjLod textureProjLod\n#define shadow2D texture\n#define gl_FragDepthEXT gl_FragDepth\n";
	if (!vertex && src.find("gl_FragCoord") != std::string::npos) {
		// dynamic resolution renders framebuffer 0 smaller; scale gl_FragCoord back to GL's pixel units
		if (!T.uniformIndex.count("dkgl_FragScale")) {
			Uniform u;
			u.name = "dkgl_FragScale"; u.type = "vec4";
			T.uniformIndex[u.name] = (int)T.uniforms->size();
			T.uniforms->push_back(u);
		}
		head += "#define gl_FragCoord vec4(gl_FragCoord.xy * dkgl_FragScale.xy + dkgl_FragScale.zw, gl_FragCoord.zw)\n";
	}
	if (!vertex && fragOutLoc == 0) {   // shaders that declare their own outputs keep them
		bool usesColor = src.find("gl_FragColor") != std::string::npos;
		bool usesData = src.find("gl_FragData") != std::string::npos;
		if (usesData) head += "layout(location = 0) out vec4 dkgl_FragData[1];\n#define gl_FragData dkgl_FragData\n";
		else if (usesColor) head += "layout(location = 0) out vec4 dkgl_FragColor;\n#define gl_FragColor dkgl_FragColor\n";
	}
	out = head + "@@DKGL_UNIFORMS@@\n" + body;
	return true;
}

static uint64_t fnv1a(const std::string& s, uint64_t h = 1469598103934665603ULL)
{
	for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
	return h;
}

static void logNumbered(const char* title, const std::string& src)
{
	logf("%s", title);
	int line = 1;
	size_t st = 0;
	while (st < src.size()) {
		size_t e = src.find('\n', st);
		if (e == std::string::npos) e = src.size();
		logf("%4d| %s", line++, src.substr(st, e - st).c_str());
		st = e + 1;
	}
}

static bool compileStage(const std::string& glsl, bool vertex, DkShader& out, std::string& log)
{
	char path[256];
	uint64_t h = fnv1a(glsl, fnv1a(vertex ? "vs-dkgl1" : "fs-dkgl1"));
	snprintf(path, sizeof path, "%s/%016llx.dksh", g_shaderCacheDir, (unsigned long long)h);

	// cached?
	FILE* f = fopen(path, "rb");
	if (f) {
		fseek(f, 0, SEEK_END);
		long n = ftell(f);
		rewind(f);
		std::vector<uint8_t> data(n > 0 ? n : 0);
		bool ok = n > 0 && fread(data.data(), 1, n, f) == (size_t)n;
		fclose(f);
		if (ok && loadDksh(data.data(), data.size(), out)) return true;
	}

	unsigned char* blob = nullptr;
	unsigned int size = 0;
	static char clog[8192];
	u64 t0 = armGetSystemTick();
	int ok = uam_compile(vertex ? UAM_STAGE_VERTEX : UAM_STAGE_FRAGMENT, glsl.c_str(), &blob, &size, clog, sizeof clog);
	double ms = armTicksToNs(armGetSystemTick() - t0) / 1e6;
	if (!ok) {
		log += std::string(vertex ? "vertex" : "fragment") + " shader failed to compile:\n" + clog;
		logf("dkgl: %s shader failed (%.1f ms):\n%s", vertex ? "vertex" : "fragment", ms, clog);
		logNumbered("dkgl: rewritten source:", glsl);
		return false;
	}
	logf("dkgl: compiled %s shader in %.1f ms (%u bytes) -> %s", vertex ? "vertex" : "fragment", ms, size, path);
	mkdir("sdmc:/switch/CodenameEngine", 0777);
	mkdir(g_shaderCacheDir, 0777);
	f = fopen(path, "wb");
	if (f) { fwrite(blob, 1, size, f); fclose(f); }
	bool loaded = loadDksh(blob, size, out);
	free(blob);
	return loaded;
}

extern std::unordered_map<GLuint, Shader> g_shaders;

bool linkProgram(Program& p, std::string& log)
{
	const Shader* vsh = nullptr;
	const Shader* fsh = nullptr;
	for (GLuint id : p.shaders) {
		auto it = g_shaders.find(id);
		if (it == g_shaders.end()) continue;
		if (it->second.type == GL_VERTEX_SHADER) vsh = &it->second;
		if (it->second.type == GL_FRAGMENT_SHADER) fsh = &it->second;
	}
	if (!vsh || !fsh) { log = "program needs a vertex and a fragment shader"; return false; }

	p.uniforms.clear();
	p.attribs.clear();
	p.attribTypes.clear();
	LinkTables T;
	T.uniforms = &p.uniforms;
	T.attribs = &p.attribs;
	T.attribTypes = &p.attribTypes;
	T.boundAttribs = &p.boundAttribs;

	std::string vsrc, fsrc;
	if (!rewriteStage(vsh->source, true, T, vsrc) || !rewriteStage(fsh->source, false, T, fsrc)) {
		log = "dkgl: " + T.error;
		logf("dkgl: link failed: %s", T.error.c_str());
		return false;
	}

	// std140 layout of the non-sampler uniforms, identical block in both stages
	std::string block;
	uint32_t offset = 0;
	int location = 0;
	for (Uniform& u : p.uniforms) {
		u.location = location;
		location += u.arraySize;
		if (u.binding >= 0) continue;
		uint32_t sz, al; int locs;
		std140(u.type, sz, al, locs);
		if (u.arraySize > 1) { al = 16; u.stride = (sz + 15) & ~15u; }
		else u.stride = sz;
		offset = (offset + al - 1) & ~(al - 1);
		u.offset = offset;
		offset += u.arraySize > 1 ? u.stride * u.arraySize : sz;
		block += "    " + u.type + " " + u.name + (u.arraySize > 1 ? "[" + std::to_string(u.arraySize) + "]" : "") + ";\n";
	}
	p.locationCount = location;
	p.fragScaleOffset = -1;
	for (Uniform& u : p.uniforms) if (u.name == "dkgl_FragScale") p.fragScaleOffset = (int)u.offset;
	p.uboSize = (offset + 15) & ~15u;
	p.uboData.assign(p.uboSize, 0);
	std::string decl = block.empty() ? "" : "layout(std140, binding = 0) uniform DkglUniforms {\n" + block + "};\n";
	size_t at;
	if ((at = vsrc.find("@@DKGL_UNIFORMS@@")) != std::string::npos) vsrc.replace(at, 17, decl);
	if ((at = fsrc.find("@@DKGL_UNIFORMS@@")) != std::string::npos) fsrc.replace(at, 17, decl);

	if (!compileStage(vsrc, true, p.vs, log)) return false;
	if (!compileStage(fsrc, false, p.fs, log)) return false;
	p.fsCodeSize = g_lastDkshCodeSize;
	p.heavy = p.fsCodeSize > 12288;
	if (p.heavy) logf("dkgl: heavy fragment shader (%u bytes of code): drawn at reduced resolution when the GPU is late", p.fsCodeSize);
	if (p.fragScaleOffset >= 0) { float one[4] = {1, 1, 0, 0}; memcpy(p.uboData.data() + p.fragScaleOffset, one, sizeof one); }
	return true;
}

bool compileInternalFragment(const std::string& glsl, DkShader& out)
{
	std::string log;
	return compileStage(glsl, false, out, log);
}

} // namespace dkgl
