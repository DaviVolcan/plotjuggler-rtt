/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>

#include "binary_parser.h"

namespace
{
constexpr uint32_t kMagic = 0xC3A55A01u;
constexpr uint32_t kDumpMagic = 0xC3A55A81u;

// magic | seq (uint32) | ib (float32) | ic (float32)
RttDataMap basicMap()
{
  return RttDataMap::loadFromJsonText(R"json(
  {
    "record_size": 16,
    "magic": "0xC3A55A01",
    "seq_field": "seq",
    "nominal_rate_hz": 1000,
    "fields": [
      {"name": "seq", "offset": 4,  "type": "uint32"},
      {"name": "ib",  "offset": 8,  "type": "float32", "unit": "A"},
      {"name": "ic",  "offset": 12, "type": "float32", "unit": "A"}
    ]
  }
  )json");
}

std::string packRecord(uint32_t seq, float ib, float ic, uint32_t magic = kMagic)
{
  std::string buf(16, '\0');
  std::memcpy(&buf[0], &magic, 4);
  std::memcpy(&buf[4], &seq, 4);
  std::memcpy(&buf[8], &ib, 4);
  std::memcpy(&buf[12], &ic, 4);
  return buf;
}

std::string packStream(uint32_t first_seq, int count)
{
  std::string out;
  for (int i = 0; i < count; i++)
  {
    out += packRecord(first_seq + static_cast<uint32_t>(i), static_cast<float>(i),
                      -static_cast<float>(i));
  }
  return out;
}

// The parser holds the newest record until the next magic arrives; tests
// append this to release it.
std::string next(uint32_t magic = kMagic)
{
  std::string buf(4, '\0');
  std::memcpy(&buf[0], &magic, 4);
  return buf;
}

// Target that runs an 8 kHz counter, streams every 8th tick live and
// replays burst captures with the dump magic.
RttDataMap dumpMap()
{
  return RttDataMap::loadFromJsonText(R"json(
  {
    "record_size": 12, "seq_field": "seq", "nominal_rate_hz": 8000,
    "magic": "0xC3A55A01", "dump_magic": "0xC3A55A81",
    "seq_step": 8, "reset_below_seq": 80000, "dump_fields": ["ib"],
    "fields": [
      {"name": "seq", "offset": 4, "type": "uint32"},
      {"name": "ib",  "offset": 8, "type": "float32", "unit": "A"}
    ]
  }
  )json");
}

std::string packDumpMapRecord(uint32_t magic, uint32_t seq, float ib)
{
  std::string buf(12, '\0');
  std::memcpy(&buf[0], &magic, 4);
  std::memcpy(&buf[4], &seq, 4);
  std::memcpy(&buf[8], &ib, 4);
  return buf;
}
}  // namespace

TEST(BinaryParser, DecodesSingleRecord)
{
  BinaryParser parser(basicMap());
  const std::string record = packRecord(1, 1.5f, -2.5f) + next();
  const auto samples = parser.feed(record.data(), record.size());
  ASSERT_EQ(samples.size(), 1u);
  EXPECT_DOUBLE_EQ(samples[0].t, 0.001);
  ASSERT_EQ(samples[0].values.size(), 3u);
  EXPECT_EQ(samples[0].values[1].first, "ib");
  EXPECT_FLOAT_EQ(static_cast<float>(samples[0].values[1].second), 1.5f);
  EXPECT_FLOAT_EQ(static_cast<float>(samples[0].values[2].second), -2.5f);
  EXPECT_TRUE(parser.synced());
  EXPECT_EQ(parser.discardedBytes(), 0u);
}

TEST(BinaryParser, BuffersPartialRecord)
{
  BinaryParser parser(basicMap());
  const std::string record = packRecord(1, 1.0f, 2.0f);
  auto samples = parser.feed(record.data(), 6);
  EXPECT_TRUE(samples.empty());
  samples = parser.feed(record.data() + 6, record.size() - 6);
  EXPECT_TRUE(samples.empty());  // held until the next magic shows up
  const std::string tail = next();
  samples = parser.feed(tail.data(), tail.size());
  ASSERT_EQ(samples.size(), 1u);
}

TEST(BinaryParser, DetectsSequenceGap)
{
  BinaryParser parser(basicMap());
  const std::string s = packRecord(1, 0, 0) + packRecord(3, 0, 0) + next();  // 2 is missing
  parser.feed(s.data(), s.size());
  EXPECT_EQ(parser.sequenceGaps(), 1u);
}

TEST(BinaryParser, DetectsTargetReset)
{
  BinaryParser parser(basicMap());
  const std::string s = packRecord(10, 0, 0) + packRecord(0, 0, 0) + next();
  const auto samples = parser.feed(s.data(), s.size());
  ASSERT_EQ(samples.size(), 2u);
  EXPECT_TRUE(samples[1].target_reset);
}

TEST(BinaryParser, RejectsNonFiniteFloat)
{
  BinaryParser parser(basicMap());
  const std::string record = packRecord(1, std::nanf(""), 0.0f) + next();
  const auto samples = parser.feed(record.data(), record.size());
  EXPECT_TRUE(samples.empty());
  EXPECT_EQ(parser.malformedCount(), 1u);
}

