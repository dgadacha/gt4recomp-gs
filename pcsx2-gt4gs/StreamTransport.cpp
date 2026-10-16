// SPDX-License-Identifier: MIT
// GT4Recomp fork: see StreamTransport.h. Compiled with C++ exceptions enabled.
#include "StreamTransport.h"

#include "gs_stream_file.hpp"
#include "gs_stream_ipc.hpp"
#include "gs_stream_protocol.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <vector>

namespace gt4gs
{
	namespace
	{
		namespace proto = ::gs_stream;
		namespace ipc = ::gs_stream::ipc;

		std::string FieldText(const char* text, std::size_t size)
		{
			std::size_t length = 0;
			while (length < size && text[length])
				++length;
			return std::string(text, length);
		}

		std::uint64_t NowUs()
		{
			return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		}

		void PostFatal(ipc::Session& session, proto::ErrorCode code, const std::string& text)
		{
			proto::ErrorPayload payload{};
			payload.code = static_cast<std::uint32_t>(code);
			payload.fatal = 1;
			std::strncpy(payload.text, text.c_str(), sizeof(payload.text) - 1);
			try
			{
				session.post_event(proto::RecordType::error, &payload, sizeof(payload));
				std::atomic_ref<std::uint32_t>(session.control().state).fetch_or(proto::kStateFatal);
			}
			catch (...)
			{
			}
		}
	} // namespace

	struct LiveTransport::Impl
	{
		std::unique_ptr<ipc::Session> session;
		ipc::NativeHandle peer = nullptr;
		ipc::RecordView view{};
		bool holding = false;
	};

	LiveTransport::LiveTransport(std::unique_ptr<Impl> impl)
		: m_impl(std::move(impl))
	{
	}

	LiveTransport::~LiveTransport()
	{
		if (m_impl && m_impl->peer)
			ipc::close_handle(m_impl->peer);
	}

	std::unique_ptr<LiveTransport> LiveTransport::Open(const std::string& name, std::uint32_t parent_pid, std::string* error) noexcept
	{
		try
		{
			auto impl = std::make_unique<Impl>();
			impl->session = ipc::Session::open_consumer(name);
			proto::ControlBlock& control = impl->session->control();
			if (control.producer_pid != parent_pid)
			{
				const std::string text = "producer pid " + std::to_string(control.producer_pid) + " is not --parent " +
										 std::to_string(parent_pid);
				PostFatal(*impl->session, proto::ErrorCode::layout_mismatch, text);
				*error = text;
				return nullptr;
			}
			if (FieldText(control.serial, sizeof(control.serial)).empty())
			{
				PostFatal(*impl->session, proto::ErrorCode::layout_mismatch, "the control block has no game serial");
				*error = "the control block has no game serial";
				return nullptr;
			}
			impl->peer = ipc::open_process_for_wait(parent_pid);
			if (!impl->peer)
			{
				*error = "cannot open the producer process " + std::to_string(parent_pid);
				PostFatal(*impl->session, proto::ErrorCode::internal, *error);
				return nullptr;
			}
			impl->session->set_peer(impl->peer);
			std::atomic_ref<std::uint32_t>(control.consumer_pid).store(ipc::current_process_id());
			return std::unique_ptr<LiveTransport>(new LiveTransport(std::move(impl)));
		}
		catch (const std::exception& e)
		{
			*error = e.what();
		}
		catch (...)
		{
			*error = "unknown error";
		}
		return nullptr;
	}

	std::string LiveTransport::Serial() const noexcept
	{
		const proto::ControlBlock& control = m_impl->session->control();
		return FieldText(control.serial, sizeof(control.serial));
	}

	std::uint32_t LiveTransport::FramesInFlight() const noexcept
	{
		return m_impl->session->control().frames_in_flight_limit;
	}

