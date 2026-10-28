/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtt_connection.h"

#include <array>
#include <chrono>
#include <thread>

#include <asio.hpp>

struct RttConnection::Impl
{
  Impl(std::string host_in, uint16_t port_in, int channel)
    : host(std::move(host_in))
    , port(port_in)
    // J-Link selects the RTT channel from this string, which has to arrive
    // within 100 ms of connecting; otherwise it serves channel 0.
    , config("$$SEGGER_TELNET_ConfigStr=RTTCh;" + std::to_string(channel) + "$$")
  {
  }

  void connect()
  {
    resolver.async_resolve(host, std::to_string(port),
                           [this](asio::error_code ec, asio::ip::tcp::resolver::results_type results) {
                             if (ec)
                             {
                               retry(ec.message());
                               return;
                             }
                             asio::async_connect(socket, results,
                                                 [this](asio::error_code ec2, const asio::ip::tcp::endpoint&) {
                                                   if (ec2)
                                                   {
                                                     retry(ec2.message());
                                                     return;
                                                   }
                                                   asio::error_code ignored;
                                                   socket.set_option(asio::ip::tcp::no_delay(true), ignored);
                                                   push({ RttEvent::Kind::Connected, {}, {} });
                                                   asio::async_write(socket, asio::buffer(config),
                                                                     [](asio::error_code, std::size_t) {});
                                                   read();
                                                 });
                           });
  }

  void read()
  {
    socket.async_read_some(asio::buffer(buffer), [this](asio::error_code ec, std::size_t n) {
      if (n > 0)
      {
        push({ RttEvent::Kind::Data, std::vector<char>(buffer.data(), buffer.data() + n), {} });
      }
      if (ec)
      {
        retry(ec == asio::error::eof ? std::string("connection closed by the server") : ec.message());
        return;
      }
      read();
    });
  }

  void retry(const std::string& why)
  {
    asio::error_code ignored;
    socket.close(ignored);
    push({ RttEvent::Kind::Disconnected, {}, why });
    timer.expires_after(std::chrono::seconds(1));
    timer.async_wait([this](asio::error_code ec) {
      if (!ec)
      {
        connect();
      }
    });
  }

  void push(RttEvent event)
  {
    std::lock_guard<std::mutex> lock(mutex);
    // Merge consecutive data chunks to keep the queue short.
    if (event.kind == RttEvent::Kind::Data && !events.empty() &&
        events.back().kind == RttEvent::Kind::Data)
    {
      auto& bytes = events.back().bytes;
      bytes.insert(bytes.end(), event.bytes.begin(), event.bytes.end());
      return;
    }
    events.push_back(std::move(event));
  }

  std::string host;
  uint16_t port;
  std::string config;

  asio::io_context io;
  asio::ip::tcp::resolver resolver{ io };
  asio::ip::tcp::socket socket{ io };
  asio::steady_timer timer{ io };
  std::array<char, 64 * 1024> buffer{};
  std::thread thread;

  std::mutex mutex;
  std::vector<RttEvent> events;
};

RttConnection::RttConnection(std::string host, uint16_t port, int channel)
  : _impl(std::make_unique<Impl>(std::move(host), port, channel))
{
  _impl->connect();
  _impl->thread = std::thread([impl = _impl.get()] { impl->io.run(); });
}

RttConnection::~RttConnection()
{
  _impl->io.stop();
  if (_impl->thread.joinable())
  {
    _impl->thread.join();
  }
}

std::vector<RttEvent> RttConnection::drain()
{
  std::vector<RttEvent> out;
  std::lock_guard<std::mutex> lock(_impl->mutex);
  out.swap(_impl->events);
  return out;
}
