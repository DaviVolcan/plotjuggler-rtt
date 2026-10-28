/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include <gtest/gtest.h>

#include "rtt_data_map.h"

namespace
{
const char* kValidMap = R"json(
{
  "version": 1,
  "record_size": 16,
  "byte_order": "little",
  "magic": "0xC3A55A01",
  "nominal_rate_hz": 1000,
  "seq_field": "seq",
  "fields": [
    {"name": "seq", "offset": 4,  "type": "uint32"},
    {"name": "ib",  "offset": 8,  "type": "float32", "unit": "A"},
    {"name": "ic",  "offset": 12, "type": "float32", "unit": "A"}
  ]
}
)json";

void expectRejected(const char* json)
{
  EXPECT_THROW(RttDataMap::loadFromJsonText(json), std::runtime_error) << json;
}
}  // namespace

TEST(RttDataMap, ParsesValidMap)
{
  const RttDataMap map = RttDataMap::loadFromJsonText(kValidMap);
  EXPECT_EQ(map.record_size, 16u);
  EXPECT_EQ(map.magic, 0xC3A55A01u);
  EXPECT_EQ(map.version, 1);
  EXPECT_EQ(map.seq_field, "seq");
  EXPECT_DOUBLE_EQ(map.nominal_rate_hz, 1000.0);
  ASSERT_EQ(map.fields.size(), 3u);
  EXPECT_EQ(map.fields[1].name, "ib");
  EXPECT_EQ(map.fields[1].offset, 8u);
  EXPECT_EQ(map.fields[1].type, RttFieldType::Float32);
  EXPECT_EQ(map.fields[1].unit, "A");
}

TEST(RttDataMap, AcceptsMagicAsNumber)
{
  const RttDataMap map = RttDataMap::loadFromJsonText(R"json(
  {"record_size": 8, "magic": 3282393601, "seq_field": "seq",
   "fields": [{"name": "seq", "offset": 4, "type": "uint32"}]}
  )json");
  EXPECT_EQ(map.magic, 0xC3A55A01u);
}

TEST(RttDataMap, ParsesAllTypes)
{
  const RttDataMap map = RttDataMap::loadFromJsonText(R"json(
  {"record_size": 32, "magic": "0x1", "seq_field": "u32",
   "fields": [
     {"name": "u8",  "offset": 4,  "type": "uint8"},
     {"name": "i8",  "offset": 5,  "type": "int8"},
     {"name": "u16", "offset": 6,  "type": "uint16"},
     {"name": "i16", "offset": 8,  "type": "int16"},
     {"name": "u32", "offset": 10, "type": "uint32"},
     {"name": "i32", "offset": 14, "type": "int32"},
     {"name": "f32", "offset": 18, "type": "float32"},
     {"name": "f64", "offset": 22, "type": "float64"}
   ]}
  )json");
  ASSERT_EQ(map.fields.size(), 8u);
  EXPECT_EQ(map.fields[7].type, RttFieldType::Float64);
}

TEST(RttDataMap, RejectsMalformedJson)
{
  expectRejected("{not valid json");
}

TEST(RttDataMap, RejectsMissingRequiredKeys)
{
  expectRejected(R"json({"magic": "0x1", "seq_field": "s", "fields": []})json");
  expectRejected(R"json({"record_size": 8, "seq_field": "s",
    "fields": [{"name": "s", "offset": 4, "type": "uint32"}]})json");
  expectRejected(R"json({"record_size": 8, "magic": "0x1",
    "fields": [{"name": "s", "offset": 4, "type": "uint32"}]})json");
}

TEST(RttDataMap, RejectsFieldPastRecordSize)
{
  expectRejected(R"json({"record_size": 8, "magic": "0x1", "seq_field": "s",
    "fields": [{"name": "s", "offset": 6, "type": "uint32"}]})json");
}

TEST(RttDataMap, RejectsFieldOverlappingMagic)
{
  expectRejected(R"json({"record_size": 8, "magic": "0x1", "seq_field": "s",
    "fields": [{"name": "s", "offset": 0, "type": "uint32"}]})json");
}

