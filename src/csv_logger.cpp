/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "csv_logger.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>

void CsvLogger::setDirectory(const std::string& dir)
{
  close();
  _dir = dir;
}

void CsvLogger::close()
{
  if (_file.is_open())
  {
    _file.close();
  }
  _header.clear();
}

std::string CsvLogger::formatValue(double value)
{
  char buf[32];
  // %.10g keeps full float precision without trailing zeros
  std::snprintf(buf, sizeof(buf), "%.10g", value);
  return buf;
}

std::string CsvLogger::formatTime(double t)
{
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.6f", t);
  return buf;
}

void CsvLogger::openNewFile(const TelemetrySample& first)
{
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", std::localtime(&now));

  std::string candidate = _dir + "/telemetry_" + stamp + ".csv";
  int counter = 0;
  while (std::filesystem::exists(candidate))
  {
    candidate = _dir + "/telemetry_" + stamp + "_" + std::to_string(++counter) + ".csv";
  }

  _current_file = candidate;
  _file.open(_current_file, std::ios::trunc);

  _header.clear();
  std::string header_line = "t";
  for (const auto& [key, value] : first.values)
  {
    _header.push_back(key);
    header_line += "," + key;
  }
  _file << header_line << "\n";
}

void CsvLogger::onSample(const TelemetrySample& sample)
{
  if (_dir.empty())
  {
    return;
  }
  if (sample.target_reset)
  {
    close();
  }
  if (!_file.is_open())
  {
    // A dump record only has the dump_fields columns. Opening the file
    // with it would truncate the header for every live record after it,
    // so the header always comes from the first live record.
    if (sample.is_dump)
    {
      return;
    }
    openNewFile(sample);
  }

  std::string row = formatTime(sample.t);
  for (const auto& key : _header)
  {
    row += ",";
    for (const auto& [k, v] : sample.values)
    {
      if (k == key)
      {
        row += formatValue(v);
        break;
      }
    }
  }
  _file << row << "\n";
}
