/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

// End-to-end test of the built plugin: loads the shared library the way
// PlotJuggler 4 does, binds a recording host, and streams from a fake J-Link
// RTT telnet server running in this process.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <asio.hpp>
#include <nlohmann/json.hpp>
#include <pj_base/data_source_protocol.h>
#include <pj_base/plugin_data_api.h>
#include <pj_base/sdk/service_traits.hpp>
#include <pj_plugins/host/data_source_library.hpp>
#include <pj_plugins/host/service_registry_builder.hpp>

namespace
{
// ---------------------------------------------------------------- fake host
struct Row
{
  std::string topic;
  int64_t ts;
  std::map<std::string, double> values;
};

class RecordingHost
{
public:
  RecordingHost()
  {
    write_vt_.abi_version = PJ_PLUGIN_DATA_API_VERSION;
    write_vt_.struct_size = sizeof(write_vt_);
    write_vt_.ensure_topic = &ensureTopic;
    write_vt_.ensure_field = &ensureField;
    write_vt_.append_record = &appendRecord;
    write_vt_.append_bound_record = &unsupportedBound;
    write_vt_.append_arrow_stream = &unsupportedArrow;

    runtime_vt_.protocol_version = 1;
    runtime_vt_.struct_size = sizeof(runtime_vt_);
    runtime_vt_.report_message = &reportMessage;
    runtime_vt_.progress_start = &progressStart;
    runtime_vt_.progress_update = [](void*, uint64_t) noexcept { return true; };
    runtime_vt_.progress_finish = [](void*) noexcept {};
    runtime_vt_.is_stop_requested = [](void*) noexcept { return false; };
    runtime_vt_.notify_state = [](void*, PJ_data_source_state_t) noexcept {};
    runtime_vt_.request_stop = [](void*, PJ_data_source_state_t, PJ_string_view_t) noexcept {};
    runtime_vt_.ensure_parser_binding = &unsupportedBinding;
    runtime_vt_.show_message_box = [](void*, PJ_message_box_type_t, PJ_string_view_t, PJ_string_view_t,
                                      int) noexcept { return 0; };

    registry_.registerService<PJ::sdk::SourceWriteHostService>(PJ_source_write_host_t{ this, &write_vt_ });
    registry_.registerService<PJ::sdk::DataSourceRuntimeHostService>(
        PJ_data_source_runtime_host_t{ this, &runtime_vt_ });
  }

  PJ_service_registry_t registry()
  {
    return registry_.view();
  }

  std::vector<Row> rows()
  {
    std::lock_guard lock(mutex_);
    return rows_;
  }
  std::vector<std::string> messages()
  {
    std::lock_guard lock(mutex_);
    return messages_;
  }

private:
  static std::string str(PJ_string_view_t s)
  {
    return s.data ? std::string(s.data, s.size) : std::string();
  }
  static bool fail(PJ_error_t* err, const char* msg)
  {
    if (err)
    {
      std::memset(err, 0, sizeof(*err));
      err->code = 1;
      std::strncpy(err->message, msg, sizeof(err->message) - 1);
    }
    return false;
  }

  static bool ensureTopic(void* ctx, PJ_string_view_t name, PJ_topic_handle_t* out, PJ_error_t*) noexcept
  {
    auto* self = static_cast<RecordingHost*>(ctx);
    std::lock_guard lock(self->mutex_);
    const std::string n = str(name);
    for (size_t i = 0; i < self->topics_.size(); i++)
    {
      if (self->topics_[i] == n)
      {
        out->id = static_cast<uint32_t>(i + 1);
        return true;
      }
    }
    self->topics_.push_back(n);
    out->id = static_cast<uint32_t>(self->topics_.size());
    return true;
  }

  static bool ensureField(void*, PJ_topic_handle_t topic, PJ_string_view_t, PJ_primitive_type_t,
                          PJ_field_handle_t* out, PJ_error_t*) noexcept
  {
    static std::atomic<uint32_t> next{ 1 };
    out->topic = topic;
    out->id = next++;
    return true;
  }

