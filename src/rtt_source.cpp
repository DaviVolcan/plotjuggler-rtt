/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

// PlotJuggler 4 data source: streams packed binary records from a SEGGER
// RTT up-channel, through the J-Link telnet server, and decodes them with a
// JSON data map. See README.md and docs/data-map.md.

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <pj_base/sdk/data_source_patterns.hpp>
#include <pj_base/sdk/text_utils.hpp>
#include <pj_plugins/sdk/dialog_plugin_typed.hpp>
#include <pj_plugins/sdk/widget_data.hpp>

#include "binary_parser.h"
#include "csv_logger.h"
#include "rtt_connection.h"
#include "rtt_data_map.h"
#include "rtt_dialog_ui.hpp"
#include "rtt_manifest.hpp"
#include "run_timeline.h"

namespace
{
constexpr const char* kDefaultHost = "127.0.0.1";
constexpr int kDefaultPort = 19021;  // SEGGER's default RTT telnet port
constexpr int kDefaultChannel = 1;   // channel 0 is usually the text terminal

constexpr const char* kTopic = "rtt";
constexpr const char* kDiagTopic = "rtt/_diag";

struct RttConfig
{
  std::string host = kDefaultHost;
  int port = kDefaultPort;
  int channel = kDefaultChannel;
  std::string map_path;
  bool csv_enabled = false;
  std::string csv_dir;

  std::string toJson() const
  {
    nlohmann::json j;
    j["host"] = host;
    j["port"] = port;
    j["channel"] = channel;
    j["data_map_path"] = map_path;
    j["csv_enabled"] = csv_enabled;
    j["csv_dir"] = csv_dir;
    return j.dump();
  }

  bool fromJson(std::string_view text)
  {
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object())
    {
      return false;
    }
    host = j.value("host", std::string(kDefaultHost));
    port = j.value("port", kDefaultPort);
    channel = j.value("channel", kDefaultChannel);
    map_path = j.value("data_map_path", std::string());
    csv_enabled = j.value("csv_enabled", false);
    csv_dir = j.value("csv_dir", std::string());
    return true;
  }
};

class RttDialog : public PJ::DialogPluginTyped
{
public:
  RttConfig config;

  std::string manifest() const override
  {
    return kRttManifest;
  }

  std::string ui_content() const override
  {
    return kRttDialogUi;
  }

  std::string widget_data() override
  {
    PJ::WidgetData wd;
    wd.setText("lineEditHost", config.host);
    wd.setText("lineEditPort", std::to_string(config.port));
    wd.setRange("spinBoxChannel", 0, 15);
    wd.setValue("spinBoxChannel", config.channel);
    wd.setText("lineEditMap", config.map_path);
    wd.setFilePicker("buttonMap", "...", "JSON (*.json)", "Select data map");
    wd.setChecked("checkBoxCsv", config.csv_enabled);
    wd.setText("lineEditCsvDir", config.csv_dir);
    wd.setFolderPicker("buttonCsvDir", "...", "Select CSV directory");
    wd.setEnabled("lineEditCsvDir", config.csv_enabled);
    wd.setEnabled("buttonCsvDir", config.csv_enabled);
    wd.setOkEnabled(!config.map_path.empty() && !config.host.empty() &&
                    (!config.csv_enabled || !config.csv_dir.empty()));
    return wd.toJson();
  }

  bool onTextChanged(std::string_view widget, std::string_view text) override
  {
    if (widget == "lineEditHost")
    {
      config.host = std::string(text);
      return true;
    }
    if (widget == "lineEditPort")
    {
      if (const auto port = PJ::sdk::parsePort(text))
      {
        config.port = *port;
      }
      return false;
    }
    if (widget == "lineEditMap")
    {
      config.map_path = std::string(text);
      return true;
    }
    if (widget == "lineEditCsvDir")
    {
      config.csv_dir = std::string(text);
      return true;
    }
    return false;
  }

  bool onValueChanged(std::string_view widget, int value) override
  {
    if (widget == "spinBoxChannel")
    {
      config.channel = value;
    }
    return false;
  }

  bool onToggled(std::string_view widget, bool checked) override
  {
    if (widget == "checkBoxCsv")
    {
      config.csv_enabled = checked;
      return true;
    }
    return false;
  }

  bool onFileSelected(std::string_view widget, std::string_view path) override
  {
    if (widget == "buttonMap")
    {
      config.map_path = std::string(path);
      return true;
    }
    return false;
  }

  bool onFolderSelected(std::string_view widget, std::string_view path) override
  {
    if (widget == "buttonCsvDir")
    {
      config.csv_dir = std::string(path);
      return true;
    }
    return false;
  }

  void onAccepted(std::string_view) override
  {
  }
  void onRejected() override
  {
  }

  std::string saveConfig() const override
  {
    return config.toJson();
  }

  bool loadConfig(std::string_view json) override
  {
    return config.fromJson(json);
  }
};

class RttSource : public PJ::StreamSourceBase
{
public:
  PJ_borrowed_dialog_t getDialog() override
  {
    return PJ::borrowDialog(_dialog);
  }

  uint64_t extraCapabilities() const override
  {
    return PJ::kCapabilityDirectIngest | PJ::kCapabilityHasDialog;
  }

  std::string saveConfig() const override
  {
    return _dialog.saveConfig();
  }

  PJ::Status loadConfig(std::string_view json) override
  {
    if (!json.empty())
    {
      (void)_dialog.loadConfig(json);
    }
    return PJ::okStatus();
  }

