/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <cmath>
#include <cstdint>

#include "telemetry_sample.h"

// Maps target time (seq / nominal_rate, seconds since the target booted)
// to the timestamp written to PlotJuggler.
//
// PlotJuggler 4 keeps every series sorted by time and has no way for a
// source to clear what it already wrote. A target reset restarts seq at
// zero, so writing target time as is would put the new run on top of the
// old one, interleaved point by point. Instead, each run after a reset is
// placed right after the previous one, `gap_s` seconds later. Inside a run
// timestamps are exactly seq / nominal_rate plus a constant, so the spacing
// between samples still comes from the target's clock.
class RunTimeline
{
public:
  explicit RunTimeline(double gap_s = 1.0) : _gap_s(gap_s)
  {
  }

  // Timestamp in nanoseconds for this sample.
  int64_t toNs(const TelemetrySample& sample)
  {
    double t = _offset_s + sample.t;
    if (!sample.is_dump)
    {
      // Any live sample that would land before the previous one means the
      // target restarted (whether or not the parser flagged it, e.g. if
      // it happened while disconnected): start a new run.
      if (_has_live && (sample.target_reset || t < _last_live_s))
      {
        _offset_s = _last_live_s + _gap_s - sample.t;
        t = _offset_s + sample.t;
        _runs++;
      }
      _last_live_s = t;
      _has_live = true;
    }
    return static_cast<int64_t>(std::llround(t * 1e9));
  }

  // Number of runs started after the first one.
  int runsAfterFirst() const
  {
    return _runs;
  }

private:
  double _gap_s;
  double _offset_s = 0.0;
  double _last_live_s = 0.0;
  bool _has_live = false;
  int _runs = 0;
};
