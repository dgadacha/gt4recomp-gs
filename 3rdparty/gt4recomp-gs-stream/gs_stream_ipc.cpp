// SPDX-License-Identifier: MIT
// Shared-memory transport for the GS stream protocol; see gs_stream_ipc.hpp.
#include "gs_stream_ipc.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <immintrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <random>

namespace gs_stream::ipc {
namespace {

constexpr std::uint64_t kPageBytes = 4096;
constexpr DWORD kSleepSliceMs = 100;  // a sleeping side re-checks the fatal flag at least this often
const wchar_t *const kEventSuffix[4] = {L"-data", L"-space", L"-response", L"-event"};

std::uint64_t load(const std::uint64_t &value, std::memory_order order = std::memory_order_seq_cst) {
  return std::atomic_ref<const std::uint64_t>(value).load(order);
}
void store(std::uint64_t &value, std::uint64_t next, std::memory_order order = std::memory_order_seq_cst) {
  std::atomic_ref<std::uint64_t>(value).store(next, order);
}
std::uint32_t load32(const std::uint32_t &value) { return std::atomic_ref<const std::uint32_t>(value).load(std::memory_order_seq_cst); }
void store32(std::uint32_t &value, std::uint32_t next) { std::atomic_ref<std::uint32_t>(value).store(next, std::memory_order_seq_cst); }

std::uint64_t now_us() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint64_t round_up(std::uint64_t value, std::uint64_t unit) { return (value + unit - 1) / unit * unit; }

std::string last_error_text(const char *what) {
  return std::string(what) + " failed (Windows error " + std::to_string(GetLastError()) + ")";
}

void check_capacity(std::uint64_t capacity, const char *what) {
  if (capacity < kPageBytes || capacity % kPageBytes != 0 || capacity > (1ull << 32))
    throw IpcError(std::string(what) + " capacity must be a multiple of 4096 between 4 KiB and 4 GiB");
}

std::string random_hex128() {
  std::random_device device;  // RtlGenRandom-backed in the Microsoft STL
  std::string text;
  static const char kDigits[] = "0123456789abcdef";
  for (int word = 0; word < 4; ++word) {
    const std::uint32_t value = device();
    for (int nibble = 7; nibble >= 0; --nibble) text.push_back(kDigits[(value >> (nibble * 4)) & 15u]);
  }
  return text;
}

}  // namespace

std::wstring widen(const std::string &text) {
  if (text.empty()) return {};
  const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), count);
  return wide;
}

std::uint32_t current_process_id() { return GetCurrentProcessId(); }

void close_handle(NativeHandle handle) {
  if (handle) CloseHandle(handle);
}

NativeHandle open_process_for_wait(std::uint32_t pid) {
  return OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
}

// ---------------------------------------------------------------------------------------------------------------
// Rings
// ---------------------------------------------------------------------------------------------------------------

RingWriter::RingWriter(std::uint8_t *base, std::uint64_t capacity, std::uint64_t *write_position,
                       std::uint64_t *read_position)
    : base_(base), capacity_(capacity), write_(write_position), read_(read_position) {}

std::uint64_t RingWriter::used_bytes() const { return load(*write_, std::memory_order_relaxed) - load(*read_, std::memory_order_acquire); }