  static double toDouble(const PJ_scalar_value_t& v)
  {
    switch (v.type)
    {
      case PJ_PRIMITIVE_TYPE_FLOAT32:
        return v.data.as_float32;
      case PJ_PRIMITIVE_TYPE_FLOAT64:
        return v.data.as_float64;
      case PJ_PRIMITIVE_TYPE_INT32:
        return v.data.as_int32;
      case PJ_PRIMITIVE_TYPE_UINT32:
        return v.data.as_uint32;
      case PJ_PRIMITIVE_TYPE_INT64:
        return static_cast<double>(v.data.as_int64);
      case PJ_PRIMITIVE_TYPE_UINT64:
        return static_cast<double>(v.data.as_uint64);
      default:
        return 0.0;
    }
  }

  static bool appendRecord(void* ctx, PJ_topic_handle_t topic, int64_t ts, const PJ_named_field_value_t* fields,
                           uint64_t count, PJ_error_t*) noexcept
  {
    auto* self = static_cast<RecordingHost*>(ctx);
    std::lock_guard lock(self->mutex_);
    Row row{ self->topics_.at(topic.id - 1), ts, {} };
    for (uint64_t i = 0; i < count; i++)
    {
      if (!fields[i].is_null)
      {
        row.values[str(fields[i].name)] = toDouble(fields[i].value);
      }
    }
    self->rows_.push_back(std::move(row));
    return true;
  }

  static bool unsupportedBound(void*, PJ_topic_handle_t, int64_t, const PJ_bound_field_value_t*, uint64_t,
                               PJ_error_t* err) noexcept
  {
    return fail(err, "not supported by test host");
  }
  static bool unsupportedArrow(void*, PJ_topic_handle_t, struct ArrowArrayStream*, PJ_string_view_t,
                               PJ_error_t* err) noexcept
  {
    return fail(err, "not supported by test host");
  }
  static bool unsupportedBinding(void*, const PJ_parser_binding_request_t*, PJ_parser_binding_handle_t*,
                                 PJ_error_t* err) noexcept
  {
    return fail(err, "not supported by test host");
  }
  static bool progressStart(void*, PJ_string_view_t, uint64_t, bool, PJ_error_t* err) noexcept
  {
    return fail(err, "not supported by test host");
  }
  static void reportMessage(void* ctx, PJ_data_source_message_level_t, PJ_string_view_t msg) noexcept
  {
    auto* self = static_cast<RecordingHost*>(ctx);
    std::lock_guard lock(self->mutex_);
    self->messages_.push_back(str(msg));
  }

  PJ_source_write_host_vtable_t write_vt_{};
  PJ_data_source_runtime_host_vtable_t runtime_vt_{};
  PJ::ServiceRegistryBuilder registry_;
  std::mutex mutex_;
  std::vector<std::string> topics_;
  std::vector<Row> rows_;
  std::vector<std::string> messages_;
};

// ------------------------------------------------------- fake J-Link server
// Record layout of examples/burst_map.json (20 bytes).
constexpr uint32_t kLive = 0xC3A55A02u;
constexpr uint32_t kDump = 0xC3A55A82u;

std::string record(uint32_t magic, uint32_t seq, float a, float b, float rpm)
{
  std::string r(20, '\0');
  std::memcpy(&r[0], &magic, 4);
  std::memcpy(&r[4], &seq, 4);
  std::memcpy(&r[8], &a, 4);
  std::memcpy(&r[12], &b, 4);
  std::memcpy(&r[16], &rpm, 4);
  return r;
}

// Live records every 8 ticks (1 kHz at 8 kHz ticks), then a 16-sample burst
// replayed from the past, more live data, then a target reset.
std::string scenario()
{
  std::string s = "SEGGER J-Link V8.68 - Real time terminal output\r\n";
  for (uint32_t tick = 8000; tick < 16000; tick += 8)  // 1.0 .. 2.0 s
  {
    s += record(kLive, tick, 1.0f, 2.0f, 1500.0f);
  }
  for (uint32_t i = 0; i < 16; i++)  // burst sampled at 1.5 s
  {
    s += record(kDump, 12000 + i, 10.0f + i, -10.0f, 0.0f);
  }
  for (uint32_t tick = 16000; tick < 24000; tick += 8)  // 2.0 .. 3.0 s
  {
    s += record(kLive, tick, 1.0f, 2.0f, 1500.0f);
  }
  for (uint32_t tick = 0; tick < 8000; tick += 8)  // reboot: 0 .. 1.0 s
  {
    s += record(kLive, tick, 3.0f, 4.0f, 0.0f);
  }
  s += record(kLive, 8000, 3.0f, 4.0f, 0.0f);  // releases the last record
  return s;
}

class FakeJLink
{
public:
  FakeJLink() : acceptor_(io_, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0))
  {
    port_ = acceptor_.local_endpoint().port();
    thread_ = std::thread([this] {
      asio::ip::tcp::socket socket(io_);
      asio::error_code ec;
      acceptor_.accept(socket, ec);
      if (ec)
      {
        return;
      }
      std::array<char, 128> buf{};
      const size_t n = socket.read_some(asio::buffer(buf), ec);
      config_ = std::string(buf.data(), n);
      const std::string data = scenario();
      asio::write(socket, asio::buffer(data), ec);
      std::this_thread::sleep_for(std::chrono::seconds(2));
    });
  }
  ~FakeJLink()
  {
    acceptor_.close();
    if (thread_.joinable())
    {
      thread_.join();
    }
  }
  uint16_t port() const
  {
    return port_;
  }
  std::string config() const
  {
    return config_;
  }

private:
  asio::io_context io_;
  asio::ip::tcp::acceptor acceptor_;
  uint16_t port_ = 0;
  std::thread thread_;
  std::string config_;
};
}  // namespace

