// SPDX-FileCopyrightText: 2026 GT4Recomp contributors
// SPDX-License-Identifier: GPL-3.0+
//
// GT4Recomp fork (gt4-gs 1.0.0, tasks 061 and 062): see GSStreamOverlay.h.

#include "GSStreamOverlay.h"

#include "GS/GS.h"
#include "GS/GSShaderCompileIndicator.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "ImGui/ImGuiManager.h"
#include "PerformanceMetrics.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/RedtapeWindows.h"

#include "gs_stream_protocol.h"

#include "fmt/format.h"
#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <pdh.h>
#include <pdhmsg.h>
#pragma comment(lib, "pdh.lib")
#endif

namespace proto = gs_stream;

namespace
{
	constexpr double kNoticeHoldSeconds = 0.5; // the notice stays this long after the last real compile
	constexpr auto kSamplePeriod = std::chrono::milliseconds(500);

	GSStreamOverlay::Config s_config;
	bool s_configured = false;
	u32 s_game_pid = 0;

	// Game numbers (OSD_STATS) and sampled CPU/GPU usage, shared by the CPU thread, the sampler and the GS thread.
	std::mutex s_mutex;
	proto::OsdStatsPayload s_game{}; // the latest record; valid = 0 until one arrived
	double s_cpu_percent = -1.0; // both processes, percent of all logical processors; negative: unknown
	double s_cpu_cores = -1.0; // the same as a number of busy logical processors
	double s_gpu_percent = -1.0; // both processes, busiest GPU engine; negative: unknown
	std::string s_gpu_state = "not sampled";

	// Counters for the exit summary.
	std::atomic<u64> s_osd_records{0};
	std::atomic<u64> s_overlay_frames{0};
	std::atomic<u64> s_notice_frames{0};
	std::atomic<u64> s_draw_ns{0};
	std::atomic<u64> s_sampler_ticks{0};
	std::atomic<u64> s_sampler_ns{0}; // sampler thread time per tick (GetProcessTimes and the PDH query)

	// Sampler thread.
	std::thread s_sampler;
	std::mutex s_sampler_mutex;
	std::condition_variable s_sampler_wake;
	bool s_sampler_stop = false;

#ifdef _WIN32
	u64 FileTimeUs(const FILETIME& value)
	{
		return ((static_cast<u64>(value.dwHighDateTime) << 32) | value.dwLowDateTime) / 10;
	}

	// Kernel plus user time of a process in microseconds; false when the process cannot be queried.
	bool ProcessCpuUs(HANDLE process, u64* cpu_us)
	{
		FILETIME created, exited, kernel, user;
		if (!process || !GetProcessTimes(process, &created, &exited, &kernel, &user))
			return false;
		*cpu_us = FileTimeUs(kernel) + FileTimeUs(user);
		return true;
	}

	// PDH "GPU Engine" instance names look like pid_1234_luid_0x00000000_0x0000D1E7_phys_0_eng_0_engtype_3D. Returns the
	// pid and the engine part (everything from "luid"), or pid 0 for another format.
	u32 ParseGpuEngineInstance(const wchar_t* name, std::wstring* engine)
	{
		if (std::wcsncmp(name, L"pid_", 4) != 0)
			return 0;
		wchar_t* end = nullptr;
		const unsigned long pid = std::wcstoul(name + 4, &end, 10);
		if (!end || *end != L'_')
			return 0;
		*engine = end + 1;
		return static_cast<u32>(pid);
	}

