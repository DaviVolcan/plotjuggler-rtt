/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <fstream>
#include <string>
#include <vector>

#include "telemetry_sample.h"

// Writes every connection (and every target reset) to its own CSV file,
// so each test run ends up in a separate file without touching the UI.
class CsvLogger
{
public:
  // Empty directory disables logging.
  void setDirectory(const std::string& dir);

  void onSample(const TelemetrySample& sample);

  void close();

  const std::string& currentFile() const
  {
    return _current_file;
  }

private:
  void openNewFile(const TelemetrySample& first);
  static std::string formatValue(double value);
  static std::string formatTime(double t);

  std::string _dir;
  std::string _current_file;
  std::ofstream _file;
  std::vector<std::string> _header;
};
