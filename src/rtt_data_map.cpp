/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtt_data_map.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace
{
constexpr size_t kMagicSize = sizeof(uint32_t);

[[noreturn]] void fail(const std::string& message)
{
  throw std::runtime_error("RTT data map: " + message);
}

// Accepts a string ("0xC3A55A01", any base understood by strtoull with
// base 0) or a JSON number.
uint32_t parseU32(const json& value, const char* key)
{
  unsigned long long parsed = 0;
  bool ok = false;
  if (value.is_string())
  {
    const std::string text = value.get<std::string>();
    if (!text.empty() && text[0] != '-')
    {
      char* end = nullptr;
      errno = 0;
      parsed = std::strtoull(text.c_str(), &end, 0);
      ok = errno == 0 && end != nullptr && *end == '\0';
    }
  }
  else if (value.is_number_unsigned())
  {
    parsed = value.get<unsigned long long>();
    ok = true;
  }
  else if (value.is_number_integer())
  {
    const long long v = value.get<long long>();
    ok = v >= 0;
    parsed = static_cast<unsigned long long>(v);
  }
  if (!ok || parsed > 0xFFFFFFFFull)
  {
    fail(std::string("'") + key + "' must be a 32-bit unsigned value, e.g. \"0xC3A55A01\"");
  }
  return static_cast<uint32_t>(parsed);
}

// JSON integer, or a float with no fractional part (some tools write 8.0).
long long intValue(const json& value, const char* key)
{
  if (value.is_number_integer())
  {
    return value.get<long long>();
  }
  if (value.is_number_float())
  {
    const double d = value.get<double>();
    if (d == static_cast<double>(static_cast<long long>(d)))
    {
      return static_cast<long long>(d);
    }
  }
  fail(std::string("'") + key + "' must be an integer");
}

bool hasField(const std::vector<RttFieldDef>& fields, const std::string& name)
{
  return std::any_of(fields.begin(), fields.end(),
                     [&](const RttFieldDef& f) { return f.name == name; });
}

std::string stringValue(const json& obj, const char* key)
{
  const json& v = obj.at(key);
  if (!v.is_string())
  {
    fail(std::string("'") + key + "' must be a string");
  }
  return v.get<std::string>();
}
}  // namespace

size_t RttDataMap::typeSize(RttFieldType type)
{
  switch (type)
  {
    case RttFieldType::UInt8:
    case RttFieldType::Int8:
      return 1;
    case RttFieldType::UInt16:
    case RttFieldType::Int16:
      return 2;
    case RttFieldType::UInt32:
    case RttFieldType::Int32:
    case RttFieldType::Float32:
      return 4;
    case RttFieldType::Float64:
      return 8;
  }
  return 0;
}

RttFieldType RttDataMap::parseType(const std::string& name)
{
  if (name == "uint8")
    return RttFieldType::UInt8;
  if (name == "int8")
    return RttFieldType::Int8;
  if (name == "uint16")
    return RttFieldType::UInt16;
  if (name == "int16")
    return RttFieldType::Int16;
  if (name == "uint32")
    return RttFieldType::UInt32;
  if (name == "int32")
    return RttFieldType::Int32;
  if (name == "float32")
    return RttFieldType::Float32;
  if (name == "float64")
    return RttFieldType::Float64;
  fail("unknown type '" + name + "'");
}