	// Every kSamplePeriod: CPU time of this process and the game process, and the GPU engine utilisation of both pids.
	void SamplerMain()
	{
		const u32 self_pid = GetCurrentProcessId();
		HANDLE self = GetCurrentProcess();
		HANDLE game = s_game_pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, s_game_pid) : nullptr;
		const DWORD processors = std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));

		PDH_HQUERY query = nullptr;
		PDH_HCOUNTER counter = nullptr;
		if (s_config.gpu)
		{
			PDH_STATUS status = PdhOpenQueryW(nullptr, 0, &query);
			if (status == ERROR_SUCCESS)
				status = PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter);
			if (status == ERROR_SUCCESS)
				status = PdhCollectQueryData(query); // the first sample of a rate counter
			std::lock_guard lock(s_mutex);
			s_gpu_state = status == ERROR_SUCCESS ? "PDH GPU Engine counters" : fmt::format("PDH unavailable (0x{:08X})", static_cast<u32>(status));
			if (status != ERROR_SUCCESS && query)
			{
				PdhCloseQuery(query);
				query = nullptr;
			}
		}

		u64 last_wall_us = 0, last_cpu_us = 0;
		std::vector<u8> buffer;
		std::unique_lock wait_lock(s_sampler_mutex);
		while (!s_sampler_stop)
		{
			wait_lock.unlock();
			const auto tick_start = std::chrono::steady_clock::now();
			const u64 wall_us = static_cast<u64>(
				std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
			u64 self_us = 0, game_us = 0;
			const bool self_ok = ProcessCpuUs(self, &self_us);
			const bool game_ok = !game || ProcessCpuUs(game, &game_us);
			double cpu_percent = -1.0, cpu_cores = -1.0;
			const u64 cpu_us = self_us + game_us;
			if (self_ok && game_ok && last_wall_us && wall_us > last_wall_us && cpu_us >= last_cpu_us)
			{
				cpu_cores = static_cast<double>(cpu_us - last_cpu_us) / static_cast<double>(wall_us - last_wall_us);
				cpu_percent = 100.0 * cpu_cores / processors;
			}
			last_wall_us = wall_us;
			last_cpu_us = cpu_us;

			double gpu_percent = -1.0;
			if (query && PdhCollectQueryData(query) == ERROR_SUCCESS)
			{
				DWORD bytes = 0, items = 0;
				PDH_STATUS status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &items, nullptr);
				if (status == PDH_MORE_DATA && bytes)
				{
					buffer.resize(bytes);
					auto* values = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
					status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &items, values);
					if (status == ERROR_SUCCESS)
					{
						// Sum per engine over the two processes, then the busiest engine (as Task Manager shows it).
						std::map<std::wstring, double> engines;
						std::wstring engine;
						for (DWORD i = 0; i < items; ++i)
						{
							if (values[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && values[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
								continue;
							const u32 pid = ParseGpuEngineInstance(values[i].szName, &engine);
							if (pid != self_pid && (pid == 0 || pid != s_game_pid))
								continue;
							engines[engine] += values[i].FmtValue.doubleValue;
						}
						gpu_percent = 0.0;
						for (const auto& [name, value] : engines)
							gpu_percent = std::max(gpu_percent, value);
						gpu_percent = std::min(gpu_percent, 100.0);
					}
				}
			}
			{
				std::lock_guard lock(s_mutex);
				s_cpu_percent = cpu_percent;
				s_cpu_cores = cpu_cores;
				s_gpu_percent = gpu_percent;
			}
			s_sampler_ticks.fetch_add(1, std::memory_order_relaxed);
			s_sampler_ns.fetch_add(static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tick_start).count()), std::memory_order_relaxed);
			wait_lock.lock();
			s_sampler_wake.wait_for(wait_lock, kSamplePeriod, [] { return s_sampler_stop; });
		}
		wait_lock.unlock();
		if (query)
			PdhCloseQuery(query);
		if (game)
			CloseHandle(game);
	}
#endif

	// One right-aligned line of the top-right block, PCSX2 OSD style (text with a soft shadow).
	struct LineWriter
	{
		ImDrawList* list;
		ImFont* font;
		float size;
		float right;
		float y;
		float shadow;
		float spacing;

		void Line(const std::string& text, ImU32 colour = IM_COL32(255, 255, 255, 255))
		{
			const ImVec2 extent = font->CalcTextSizeA(size, std::numeric_limits<float>::max(), -1.0f, text.c_str(),
				text.c_str() + text.size(), nullptr);
			const ImVec2 at(right - extent.x, y);
			list->AddText(font, size, ImVec2(at.x + shadow, at.y + shadow), IM_COL32(0, 0, 0, 100), text.c_str(),
				text.c_str() + text.size());
			list->AddText(font, size, at, colour, text.c_str(), text.c_str() + text.size());
			y += extent.y + spacing;
		}
	};

	std::string Ms(u32 us) { return fmt::format("{:.2f}", us / 1000.0); }
} // namespace

