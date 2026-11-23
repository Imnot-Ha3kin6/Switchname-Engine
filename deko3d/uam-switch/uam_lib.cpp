#include "uam_lib.h"
#include "compiler_iface.h"

#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static std::mutex s_lock;

int uam_compile(int stage, const char *glsl, unsigned char **out, unsigned int *out_size,
                char *log, unsigned int log_len)
{
	std::lock_guard<std::mutex> guard(s_lock);
	if (out) *out = nullptr;
	if (out_size) *out_size = 0;
	if (log && log_len) log[0] = 0;
	if (!glsl || stage < UAM_STAGE_VERTEX || stage > UAM_STAGE_COMPUTE) return 0;

	// UAM and mesa report errors/warnings on stderr; capture them for the caller
	char *logBuf = nullptr;
	size_t logSize = 0;
	FILE *logStream = open_memstream(&logBuf, &logSize);
	FILE *savedStderr = stderr;
	if (logStream) stderr = logStream;

	int ok = 0;
	{
		DekoCompiler compiler{(pipeline_stage)stage};
		if (compiler.CompileGlsl(glsl)) {
			char *blob = nullptr;
			size_t blobSize = 0;
			FILE *blobStream = open_memstream(&blob, &blobSize);
			if (blobStream) {
				compiler.OutputDksh(blobStream);
				fclose(blobStream);
				if (blob && blobSize) {
					*out = (unsigned char *)blob;
					*out_size = (unsigned int)blobSize;
					ok = 1;
				} else {
					free(blob);
				}
			}
		}
	}

	stderr = savedStderr;
	if (logStream) {
		fclose(logStream);
		if (log && log_len && logBuf) {
			strncpy(log, logBuf, log_len - 1);
			log[log_len - 1] = 0;
		}
		free(logBuf);
	}
	return ok;
}