bool RingWriter::try_write(RecordType type, std::uint16_t flags, const void *first, std::size_t first_bytes,
                           const void *second, std::size_t second_bytes) {
  const std::uint64_t payload = static_cast<std::uint64_t>(first_bytes) + second_bytes;
  if (payload > max_payload()) throw IpcError("GS stream record larger than half the ring");
  const std::uint64_t total = record_size(payload);
  const std::uint64_t head = load(*write_, std::memory_order_relaxed);
  const std::uint64_t tail = load(*read_, std::memory_order_acquire);
  const std::uint64_t offset = head % capacity_;
  const std::uint64_t filler = offset + total > capacity_ ? capacity_ - offset : 0;
  if (capacity_ - (head - tail) < filler + total) return false;

  std::uint64_t at = offset;
  if (filler != 0) {
    RecordHeader wrap{};
    wrap.type = static_cast<std::uint16_t>(RecordType::wrap);
    wrap.payload_bytes = static_cast<std::uint32_t>(filler - sizeof(RecordHeader));
    std::memcpy(base_ + offset, &wrap, sizeof wrap);
    at = 0;
  }
  RecordHeader header{};
  header.type = static_cast<std::uint16_t>(type);
  header.flags = flags;
  header.payload_bytes = static_cast<std::uint32_t>(payload);
  header.sequence = ++sequence_;
  std::uint8_t *out = base_ + at;
  std::memcpy(out, &header, sizeof header);
  out += sizeof header;
  if (first_bytes) std::memcpy(out, first, first_bytes);
  if (second_bytes) std::memcpy(out + first_bytes, second, second_bytes);
  const std::uint64_t padding = align_record(payload) - payload;
  if (padding) std::memset(out + payload, 0, static_cast<std::size_t>(padding));
  bytes_written_ += total;
  store(*write_, head + filler + total);  // seq_cst: ordered before the waiter-flag load of the caller
  return true;
}

RingReader::RingReader(std::uint8_t *base, std::uint64_t capacity, std::uint64_t *write_position,
                       std::uint64_t *read_position)
    : base_(base), capacity_(capacity), write_(write_position), read_(read_position) {}

bool RingReader::has_data() const { return load(*write_) != load(*read_, std::memory_order_relaxed); }

bool RingReader::peek(RecordView &view) {
  std::uint64_t tail = load(*read_, std::memory_order_relaxed);
  const std::uint64_t head = load(*write_, std::memory_order_acquire);
  while (tail != head) {
    const std::uint64_t offset = tail % capacity_;
    const auto *header = reinterpret_cast<const RecordHeader *>(base_ + offset);
    const std::uint64_t total = record_size(header->payload_bytes);
    if (header->type == static_cast<std::uint16_t>(RecordType::wrap)) {
      if (total != capacity_ - offset || total > head - tail) throw IpcError("GS stream ring: malformed wrap filler");
      tail += total;
      store(*read_, tail);
      continue;
    }
    if (offset + total > capacity_ || total > head - tail || header->sequence == 0)
      throw IpcError("GS stream ring: malformed record header");
    view.header = header;
    view.payload = base_ + offset + sizeof(RecordHeader);
    view.total_bytes = total;
    return true;
  }
  return false;
}

void RingReader::release(const RecordView &view) {
  store(*read_, load(*read_, std::memory_order_relaxed) + view.total_bytes);
}

// ---------------------------------------------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------------------------------------------

