// SPDX-License-Identifier: MIT
// GT4Recomp fork: the C11 Annex K name the vendored .gscap file code uses (gs_stream_file.cpp, fopen_s), for the
// transport library on macOS and Linux. Force-included by pcsx2-gt4gs/CMakeLists.txt outside Windows.
#pragma once

#include <errno.h>
#include <stdio.h>

static inline int fopen_s(FILE **stream, const char *path, const char *mode)
{
	*stream = fopen(path, mode);
	return *stream ? 0 : errno;
}