void GSStreamOverlay::Configure(const Config& config, u32 game_pid)
{
	s_config = config;
	s_game_pid = game_pid;
	s_configured = true;
	// Plain stderr: the frontend configures this before the PCSX2 console is set up (the VM starts later).
	const std::string line = fmt::format("gt4-gs overlay: shader compile notice {}; performance overlay {}{}",
		config.shader_notice ? "on" : "off", config.overlay ? "on" : "off",
		config.overlay ? fmt::format(" (FPS {}, frame time {}, CPU {}, GPU {}, resolution {}, breakdown {})", config.fps ? "on" : "off",
							 config.frame_time ? "on" : "off", config.cpu ? "on" : "off", config.gpu ? "on" : "off",
							 config.resolution ? "on" : "off", config.breakdown ? "on" : "off") :
						 std::string());
	std::fprintf(stderr, "%s\n", line.c_str());
	std::fflush(stderr);
#ifdef _WIN32
	if (config.overlay && (config.cpu || config.gpu) && !s_sampler.joinable())
	{
		s_sampler_stop = false;
		s_sampler = std::thread([] {
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
			SamplerMain();
		});
	}
#endif
}

void GSStreamOverlay::Shutdown()
{
	if (!s_sampler.joinable())
		return;
	{
		std::lock_guard lock(s_sampler_mutex);
		s_sampler_stop = true;
	}
	s_sampler_wake.notify_all();
	s_sampler.join();
}

bool GSStreamOverlay::WantsOsdStats()
{
	return s_configured && s_config.overlay;
}

void GSStreamOverlay::OnOsdStats(const void* payload, u32 bytes)
{
	proto::OsdStatsPayload stats{};
	std::memcpy(&stats, payload, std::min<size_t>(bytes, sizeof(stats)));
	s_osd_records.fetch_add(1, std::memory_order_relaxed);
	std::lock_guard lock(s_mutex);
	s_game = stats;
}