  PJ::Status onStart() override
  {
    const RttConfig& cfg = _dialog.config;
    try
    {
      _map = RttDataMap::loadFromFile(cfg.map_path);
    }
    catch (const std::exception& e)
    {
      return PJ::unexpected(std::string(e.what()));
    }

    if (cfg.csv_enabled)
    {
      std::error_code ec;
      std::filesystem::create_directories(cfg.csv_dir, ec);
      if (cfg.csv_dir.empty() || !std::filesystem::is_directory(cfg.csv_dir))
      {
        return PJ::unexpected("CSV directory '" + cfg.csv_dir + "' does not exist and cannot be created");
      }
    }
    _csv.setDirectory(cfg.csv_enabled ? cfg.csv_dir : std::string());

    auto topic = writeHost().ensureTopic(kTopic);
    if (!topic)
    {
      return PJ::unexpected(topic.error());
    }
    _topic = *topic;
    for (const auto& field : _map.fields)
    {
      // Declared up front: the host does not accept new columns once rows
      // exist, and dump records only fill some of them.
      if (auto f = writeHost().ensureField(_topic, field.name, PJ::PrimitiveType::kFloat64); !f)
      {
        return PJ::unexpected(f.error());
      }
    }
    auto diag = writeHost().ensureTopic(kDiagTopic);
    if (!diag)
    {
      return PJ::unexpected(diag.error());
    }
    _diag_topic = *diag;
    for (const char* name : { "lost_records", "malformed" })
    {
      if (auto f = writeHost().ensureField(_diag_topic, name, PJ::PrimitiveType::kFloat64); !f)
      {
        return PJ::unexpected(f.error());
      }
    }

    _parser = std::make_unique<BinaryParser>(_map);
    _timeline = RunTimeline();
    _warned_disconnected = false;
    _warned_busy = false;
    _connection = std::make_unique<RttConnection>(cfg.host, static_cast<uint16_t>(cfg.port), cfg.channel);
    return PJ::okStatus();
  }

  PJ::Status onPoll() override
  {
    if (!_connection)
    {
      return PJ::okStatus();
    }
    for (auto& event : _connection->drain())
    {
      switch (event.kind)
      {
        case RttEvent::Kind::Connected:
          _parser->resync();
          _csv.close();
          _warned_disconnected = false;
          runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kInfo,
                                      "Connected to " + endpoint() + ", RTT channel " +
                                          std::to_string(_dialog.config.channel));
          break;

        case RttEvent::Kind::Disconnected:
          if (!_warned_disconnected)
          {
            _warned_disconnected = true;
            runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning,
                                        "No connection to " + endpoint() + " (" + event.message +
                                            "). Is a J-Link debug session running? Retrying every second.");
          }
          break;

        case RttEvent::Kind::Data:
          checkBusyBanner(event.bytes);
          if (auto status = ingest(event.bytes); !status)
          {
            return status;
          }
          break;
      }
    }
    return PJ::okStatus();
  }

  void onStop() override
  {
    _connection.reset();
    _csv.close();
  }

private:
  std::string endpoint() const
  {
    return _dialog.config.host + ":" + std::to_string(_dialog.config.port);
  }

  // J-Link serves one telnet client per port and tells the second one so in
  // plain text before closing the connection.
  void checkBusyBanner(const std::vector<char>& bytes)
  {
    static const std::string kBusy = "already is an active connection";
    if (_warned_busy || _parser->synced())
    {
      return;
    }
    if (std::search(bytes.begin(), bytes.end(), kBusy.begin(), kBusy.end()) != bytes.end())
    {
      _warned_busy = true;
      runtimeHost().reportMessage(PJ::DataSourceMessageLevel::kWarning,
                                  "J-Link refused the connection: another program (RTT Viewer, another "
                                  "PlotJuggler) is already connected to " +
                                      endpoint() + ".");
    }
  }

  PJ::Status ingest(const std::vector<char>& bytes)
  {
    const auto samples = _parser->feed(bytes.data(), bytes.size());
    std::vector<PJ::sdk::NamedFieldValue> row;
    for (const auto& sample : samples)
    {
      _csv.onSample(sample);
      const PJ::Timestamp ts = _timeline.toNs(sample);

      row.clear();
      for (const auto& [name, value] : sample.values)
      {
        row.push_back({ .name = name, .value = value });
      }
      if (auto st = writeHost().appendRecord(_topic, ts, PJ::Span<const PJ::sdk::NamedFieldValue>(row));
          !st)
      {
        return PJ::unexpected(st.error());
      }

      if (!sample.is_dump)
      {
        auto st = writeHost().appendRecord(
            _diag_topic, ts,
            { { .name = "lost_records", .value = static_cast<double>(_parser->sequenceGaps()) },
              { .name = "malformed", .value = static_cast<double>(_parser->malformedCount()) } });
        if (!st)
        {
          return PJ::unexpected(st.error());
        }
      }
    }
    return PJ::okStatus();
  }

  RttDialog _dialog;
  RttDataMap _map;
  std::unique_ptr<BinaryParser> _parser;
  std::unique_ptr<RttConnection> _connection;
  RunTimeline _timeline;
  CsvLogger _csv;
  PJ::sdk::TopicHandle _topic{};
  PJ::sdk::TopicHandle _diag_topic{};
  bool _warned_disconnected = false;
  bool _warned_busy = false;
};

}  // namespace

PJ_DATA_SOURCE_PLUGIN(RttSource, kRttManifest)

PJ_DIALOG_PLUGIN(RttDialog, kRttManifest)