std::unique_ptr<Session> Session::create_producer(const SessionOptions &options) {
  check_capacity(options.command_capacity, "command ring");
  check_capacity(options.event_capacity, "event ring");
  if (options.response_capacity < 16 || options.response_capacity % 16 != 0)
    throw IpcError("response capacity must be a positive multiple of 16");
  if (options.frames_in_flight < 1 || options.frames_in_flight > 3) throw IpcError("frames in flight must be 1, 2 or 3");
  if (options.serial.size() > 15) throw IpcError("serial longer than 15 characters");
  if (options.prefix.empty() || options.prefix.find_first_of("\\/") != std::string::npos)
    throw IpcError("session prefix must be a plain name");

  std::unique_ptr<Session> session(new Session);
  session->producer_ = true;
  session->name_ = "Local\\" + options.prefix + "-" + std::to_string(GetCurrentProcessId()) + "-" + random_hex128();
  const std::uint64_t command_offset = kControlBlockBytes;
  const std::uint64_t response_offset = command_offset + options.command_capacity;
  const std::uint64_t event_offset = response_offset + round_up(options.response_capacity, kPageBytes);
  const std::uint64_t mapping_bytes = event_offset + options.event_capacity;

  const std::wstring wide = widen(session->name_);
  SetLastError(0);
  session->mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                         static_cast<DWORD>(mapping_bytes >> 32), static_cast<DWORD>(mapping_bytes),
                                         wide.c_str());
  if (!session->mapping_) throw IpcError(last_error_text("CreateFileMappingW"));
  if (GetLastError() == ERROR_ALREADY_EXISTS) throw IpcError("GS stream mapping name already exists");
  session->view_ = static_cast<std::uint8_t *>(MapViewOfFile(session->mapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0));
  if (!session->view_) throw IpcError(last_error_text("MapViewOfFile"));
  for (int index = 0; index < 4; ++index) {
    const std::wstring event_name = wide + kEventSuffix[index];
    SetLastError(0);
    session->events_[index] = CreateEventW(nullptr, FALSE, FALSE, event_name.c_str());
    if (!session->events_[index]) throw IpcError(last_error_text("CreateEventW"));
    if (GetLastError() == ERROR_ALREADY_EXISTS) throw IpcError("GS stream event name already exists");
  }

  ControlBlock &control = *reinterpret_cast<ControlBlock *>(session->view_);  // the mapping starts zeroed
  control.magic = kMagic;
  control.version_major = kVersionMajor;
  control.version_minor = kVersionMinor;
  control.control_block_bytes = kControlBlockBytes;
  control.record_header_bytes = sizeof(RecordHeader);
  control.producer_pid = GetCurrentProcessId();
  control.frames_in_flight_limit = options.frames_in_flight;
  std::memcpy(control.serial, options.serial.data(), options.serial.size());
  control.mapping_bytes = mapping_bytes;
  control.command_offset = command_offset;
  control.command_capacity = options.command_capacity;
  control.response_offset = response_offset;
  control.response_capacity = options.response_capacity;
  control.event_offset = event_offset;
  control.event_capacity = options.event_capacity;
  session->map_regions();
  store32(control.state, kStateProducerReady);
  return session;
}

std::unique_ptr<Session> Session::open_consumer(const std::string &name) {
  std::unique_ptr<Session> session(new Session);
  session->name_ = name;
  const std::wstring wide = widen(name);
  session->mapping_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wide.c_str());
  if (!session->mapping_) throw IpcError(last_error_text("OpenFileMappingW") + " for " + name);
  session->view_ = static_cast<std::uint8_t *>(MapViewOfFile(session->mapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0));
  if (!session->view_) throw IpcError(last_error_text("MapViewOfFile"));
  MEMORY_BASIC_INFORMATION region{};
  if (!VirtualQuery(session->view_, &region, sizeof region)) throw IpcError(last_error_text("VirtualQuery"));
  if (region.RegionSize < kControlBlockBytes) throw IpcError("GS stream mapping smaller than the control block");

  const ControlBlock &control = *reinterpret_cast<const ControlBlock *>(session->view_);
  if (control.magic != kMagic) throw IpcError("GS stream magic mismatch");
  if (control.version_major != kVersionMajor)
    throw IpcError("GS stream protocol major version " + std::to_string(control.version_major) + " is not " +
                   std::to_string(kVersionMajor));
  if (control.control_block_bytes != kControlBlockBytes || control.record_header_bytes != sizeof(RecordHeader))
    throw IpcError("GS stream struct sizes differ");
  if (!(load32(control.state) & kStateProducerReady)) throw IpcError("GS stream producer is not ready");
  const auto inside = [&](std::uint64_t offset, std::uint64_t bytes) {
    return offset >= kControlBlockBytes && offset % kPageBytes == 0 && bytes <= control.mapping_bytes &&
           offset <= control.mapping_bytes - bytes;
  };
  if (control.mapping_bytes > region.RegionSize || !inside(control.command_offset, control.command_capacity) ||
      !inside(control.response_offset, control.response_capacity) || !inside(control.event_offset, control.event_capacity) ||
      control.command_capacity % kPageBytes != 0 || control.event_capacity % kPageBytes != 0 ||
      control.response_capacity % 16 != 0)
    throw IpcError("GS stream region layout is invalid");
  if (control.frames_in_flight_limit < 1 || control.frames_in_flight_limit > 3)
    throw IpcError("GS stream frames-in-flight limit out of range");
  for (int index = 0; index < 4; ++index) {
    const std::wstring event_name = wide + kEventSuffix[index];
    session->events_[index] = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, event_name.c_str());
    if (!session->events_[index]) throw IpcError(last_error_text("OpenEventW"));
  }
  session->map_regions();
  return session;
}