void GSStreamOverlay::Draw(float scale, float margin, float spacing)
{
	if (!s_configured)
		return;
	const auto start = std::chrono::steady_clock::now();
	ImFont* const font = ImGuiManager::GetOSDFont();
	const float size = ImGuiManager::GetFontSizeStandard();
	const float shadow = std::ceil(scale);
	ImDrawList* const list = ImGui::GetBackgroundDrawList();

	// Task 061: plain text, bottom-left, while shaders or pipelines are being compiled (no background, no animation).
	if (s_config.shader_notice && GSShaderCompileIndicator::RealCompileWithin(kNoticeHoldSeconds))
	{
		static constexpr const char* kNotice = "Compiling shaders\xE2\x80\xA6";
		const ImVec2 at(margin, ImGuiManager::GetWindowHeight() - margin - size);
		list->AddText(font, size, ImVec2(at.x + shadow, at.y + shadow), IM_COL32(0, 0, 0, 100), kNotice);
		list->AddText(font, size, at, IM_COL32(255, 255, 255, 255), kNotice);
		s_notice_frames.fetch_add(1, std::memory_order_relaxed);
	}

	if (!s_config.overlay)
		return;

	proto::OsdStatsPayload game;
	double cpu_percent, cpu_cores, gpu_percent;
	{
		std::lock_guard lock(s_mutex);
		game = s_game;
		cpu_percent = s_cpu_percent;
		cpu_cores = s_cpu_cores;
		gpu_percent = s_gpu_percent;
	}
	const bool frame_times = (game.valid & proto::kOsdStatFrameTimes) && game.frames && game.interval_us;

	LineWriter out{list, font, size, ImGuiManager::GetWindowWidth() - margin, margin, shadow, std::ceil(spacing * 0.4f)};
	if (s_config.fps)
	{
		const double fps = frame_times ? game.frames * 1e6 / static_cast<double>(game.interval_us) : PerformanceMetrics::GetFPS();
		out.Line(fmt::format("FPS {:.2f}", fps));
	}
	if (s_config.frame_time)
	{
		if (frame_times)
			out.Line(fmt::format("Frame {} ms (max {} ms)", Ms(game.frame_time_avg_us), Ms(game.frame_time_max_us)));
		else
			out.Line(fmt::format("Frame {:.2f} ms (max {:.2f} ms)", PerformanceMetrics::GetAverageFrameTime(),
				PerformanceMetrics::GetMaximumFrameTime()));
	}
	if (s_config.cpu)
		out.Line(cpu_percent < 0 ? std::string("CPU (game) ...") :
								   fmt::format("CPU (game) {:.1f}% ({:.2f} cores)", cpu_percent, cpu_cores));
	if (s_config.gpu)
		out.Line(gpu_percent < 0 ? std::string("GPU (game) ...") : fmt::format("GPU (game) {:.1f}%", gpu_percent));
	if (s_config.resolution)
	{
		int internal_width = 0, internal_height = 0;
		GSgetInternalResolution(&internal_width, &internal_height);
		const int output_width = g_gs_device ? g_gs_device->GetWindowWidth() : 0;
		const int output_height = g_gs_device ? g_gs_device->GetWindowHeight() : 0;
		out.Line(fmt::format("Output {}x{} | Internal {}x{} ({:g}x)", output_width, output_height, internal_width,
			internal_height, GSConfig.UpscaleMultiplier));
	}
	if (s_config.breakdown)
	{
		const double gs_thread_ms = PerformanceMetrics::GetGSThreadAverageTime();
		if (game.valid & proto::kOsdStatStreamCosts)
		{
			// Two short lines, so the block fits a small window too (ms per frame).
			out.Line(fmt::format("EE {} | VU1 {} | GS write {} ms",
				(game.valid & proto::kOsdStatMainThreadCpu) ? Ms(game.main_thread_cpu_us) : std::string("?"),
				(game.valid & proto::kOsdStatWorkerBusy) ? Ms(game.worker_busy_us) : std::string("?"), Ms(game.stream_write_us)));
			out.Line(fmt::format("Readback {} | GS thread {:.2f} ms", Ms(game.readback_wait_us), gs_thread_ms));
			if (game.valid & proto::kOsdStatCriticalPath)
			{
				const double headroom_ms = (static_cast<double>(game.frame_budget_us) - game.critical_path_us) / 1000.0;
				out.Line(fmt::format("Critical path {} of {} ms, headroom {:.2f} ms", Ms(game.critical_path_us),
							 Ms(game.frame_budget_us), headroom_ms),
					headroom_ms < 0 ? IM_COL32(255, 96, 96, 255) : IM_COL32(255, 255, 255, 255));
			}
		}
		else
		{
			out.Line(fmt::format("GS thread {:.2f} ms | game breakdown: {}", gs_thread_ms,
				game.valid ? "needs GT4_PERF=1" : "waiting for the game"));
		}
	}
	s_overlay_frames.fetch_add(1, std::memory_order_relaxed);
	s_draw_ns.fetch_add(static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
							std::chrono::steady_clock::now() - start)
											.count()),
		std::memory_order_relaxed);
}

std::string GSStreamOverlay::Summary()
{
	const u64 frames = s_overlay_frames.load();
	std::string gpu_state;
	{
		std::lock_guard lock(s_mutex);
		gpu_state = s_gpu_state;
	}
	return fmt::format("gt4-gs overlay: performance overlay drawn in {} frames (mean {:.1f} us of GS thread time per frame, "
					   "ImGui text only), {} OSD_STATS records received, CPU/GPU sampler: {} ticks, mean {:.2f} ms each, GPU source: {}; shader compile notice shown in {} "
					   "frames; {} real shader/pipeline compiles, {} pipelines from the driver cache",
		frames, frames ? s_draw_ns.load() / 1000.0 / static_cast<double>(frames) : 0.0, s_osd_records.load(),
		s_sampler_ticks.load(), s_sampler_ticks.load() ? s_sampler_ns.load() / 1e6 / static_cast<double>(s_sampler_ticks.load()) : 0.0,
		s_config.overlay && s_config.gpu ? gpu_state : std::string("off"), s_notice_frames.load(),
		GSShaderCompileIndicator::s_gt4gs_real_compiles.load(), GSShaderCompileIndicator::s_gt4gs_cache_hits.load());
}