TEST(PluginE2E, StreamsFromFakeJLink)
{
  FakeJLink server;
  RecordingHost host;

  auto lib = PJ::DataSourceLibrary::load(PJRTT_PLUGIN_PATH);
  ASSERT_TRUE(lib) << lib.error();
  auto plugin = lib->createHandle();
  ASSERT_TRUE(plugin.valid());
  ASSERT_TRUE(plugin.bind(host.registry()));

  nlohmann::json cfg;
  cfg["host"] = "127.0.0.1";
  cfg["port"] = server.port();
  cfg["channel"] = 1;
  cfg["data_map_path"] = PJRTT_BURST_MAP_PATH;
  ASSERT_TRUE(plugin.loadConfig(cfg.dump()));
  auto started = plugin.start();
  ASSERT_TRUE(started) << started.error();

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  size_t expected_rows = 1000 + 16 + 1000 + 1000;
  while (std::chrono::steady_clock::now() < deadline)
  {
    ASSERT_TRUE(plugin.poll());
    size_t data_rows = 0;
    for (const auto& r : host.rows())
    {
      data_rows += r.topic == "rtt";
    }
    if (data_rows >= expected_rows)
    {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  plugin.stop();

  EXPECT_EQ(server.config(), "$$SEGGER_TELNET_ConfigStr=RTTCh;1$$");

  std::vector<Row> live, dump, diag;
  for (const auto& r : host.rows())
  {
    if (r.topic == "rtt/_diag")
      diag.push_back(r);
    else if (r.values.count("speed_rpm"))
      live.push_back(r);
    else
      dump.push_back(r);
  }
  ASSERT_EQ(live.size(), 3000u);
  ASSERT_EQ(dump.size(), 16u);
  EXPECT_EQ(diag.size(), live.size());

  // First run: target time (seq / 8000).
  EXPECT_EQ(live.front().ts, 1'000'000'000);
  EXPECT_EQ(live[1999].ts, 2'999'000'000);

  // Burst lands at 1.5 s at 8 kHz spacing and only carries its own fields.
  EXPECT_EQ(dump.front().ts, 1'500'000'000);
  EXPECT_EQ(dump[1].ts - dump[0].ts, 125'000);
  EXPECT_EQ(dump.front().values.size(), 2u);  // only dump_fields
  EXPECT_DOUBLE_EQ(dump.back().values.at("current_a"), 25.0);

  // After the reset the new run continues one second after the old one.
  EXPECT_EQ(live[2000].ts, 2'999'000'000 + 1'000'000'000);
  for (size_t i = 1; i < live.size(); i++)
  {
    ASSERT_GT(live[i].ts, live[i - 1].ts) << "live time went backwards at " << i;
  }

  EXPECT_EQ(diag.back().values.at("lost_records"), 0.0);
  EXPECT_EQ(diag.back().values.at("malformed"), 0.0);
}
