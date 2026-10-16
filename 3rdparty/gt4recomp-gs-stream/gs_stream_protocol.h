// SPDX-License-Identifier: MIT
// GS stream protocol, version 1.2 (normative text: docs/GS-STREAM-PROTOCOL.md).
//
// A generic stream of PlayStation 2 GS hardware traffic between a producer process (the game, which runs the EE
// and VU code) and a consumer process (a GS renderer). The stream carries only GS hardware data: GIF packets per
// path, privileged GS register writes, vertical syncs with the field bit, local-to-host FIFO read requests and
// their replies, and control messages. It carries no guest code, no guest RAM and no CPU state.
//
// Written from the GS hardware register map and from our own capture format (.gscap); no renderer code was used.
// The header is self-contained: C++20, fixed-width little-endian structs, no dependencies beyond <cstdint> and
// <cstddef>. Every struct is a standard-layout aggregate; sizes and offsets are checked by static_assert so that
// both processes (built by different toolchains) agree on the layout.
//
// Shared fields that both processes update concurrently are plain std::uint64_t / std::uint32_t members, accessed
// through std::atomic_ref by the transport (gs_stream_ipc.hpp). They are 8-byte (or 4-byte) aligned, so the
// accesses are lock-free on x86-64.
#pragma once

#include <cstddef>
#include <cstdint>

namespace gs_stream {

// ---------------------------------------------------------------------------------------------------------------
// Versioning
// ---------------------------------------------------------------------------------------------------------------

inline constexpr std::uint32_t kMagic = 0x54535347u;  // bytes 'G','S','S','T' in memory (little-endian u32)
inline constexpr std::uint16_t kVersionMajor = 1;     // a consumer refuses a different major version
inline constexpr std::uint16_t kVersionMinor = 2;     // a consumer accepts a producer minor >= its own minimum
// Version history: 1.0 (TASK-035), 1.1 (TASK-047: VSYNC field is the display field), 1.2 (TASK-060/062: CONTROL cursor,
// OSD_STATS record, READY flag kReadyFlagOsdStats). A producer sends the 1.2 additions only to a consumer whose READY
// version_minor is 2 or more.

inline constexpr std::uint32_t kRecordAlign = 16;  // every record starts on a 16-byte boundary of the ring/file
inline constexpr std::uint32_t kControlBlockBytes = 4096;

// Default region sizes of a session mapping (docs/GS-STREAM-PROTOCOL.md, section 3). The control block records the
// sizes actually used, so a consumer must read them from there.
inline constexpr std::uint64_t kDefaultCommandCapacity = 32ull << 20;            // game -> GS command ring
inline constexpr std::uint64_t kDefaultResponseCapacity = (4ull << 20) + 65536;  // readback reply area
inline constexpr std::uint64_t kDefaultEventCapacity = 256ull << 10;             // GS -> game event ring

// Privileged GS registers: 0x12000000-0x12001FFF in the EE physical map (8 KiB). PRIV addresses are 8-byte aligned.
inline constexpr std::uint32_t kPrivilegedBase = 0x12000000u;
inline constexpr std::uint32_t kPrivilegedBytes = 0x2000u;

// ---------------------------------------------------------------------------------------------------------------
// Records
// ---------------------------------------------------------------------------------------------------------------

enum class RecordType : std::uint16_t {
  wrap = 0,  // ring filler up to the end of the buffer; never delivered and never written to a file

  // Producer (game) -> consumer (GS), in stream order.
  gif = 1,        // GifPayload + GIF bytes (multiple of 16) for one path
  priv = 2,       // PrivPayload: one privileged register write
  vsync = 3,      // VsyncPayload: start of a vertical blank (end of the frame's work)
  read_fifo = 4,  // ReadFifoPayload: local-to-host FIFO read; the reply goes to the response area
  reset = 5,      // ResetPayload: GS hard reset or GIF path reset
  control = 6,    // ControlPayload: window, snapshot, configuration and shutdown requests
  // Reserved in versions 1.0 to 1.2: a consumer answers them with ERROR (unsupported) and ignores them.
  state_save = 7,
  state_load = 8,
  aux_frame_begin = 9,
  aux_frame_end = 10,
  // Version 1.2: producer performance numbers for a consumer's on-screen display (not GS traffic; never in a .gscap).
  osd_stats = 11,  // OsdStatsPayload

