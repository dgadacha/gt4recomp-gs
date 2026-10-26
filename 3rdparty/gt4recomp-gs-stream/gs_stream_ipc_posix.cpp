// SPDX-License-Identifier: MIT
// Shared-memory transport for the GS stream protocol on macOS and Linux; see gs_stream_ipc.hpp.
//
// The rings and the session rules are those of gs_stream_ipc.cpp (Windows) line for line; only the operating system
// parts differ (docs/GS-STREAM-PROTOCOL.md section 5, POSIX note):
// - the session is a POSIX shared memory object "/<prefix>-<producer pid>-<64-bit random hex>" (macOS limits the
//   name to 31 characters, so the random part is 32 to 64 bits instead of 128), created with O_EXCL, mode 0600;
// - a sleeping side waits on its own waiter flag in the control block (os_sync_wait_on_address on macOS, a shared
//   futex on Linux) instead of a named event. The waking side, after publishing its position, finds the flag set,
//   clears it and wakes the address: a sleeper that has not entered its wait yet then sees the flag changed and
//   returns at once, so no wake-up is lost;
// - the peer process is a pid: a child is watched with waitpid, the parent with kill(pid, 0) and getppid(). Waits
//   still sleep in slices of at most 100 ms and check the peer and the fatal flag between slices;
// - a child process is spawned with posix_spawn and killed (SIGKILL) when its ChildProcess object goes away, the
//   equivalent of the kill-on-close Job Object.
#include "gs_stream_ipc.hpp"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <os/os_sync_wait_on_address.h>
#elif defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <ctime>
#else
#error "gs_stream_ipc_posix.cpp needs os_sync_wait_on_address (macOS) or futex (Linux)"
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <random>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

extern char **environ;

