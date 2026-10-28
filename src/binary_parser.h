/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rtt_data_map.h"
#include "telemetry_sample.h"

// Turns an unframed RTT byte stream into decoded records, using the magic
// word to find record boundaries. Feed it whatever arrives from the
// socket, in chunks of any size.
class BinaryParser
{
public:
  explicit BinaryParser(RttDataMap map);

  std::vector<TelemetrySample> feed(const char* data, size_t len);

  // Records rejected after framing (bad magic mid-stream, NaN/Inf float).
  uint64_t malformedCount() const
  {
    return _malformed;
  }

  // Records missing according to the seq field.
  uint64_t sequenceGaps() const
  {
    return _seq_gaps;
  }

  // Bytes thrown away while searching for the magic: the J-Link banner,
  // the partial record at connect time, resynchronisation. If this keeps
  // growing and synced() never becomes true, the target is sending a
  // layout that does not match the map.
  uint64_t discardedBytes() const
  {
    return _discarded;
  }

  bool synced() const
  {
    return _synced;
  }

  // Times sync was lost after having been acquired.
  uint64_t resyncs() const
  {
    return _resyncs;
  }

private:
  bool decodeRecord(const uint8_t* record, bool is_dump, TelemetrySample& out);
  uint32_t wordAt(size_t pos) const;
  bool magicAt(size_t pos) const;
  bool dumpMagicAt(size_t pos) const;
  size_t findSync() const;

  RttDataMap _map;
  std::string _buffer;
  int64_t _last_seq = -1;
  bool _after_dump = false;
  uint64_t _malformed = 0;
  uint64_t _seq_gaps = 0;
  uint64_t _discarded = 0;
  uint64_t _resyncs = 0;
  bool _synced = false;
};