void Session::map_regions() {
  control_ = reinterpret_cast<ControlBlock *>(view_);
  ControlBlock &c = *control_;
  response_ = view_ + c.response_offset;
  if (producer_) {
    writer_ = RingWriter(view_ + c.command_offset, c.command_capacity, &c.command_write, &c.command_read);
    reader_ = RingReader(view_ + c.event_offset, c.event_capacity, &c.event_write, &c.event_read);
  } else {
    reader_ = RingReader(view_ + c.command_offset, c.command_capacity, &c.command_write, &c.command_read);
    writer_ = RingWriter(view_ + c.event_offset, c.event_capacity, &c.event_write, &c.event_read);
  }
}

Session::~Session() {
  for (NativeHandle &event : events_) close_handle(event);
  if (view_) UnmapViewOfFile(view_);
  close_handle(mapping_);
}

RingWriter &Session::commands_out() {
  if (!producer_) throw IpcError("commands_out on the consumer side");
  return writer_;
}
RingReader &Session::events_in() {
  if (!producer_) throw IpcError("events_in on the consumer side");
  return reader_;
}
RingReader &Session::commands_in() {
  if (producer_) throw IpcError("commands_in on the producer side");
  return reader_;
}
RingWriter &Session::events_out() {
  if (producer_) throw IpcError("events_out on the producer side");
  return writer_;
}

void Session::signal(Wake which) { SetEvent(events_[static_cast<int>(which)]); }

void Session::wake_if_waiting(std::uint32_t *waiter, Wake which) {
  if (load32(*waiter)) SetEvent(events_[static_cast<int>(which)]);
}

template <typename Ready>
void Session::wait_until(Ready ready, std::uint32_t *waiter, Wake which, std::uint64_t *wait_us, std::uint32_t timeout_ms,
                         const char *what) {
  if (ready()) return;
  const std::uint64_t start = now_us();
  struct HookScope {
    const WaitHooks &hooks;
    Wake which;
    HookScope(const WaitHooks &h, Wake w) : hooks(h), which(w) {
      if (hooks.begin) hooks.begin(which);
    }
    ~HookScope() {
      if (hooks.end) hooks.end(which);
    }
  } hook_scope(hooks_, which);
  const auto account = [&] {
    if (wait_us) *wait_us += now_us() - start;
  };
  while (now_us() - start < spin_us_) {
    if (ready()) {
      account();
      return;
    }
    _mm_pause();
  }
  const HANDLE handles[2] = {events_[static_cast<int>(which)], peer_};
  const DWORD count = peer_ ? 2u : 1u;
  for (;;) {
    store32(*waiter, 1);
    if (ready()) {
      store32(*waiter, 0);
      break;
    }
    const DWORD result = WaitForMultipleObjects(count, handles, FALSE, kSleepSliceMs);
    store32(*waiter, 0);
    if (ready()) break;
    if (result == WAIT_OBJECT_0 + 1) {
      account();
      throw PeerExited(std::string("GS stream peer process exited while waiting for ") + what);
    }
    if (result == WAIT_FAILED) throw IpcError(last_error_text("WaitForMultipleObjects"));
    if (load32(control_->state) & kStateFatal) {
      account();
      throw PeerExited(std::string("GS stream peer reported a fatal error while waiting for ") + what);
    }
    if (timeout_ms != INFINITE && now_us() - start >= static_cast<std::uint64_t>(timeout_ms) * 1000) {
      account();
      throw TimedOut(std::string("GS stream wait for ") + what + " timed out");
    }
  }
  account();
}