  // Consumer (GS) -> producer (game), on the event ring.
  ready = 0x81,          // ReadyPayload: handshake accepted, the consumer processes commands from now on
  stats = 0x82,          // StatsPayload: cumulative consumer counters
  error = 0x83,          // ErrorPayload: a refused record or a fatal condition
  snapshot_done = 0x84,  // SnapshotDonePayload: a CONTROL snapshot request is complete
  window = 0x85,         // WindowPayload: window state change
  exiting = 0x86,        // ExitingPayload: the consumer is leaving (after shutdown or a fatal error)
};

// Record header flags.
inline constexpr std::uint16_t kRecordFlagMore = 1u;  // the next record of the same type continues this one (split)

// Every record: this 16-byte header, then `payload_bytes` bytes, then zero padding to a multiple of 16.
struct RecordHeader {
  std::uint16_t type;           // RecordType
  std::uint16_t flags;          // kRecordFlag*
  std::uint32_t payload_bytes;  // bytes after the header, excluding the padding
  std::uint64_t sequence;       // 1, 2, 3, ... per direction; wrap fillers carry 0 and do not consume a number
};
static_assert(sizeof(RecordHeader) == 16);
static_assert(offsetof(RecordHeader, payload_bytes) == 4 && offsetof(RecordHeader, sequence) == 8);

constexpr std::uint64_t align_record(std::uint64_t bytes) { return (bytes + kRecordAlign - 1) & ~std::uint64_t(kRecordAlign - 1); }
constexpr std::uint64_t record_size(std::uint64_t payload_bytes) { return sizeof(RecordHeader) + align_record(payload_bytes); }

// --- GIF -------------------------------------------------------------------------------------------------------
// One accepted GIF transfer chunk for one path, in acceptance order across all paths. The bytes are exactly the
// qwords the GIF received on that path (tags and data); a consumer runs one GIF tag parser per path. A chunk that
// is larger than the largest record is split into several GIF records with kRecordFlagMore on all but the last;
// each part repeats the GifPayload with its own `bytes`.
inline constexpr std::uint32_t kGifFlagSignalsApplied = 1u;  // the producer already applied SIGNAL/FINISH/LABEL to its own CSR/SIGLBLID copy

struct GifPayload {
  std::uint8_t path;  // 1 = PATH1 (VU1 XGKICK), 2 = PATH2 (VIF1 DIRECT/DIRECTHL), 3 = PATH3 (GIF DMA / FIFO)
  std::uint8_t reserved0[3];
  std::uint32_t flags;  // kGifFlag*
  std::uint32_t bytes;  // GIF bytes that follow this struct; a multiple of 16
  std::uint32_t reserved1;
};
static_assert(sizeof(GifPayload) == 16);

// --- PRIV ------------------------------------------------------------------------------------------------------
// A 64-bit write to a privileged GS register (PMODE, SMODE1/2, SRFSH, SYNCH1/2, SYNCV, DISPFB1/2, DISPLAY1/2, EXTBUF,
// EXTDATA, EXTWRITE, BGCOLOR, CSR, IMR, BUSDIR, SIGLBLID). The producer keeps CSR, IMR and SIGLBLID itself and
// answers EE reads of them locally; the consumer keeps its own 8 KiB block for display. CSR bit 9 (RESET) in a
// PRIV write is a GS reset request; CSR bits 0-4 written as 1 clear the matching interrupt flags.
struct PrivPayload {
  std::uint32_t address;  // kPrivilegedBase .. kPrivilegedBase + kPrivilegedBytes - 8, 8-byte aligned
  std::uint32_t reserved;
  std::uint64_t value;
};
static_assert(sizeof(PrivPayload) == 16);

// --- VSYNC -----------------------------------------------------------------------------------------------------
// Start of a vertical blank. Everything posted before it belongs to the frame that ends here. `field` is the display
// field of that frame: the CSR FIELD bit (bit 13, 1 = odd) the consumer uses when it presents the frame that ends
// here, i.e. the field the game prepared that frame's display for (version 1.1; docs/GS-STREAM-PROTOCOL.md 6.1).
// A consumer uses it as is, without shifting it by one VSYNC.
inline constexpr std::uint32_t kVsyncFlagFieldValid = 1u;  // `field` carries the display field

struct VsyncPayload {
  std::uint64_t frame;  // 0 for the first VSYNC of the session, then +1 per VSYNC
  std::uint8_t field;   // 0 or 1
  std::uint8_t reserved0[3];
  std::uint32_t flags;  // kVsyncFlag*
};
static_assert(sizeof(VsyncPayload) == 16);

// --- READ_FIFO -------------------------------------------------------------------------------------------------
// Local-to-host transfer read (TRXDIR = 1 set up earlier in the stream). The consumer executes every earlier record,
// writes `qwords * 16` bytes into the response area and completes `request_id` in the control block.
struct ReadFifoPayload {
  std::uint32_t qwords;  // at most response_capacity / 16
  std::uint32_t reserved;
  std::uint64_t request_id;  // equals the control block readback_request value set for this read
};
static_assert(sizeof(ReadFifoPayload) == 16);

// --- RESET -----------------------------------------------------------------------------------------------------
enum class ResetKind : std::uint32_t {
  gs_hard = 1,    // power-on state of the GS: registers, transfer state and local memory cleared
  gif_paths = 2,  // GIF path parsers listed in path_mask return to "waiting for a tag"
};

struct ResetPayload {
  std::uint32_t kind;       // ResetKind
  std::uint32_t path_mask;  // gif_paths: bit N = path N (bits 1..3); 0 for gs_hard
  std::uint64_t reserved;
};
static_assert(sizeof(ResetPayload) == 16);

// --- CONTROL ---------------------------------------------------------------------------------------------------
enum class ControlKind : std::uint32_t {
  shutdown = 1,       // finish the stream, post EXITING, exit
  attach_window = 2,  // args: native parent window handle (HWND on Windows), width, height
  resize = 3,         // args: width, height of the parent client area
  reload_config = 4,  // re-read the consumer's configuration
  snapshot = 5,       // args: snapshot id, width (0 = native), height (0 = native); answered by SNAPSHOT_DONE
  // Version 1.2: mouse cursor policy over the consumer's child window. args: flags (kCursorFlag*), hide delay in
  // milliseconds (1..60000), 0. Without this record a consumer never changes the cursor.
  cursor = 6,
};

inline constexpr std::uint64_t kCursorFlagAutoHide = 1u;  // hide the cursor over the picture after the delay without movement

struct ControlPayload {
  std::uint32_t kind;  // ControlKind
  std::uint32_t reserved;
  std::uint64_t args[3];
};
static_assert(sizeof(ControlPayload) == 32);

// --- Back channel (event ring) -----------------------------------------------------------------------------------
inline constexpr std::uint32_t kReadyFlagReadbacksZeroFilled = 1u;  // READ_FIFO replies are zeros (no VRAM model)
inline constexpr std::uint32_t kReadyFlagPresents = 2u;             // the consumer presents frames itself
inline constexpr std::uint32_t kReadyFlagRecords = 4u;              // the consumer records the stream to a file
inline constexpr std::uint32_t kReadyFlagOsdStats = 8u;  // version 1.2: the consumer draws an OSD and wants OSD_STATS

struct ReadyPayload {
  std::uint16_t version_major;  // protocol version implemented by the consumer
  std::uint16_t version_minor;
  std::uint32_t consumer_pid;
  std::uint32_t flags;  // kReadyFlag*
  std::uint32_t reserved;
  char consumer[32];  // consumer program name, NUL padded
  char renderer[32];  // renderer name, NUL padded
  char version[64];   // consumer build version, NUL padded
};
static_assert(sizeof(ReadyPayload) == 144);

// --- OSD_STATS (version 1.2) -------------------------------------------------------------------------------------
// Producer-side performance numbers over one interval (about 0.5 s), for a consumer that draws an on-screen display.
// Sent only to a consumer that set kReadyFlagOsdStats in READY. Times are microseconds per frame (VSYNC) averaged over
// the interval, except the frame times. A field is meaningful only when its kOsdStat* bit is set in `valid`. The
// record is not GS hardware traffic: a consumer MUST NOT let it change the picture, and producers do not write it to
// .gscap files. Field meaning (the producer's own threads): main thread = the thread that posts the stream (for a game:
// its emulation thread); worker = a second producer thread whose output the main thread waits for (for a game: a VU1
// worker thread).
inline constexpr std::uint32_t kOsdStatFrameTimes = 1u;      // frames, interval_us, frame_time_avg_us, frame_time_max_us
inline constexpr std::uint32_t kOsdStatMainThreadCpu = 2u;   // main_thread_cpu_us
inline constexpr std::uint32_t kOsdStatWorkerBusy = 4u;      // worker_busy_us
inline constexpr std::uint32_t kOsdStatStreamCosts = 8u;     // stream_write_us, readback_wait_us, backpressure_us
inline constexpr std::uint32_t kOsdStatWorkerWait = 16u;     // worker_wait_us
inline constexpr std::uint32_t kOsdStatCriticalPath = 32u;   // critical_path_us, frame_budget_us

struct OsdStatsPayload {
  std::uint32_t valid;              // kOsdStat* bits
  std::uint32_t frames;             // VSYNCs in the interval
  std::uint64_t interval_us;        // wall time of the interval
  std::uint32_t frame_time_avg_us;  // interval_us / frames
  std::uint32_t frame_time_max_us;  // longest time between two VSYNC posts of the interval
  std::uint32_t main_thread_cpu_us;   // CPU time of the producer's main thread per frame
  std::uint32_t worker_busy_us;       // busy time of the producer's worker thread per frame
  std::uint32_t stream_write_us;      // main thread time writing records into the command ring per frame
  std::uint32_t readback_wait_us;     // main thread time waiting for READ_FIFO replies per frame
  std::uint32_t backpressure_us;      // main thread time waiting for ring space or frames in flight per frame
  std::uint32_t worker_wait_us;       // main thread time waiting for the worker per frame
  std::uint32_t critical_path_us;     // main thread CPU + every wait above, per frame
  std::uint32_t frame_budget_us;      // the frame period the producer paces to (16,683 us at 59.94 Hz)
  std::uint32_t reserved[2];
};
static_assert(sizeof(OsdStatsPayload) == 64);
static_assert(offsetof(OsdStatsPayload, frame_time_avg_us) == 16 && offsetof(OsdStatsPayload, frame_budget_us) == 52);

// Cumulative counters since READY. A consumer posts STATS at least at every 500th VSYNC and at shutdown.
struct StatsPayload {
  std::uint64_t vsyncs;
  std::uint64_t records;
  std::uint64_t bytes;  // record bytes consumed, headers and padding included
  std::uint64_t readbacks;
  std::uint64_t readbacks_zero_filled;
  std::uint64_t busy_us;  // consumer time spent processing records
  std::uint64_t wait_us;  // consumer time spent waiting for records
  std::uint64_t reserved;
};
static_assert(sizeof(StatsPayload) == 64);

enum class ErrorCode : std::uint32_t {
  version_mismatch = 1,
  layout_mismatch = 2,
  bad_record = 3,
  unsupported_record = 4,
  device_failure = 5,
  internal = 6,
};

struct ErrorPayload {
  std::uint32_t code;   // ErrorCode
  std::uint32_t fatal;  // 1: the consumer exits after this record
  char text[248];       // UTF-8, NUL terminated
};
static_assert(sizeof(ErrorPayload) == 256);

struct SnapshotDonePayload {
  std::uint64_t id;
  std::uint32_t width;
  std::uint32_t height;
  std::uint64_t response_offset;  // byte offset of RGBA8 pixels inside the response area
  std::uint64_t bytes;
};
static_assert(sizeof(SnapshotDonePayload) == 32);

enum class WindowState : std::uint32_t { lost = 1, restored = 2 };

struct WindowPayload {
  std::uint32_t state;  // WindowState
  std::uint32_t reserved;
  std::uint64_t reserved2;
};
static_assert(sizeof(WindowPayload) == 16);

struct ExitingPayload {
  std::uint32_t exit_code;
  std::uint32_t reserved;
  std::uint64_t reserved2;
};
static_assert(sizeof(ExitingPayload) == 16);

// ---------------------------------------------------------------------------------------------------------------
// Control block: the first 4 KiB of the session mapping
// ---------------------------------------------------------------------------------------------------------------

// state bits (ControlBlock::state)
inline constexpr std::uint32_t kStateProducerReady = 1u;  // producer filled the block and created the events
inline constexpr std::uint32_t kStateConsumerReady = 2u;  // consumer validated the block (READY follows)
inline constexpr std::uint32_t kStateFatal = 4u;          // either side hit a fatal error; the peer stops waiting
inline constexpr std::uint32_t kStateExiting = 8u;        // the consumer is exiting

// readback_status values
inline constexpr std::uint32_t kReadbackOk = 0u;
inline constexpr std::uint32_t kReadbackZeroFilled = 1u;  // reply bytes are zeros (consumer without VRAM)
inline constexpr std::uint32_t kReadbackFailed = 2u;

struct ControlBlock {
  // --- written once by the producer before the consumer starts (offsets 0-127) ---
  std::uint32_t magic;          // kMagic
  std::uint16_t version_major;  // kVersionMajor
  std::uint16_t version_minor;  // kVersionMinor
  std::uint32_t control_block_bytes;  // kControlBlockBytes
  std::uint32_t record_header_bytes;  // sizeof(RecordHeader)
  std::uint32_t producer_pid;
  std::uint32_t consumer_pid;  // written by the consumer during the handshake
  std::uint32_t state;         // kState* bits, atomic
  std::uint32_t frames_in_flight_limit;  // VSYNCs the producer may post ahead of the consumer (1..3)
  char serial[16];             // game serial, e.g. "SCUS-97328", NUL padded
  std::uint64_t mapping_bytes;  // total size of the mapping
  std::uint64_t command_offset;
  std::uint64_t command_capacity;
  std::uint64_t response_offset;
  std::uint64_t response_capacity;
  std::uint64_t event_offset;
  std::uint64_t event_capacity;
  std::uint8_t reserved0[128 - 104];