namespace gs_stream::ipc {
namespace {

constexpr std::uint64_t kPageBytes = 4096;
constexpr std::uint32_t kSleepSliceMs = 100;  // a sleeping side re-checks the peer and the fatal flag this often
constexpr std::uint32_t kInfinite = UINT32_MAX;
constexpr std::size_t kMaxNameBytes = 31;  // macOS PSHMNAMLEN, the leading slash included

// libc++ has no atomic_ref<const T> (the Windows transport uses it with the Microsoft STL): the value is shared
// memory written by both sides, so dropping const here changes nothing.
std::uint64_t load(const std::uint64_t &value, std::memory_order order = std::memory_order_seq_cst) {
  return std::atomic_ref<std::uint64_t>(const_cast<std::uint64_t &>(value)).load(order);
}
void store(std::uint64_t &value, std::uint64_t next, std::memory_order order = std::memory_order_seq_cst) {
  std::atomic_ref<std::uint64_t>(value).store(next, order);
}
std::uint32_t load32(const std::uint32_t &value) {
  return std::atomic_ref<std::uint32_t>(const_cast<std::uint32_t &>(value)).load(std::memory_order_seq_cst);
}
void store32(std::uint32_t &value, std::uint32_t next) { std::atomic_ref<std::uint32_t>(value).store(next, std::memory_order_seq_cst); }

std::uint64_t now_us() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint64_t round_up(std::uint64_t value, std::uint64_t unit) { return (value + unit - 1) / unit * unit; }

std::string errno_text(const char *what) { return std::string(what) + " failed (" + std::strerror(errno) + ")"; }

void check_capacity(std::uint64_t capacity, const char *what) {
  if (capacity < kPageBytes || capacity % kPageBytes != 0 || capacity > (1ull << 32))
    throw IpcError(std::string(what) + " capacity must be a multiple of 4096 between 4 KiB and 4 GiB");
}

// `digits` hexadecimal digits (at most 16) of a random 64-bit value.
std::string random_hex(std::size_t digits) {
  std::uint64_t value = 0;
#if defined(__APPLE__)
  arc4random_buf(&value, sizeof value);
#else
  std::random_device device;
  value = (static_cast<std::uint64_t>(device()) << 32) | device();
#endif
  static const char kDigits[] = "0123456789abcdef";
  std::string text;
  for (std::size_t nibble = 0; nibble < digits && nibble < 16; ++nibble) text.push_back(kDigits[(value >> (nibble * 4)) & 15u]);
  return text;
}

void spin_pause() {
#if defined(__aarch64__)
  asm volatile("yield");
#elif defined(__x86_64__)
  __builtin_ia32_pause();
#endif
}

// Sleeps while *address == expected, at most timeout_ms; spurious returns are allowed.
void wait_address(std::uint32_t *address, std::uint32_t expected, std::uint32_t timeout_ms) {
#if defined(__APPLE__)
  os_sync_wait_on_address_with_timeout(address, expected, sizeof *address, OS_SYNC_WAIT_ON_ADDRESS_SHARED,
                                       OS_CLOCK_MACH_ABSOLUTE_TIME, static_cast<std::uint64_t>(timeout_ms) * 1000000u);
#else
  const timespec timeout{static_cast<time_t>(timeout_ms / 1000), static_cast<long>(timeout_ms % 1000) * 1000000L};
  syscall(SYS_futex, address, FUTEX_WAIT, expected, &timeout, nullptr, 0);
#endif
}

void wake_address(std::uint32_t *address) {
#if defined(__APPLE__)
  os_sync_wake_by_address_any(address, sizeof *address, OS_SYNC_WAKE_BY_ADDRESS_SHARED);
#else
  syscall(SYS_futex, address, FUTEX_WAKE, 1, nullptr, nullptr, 0);
#endif
}

}  // namespace

// The peer process behind a NativeHandle (open_process_for_wait, ChildProcess::process).
struct PosixProcess {
  pid_t pid = 0;
  bool child = false;       // ours: reaped with waitpid
  bool was_parent = false;  // our parent when opened: its death reparents us
  // Read and set by any thread that waits on this process (the fixtures kill a peer from another thread).
  std::atomic<bool> exited{false};
  std::atomic<int> status{0};
};

namespace {

PosixProcess *as_process(NativeHandle handle) { return static_cast<PosixProcess *>(handle); }

bool process_exited(PosixProcess &process) {
  if (process.exited) return true;
  if (process.child) {
    int status = 0;
    const pid_t result = waitpid(process.pid, &status, WNOHANG);
    if (result == process.pid) {
      process.status = status;
      process.exited = true;
    } else if (result < 0 && errno == ECHILD) {
      process.exited = true;  // reaped by another thread (ChildProcess::kill)
    }
  } else if ((process.was_parent && getppid() != process.pid) || (kill(process.pid, 0) != 0 && errno == ESRCH)) {
    process.exited = true;
  }
  return process.exited;
}

std::uint32_t exit_code_of(const PosixProcess &process) {
  const int status = process.status;
  if (WIFEXITED(status)) return static_cast<std::uint32_t>(WEXITSTATUS(status));
  if (WIFSIGNALED(status)) return 128u + static_cast<std::uint32_t>(WTERMSIG(status));
  return 0;
}

}  // namespace

std::uint32_t current_process_id() { return static_cast<std::uint32_t>(getpid()); }

void close_handle(NativeHandle handle) { delete as_process(handle); }

NativeHandle open_process_for_wait(std::uint32_t pid) {
  const auto target = static_cast<pid_t>(pid);
  if (pid == 0 || (kill(target, 0) != 0 && errno == ESRCH)) return nullptr;
  auto *process = new PosixProcess;
  process->pid = target;
  process->was_parent = getppid() == target;
  return process;
}

// ---------------------------------------------------------------------------------------------------------------
// Rings (gs_stream_ipc.cpp)
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
  // As many random digits as the name limit leaves, 16 at most (64 bits) and 8 at least.
  const std::string stem = "/" + options.prefix + "-" + std::to_string(getpid()) + "-";
  if (stem.size() + 8 > kMaxNameBytes) throw IpcError("GS stream session prefix too long for a 31-character name");
  session->name_ = stem + random_hex(std::min<std::size_t>(16, kMaxNameBytes - stem.size()));
  const std::uint64_t command_offset = kControlBlockBytes;
  const std::uint64_t response_offset = command_offset + options.command_capacity;
  const std::uint64_t event_offset = response_offset + round_up(options.response_capacity, kPageBytes);
  const std::uint64_t mapping_bytes = event_offset + options.event_capacity;

