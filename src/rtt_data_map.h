/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

enum class RttFieldType
{
  UInt8,
  Int8,
  UInt16,
  Int16,
  UInt32,
  Int32,
  Float32,
  Float64,
};

struct RttFieldDef
{
  std::string name;
  size_t offset = 0;
  RttFieldType type = RttFieldType::Float32;
  std::string unit;
};

// Binary layout of one telemetry record, loaded from a JSON file.
// See docs/data-map.md for the file format.
struct RttDataMap
{
  size_t record_size = 0;

  // Every record starts with this 32-bit little-endian word. RTT is a raw
  // byte stream with no framing, and the host almost never connects on a
  // record boundary; the magic is what lets the parser find (and re-find)
  // the start of each record.
  uint32_t magic = 0;

  // Name of the field holding the record counter. The time axis is
  // seq / nominal_rate_hz, so plots use the target's clock, not the host's.
  std::string seq_field;
  double nominal_rate_hz = 1000.0;

  // Expected increment of seq between consecutive records. A jump larger
  // than twice this value is counted as lost records.
  int64_t seq_step = 1;

  // seq going backwards means the target rebooted, but only when the new
  // value is below this threshold. -1: any backwards step is a reset.
  int64_t reset_below_seq = -1;

  // Optional second record type for burst captures: same size, its own
  // magic, carries a seq from the past and only the fields listed in
  // dump_fields are published.
  bool has_dump_magic = false;
  uint32_t dump_magic = 0;
  std::vector<std::string> dump_fields;

  // Informational only; not checked by the parser.
  int version = 0;

  std::vector<RttFieldDef> fields;

  static size_t typeSize(RttFieldType type);
  // Throws std::runtime_error on an unknown type name.
  static RttFieldType parseType(const std::string& name);

  // Throw std::runtime_error with a readable message on any malformed or
  // missing content (bad JSON, missing key, field past record_size,
  // unknown type, seq_field not among the fields).
  static RttDataMap loadFromJsonText(const std::string& json_text);
  static RttDataMap loadFromFile(const std::string& path);
};
