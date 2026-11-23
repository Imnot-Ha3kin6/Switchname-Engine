#pragma once
/* Runtime GLSL -> deko3d DKSH compiler (UAM, built for the Switch). */
#ifdef __cplusplus
extern "C" {
#endif

enum uam_stage {
	UAM_STAGE_VERTEX = 0,
	UAM_STAGE_TESS_CTRL = 1,
	UAM_STAGE_TESS_EVAL = 2,
	UAM_STAGE_GEOMETRY = 3,
	UAM_STAGE_FRAGMENT = 4,
	UAM_STAGE_COMPUTE = 5,
};

/*
 * Compiles one GLSL shader stage. On success returns 1 and sets *out / *out_size to a malloc'd
 * DKSH blob (free() it). On failure returns 0. In both cases the compiler's messages (errors and
 * warnings) are copied into log (if log_len > 0). Thread-safe (calls are serialised).
 */
int uam_compile(int stage, const char *glsl, unsigned char **out, unsigned int *out_size,
                char *log, unsigned int log_len);

#ifdef __cplusplus
}
#endif