  const int descriptor = shm_open(session->name_.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
  if (descriptor < 0) {
    if (errno == EEXIST) throw IpcError("GS stream mapping name already exists");
    throw IpcError(errno_text("shm_open"));
  }
  session->name_owned_ = true;
  if (ftruncate(descriptor, static_cast<off_t>(mapping_bytes)) != 0) {
    const std::string text = errno_text("ftruncate");
    close(descriptor);
    throw IpcError(text);
  }
  void *view = mmap(nullptr, static_cast<std::size_t>(mapping_bytes), PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
  close(descriptor);
  if (view == MAP_FAILED) throw IpcError(errno_text("mmap"));
  session->view_ = static_cast<std::uint8_t *>(view);
  session->view_bytes_ = mapping_bytes;

  ControlBlock &control = *reinterpret_cast<ControlBlock *>(session->view_);  // the object starts zeroed
  control.magic = kMagic;
  control.version_major = kVersionMajor;
  control.version_minor = kVersionMinor;
  control.control_block_bytes = kControlBlockBytes;
  control.record_header_bytes = sizeof(RecordHeader);
  control.producer_pid = static_cast<std::uint32_t>(getpid());
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
  const int descriptor = shm_open(name.c_str(), O_RDWR, 0);
  if (descriptor < 0) throw IpcError(errno_text("shm_open") + " for " + name);
  struct stat info {};
  if (fstat(descriptor, &info) != 0) {
    const std::string text = errno_text("fstat");
    close(descriptor);
    throw IpcError(text);
  }
  if (info.st_size < static_cast<off_t>(kControlBlockBytes)) {
    close(descriptor);
    throw IpcError("GS stream mapping smaller than the control block");
  }
  const auto region_bytes = static_cast<std::uint64_t>(info.st_size);
  void *view = mmap(nullptr, static_cast<std::size_t>(region_bytes), PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
  close(descriptor);
  if (view == MAP_FAILED) throw IpcError(errno_text("mmap"));
  session->view_ = static_cast<std::uint8_t *>(view);
  session->view_bytes_ = region_bytes;

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
  if (control.mapping_bytes > region_bytes || !inside(control.command_offset, control.command_capacity) ||
      !inside(control.response_offset, control.response_capacity) || !inside(control.event_offset, control.event_capacity) ||
      control.command_capacity % kPageBytes != 0 || control.event_capacity % kPageBytes != 0 ||
      control.response_capacity % 16 != 0)
    throw IpcError("GS stream region layout is invalid");
  if (control.frames_in_flight_limit < 1 || control.frames_in_flight_limit > 3)
    throw IpcError("GS stream frames-in-flight limit out of range");
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

void Session::release_name() {
  if (name_owned_) shm_unlink(name_.c_str());
  name_owned_ = false;
}

Session::~Session() {
  release_name();
  if (view_) munmap(view_, static_cast<std::size_t>(view_bytes_));
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

std::uint32_t *Session::waiter_flag(Wake which) {
  switch (which) {
    case Wake::data: return &control_->data_waiter;
    case Wake::space: return &control_->space_waiter;
    case Wake::response: return &control_->response_waiter;
    case Wake::event: return &control_->event_waiter;
  }
  return &control_->data_waiter;
}

void Session::signal(Wake which) { wake_address(waiter_flag(which)); }

void Session::wake_if_waiting(std::uint32_t *waiter, Wake which) {
  if (load32(*waiter)) {
    ++waits_.wakes_signalled[static_cast<int>(which)];
    // Clearing the flag before the wake makes a sleeper that has not entered its wait return at once.
    store32(*waiter, 0);
    wake_address(waiter);
  }
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
    spin_pause();
  }
  for (;;) {
    store32(*waiter, 1);
    if (ready()) {
      store32(*waiter, 0);
      break;
    }
    wait_address(waiter, 1, kSleepSliceMs);
    store32(*waiter, 0);
    if (ready()) break;
    if (peer_ && process_exited(*as_process(peer_))) {
      account();
      throw PeerExited(std::string("GS stream peer process exited while waiting for ") + what);
    }
    if (load32(control_->state) & kStateFatal) {
      account();
      throw PeerExited(std::string("GS stream peer reported a fatal error while waiting for ") + what);
    }
    if (timeout_ms != kInfinite && now_us() - start >= static_cast<std::uint64_t>(timeout_ms) * 1000) {
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
    // Back-pressure: the consumer must be awake to free space, so wake it first (batched records may be waiting).
    wake_if_waiting(&control_->data_waiter, Wake::data);
    bytes_since_data_wake_ = 0;
    // Wait until the consumer released enough bytes for the record (and a possible filler).
    const std::uint64_t need = record_size(first_bytes + second_bytes) * 2;
    ++waits_.space_waits;
    wait_until([&] { return ring.capacity() - ring.used_bytes() >= std::min(need, ring.capacity()); },
               &control_->space_waiter, Wake::space, &waits_.space_wait_us, kInfinite, "ring space");
  }
  // TASK-041 wake batching (gs_stream_ipc.cpp).
  bytes_since_data_wake_ += record_size(first_bytes + second_bytes);
  const bool batchable = type == RecordType::gif || type == RecordType::priv;
  if (!batchable || data_wake_bytes_ == 0 || bytes_since_data_wake_ >= data_wake_bytes_) {
    wake_if_waiting(&control_->data_waiter, Wake::data);
    bytes_since_data_wake_ = 0;
  }
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
               &waits_.frames_wait_us, kInfinite, "frames in flight");
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
             kInfinite, "a readback reply");
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
    wait_until([&] { return ring.has_data(); }, &control_->data_waiter, Wake::data, &waits_.data_wait_us, kInfinite,
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
    if (peer_ && process_exited(*as_process(peer_))) throw PeerExited("GS stream producer exited");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  wake_if_waiting(&control_->event_waiter, Wake::event);
}

// ---------------------------------------------------------------------------------------------------------------
// Child process
// ---------------------------------------------------------------------------------------------------------------

ChildProcess::~ChildProcess() {
  if (process_ && !process_exited(*as_process(process_))) kill();  // the kill-on-close job of the Windows transport
  close_handle(process_);
}

ChildProcess::ChildProcess(ChildProcess &&other) noexcept : process_(other.process_), job_(other.job_), pid_(other.pid_) {
  other.process_ = other.job_ = nullptr;
  other.pid_ = 0;
}

ChildProcess &ChildProcess::operator=(ChildProcess &&other) noexcept {
  if (this != &other) {
    if (process_ && !process_exited(*as_process(process_))) kill();
    close_handle(process_);
    process_ = other.process_;
    job_ = other.job_;
    pid_ = other.pid_;
    other.process_ = other.job_ = nullptr;
    other.pid_ = 0;
  }
  return *this;
}

ChildProcess ChildProcess::spawn(const std::string &executable, const std::vector<std::string> &arguments) {
  std::vector<char *> argv;
  argv.push_back(const_cast<char *>(executable.c_str()));
  for (const std::string &argument : arguments) argv.push_back(const_cast<char *>(argument.c_str()));
  argv.push_back(nullptr);
  pid_t pid = 0;
  // The child shares our standard streams, so its log lines reach the same output.
  const int result = posix_spawn(&pid, executable.c_str(), nullptr, nullptr, argv.data(), environ);
  if (result != 0) throw IpcError(std::string("posix_spawn failed (") + std::strerror(result) + ") for " + executable);
  ChildProcess child;
  auto *process = new PosixProcess;
  process->pid = pid;
  process->child = true;
  child.process_ = process;
  child.pid_ = static_cast<std::uint32_t>(pid);
  return child;
}

bool ChildProcess::running() const { return process_ && !process_exited(*as_process(process_)); }

bool ChildProcess::wait_exit(std::uint32_t timeout_ms, std::uint32_t *exit_code) const {
  if (!process_) return true;
  PosixProcess &process = *as_process(process_);
  const std::uint64_t start = now_us();
  while (!process_exited(process)) {
    if (timeout_ms != kInfinite && now_us() - start >= static_cast<std::uint64_t>(timeout_ms) * 1000) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (exit_code) *exit_code = exit_code_of(process);
  return true;
}

void ChildProcess::kill() {
  if (!process_) return;
  PosixProcess &process = *as_process(process_);
  if (process_exited(process)) return;
  ::kill(process.pid, SIGKILL);
  int status = 0;
  if (waitpid(process.pid, &status, 0) == process.pid) process.status = status;
  process.exited = true;
}

}  // namespace gs_stream::ipc
