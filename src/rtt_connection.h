/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// What the network thread hands to the plugin's poll loop.
struct RttEvent
{
  enum class Kind
  {
    Connected,
    Disconnected,
    Data,
  };
  Kind kind = Kind::Data;
  std::vector<char> bytes;  // Data only
  std::string message;      // Disconnected: why
};

// TCP client for the RTT telnet server that J-Link software opens while a
// debug session runs. Runs its own thread, reconnects every second when the
// server is not there, and never blocks the caller: events are collected
// with drain().
class RttConnection
{
public:
  RttConnection(std::string host, uint16_t port, int channel);
  ~RttConnection();

  RttConnection(const RttConnection&) = delete;
  RttConnection& operator=(const RttConnection&) = delete;

  std::vector<RttEvent> drain();

private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
};
