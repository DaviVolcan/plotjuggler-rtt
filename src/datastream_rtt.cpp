/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "datastream_rtt.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>

namespace
{
const char* kSettingsPrefix = "DataStreamRTT/";

QString key(const char* name)
{
  return QString(kSettingsPrefix) + name;
}

// Line edit plus a "..." button that opens a file or directory picker.
QHBoxLayout* pathRow(QDialog* dialog, QLineEdit* edit, bool directory, const QString& title,
                     const QString& filter = QString())
{
  auto* browse = new QPushButton("...");
  QObject::connect(browse, &QPushButton::clicked, dialog, [=]() {
    const QString path = directory ? QFileDialog::getExistingDirectory(dialog, title) :
                                     QFileDialog::getOpenFileName(dialog, title, QString(), filter);
    if (!path.isEmpty())
    {
      edit->setText(path);
    }
  });
  auto* row = new QHBoxLayout();
  row->addWidget(edit);
  row->addWidget(browse);
  return row;
}
}  // namespace

DataStreamRTT::DataStreamRTT()
{
  _socket = new QTcpSocket(this);
  _reconnect_timer = new QTimer(this);
  _reconnect_timer->setInterval(1000);
  _reconnect_timer->setSingleShot(true);

  connect(_socket, &QTcpSocket::connected, this, &DataStreamRTT::onConnected);
  connect(_socket, &QTcpSocket::readyRead, this, &DataStreamRTT::onReadyRead);
  connect(_socket, &QTcpSocket::disconnected, this, &DataStreamRTT::onSocketClosed);
  connect(_socket, &QTcpSocket::errorOccurred, this, &DataStreamRTT::onSocketClosed);
  connect(_reconnect_timer, &QTimer::timeout, this, &DataStreamRTT::connectToServer);
}

DataStreamRTT::~DataStreamRTT()
{
  shutdown();
}

bool DataStreamRTT::start(QStringList*)
{
  QSettings settings;
  QDialog dialog;
  dialog.setWindowTitle("SEGGER RTT");
  auto* layout = new QFormLayout(&dialog);

  auto* host_edit = new QLineEdit(settings.value(key("host"), _host).toString());
  auto* port_spin = new QSpinBox();
  port_spin->setRange(1, 65535);
  port_spin->setValue(settings.value(key("port"), _port).toInt());
  auto* channel_spin = new QSpinBox();
  channel_spin->setRange(0, 15);
  channel_spin->setValue(settings.value(key("channel"), _channel).toInt());
  auto* map_edit = new QLineEdit(settings.value(key("data_map_path")).toString());
  auto* csv_check = new QCheckBox("Write each run to a CSV file");
  csv_check->setChecked(settings.value(key("csv_enabled"), false).toBool());
  auto* csv_dir_edit = new QLineEdit(settings.value(key("csv_dir")).toString());

  layout->addRow("Host", host_edit);
  layout->addRow("Telnet port", port_spin);
  layout->addRow("RTT channel", channel_spin);
  layout->addRow("Data map (JSON)",
                 pathRow(&dialog, map_edit, false, "Data map", "JSON (*.json)"));
  layout->addRow(csv_check);
  layout->addRow("CSV directory", pathRow(&dialog, csv_dir_edit, true, "CSV directory"));

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addRow(buttons);

  if (dialog.exec() != QDialog::Accepted)
  {
    return false;
  }

  _host = host_edit->text();
  _port = port_spin->value();
  _channel = channel_spin->value();
  _data_map_path = map_edit->text();
  _csv_enabled = csv_check->isChecked();
  _csv_dir = csv_dir_edit->text();

  if (_csv_enabled && (_csv_dir.isEmpty() || !QDir().mkpath(_csv_dir)))
  {
    QMessageBox::warning(nullptr, "SEGGER RTT",
                         "CSV logging is enabled but the directory is empty or cannot be "
                         "created.");
    return false;
  }

  try
  {
    _data_map = RttDataMap::loadFromFile(_data_map_path.toStdString());
  }
  catch (const std::exception& e)
  {
    QMessageBox::warning(nullptr, "SEGGER RTT", QString::fromStdString(e.what()));
    return false;
  }

  settings.setValue(key("host"), _host);
  settings.setValue(key("port"), _port);
  settings.setValue(key("channel"), _channel);
  settings.setValue(key("data_map_path"), _data_map_path);
  settings.setValue(key("csv_enabled"), _csv_enabled);
  settings.setValue(key("csv_dir"), _csv_dir);

  _csv_logger.setDirectory(_csv_enabled ? _csv_dir.toStdString() : std::string());
  _running = true;
  _warned_once = false;
  connectToServer();
  return true;
}

