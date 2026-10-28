/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <string>
#include <utility>
#include <vector>

// One decoded record.
struct TelemetrySample
{
  double t = 0.0;              // seconds, from the seq field
  bool target_reset = false;   // seq went back to near zero
  bool is_dump = false;        // record carried the dump magic
  std::vector<std::pair<std::string, double>> values;
};