  // --- command ring positions (monotonic byte counters; index = position % capacity) ---
  std::uint64_t command_write;  // producer, release
  std::uint8_t pad_command_write[56];
  std::uint64_t command_read;  // consumer, release
  std::uint8_t pad_command_read[56];

  // --- event ring positions ---
  std::uint64_t event_write;  // consumer
  std::uint8_t pad_event_write[56];
  std::uint64_t event_read;  // producer
  std::uint8_t pad_event_read[56];

  // --- waiter flags: set by a side before it sleeps on the matching event (offset 384) ---
  std::uint32_t data_waiter;      // consumer waits for command records ("-data" event)
  std::uint32_t space_waiter;     // producer waits for ring space or frames in flight ("-space" event)
  std::uint32_t response_waiter;  // producer waits for a readback reply ("-response" event)
  std::uint32_t event_waiter;     // producer waits for an event record ("-event" event)
  std::uint8_t pad_waiters[48];

  // --- frame pacing (offset 448) ---
  std::uint64_t vsyncs_posted;  // producer, after each VSYNC record
  std::uint64_t vsyncs_done;    // consumer, after it processed a VSYNC record
  std::uint8_t pad_frames[48];

  // --- readback (offset 512) ---
  std::uint64_t readback_request;  // producer: id of the last READ_FIFO posted
  std::uint64_t readback_done;     // consumer: id of the last READ_FIFO answered
  std::uint64_t readback_bytes;    // consumer: reply bytes in the response area
  std::uint32_t readback_status;   // consumer: kReadback*
  std::uint8_t pad_readback[36];

