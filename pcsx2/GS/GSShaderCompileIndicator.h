// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"
#include "common/Timer.h"

#include <atomic>

namespace GSShaderCompileIndicator
{
	inline constexpr u64 RECENT_COMPILE_HOLD_NS = 1'500'000'000ULL;

	inline std::atomic<u32> s_count{0};
	inline std::atomic<u64> s_time_ns{0};
	inline std::atomic<u64> s_last_time{0};

	inline u64 GetRecentCompileHold()
	{
		static const u64 hold = static_cast<u64>(Common::Timer::ConvertNanosecondsToValue(static_cast<double>(RECENT_COMPILE_HOLD_NS)));
		return hold;
	}

	inline void OnCompileDone(u64 duration_ns, u64 start_time)
	{
		const u64 now = Common::Timer::GetCurrentValue();
		const u64 last = s_last_time.load(std::memory_order_relaxed);
		if (last != 0 && start_time > last && (start_time - last) >= GetRecentCompileHold())
		{
			s_count.store(0, std::memory_order_relaxed);
			s_time_ns.store(0, std::memory_order_relaxed);
		}

		s_count.fetch_add(1, std::memory_order_relaxed);
		s_time_ns.fetch_add(duration_ns, std::memory_order_relaxed);
		s_last_time.store(now, std::memory_order_relaxed);
	}

	inline u32 GetCount()
	{
		return s_count.load(std::memory_order_relaxed);
	}

	inline u32 GetTimeMs()
	{
		const u64 time_ns = s_time_ns.load(std::memory_order_relaxed);
		const u32 ms = static_cast<u32>(time_ns / 1000000);
		if (ms > 0)
			return ms;

		return GetCount() > 0 ? 1u : 0u;
	}

	inline bool IsVisible()
	{
		if (GetCount() == 0)
			return false;

		const u64 last = s_last_time.load(std::memory_order_relaxed);
		if (last == 0)
			return false;

		return (Common::Timer::GetCurrentValue() - last) < GetRecentCompileHold();
	}

	inline float GetFadeAlpha()
	{
		const u64 last = s_last_time.load(std::memory_order_relaxed);
		if (last == 0)
			return 0.0f;

		const u64 now = Common::Timer::GetCurrentValue();
		if (now <= last)
			return 1.0f;

		const u64 hold = GetRecentCompileHold();
		const u64 elapsed = now - last;
		if (elapsed >= hold)
			return 0.0f;

		return 1.0f - static_cast<float>(elapsed) / static_cast<float>(hold);
	}

	// GT4Recomp fork (gt4-gs 1.0.0, task 061): compiles that really compiled: a shader compiled to SPIR-V/bytecode, or a
	// pipeline the driver did not take from its pipeline cache (Vulkan: VK_EXT_pipeline_creation_feedback). PCSX2's own
	// counters above are unchanged. The gt4-gs "Compiling shaders" notice shows while the last one is recent.
	inline std::atomic<u64> s_gt4gs_last_real_compile{0}; // Common::Timer value; 0 = none yet
	inline std::atomic<u64> s_gt4gs_real_compiles{0};
	inline std::atomic<u64> s_gt4gs_cache_hits{0};

	inline void NoteRealCompile()
	{
		s_gt4gs_real_compiles.fetch_add(1, std::memory_order_relaxed);
		s_gt4gs_last_real_compile.store(Common::Timer::GetCurrentValue(), std::memory_order_relaxed);
	}

	inline bool RealCompileWithin(double seconds)
	{
		const u64 last = s_gt4gs_last_real_compile.load(std::memory_order_relaxed);
		if (last == 0)
			return false;
		const u64 now = Common::Timer::GetCurrentValue();
		return now <= last || Common::Timer::ConvertValueToSeconds(now - last) < seconds;
	}

	struct CompileTimer
	{
		Common::Timer timer;
		// GT4Recomp fork: set by a pipeline create that the driver answered from its pipeline cache.
		bool from_cache = false;

		CompileTimer() = default;

		~CompileTimer()
		{
			OnCompileDone(static_cast<u64>(timer.GetTimeNanoseconds()), timer.GetStartValue());
			if (from_cache)
				s_gt4gs_cache_hits.fetch_add(1, std::memory_order_relaxed);
			else
				NoteRealCompile();
		}

		CompileTimer(const CompileTimer&) = delete;
		CompileTimer& operator=(const CompileTimer&) = delete;
	};
} // namespace GSShaderCompileIndicator
