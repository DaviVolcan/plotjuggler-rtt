/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtt_data_map.h"

#include <algorithm>
#include <stdexcept>

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QString>

namespace
{
constexpr size_t kMagicSize = sizeof(uint32_t);

[[noreturn]] void fail(const std::string& message)
{
  throw std::runtime_error("RTT data map: " + message);
}

// Accepts a string ("0xC3A55A01", any base understood by strtoull with
// base 0) or a JSON number.
uint32_t parseU32(const QJsonValue& value, const char* key)
{
  bool ok = false;
  quint64 parsed = 0;
  if (value.isString())
  {
    parsed = value.toString().toULongLong(&ok, 0);
  }
  else if (value.isDouble())
  {
    const double as_double = value.toDouble();
    ok = as_double >= 0.0 && as_double <= 4294967295.0;
    parsed = static_cast<quint64>(as_double);
  }
  if (!ok || parsed > 0xFFFFFFFFull)
  {
    fail(std::string("'") + key + "' must be a 32-bit unsigned value, e.g. \"0xC3A55A01\"");
  }
  return static_cast<uint32_t>(parsed);
}

bool hasField(const std::vector<RttFieldDef>& fields, const std::string& name)
{
  return std::any_of(fields.begin(), fields.end(),
                     [&](const RttFieldDef& f) { return f.name == name; });
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
  QJsonParseError parse_error;
  const QJsonDocument doc =
      QJsonDocument::fromJson(QByteArray::fromStdString(json_text), &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !doc.isObject())
  {
    fail("invalid JSON (" + parse_error.errorString().toStdString() + ")");
  }
  const QJsonObject root = doc.object();

  for (const char* key : { "record_size", "magic", "seq_field", "fields" })
  {
    if (!root.contains(key))
    {
      fail(std::string("missing required key '") + key + "'");
    }
  }

  // Fields are decoded with a plain memcpy, so the target must be
  // little-endian, like every Cortex-M.
  if (root.contains("byte_order") && root.value("byte_order").toString() != "little")
  {
    fail("only \"byte_order\": \"little\" is supported");
  }

  RttDataMap map;
  const int record_size = root.value("record_size").toInt();
  if (record_size <= static_cast<int>(kMagicSize))
  {
    fail("'record_size' must be larger than the 4-byte magic");
  }
  map.record_size = static_cast<size_t>(record_size);
  map.magic = parseU32(root.value("magic"), "magic");
  map.seq_field = root.value("seq_field").toString().toStdString();
  map.nominal_rate_hz = root.value("nominal_rate_hz").toDouble(1000.0);
  if (map.nominal_rate_hz <= 0.0)
  {
    fail("'nominal_rate_hz' must be positive");
  }
  map.version = root.value("version").toInt(0);

  map.seq_step = static_cast<int64_t>(root.value("seq_step").toDouble(1.0));
  if (map.seq_step < 1)
  {
    fail("'seq_step' must be >= 1");
  }
  map.reset_below_seq = static_cast<int64_t>(root.value("reset_below_seq").toDouble(-1.0));

  if (root.contains("dump_magic"))
  {
    map.dump_magic = parseU32(root.value("dump_magic"), "dump_magic");
    map.has_dump_magic = true;
    if (map.dump_magic == map.magic)
    {
      fail("'dump_magic' must differ from 'magic'");
    }
  }

  if (!root.value("fields").isArray())
  {
    fail("'fields' must be an array");
  }
  for (const QJsonValue& field_value : root.value("fields").toArray())
  {
    const QJsonObject obj = field_value.toObject();
    if (!obj.contains("name") || !obj.contains("offset") || !obj.contains("type"))
    {
      fail("every field needs 'name', 'offset' and 'type'");
    }
    RttFieldDef field;
    field.name = obj.value("name").toString().toStdString();
    const int offset = obj.value("offset").toInt(-1);
    if (offset < static_cast<int>(kMagicSize))
    {
      fail("field '" + field.name + "' must start at offset 4 or later (0-3 hold the magic)");
    }
    field.offset = static_cast<size_t>(offset);
    field.type = parseType(obj.value("type").toString().toStdString());
    field.unit = obj.value("unit").toString().toStdString();

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
    if (!root.value("dump_fields").isArray())
    {
      fail("'dump_fields' must be an array");
    }
    for (const QJsonValue& v : root.value("dump_fields").toArray())
    {
      const std::string name = v.toString().toStdString();
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
  QFile file(QString::fromStdString(path));
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
  {
    fail("cannot open '" + path + "'");
  }
  return loadFromJsonText(file.readAll().toStdString());
}