  // --- liveness counters (offset 576) ---
  std::uint64_t producer_heartbeat;  // +1 per VSYNC posted
  std::uint64_t consumer_heartbeat;  // +1 per batch of records processed
  std::uint8_t reserved1[kControlBlockBytes - 592];
};
static_assert(sizeof(ControlBlock) == kControlBlockBytes);
static_assert(offsetof(ControlBlock, serial) == 32);
static_assert(offsetof(ControlBlock, mapping_bytes) == 48);
static_assert(offsetof(ControlBlock, event_capacity) == 96);
static_assert(offsetof(ControlBlock, command_write) == 128);
static_assert(offsetof(ControlBlock, command_read) == 192);
static_assert(offsetof(ControlBlock, event_write) == 256);
static_assert(offsetof(ControlBlock, event_read) == 320);
static_assert(offsetof(ControlBlock, data_waiter) == 384);
static_assert(offsetof(ControlBlock, vsyncs_posted) == 448);
static_assert(offsetof(ControlBlock, readback_request) == 512);
static_assert(offsetof(ControlBlock, readback_status) == 536);
static_assert(offsetof(ControlBlock, producer_heartbeat) == 576);

// ---------------------------------------------------------------------------------------------------------------
// Stream file (.gscap version 2): "the protocol written to a file"
// ---------------------------------------------------------------------------------------------------------------
// FileHeader, then the game -> GS records exactly as they travel in the command ring (header, payload, zero padding;
// no wrap fillers). Version 1 .gscap files (single-letter records 'G','P','R','V') have no header.
inline constexpr char kFileMagic[8] = {'G', 'S', 'S', 'T', 'R', 'E', 'A', 'M'};
inline constexpr std::uint16_t kFileVersion = 2;

struct FileHeader {
  char magic[8];                // kFileMagic
  std::uint16_t file_version;   // kFileVersion
  std::uint16_t protocol_major;  // kVersionMajor of the records
  std::uint16_t protocol_minor;  // kVersionMinor of the records
  std::uint16_t header_bytes;    // sizeof(FileHeader)
  std::uint32_t record_header_bytes;  // sizeof(RecordHeader)
  std::uint32_t flags;          // 0 in file version 2
  char serial[16];              // game serial, NUL padded
  std::uint64_t first_frame;    // VSYNC frame number of the first recorded frame (0 = from the start)
  std::uint64_t reserved[2];
};
static_assert(sizeof(FileHeader) == 64);

}  // namespace gs_stream
