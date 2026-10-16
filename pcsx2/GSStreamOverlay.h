// SPDX-FileCopyrightText: 2026 GT4Recomp contributors
// SPDX-License-Identifier: GPL-3.0+
//
// GT4Recomp fork (gt4gs/osd, gt4-gs 1.0.0): the on-screen items gt4-gs draws over the game picture with the PCSX2
// ImGui OSD (font, scale and margins of ImGuiManager), called from ImGuiManager::RenderOverlays on the GS thread.
//
//   Shader compile notice (task 061): plain text "Compiling shaders..." in the bottom-left corner while the GS compiles
//   shaders or pipelines (PCSX2's GSShaderCompileIndicator signal; pipelines the driver takes from its cache do not
//   count), shown until 0.5 s after the last compile. [GT4GS] ShaderCompileNotice, default true.
//   Performance overlay (task 062): top-right lines, each switchable: FPS, frame time, CPU and GPU usage of the game
//   (game process plus gt4-gs), output and internal resolution, and the per-part breakdown of a frame with the headroom
//   against the frame budget. [GT4GS] PerfOverlay (master, default false) and PerfOverlayFPS, PerfOverlayFrameTime,
//   PerfOverlayCPU, PerfOverlayGPU, PerfOverlayResolution, PerfOverlayBreakdown (default true).
//
// The game-side numbers arrive in protocol 1.2 OSD_STATS records (GSStreamReplayer, CPU thread); CPU and GPU usage are
// sampled every 0.5 s by a thread of this module (GetProcessTimes; PDH "GPU Engine" counters by pid). Nothing here runs
// unless the frontend configured it (stream modes only).

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

namespace GSStreamOverlay
{
	struct Config
	{
		bool shader_notice = true;
		bool overlay = false;
		bool fps = true;
		bool frame_time = true;
		bool cpu = true;
		bool gpu = true;
		bool resolution = true;
		bool breakdown = true;
	};

	// Frontend, before the GS opens. game_pid: the producer process (0 for a file replay: only this process is measured).
	// Starts the CPU/GPU sampler thread when the overlay shows CPU or GPU usage.
	void Configure(const Config& config, u32 game_pid);
	// Stops the sampler thread (frontend, at exit).
	void Shutdown();

	// True when the overlay is on: READY then carries kReadyFlagOsdStats so the game sends OSD_STATS.
	bool WantsOsdStats();
	// CPU thread: one OSD_STATS payload (protocol 1.2).
	void OnOsdStats(const void* payload, u32 bytes);

	// GS thread, from ImGuiManager::RenderOverlays.
	void Draw(float scale, float margin, float spacing);

	// One log line for the end of the session (frames drawn, records, sampler state).
	std::string Summary();
} // namespace GSStreamOverlay
