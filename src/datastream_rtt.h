/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <QTcpSocket>
#include <QTimer>
#include <QtPlugin>

#include <memory>
#include <string>

#include "PlotJuggler/datastreamer_base.h"
#include "binary_parser.h"
#include "csv_logger.h"
#include "rtt_data_map.h"

// Streams binary telemetry from a SEGGER RTT up-channel into PlotJuggler.
// It connects to the RTT telnet server that J-Link software opens on
// localhost while a debug session is running, and decodes the records with
// a user-supplied JSON data map.
class DataStreamRTT : public PJ::DataStreamer
{
  Q_OBJECT
  Q_PLUGIN_METADATA(IID "facontidavide.PlotJuggler3.DataStreamer")
  Q_INTERFACES(PJ::DataStreamer)

public:
  DataStreamRTT();

  ~DataStreamRTT() override;

  bool start(QStringList*) override;

  void shutdown() override;

  bool isRunning() const override
  {
    return _running;
  }

  const char* name() const override
  {
    return "SEGGER RTT";
  }

  bool xmlSaveState(QDomDocument& doc, QDomElement& parent_element) const override;

  bool xmlLoadState(const QDomElement& parent_element) override;

private slots:
  void onConnected();
  void onReadyRead();
  void onSocketClosed();

private:
  void connectToServer();
  void pushSamples(const std::vector<TelemetrySample>& samples);
  void pushPoint(const std::string& name, double t, double value);

  QTcpSocket* _socket = nullptr;
  QTimer* _reconnect_timer = nullptr;
  std::unique_ptr<BinaryParser> _parser;
  RttDataMap _data_map;
  CsvLogger _csv_logger;
  bool _running = false;
  bool _warned_once = false;

  QString _host = "127.0.0.1";
  int _port = 19021;  // SEGGER's default RTT telnet port
  int _channel = 1;   // channel 0 is usually the text terminal
  QString _data_map_path;
  bool _csv_enabled = false;
  QString _csv_dir;
};