void Session::post(RecordType type, std::uint16_t flags, const void *first, std::size_t first_bytes, const void *second,
                   std::size_t second_bytes) {
  RingWriter &ring = commands_out();
  while (!ring.try_write(type, flags, first, first_bytes, second, second_bytes)) {
    // Back-pressure: wait until the consumer released enough bytes for the record (and a possible filler).
    const std::uint64_t need = record_size(first_bytes + second_bytes) * 2;
    ++waits_.space_waits;
    wait_until([&] { return ring.capacity() - ring.used_bytes() >= std::min(need, ring.capacity()); },
               &control_->space_waiter, Wake::space, &waits_.space_wait_us, INFINITE, "ring space");
  }
  wake_if_waiting(&control_->data_waiter, Wake::data);
}

void Session::post_vsync(const VsyncPayload &vsync) {
  post(RecordType::vsync, 0, &vsync, sizeof vsync);
  ControlBlock &c = *control_;
  const std::uint64_t posted = load(c.vsyncs_posted, std::memory_order_relaxed) + 1;
  store(c.vsyncs_posted, posted);
  store(c.producer_heartbeat, load(c.producer_heartbeat, std::memory_order_relaxed) + 1);
  const std::uint64_t limit = c.frames_in_flight_limit;
  if (posted - load(c.vsyncs_done) > limit) {
    ++waits_.frames_waits;
    wait_until([&] { return posted - load(c.vsyncs_done) <= limit; }, &c.space_waiter, Wake::space,
               &waits_.frames_wait_us, INFINITE, "frames in flight");
  }
}

std::uint32_t Session::read_fifo(std::uint32_t qwords, void *out) {
  ControlBlock &c = *control_;
  const std::uint64_t bytes = static_cast<std::uint64_t>(qwords) * 16;
  if (bytes > c.response_capacity) throw IpcError("READ_FIFO larger than the response area");
  const std::uint64_t id = load(c.readback_request, std::memory_order_relaxed) + 1;
  store(c.readback_request, id);
  const ReadFifoPayload request{qwords, 0, id};
  post(RecordType::read_fifo, 0, &request, sizeof request);
  wait_until([&] { return load(c.readback_done) >= id; }, &c.response_waiter, Wake::response, &waits_.response_wait_us,
             INFINITE, "a readback reply");
  if (load(c.readback_done) != id) throw IpcError("GS stream readback reply out of order");
  const std::uint64_t reply = std::min(load(c.readback_bytes), bytes);
  std::memcpy(out, response_, static_cast<std::size_t>(reply));
  if (reply < bytes) std::memset(static_cast<std::uint8_t *>(out) + reply, 0, static_cast<std::size_t>(bytes - reply));
  return load32(c.readback_status);
}

bool Session::wait_event(RecordView &view, std::uint32_t timeout_ms) {
  RingReader &ring = events_in();
  try {
    wait_until([&] { return ring.has_data(); }, &control_->event_waiter, Wake::event, nullptr, timeout_ms, "an event");
  } catch (const TimedOut &) {
    return false;
  }
  return ring.peek(view);
}

void Session::next_command(RecordView &view) {
  RingReader &ring = commands_in();
  for (;;) {
    wait_until([&] { return ring.has_data(); }, &control_->data_waiter, Wake::data, &waits_.data_wait_us, INFINITE,
               "commands");
    if (ring.peek(view)) return;
  }
}

void Session::release_command(const RecordView &view) {
  commands_in().release(view);
  wake_if_waiting(&control_->space_waiter, Wake::space);
}

void Session::complete_vsync() {
  ControlBlock &c = *control_;
  store(c.vsyncs_done, load(c.vsyncs_done, std::memory_order_relaxed) + 1);
  wake_if_waiting(&c.space_waiter, Wake::space);
}

void Session::complete_readback(std::uint64_t request_id, std::uint64_t bytes, std::uint32_t status) {
  ControlBlock &c = *control_;
  store(c.readback_bytes, bytes);
  store32(c.readback_status, status);
  store(c.readback_done, request_id);
  wake_if_waiting(&c.response_waiter, Wake::response);
}