void DataStreamRTT::shutdown()
{
  _running = false;
  _reconnect_timer->stop();
  _csv_logger.close();
  _socket->abort();
}

void DataStreamRTT::connectToServer()
{
  if (!_running)
  {
    return;
  }
  // A new connection starts mid-record and may follow a target reset:
  // start from a clean parser and a new CSV file.
  _parser = std::make_unique<BinaryParser>(_data_map);
  _csv_logger.close();
  _socket->abort();
  _socket->connectToHost(_host, static_cast<quint16>(_port));
}

void DataStreamRTT::onConnected()
{
  // J-Link selects the RTT channel from this string, which must arrive
  // within 100 ms of connecting; otherwise it serves channel 0.
  const QByteArray cfg = "$$SEGGER_TELNET_ConfigStr=RTTCh;" + QByteArray::number(_channel) + "$$";
  _socket->write(cfg);
}

void DataStreamRTT::onSocketClosed()
{
  if (!_running)
  {
    return;
  }
  _reconnect_timer->start();
  if (!_warned_once)
  {
    _warned_once = true;
    auto* box = new QMessageBox(
        QMessageBox::Information, "SEGGER RTT",
        QString("No connection to %1:%2. Is a J-Link debug session running?\n"
                "Retrying every second in the background.")
            .arg(_host)
            .arg(_port));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->show();
  }
}

void DataStreamRTT::onReadyRead()
{
  const QByteArray data = _socket->readAll();
  if (data.isEmpty())
  {
    return;
  }
  // The J-Link text banner needs no special handling: it never matches
  // the magic and is discarded while the parser looks for sync.
  const auto samples = _parser->feed(data.constData(), static_cast<size_t>(data.size()));
  if (!samples.empty())
  {
    pushSamples(samples);
  }
}

void DataStreamRTT::pushPoint(const std::string& name, double t, double value)
{
  auto it = dataMap().numeric.find(name);
  if (it == dataMap().numeric.end())
  {
    it = dataMap().addNumeric(name);
  }
  it->second.pushBack(PJ::PlotData::Point(t, value));
}

void DataStreamRTT::pushSamples(const std::vector<TelemetrySample>& samples)
{
  bool reset_seen = false;
  {
    std::lock_guard<std::mutex> lock(mutex());
    for (const auto& sample : samples)
    {
      _csv_logger.onSample(sample);
      reset_seen = reset_seen || sample.target_reset;

      for (const auto& [name, value] : sample.values)
      {
        pushPoint("rtt/" + name, sample.t, value);
      }
      // Dump records have timestamps in the past; adding counter points
      // there would make the diagnostic series go back in time.
      if (!sample.is_dump)
      {
        pushPoint("rtt/_diag/malformed", sample.t,
                       static_cast<double>(_parser->malformedCount()));
        pushPoint("rtt/_diag/lost_records", sample.t,
                       static_cast<double>(_parser->sequenceGaps()));
      }
    }
  }
  if (reset_seen)
  {
    emit clearBuffers();
  }
  emit dataReceived();
}

bool DataStreamRTT::xmlSaveState(QDomDocument& doc, QDomElement& parent_element) const
{
  QDomElement elem = doc.createElement("rtt_streamer");
  elem.setAttribute("host", _host);
  elem.setAttribute("port", _port);
  elem.setAttribute("channel", _channel);
  elem.setAttribute("data_map_path", _data_map_path);
  elem.setAttribute("csv_enabled", _csv_enabled ? 1 : 0);
  elem.setAttribute("csv_dir", _csv_dir);
  parent_element.appendChild(elem);
  return true;
}

// A loaded layout pre-fills the dialog shown by start().
bool DataStreamRTT::xmlLoadState(const QDomElement& parent_element)
{
  const QDomElement elem = parent_element.firstChildElement("rtt_streamer");
  if (elem.isNull())
  {
    return false;
  }
  QSettings settings;
  settings.setValue(key("host"), elem.attribute("host", _host));
  settings.setValue(key("port"), elem.attribute("port", QString::number(_port)).toInt());
  settings.setValue(key("channel"), elem.attribute("channel", QString::number(_channel)).toInt());
  settings.setValue(key("data_map_path"), elem.attribute("data_map_path"));
  settings.setValue(key("csv_enabled"), elem.attribute("csv_enabled", "0").toInt() != 0);
  settings.setValue(key("csv_dir"), elem.attribute("csv_dir"));
  return true;
}