	NextStatus LiveTransport::Next(RawRecord* record, std::uint64_t* wait_us) noexcept
	{
		const std::uint64_t start = NowUs();
		try
		{
			m_impl->session->next_command(m_impl->view);
			*wait_us = NowUs() - start;
			m_impl->holding = true;
			const proto::RecordHeader& header = *m_impl->view.header;
			record->type = header.type;
			record->flags = header.flags;
			record->payload_bytes = header.payload_bytes;
			record->sequence = header.sequence;
			record->payload = m_impl->view.payload;
			return NextStatus::record;
		}
		catch (const ipc::PeerExited& e)
		{
			*wait_us = NowUs() - start;
			m_error = e.what();
			return NextStatus::peer_exited;
		}
		catch (const std::exception& e)
		{
			*wait_us = NowUs() - start;
			m_error = e.what();
		}
		catch (...)
		{
			m_error = "unknown error";
		}
		return NextStatus::failed;
	}

	void LiveTransport::Release() noexcept
	{
		if (!m_impl->holding)
			return;
		m_impl->holding = false;
		try
		{
			m_impl->session->release_command(m_impl->view);
		}
		catch (...)
		{
		}
	}

	void LiveTransport::CompleteVsync() noexcept
	{
		try
		{
			m_impl->session->complete_vsync();
		}
		catch (...)
		{
		}
	}

	std::uint8_t* LiveTransport::ResponseArea(std::uint64_t* capacity) noexcept
	{
		*capacity = m_impl->session->response_capacity();
		return m_impl->session->response_area();
	}

	void LiveTransport::CompleteReadback(std::uint64_t request_id, std::uint64_t bytes, std::uint32_t status) noexcept
	{
		try
		{
			m_impl->session->complete_readback(request_id, bytes, status);
		}
		catch (...)
		{
		}
	}

	bool LiveTransport::PostEvent(std::uint16_t type, const void* payload, std::uint32_t bytes) noexcept
	{
		try
		{
			m_impl->session->post_event(static_cast<proto::RecordType>(type), payload, bytes);
			return true;
		}
		catch (const std::exception& e)
		{
			m_error = e.what();
		}
		catch (...)
		{
		}
		return false;
	}

	void LiveTransport::SetStateBits(std::uint32_t bits) noexcept
	{
		std::atomic_ref<std::uint32_t>(m_impl->session->control().state).fetch_or(bits);
	}

	void LiveTransport::SetSpinUs(std::uint32_t spin_us) noexcept
	{
		m_impl->session->set_spin_us(spin_us);
	}

	struct FileTransport::Impl
	{
		proto::FileReader reader;
		std::vector<std::uint8_t> payload;
		proto::RecordHeader header{};
	};

	FileTransport::FileTransport(std::unique_ptr<Impl> impl)
		: m_impl(std::move(impl))
	{
	}

	FileTransport::~FileTransport() = default;

	std::unique_ptr<FileTransport> FileTransport::Open(const std::string& path, std::string* error) noexcept
	{
		try
		{
			auto impl = std::make_unique<Impl>();
			if (!impl->reader.open(path, *error))
				return nullptr;
			if (impl->reader.header().protocol_major != proto::kVersionMajor)
			{
				*error = "the file uses protocol major version " + std::to_string(impl->reader.header().protocol_major);
				return nullptr;
			}
			return std::unique_ptr<FileTransport>(new FileTransport(std::move(impl)));
		}
		catch (const std::exception& e)
		{
			*error = e.what();
		}
		catch (...)
		{
			*error = "unknown error";
		}
		return nullptr;
	}

	std::string FileTransport::Serial() const noexcept
	{
		const proto::FileHeader& header = m_impl->reader.header();
		return FieldText(header.serial, sizeof(header.serial));
	}

	NextStatus FileTransport::Next(RawRecord* record) noexcept
	{
		try
		{
			if (!m_impl->reader.next(m_impl->header, m_impl->payload))
				return NextStatus::end;
			record->type = m_impl->header.type;
			record->flags = m_impl->header.flags;
			record->payload_bytes = m_impl->header.payload_bytes;
			record->sequence = m_impl->header.sequence;
			record->payload = m_impl->payload.data();
			return NextStatus::record;
		}
		catch (const std::exception& e)
		{
			m_error = e.what();
		}
		catch (...)
		{
			m_error = "unknown error";
		}
		return NextStatus::failed;
	}

	const char* ProtocolVersionText() noexcept
	{
		static const std::string text =
			std::to_string(proto::kVersionMajor) + "." + std::to_string(proto::kVersionMinor);
		return text.c_str();
	}
} // namespace gt4gs