void Session::post_event(RecordType type, const void *payload, std::size_t bytes) {
  RingWriter &ring = events_out();
  // The event ring holds a few hundred records and the producer drains it every VSYNC; a full ring is rare, so a
  // short sleep is enough. A dead producer ends the wait.
  while (!ring.try_write(type, 0, payload, bytes, nullptr, 0)) {
    if (peer_ && WaitForSingleObject(peer_, 1) == WAIT_OBJECT_0) throw PeerExited("GS stream producer exited");
    if (!peer_) Sleep(1);
  }
  wake_if_waiting(&control_->event_waiter, Wake::event);
}

// ---------------------------------------------------------------------------------------------------------------
// Child process
// ---------------------------------------------------------------------------------------------------------------

ChildProcess::~ChildProcess() {
  if (job_) CloseHandle(job_);  // kill-on-close ends the child if it still runs
  close_handle(process_);
}

ChildProcess::ChildProcess(ChildProcess &&other) noexcept
    : process_(other.process_), job_(other.job_), pid_(other.pid_) {
  other.process_ = other.job_ = nullptr;
  other.pid_ = 0;
}

ChildProcess &ChildProcess::operator=(ChildProcess &&other) noexcept {
  if (this != &other) {
    if (job_) CloseHandle(job_);
    close_handle(process_);
    process_ = other.process_;
    job_ = other.job_;
    pid_ = other.pid_;
    other.process_ = other.job_ = nullptr;
    other.pid_ = 0;
  }
  return *this;
}

ChildProcess ChildProcess::spawn(const std::wstring &executable, const std::wstring &arguments) {
  ChildProcess child;
  child.job_ = CreateJobObjectW(nullptr, nullptr);
  if (!child.job_) throw IpcError(last_error_text("CreateJobObjectW"));
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(child.job_, JobObjectExtendedLimitInformation, &limits, sizeof limits))
    throw IpcError(last_error_text("SetInformationJobObject"));

  // The child shares our standard handles (inheritable duplicates), so its log lines reach the same output.
  HANDLE inherited[3] = {};
  const DWORD kinds[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
  for (int index = 0; index < 3; ++index) {
    const HANDLE original = GetStdHandle(kinds[index]);
    if (original && original != INVALID_HANDLE_VALUE)
      DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(), &inherited[index], 0, TRUE,
                      DUPLICATE_SAME_ACCESS);
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof startup;
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = inherited[0];
  startup.hStdOutput = inherited[1];
  startup.hStdError = inherited[2];
  std::wstring command_line = L"\"" + executable + L"\" " + arguments;
  PROCESS_INFORMATION info{};
  const BOOL created = CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr, TRUE,
                                      CREATE_SUSPENDED, nullptr, nullptr, &startup, &info);
  const DWORD create_error = GetLastError();
  for (HANDLE handle : inherited) close_handle(handle);
  if (!created) {
    SetLastError(create_error);
    throw IpcError(last_error_text("CreateProcessW"));
  }
  child.process_ = info.hProcess;
  child.pid_ = info.dwProcessId;
  if (!AssignProcessToJobObject(child.job_, info.hProcess)) {
    const std::string text = last_error_text("AssignProcessToJobObject");
    TerminateProcess(info.hProcess, 1);
    CloseHandle(info.hThread);
    throw IpcError(text);
  }
  ResumeThread(info.hThread);
  CloseHandle(info.hThread);
  return child;
}

bool ChildProcess::running() const { return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT; }

bool ChildProcess::wait_exit(std::uint32_t timeout_ms, std::uint32_t *exit_code) const {
  if (!process_) return true;
  if (WaitForSingleObject(process_, timeout_ms) != WAIT_OBJECT_0) return false;
  if (exit_code) {
    DWORD code = 0;
    GetExitCodeProcess(process_, &code);
    *exit_code = code;
  }
  return true;
}

void ChildProcess::kill() {
  if (job_) TerminateJobObject(job_, 1);
  if (process_) WaitForSingleObject(process_, 5000);
}

}  // namespace gs_stream::ipc