RttDataMap RttDataMap::loadFromJsonText(const std::string& json_text)
{
  json root;
  try
  {
    root = json::parse(json_text);
  }
  catch (const json::parse_error& e)
  {
    fail(std::string("invalid JSON (") + e.what() + ")");
  }
  if (!root.is_object())
  {
    fail("invalid JSON (top level must be an object)");
  }

  for (const char* key : { "record_size", "magic", "seq_field", "fields" })
  {
    if (!root.contains(key))
    {
      fail(std::string("missing required key '") + key + "'");
    }
  }

  // Fields are decoded with a plain memcpy, so the target must be
  // little-endian, like every Cortex-M.
  if (root.contains("byte_order") &&
      (!root["byte_order"].is_string() || root["byte_order"].get<std::string>() != "little"))
  {
    fail("only \"byte_order\": \"little\" is supported");
  }

  RttDataMap map;
  const long long record_size = intValue(root["record_size"], "record_size");
  if (record_size <= static_cast<long long>(kMagicSize))
  {
    fail("'record_size' must be larger than the 4-byte magic");
  }
  map.record_size = static_cast<size_t>(record_size);
  map.magic = parseU32(root["magic"], "magic");
  map.seq_field = stringValue(root, "seq_field");

  if (root.contains("nominal_rate_hz"))
  {
    if (!root["nominal_rate_hz"].is_number())
    {
      fail("'nominal_rate_hz' must be a number");
    }
    map.nominal_rate_hz = root["nominal_rate_hz"].get<double>();
  }
  if (map.nominal_rate_hz <= 0.0)
  {
    fail("'nominal_rate_hz' must be positive");
  }
  if (root.contains("version"))
  {
    map.version = static_cast<int>(intValue(root["version"], "version"));
  }

  if (root.contains("seq_step"))
  {
    map.seq_step = intValue(root["seq_step"], "seq_step");
  }
  if (map.seq_step < 1)
  {
    fail("'seq_step' must be >= 1");
  }
  if (root.contains("reset_below_seq"))
  {
    map.reset_below_seq = intValue(root["reset_below_seq"], "reset_below_seq");
  }

  if (root.contains("dump_magic"))
  {
    map.dump_magic = parseU32(root["dump_magic"], "dump_magic");
    map.has_dump_magic = true;
    if (map.dump_magic == map.magic)
    {
      fail("'dump_magic' must differ from 'magic'");
    }
  }

  if (!root["fields"].is_array())
  {
    fail("'fields' must be an array");
  }
  for (const json& obj : root["fields"])
  {
    if (!obj.is_object() || !obj.contains("name") || !obj.contains("offset") ||
        !obj.contains("type"))
    {
      fail("every field needs 'name', 'offset' and 'type'");
    }
    RttFieldDef field;
    field.name = stringValue(obj, "name");
    const long long offset = obj["offset"].is_number_integer() ? obj["offset"].get<long long>() : -1;
    if (offset < static_cast<long long>(kMagicSize))
    {
      fail("field '" + field.name + "' must start at offset 4 or later (0-3 hold the magic)");
    }
    field.offset = static_cast<size_t>(offset);
    field.type = parseType(stringValue(obj, "type"));
    if (obj.contains("unit") && obj["unit"].is_string())
    {
      field.unit = obj["unit"].get<std::string>();
    }

    if (field.offset + typeSize(field.type) > map.record_size)
    {
      fail("field '" + field.name + "' extends past 'record_size'");
    }
    if (hasField(map.fields, field.name))
    {
      fail("duplicate field name '" + field.name + "'");
    }
    map.fields.push_back(std::move(field));
  }

  if (!hasField(map.fields, map.seq_field))
  {
    fail("'seq_field' \"" + map.seq_field + "\" is not one of the fields");
  }

  if (root.contains("dump_fields"))
  {
    if (!root["dump_fields"].is_array())
    {
      fail("'dump_fields' must be an array");
    }
    for (const json& v : root["dump_fields"])
    {
      const std::string name = v.is_string() ? v.get<std::string>() : std::string();
      if (!hasField(map.fields, name))
      {
        fail("dump field '" + name + "' is not one of the fields");
      }
      map.dump_fields.push_back(name);
    }
  }

  return map;
}

RttDataMap RttDataMap::loadFromFile(const std::string& path)
{
  std::ifstream file(path);
  if (!file)
  {
    fail("cannot open '" + path + "'");
  }
  std::stringstream content;
  content << file.rdbuf();
  return loadFromJsonText(content.str());
}
