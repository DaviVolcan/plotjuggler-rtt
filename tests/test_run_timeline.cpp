/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include <gtest/gtest.h>

#include "run_timeline.h"

namespace
{
TelemetrySample live(double t, bool reset = false)
{
  TelemetrySample s;
  s.t = t;
  s.target_reset = reset;
  return s;
}

TelemetrySample dump(double t)
{
  TelemetrySample s;
  s.t = t;
  s.is_dump = true;
  return s;
}

constexpr int64_t kSec = 1'000'000'000;
}  // namespace

TEST(RunTimeline, FirstRunIsTargetTime)
{
  RunTimeline tl;
  EXPECT_EQ(tl.toNs(live(0.5)), kSec / 2);
  EXPECT_EQ(tl.toNs(live(2.0)), 2 * kSec);
  EXPECT_EQ(tl.runsAfterFirst(), 0);
}

TEST(RunTimeline, ResetStartsNewRunAfterPreviousOne)
{
  RunTimeline tl(1.0);
  tl.toNs(live(10.0));
  // target rebooted: seq back to near zero
  EXPECT_EQ(tl.toNs(live(0.1, true)), 11 * kSec);
  EXPECT_EQ(tl.toNs(live(0.6)), 11 * kSec + kSec / 2);
  EXPECT_EQ(tl.runsAfterFirst(), 1);
}

// A reset while disconnected is not flagged by the parser, but time going
// backwards is enough.
TEST(RunTimeline, BackwardsTimeWithoutFlagIsAlsoANewRun)
{
  RunTimeline tl(1.0);
  tl.toNs(live(5.0));
  EXPECT_EQ(tl.toNs(live(3.0)), 6 * kSec);
  EXPECT_EQ(tl.runsAfterFirst(), 1);
}

// Burst records carry past timestamps and must land inside the current
// run, without starting a new one.
TEST(RunTimeline, DumpsUseCurrentRunOffsetAndDoNotStartRuns)
{
  RunTimeline tl(1.0);
  tl.toNs(live(10.0));
  tl.toNs(live(0.2, true));  // second run starts at 11 s
  EXPECT_EQ(tl.toNs(dump(0.1)), 11 * kSec - kSec / 10);
  EXPECT_EQ(tl.toNs(live(0.3)), 11 * kSec + kSec / 10);
  EXPECT_EQ(tl.runsAfterFirst(), 1);
}

TEST(RunTimeline, ForwardJumpIsNotANewRun)
{
  RunTimeline tl;
  tl.toNs(live(1.0));
  EXPECT_EQ(tl.toNs(live(100.0)), 100 * kSec);  // e.g. J-Link dropped data
  EXPECT_EQ(tl.runsAfterFirst(), 0);
}