TEST(BinaryParser, DecodesSignedAndDoubleTypes)
{
  BinaryParser parser(RttDataMap::loadFromJsonText(R"json(
  {
    "record_size": 24, "magic": "0xC3A55A01", "seq_field": "seq",
    "fields": [
      {"name": "seq", "offset": 4,  "type": "uint32"},
      {"name": "a",   "offset": 8,  "type": "int8"},
      {"name": "b",   "offset": 9,  "type": "int16"},
      {"name": "c",   "offset": 11, "type": "int32"},
      {"name": "d",   "offset": 15, "type": "float64"}
    ]
  }
  )json"));
  std::string rec(24, '\0');
  const uint32_t seq = 5;
  const int8_t a = -3;
  const int16_t b = -1234;
  const int32_t c = -100000;
  const double d = 0.1;
  std::memcpy(&rec[0], &kMagic, 4);
  std::memcpy(&rec[4], &seq, 4);
  std::memcpy(&rec[8], &a, 1);
  std::memcpy(&rec[9], &b, 2);
  std::memcpy(&rec[11], &c, 4);
  std::memcpy(&rec[15], &d, 8);
  rec += next();

  const auto samples = parser.feed(rec.data(), rec.size());
  ASSERT_EQ(samples.size(), 1u);
  EXPECT_EQ(samples[0].values[1].second, -3.0);
  EXPECT_EQ(samples[0].values[2].second, -1234.0);
  EXPECT_EQ(samples[0].values[3].second, -100000.0);
  EXPECT_DOUBLE_EQ(samples[0].values[4].second, 0.1);
}

// The usual case: the connection starts in the middle of a record.
TEST(BinaryParser, SyncsWhenStreamStartsMidRecord)
{
  BinaryParser parser(basicMap());
  const std::string stream = packStream(100, 6) + next();
  const size_t skew = 7;
  const auto samples = parser.feed(stream.data() + skew, stream.size() - skew);
  ASSERT_EQ(samples.size(), 5u);
  EXPECT_EQ(samples[0].values[0].second, 101.0);
  EXPECT_EQ(parser.discardedBytes(), 16u - skew);
}

// J-Link sends a text banner before the RTT data. It never matches the
// magic, so it is skipped without any text heuristics.
TEST(BinaryParser, SkipsJLinkBanner)
{
  BinaryParser parser(basicMap());
  const std::string banner = "SEGGER J-Link V8.68 - Real time terminal output\r\n"
                             "Process: JLinkGDBServerCLExe\r\n";
  const std::string stream = banner + packStream(1, 3) + next();
  const auto samples = parser.feed(stream.data(), stream.size());
  ASSERT_EQ(samples.size(), 3u);
  EXPECT_EQ(parser.discardedBytes(), banner.size());
}

TEST(BinaryParser, ResyncsAfterCorruptionMidStream)
{
  BinaryParser parser(basicMap());
  std::string stream = packStream(1, 3);
  stream += std::string("\x11\x22\x33", 3);
  stream += packStream(4, 3) + next();

  const auto samples = parser.feed(stream.data(), stream.size());
  // Record 3 is not followed by a magic, so it cannot be told apart from a
  // damaged record and is dropped too.
  EXPECT_EQ(samples.size(), 5u);
  EXPECT_GE(parser.resyncs(), 1u);
  EXPECT_TRUE(parser.synced());
  EXPECT_EQ(samples.back().values[0].second, 6.0);
}

// A target with a different layout (different magic) never syncs, and
// that is visible instead of turning into wrong data.
TEST(BinaryParser, NeverSyncsOnForeignMagic)
{
  BinaryParser parser(basicMap());
  std::string stream;
  for (int i = 0; i < 8; i++)
  {
    stream += packRecord(static_cast<uint32_t>(i), 0, 0, 0xC3A55A02u);
  }
  const auto samples = parser.feed(stream.data(), stream.size());
  EXPECT_TRUE(samples.empty());
  EXPECT_FALSE(parser.synced());
  EXPECT_GT(parser.discardedBytes(), 0u);
}

TEST(BinaryParser, HandlesMagicSplitAcrossFeeds)
{
  BinaryParser parser(basicMap());
  const std::string stream = packStream(1, 3) + next();
  auto samples = parser.feed(stream.data(), 2);
  EXPECT_TRUE(samples.empty());
  samples = parser.feed(stream.data() + 2, stream.size() - 2);
  EXPECT_EQ(samples.size(), 3u);
}

TEST(BinaryParserDump, DumpRecordDoesNotTriggerReset)
{
  BinaryParser parser(dumpMap());
  const std::string s = packDumpMapRecord(kMagic, 96000, 0) +
                        packDumpMapRecord(kDumpMagic, 20000, 1) +
                        packDumpMapRecord(kMagic, 96008, 0) + next();
  const auto samples = parser.feed(s.data(), s.size());
  ASSERT_EQ(samples.size(), 3u);
  EXPECT_FALSE(samples[1].target_reset);
  EXPECT_TRUE(samples[1].is_dump);
  EXPECT_DOUBLE_EQ(samples[1].t, 20000.0 / 8000.0);
  EXPECT_FALSE(samples[2].target_reset);
}

TEST(BinaryParserDump, PublishesOnlyDumpFields)
{
  BinaryParser parser(dumpMap());
  const std::string s =
      packDumpMapRecord(kDumpMagic, 20000, 1.5f) + packDumpMapRecord(kDumpMagic, 20001, 1.6f) +
      next(kDumpMagic);
  const auto samples = parser.feed(s.data(), s.size());
  ASSERT_EQ(samples.size(), 2u);
  ASSERT_EQ(samples[0].values.size(), 1u);
  EXPECT_EQ(samples[0].values[0].first, "ib");
  EXPECT_FLOAT_EQ(static_cast<float>(samples[0].values[0].second), 1.5f);
}

TEST(BinaryParserDump, SyncsOnMixedMagics)
{
  BinaryParser parser(dumpMap());
  const std::string s =
      std::string("junk") + packDumpMapRecord(kDumpMagic, 20000, 1) + packDumpMapRecord(kMagic, 96000, 0) + next();
  const auto samples = parser.feed(s.data(), s.size());
  ASSERT_EQ(samples.size(), 2u);
  EXPECT_TRUE(samples[0].is_dump);
  EXPECT_FALSE(samples[1].is_dump);
}

// seq deltas of 7 to 9 are scheduling jitter, not loss; a real hole counts.
TEST(BinaryParserDump, SeqStepToleratesJitter)
{
  BinaryParser parser(dumpMap());
  const std::string s = packDumpMapRecord(kMagic, 8, 0) + packDumpMapRecord(kMagic, 15, 0) +
                        packDumpMapRecord(kMagic, 24, 0) + packDumpMapRecord(kMagic, 32, 0);
  // the record with seq 32 is held back, so the hole shows up when 64 is
  // released by the tail below
  parser.feed(s.data(), s.size());
  EXPECT_EQ(parser.sequenceGaps(), 0u);
  const std::string hole = packDumpMapRecord(kMagic, 64, 0) + next();  // 40, 48, 56 missing
  parser.feed(hole.data(), hole.size());
  EXPECT_EQ(parser.sequenceGaps(), 3u);
}

TEST(BinaryParserDump, ResetOnlyBelowThreshold)
{
  BinaryParser parser(dumpMap());
  const std::string s = packDumpMapRecord(kMagic, 100000, 0) + packDumpMapRecord(kMagic, 90000, 0) +
                        packDumpMapRecord(kMagic, 800, 0) + next();
  const auto samples = parser.feed(s.data(), s.size());
  ASSERT_EQ(samples.size(), 3u);
  EXPECT_FALSE(samples[1].target_reset);
  EXPECT_TRUE(samples[2].target_reset);
}

TEST(BinaryParserDump, NoGapAfterDump)
{
  BinaryParser parser(dumpMap());
  const std::string s = packDumpMapRecord(kMagic, 96000, 0) + packDumpMapRecord(kDumpMagic, 20000, 0) +
                        packDumpMapRecord(kDumpMagic, 20001, 0) + packDumpMapRecord(kMagic, 105200, 0) + next();
  parser.feed(s.data(), s.size());
  EXPECT_EQ(parser.sequenceGaps(), 0u);
}

// Seen on hardware: J-Link's host buffer overflows while no client is
// connected and the stream is cut mid-record. What is left of the cut
// record can be just its magic, immediately followed by a full record.
// Without checking the next magic, that leftover decoded as a record
// shifted by 4 bytes, with the magic itself read as seq.
TEST(BinaryParser, RejectsRecordCutByJLinkBufferOverflow)
{
  for (size_t kept = 4; kept < 16; kept++)
  {
    BinaryParser parser(basicMap());
    const std::string cut = packRecord(99, 0, 0).substr(0, kept);
    const std::string stream = packStream(1, 3) + cut + packStream(500, 3) + next();
    const auto samples = parser.feed(stream.data(), stream.size());

    std::vector<double> seqs;
    for (const auto& s : samples)
    {
      seqs.push_back(s.values[0].second);
    }
    // Record 3 is intact (the leftover starts with a valid magic), only the
    // leftover itself is dropped.
    EXPECT_EQ(seqs, (std::vector<double>{ 1, 2, 3, 500, 501, 502 })) << "kept " << kept;
    EXPECT_GE(parser.malformedCount(), 1u);
  }
}

TEST(BinaryParser, HoldsNewestRecordUntilNextMagic)
{
  BinaryParser parser(basicMap());
  const std::string two = packStream(1, 2);
  auto samples = parser.feed(two.data(), two.size());
  ASSERT_EQ(samples.size(), 1u);
  const std::string tail = next();
  samples = parser.feed(tail.data(), tail.size());
  ASSERT_EQ(samples.size(), 1u);
  EXPECT_EQ(samples[0].values[0].second, 2.0);
}
