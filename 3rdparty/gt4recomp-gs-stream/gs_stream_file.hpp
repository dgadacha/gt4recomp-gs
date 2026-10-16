// SPDX-License-Identifier: MIT
// GS stream files (.gscap version 2): a FileHeader followed by the producer -> consumer records exactly as they travel
// in the command ring (header, payload, zero padding; no wrap fillers). See gs_stream_protocol.h and
// docs/GS-STREAM-PROTOCOL.md section 6. The in-process capture of the game (GT4_GS_CAPTURE) and the recorder process
// (tools/gs_stream_record) both write this format, so their files can be compared byte for byte.
#pragma once

#include "gs_stream_protocol.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace gs_stream {

class FileWriter {
 public:
  FileWriter() = default;
  ~FileWriter();
  FileWriter(const FileWriter &) = delete;
  FileWriter &operator=(const FileWriter &) = delete;

  // Creates (truncates) `path` and writes the header. Returns false with `error` set when the file cannot be opened.
  bool open(const std::string &path, const std::string &serial, std::uint64_t first_frame, std::string &error);
  bool is_open() const { return file_ != nullptr; }
  // Writes one record built from its parts, with zero padding to 16 bytes.
  void write(RecordType type, std::uint16_t flags, std::uint64_t sequence, const void *first, std::size_t first_bytes,
             const void *second = nullptr, std::size_t second_bytes = 0);
  // Writes `bytes` bytes that already form complete records (a header plus its padded payload, as in the ring).
  void write_raw(const void *records, std::size_t bytes);
  void flush();
  void close();
  std::uint64_t records() const { return records_; }
  std::uint64_t bytes() const { return bytes_; }

 private:
  std::FILE *file_ = nullptr;
  std::vector<char> buffer_;
  std::uint64_t records_ = 0;
  std::uint64_t bytes_ = 0;
};

// Sequential reader used by fixtures and tools. Validates the header and every record header.
class FileReader {
 public:
  ~FileReader();
  bool open(const std::string &path, std::string &error);
  const FileHeader &header() const { return header_; }
  // Next record; false at a clean end of file. Throws std::runtime_error on a truncated or malformed record.
  bool next(RecordHeader &header, std::vector<std::uint8_t> &payload);

 private:
  std::FILE *file_ = nullptr;
  FileHeader header_{};
};

}  // namespace gs_stream
