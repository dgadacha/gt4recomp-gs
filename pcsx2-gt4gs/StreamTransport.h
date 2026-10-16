// SPDX-License-Identifier: MIT
// GT4Recomp fork: no-throw wrapper around the vendored MIT GS stream transport (3rdparty/gt4recomp-gs-stream).
//
// The transport throws C++ exceptions; PCSX2 is built without them. This file and StreamTransport.cpp are compiled
// with exceptions into the static library gt4gs_stream_transport; every function catches what the transport throws
// and reports it as a result code and text. No PCSX2 header is used here.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace gt4gs
{
	struct RawRecord
	{
		std::uint16_t type = 0;
		std::uint16_t flags = 0;
		std::uint32_t payload_bytes = 0;
		std::uint64_t sequence = 0;
		const std::uint8_t* payload = nullptr;
	};

	enum class NextStatus
	{
		record,
		end,
		peer_exited,
		failed,
	};

	// Consumer end of a live session (docs/GS-STREAM-PROTOCOL.md section 8 in the main repository).
	class LiveTransport
	{
	public:
		// Opens the session mapping by name and validates it (magic, major version, sizes, region layout, frames in
		// flight, non-empty serial, producer pid == parent_pid), opens the producer process for waits and writes our
		// pid into the control block. On failure returns null and sets `error`; when the event ring was usable a fatal
		// ERROR record has been posted.
		static std::unique_ptr<LiveTransport> Open(const std::string& name, std::uint32_t parent_pid, std::string* error) noexcept;
		~LiveTransport();

		std::string Serial() const noexcept;
		std::uint32_t FramesInFlight() const noexcept;
		NextStatus Next(RawRecord* record, std::uint64_t* wait_us) noexcept;
		void Release() noexcept;
		void CompleteVsync() noexcept; // GS thread
		std::uint8_t* ResponseArea(std::uint64_t* capacity) noexcept;
		void CompleteReadback(std::uint64_t request_id, std::uint64_t bytes, std::uint32_t status) noexcept;
		bool PostEvent(std::uint16_t type, const void* payload, std::uint32_t bytes) noexcept;
		void SetStateBits(std::uint32_t bits) noexcept;
		// Spin time before a wait sleeps on its event (transport default 30 us); [GT4GS] SpinUs (task 037).
		void SetSpinUs(std::uint32_t spin_us) noexcept;
		const std::string& LastError() const noexcept { return m_error; }

	private:
		struct Impl;
		explicit LiveTransport(std::unique_ptr<Impl> impl);
		std::unique_ptr<Impl> m_impl;
		std::string m_error;
	};

	// A .gscap version 2 file (the protocol written to a file).
	class FileTransport
	{
	public:
		static std::unique_ptr<FileTransport> Open(const std::string& path, std::string* error) noexcept;
		~FileTransport();

		std::string Serial() const noexcept;
		NextStatus Next(RawRecord* record) noexcept;
		const std::string& LastError() const noexcept { return m_error; }

	private:
		struct Impl;
		explicit FileTransport(std::unique_ptr<Impl> impl);
		std::unique_ptr<Impl> m_impl;
		std::string m_error;
	};

	// Protocol version implemented by the vendored header, as "major.minor".
	const char* ProtocolVersionText() noexcept;
} // namespace gt4gs
