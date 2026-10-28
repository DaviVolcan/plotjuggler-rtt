/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "binary_parser.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace
{
template <typename T>
double readAs(const uint8_t* p)
{
  T v;
  std::memcpy(&v, p, sizeof(v));
  return static_cast<double>(v);
}

double fieldValue(const uint8_t* record, const RttFieldDef& field)
{
  const uint8_t* p = record + field.offset;
  switch (field.type)
  {
    case RttFieldType::UInt8:
      return readAs<uint8_t>(p);
    case RttFieldType::Int8:
      return readAs<int8_t>(p);
    case RttFieldType::UInt16:
      return readAs<uint16_t>(p);
    case RttFieldType::Int16:
      return readAs<int16_t>(p);
    case RttFieldType::UInt32:
      return readAs<uint32_t>(p);
    case RttFieldType::Int32:
      return readAs<int32_t>(p);
    case RttFieldType::Float32:
      return readAs<float>(p);
    case RttFieldType::Float64:
      return readAs<double>(p);
  }
  return 0.0;
}

bool isFloat(RttFieldType type)
{
  return type == RttFieldType::Float32 || type == RttFieldType::Float64;
}
}  // namespace

BinaryParser::BinaryParser(RttDataMap map) : _map(std::move(map))
{
}

bool BinaryParser::decodeRecord(const uint8_t* record, bool is_dump, TelemetrySample& out)
{
  int64_t seq = 0;
  for (const RttFieldDef& field : _map.fields)
  {
    const double value = fieldValue(record, field);
    if (isFloat(field.type) && !std::isfinite(value))
    {
      return false;
    }
    if (field.name == _map.seq_field)
    {
      seq = static_cast<int64_t>(value);
    }
    const bool publish = !is_dump || std::find(_map.dump_fields.begin(), _map.dump_fields.end(),
                                               field.name) != _map.dump_fields.end();
    if (publish)
    {
      out.values.emplace_back(field.name, value);
    }
  }
  out.is_dump = is_dump;
  out.t = static_cast<double>(seq) / _map.nominal_rate_hz;

  if (is_dump)
  {
    // A dump record carries a seq from the past. It must not look like a
    // reset, and the jump back to live data must not count as a gap.
    _skip_next_gap = true;
    return true;
  }

  if (_last_seq >= 0 && seq < _last_seq &&
      (_map.reset_below_seq < 0 || seq < _map.reset_below_seq))
  {
    out.target_reset = true;
  }
  else if (_last_seq >= 0 && !_skip_next_gap && seq > _last_seq)
  {
    const int64_t step = _map.seq_step;
    const int64_t delta = seq - _last_seq;
    // With step > 1 the target is usually decimating a faster counter and
    // the delta jitters around step; only a jump past 2*step is a loss.
    const int64_t tolerance = (step > 1) ? 2 * step : 1;
    if (delta > tolerance)
    {
      _seq_gaps += static_cast<uint64_t>((delta + step / 2) / step - 1);
    }
  }
  _skip_next_gap = false;
  _last_seq = seq;
  return true;
}

uint32_t BinaryParser::wordAt(size_t pos) const
{
  uint32_t value;
  std::memcpy(&value, _buffer.data() + pos, sizeof(value));
  return value;
}

bool BinaryParser::magicAt(size_t pos) const
{
  if (pos + sizeof(uint32_t) > _buffer.size())
  {
    return false;
  }
  const uint32_t value = wordAt(pos);
  return value == _map.magic || (_map.has_dump_magic && value == _map.dump_magic);
}

bool BinaryParser::dumpMagicAt(size_t pos) const
{
  return _map.has_dump_magic && pos + sizeof(uint32_t) <= _buffer.size() &&
         wordAt(pos) == _map.dump_magic;
}

// Finds the next plausible record start. When the following record has
// already arrived, its magic must match too; a lone false match inside the
// payload is rare, two in a row at the right distance practically never
// happen. Without enough bytes the match is accepted provisionally, since
// every record re-checks its magic and a wrong guess fixes itself.
size_t BinaryParser::findSync() const
{
  const size_t rs = _map.record_size;
  for (size_t pos = 0; pos + sizeof(uint32_t) <= _buffer.size(); pos++)
  {
    if (!magicAt(pos))
    {
      continue;
    }
    if (pos + rs + sizeof(uint32_t) <= _buffer.size() && !magicAt(pos + rs))
    {
      continue;
    }
    return pos;
  }
  return std::string::npos;
}

void BinaryParser::resync()
{
  _buffer.clear();
  _synced = false;
  // The first record after reconnecting may legitimately be far ahead of
  // the last one (J-Link drops data while nobody is connected); that jump
  // is not a gap in this connection.
  _skip_next_gap = true;
}

std::vector<TelemetrySample> BinaryParser::feed(const char* data, size_t len)
{
  _buffer.append(data, len);
  std::vector<TelemetrySample> out;

  while (true)
  {
    if (!_synced)
    {
      const size_t sync = findSync();
      if (sync == std::string::npos)
      {
        // Keep the last 3 bytes: they may be the start of a magic split
        // across two TCP packets.
        const size_t keep = std::min<size_t>(_buffer.size(), sizeof(uint32_t) - 1);
        _discarded += _buffer.size() - keep;
        _buffer.erase(0, _buffer.size() - keep);
        break;
      }
      _discarded += sync;
      _buffer.erase(0, sync);
      _synced = true;
    }

    // A record is only accepted once the magic of the next one has
    // arrived too. Checking the record's own magic is not enough: when
    // J-Link drops bytes (its host-side buffer overflows while no client
    // is connected, and it cuts the stream mid-record), a leftover magic
    // followed by a complete record passes that check and decodes
    // shifted by a few bytes into plausible garbage. The cost is holding
    // back the newest record until the next one starts arriving.
    if (_buffer.size() < _map.record_size + sizeof(uint32_t))
    {
      break;
    }

    if (!magicAt(0) || !magicAt(_map.record_size))
    {
      _synced = false;
      _resyncs++;
      _malformed++;
      _discarded += 1;
      _buffer.erase(0, 1);
      continue;
    }

    const auto* record = reinterpret_cast<const uint8_t*>(_buffer.data());
    TelemetrySample sample;
    if (decodeRecord(record, dumpMagicAt(0), sample))
    {
      out.push_back(std::move(sample));
    }
    else
    {
      _malformed++;
    }
    _buffer.erase(0, _map.record_size);
  }
  return out;
}