TEST(RttDataMap, RejectsRecordNotLargerThanMagic)
{
  expectRejected(R"json({"record_size": 4, "magic": "0x1", "seq_field": "s", "fields": []})json");
}

TEST(RttDataMap, RejectsUnknownSeqField)
{
  expectRejected(R"json({"record_size": 8, "magic": "0x1", "seq_field": "nope",
    "fields": [{"name": "x", "offset": 4, "type": "uint32"}]})json");
}

TEST(RttDataMap, RejectsUnknownType)
{
  expectRejected(R"json({"record_size": 8, "magic": "0x1", "seq_field": "s",
    "fields": [{"name": "s", "offset": 4, "type": "u32"}]})json");
}

TEST(RttDataMap, RejectsDuplicateFieldName)
{
  expectRejected(R"json({"record_size": 12, "magic": "0x1", "seq_field": "s",
    "fields": [{"name": "s", "offset": 4, "type": "uint32"},
               {"name": "s", "offset": 8, "type": "uint32"}]})json");
}

TEST(RttDataMap, RejectsNonArrayFields)
{
  expectRejected(R"json({"record_size": 8, "magic": "0x1", "seq_field": "s",
    "fields": "oops"})json");
}

TEST(RttDataMap, RejectsBigEndian)
{
  expectRejected(R"json({"record_size": 8, "magic": "0x1", "seq_field": "s",
    "byte_order": "big", "fields": [{"name": "s", "offset": 4, "type": "uint32"}]})json");
}

TEST(RttDataMap, RejectsMagicOutOfRange)
{
  expectRejected(R"json({"record_size": 8, "magic": "0x1FFFFFFFF", "seq_field": "s",
    "fields": [{"name": "s", "offset": 4, "type": "uint32"}]})json");
}

TEST(RttDataMap, LoadFromFileThrowsOnMissingFile)
{
  EXPECT_THROW(RttDataMap::loadFromFile("/no/such/file.json"), std::runtime_error);
}

TEST(RttDataMap, ParsesDumpKeys)
{
  const RttDataMap map = RttDataMap::loadFromJsonText(R"json(
  {
    "record_size": 12, "seq_field": "seq", "magic": "0xC3A55A01",
    "dump_magic": "0xC3A55A81", "seq_step": 8, "reset_below_seq": 80000,
    "dump_fields": ["ib"],
    "fields": [
      {"name": "seq", "offset": 4, "type": "uint32"},
      {"name": "ib",  "offset": 8, "type": "float32"}
    ]
  }
  )json");
  EXPECT_TRUE(map.has_dump_magic);
  EXPECT_EQ(map.dump_magic, 0xC3A55A81u);
  EXPECT_EQ(map.seq_step, 8);
  EXPECT_EQ(map.reset_below_seq, 80000);
  ASSERT_EQ(map.dump_fields.size(), 1u);
  EXPECT_EQ(map.dump_fields[0], "ib");
}

TEST(RttDataMap, DefaultsWithoutOptionalKeys)
{
  const RttDataMap map = RttDataMap::loadFromJsonText(kValidMap);
  EXPECT_FALSE(map.has_dump_magic);
  EXPECT_EQ(map.seq_step, 1);
  EXPECT_EQ(map.reset_below_seq, -1);
  EXPECT_TRUE(map.dump_fields.empty());
}

TEST(RttDataMap, RejectsDumpMagicEqualToMagic)
{
  expectRejected(R"json({"record_size": 8, "magic": "0x1", "dump_magic": "0x1",
    "seq_field": "s", "fields": [{"name": "s", "offset": 4, "type": "uint32"}]})json");
}

TEST(RttDataMap, RejectsUnknownDumpField)
{
  expectRejected(R"json({"record_size": 12, "magic": "0x1", "seq_field": "s",
    "dump_fields": ["missing"],
    "fields": [{"name": "s", "offset": 4, "type": "uint32"},
               {"name": "ib", "offset": 8, "type": "float32"}]})json");
}
