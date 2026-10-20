#include "ui/main_window.h"

#include <QAction>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <cstring>
#include <filesystem>

#include "core/bridge_model.h"
#include "core/bridge_store.h"
#include "engine/control_server.h"
#include "shell/process_launcher.h"
#include "shell/supervisor.h"
#include "ui/control_client.h"
#include "ui/signal_pill.h"

namespace ob {
namespace {

void setupTable(QTableWidget* table) {
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->setSelectionMode(QAbstractItemView::NoSelection);
  table->setFocusPolicy(Qt::NoFocus);
  table->setShowGrid(false);
  table->setAlternatingRowColors(true);
  table->verticalHeader()->setVisible(false);
  table->verticalHeader()->setDefaultSectionSize(34);
  table->horizontalHeader()->setHighlightSections(false);
}

QString formatOf(const QJsonObject& source) {
  const int width = source.value("width").toInt();
  const int height = source.value("height").toInt();
  if (width <= 0 || height <= 0) return QStringLiteral("—");
  const int n = source.value("fpsN").toInt(60);
  const int d = source.value("fpsD").toInt(1);
  const double fps = d > 0 ? static_cast<double>(n) / d : 0.0;
  const QString alpha = source.value("alpha").toBool() ? QStringLiteral("A") : QString();
  return QStringLiteral("%1%2×%3 · %4p")
      .arg(width)
      .arg(alpha)
      .arg(height)
      .arg(fps, 0, 'g', 4);
}

QString rateText(const QJsonObject& output) {
  const double fps = output.value("fps").toDouble();
  const double mbps = output.value("mbps").toDouble();
  const int connections = output.value("connections").toInt();
  QString text = QStringLiteral("%1 fps · %2 Mbps").arg(fps, 0, 'f', 1).arg(mbps, 0, 'f', 1);
  if (connections > 0) text += QStringLiteral(" · %1 conn").arg(connections);
  return text;
}

int rowOf(const QStringList& ids, const QString& id) {
  for (int i = 0; i < ids.size(); ++i)
    if (ids[i] == id) return i;
  return -1;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  QString base = qEnvironmentVariable("LOCALAPPDATA");
  if (base.isEmpty()) base = QDir::homePath() + "/AppData/Local";
  base += "/OMTBridge";
  std::error_code ec;
  std::filesystem::create_directories(base.toStdWString(), ec);
  bridgePath_ = base + "/bridge.json";
  discoveryPath_ = base + "/engine.json";

  client_ = new ControlClient(this);
  connect(client_, &ControlClient::message, this, &MainWindow::onMessage);
  connect(client_, &ControlClient::binaryMessage, this, &MainWindow::onBinaryMessage);
  connect(client_, &ControlClient::connectedChanged, this, [this] {
    connected_ = client_->connected();
    if (connected_) {
      clearError();
      requestState();
    } else {
      running_ = false;
    }
    updatePrimary();
    updateStatus();
  });

  buildUi();

  supervisorTimer_ = new QTimer(this);
  supervisorTimer_->setInterval(250);
  connect(supervisorTimer_, &QTimer::timeout, this, [this] {
    if (supervisor_) {
      const auto now = Supervisor::Clock::now();
      supervisor_->poll(now);
      if (!connected_ && launcher_ && launcher_->isRunning()) {
        const qint64 ms = QDateTime::currentMSecsSinceEpoch();
        if (ms - lastConnectAttempt_ > 900) {
          lastConnectAttempt_ = ms;
          connectClient();
        }
      }
    }
    updatePrimary();
    updateStatus();
  });

  pollTimer_ = new QTimer(this);
  pollTimer_->setInterval(1000);
  connect(pollTimer_, &QTimer::timeout, this, [this] { requestState(); });
  pollTimer_->start();

  startOrAdopt();
  const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
  resize(qMin(1680, available.width() - 40), qMin(840, available.height() - 60));
  move(available.center() - rect().center());
}

MainWindow::~MainWindow() {
  if (pollTimer_) pollTimer_->stop();
  if (supervisorTimer_) supervisorTimer_->stop();
  if (supervisor_) supervisor_->stopSupervising();
  if (client_) client_->disconnect();
}

void MainWindow::closeEvent(QCloseEvent* event) {
  // Closing the window leaves the engine bridging (design 6.1); the discovery file lets the next
  // window adopt it. Deliberately no kill().
  if (pollTimer_) pollTimer_->stop();
  if (supervisorTimer_) supervisorTimer_->stop();
  if (supervisor_) supervisor_->stopSupervising();
  if (client_) client_->disconnect();
  QMainWindow::closeEvent(event);
}

void MainWindow::buildUi() {
  auto* central = new QWidget(this);
  auto* root = new QVBoxLayout(central);
  root->setContentsMargins(14, 10, 14, 10);
  root->setSpacing(8);

  error_ = new QLabel(central);
  error_->setObjectName("errorBlock");
  error_->setWordWrap(true);
  error_->hide();
  root->addWidget(error_);

  pages_ = new QStackedWidget(central);

  auto* split = new QSplitter(Qt::Horizontal, pages_);
  auto* sourcesBox = new QGroupBox(QStringLiteral("Sources"), split);
  auto* sourcesLayout = new QVBoxLayout(sourcesBox);
  sourcesError_ = new QLabel(sourcesBox);
  sourcesError_->setObjectName("errorBlock");
  sourcesError_->setWordWrap(true);
  sourcesError_->hide();
  sourcesLayout->addWidget(sourcesError_);
  sources_ = new QTableWidget(0, 8, sourcesBox);
  sources_->setHorizontalHeaderLabels(
      {QStringLiteral("Preview"), QStringLiteral("Source"), QStringLiteral("Kind"),
       QStringLiteral("Format"), QStringLiteral("Codec"), QStringLiteral("Tally"),
       QStringLiteral("State"), QString()});
  setupTable(sources_);
  sources_->setWordWrap(false);
  sources_->setTextElideMode(Qt::ElideRight);
  sources_->verticalHeader()->setDefaultSectionSize(56);
  sources_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  sources_->horizontalHeader()->setMinimumSectionSize(54);
  sources_->setColumnWidth(0, 96);
  sources_->setColumnWidth(2, 58);
  sources_->setColumnWidth(3, 112);
  sources_->setColumnWidth(4, 58);
  sources_->setColumnWidth(5, 54);
  sources_->setColumnWidth(6, 98);
  sources_->setColumnWidth(7, 68);
  sourcesLayout->addWidget(sources_);

  auto* outputsBox = new QGroupBox(QStringLiteral("Outputs"), split);
  auto* outputsLayout = new QVBoxLayout(outputsBox);
  outputsError_ = new QLabel(outputsBox);
  outputsError_->setObjectName("errorBlock");
  outputsError_->setWordWrap(true);
  outputsError_->hide();
  outputsLayout->addWidget(outputsError_);
  outputs_ = new QTableWidget(0, 10, outputsBox);
  outputs_->setHorizontalHeaderLabels(
      {QStringLiteral("Name"), QStringLiteral("Source"), QStringLiteral("Target"),
       QStringLiteral("Codec"), QStringLiteral("Quality"), QStringLiteral("Alpha"),
       QStringLiteral("State"), QStringLiteral("Rate"), QStringLiteral("Note"), QString()});
  setupTable(outputs_);
  outputs_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  outputs_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  outputs_->horizontalHeader()->setMinimumSectionSize(56);
  outputs_->setColumnWidth(2, 92);
  outputs_->setColumnWidth(3, 72);
  outputs_->setColumnWidth(4, 76);
  outputs_->setColumnWidth(5, 64);
  outputs_->setColumnWidth(6, 102);
  outputs_->setColumnWidth(7, 158);
  outputs_->setColumnWidth(8, 140);
  outputs_->setColumnWidth(9, 74);
  outputsLayout->addWidget(outputs_);

  split->addWidget(sourcesBox);
  split->addWidget(outputsBox);
  split->setStretchFactor(0, 2);
  split->setStretchFactor(1, 4);
  split->setSizes({640, 980});
  pages_->addWidget(split);

  auto* matrixPage = new QWidget(pages_);
  auto* matrixLayout = new QVBoxLayout(matrixPage);
  auto* hint = new QLabel(QStringLiteral("Matrix: rows are sources, columns are outputs. "
                                         "Assignments are edited in the Outputs pane."),
                          matrixPage);
  hint->setObjectName("muted");
  matrixLayout->addWidget(hint);
  matrix_ = new QTableWidget(0, 0, matrixPage);
  setupTable(matrix_);
  matrix_->setShowGrid(true);
  matrix_->verticalHeader()->setVisible(true);  // the row label IS the source name here
  matrix_->verticalHeader()->setDefaultSectionSize(30);
  matrixLayout->addWidget(matrix_);
  pages_->addWidget(matrixPage);

  root->addWidget(pages_, 1);
  setCentralWidget(central);

  auto* toolbar = addToolBar(QStringLiteral("Main"));
  toolbar->setMovable(false);
  toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
  auto* addSource = toolbar->addAction(QStringLiteral("Add source"));
  connect(addSource, &QAction::triggered, this, &MainWindow::onAddSource);
  auto* addOutput = toolbar->addAction(QStringLiteral("Add output"));
  connect(addOutput, &QAction::triggered, this, &MainWindow::onAddOutput);
  toolbar->addSeparator();
  matrixAction_ = toolbar->addAction(QStringLiteral("Matrix"));
  matrixAction_->setCheckable(true);
  connect(matrixAction_, &QAction::toggled, this,
          [this](bool on) { pages_->setCurrentIndex(on ? 1 : 0); });
  auto* settings = toolbar->addAction(QStringLiteral("Settings"));
  connect(settings, &QAction::triggered, this, &MainWindow::openSettings);

  auto* spacer = new QWidget(toolbar);
  spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  toolbar->addWidget(spacer);
  primary_ = new QPushButton(QStringLiteral("Start"), toolbar);
  primary_->setObjectName("primary");
  connect(primary_, &QPushButton::clicked, this, &MainWindow::onPrimary);
  toolbar->addWidget(primary_);

  auto* file = menuBar()->addMenu(QStringLiteral("&File"));
  file->addAction(QStringLiteral("&Save bridge"), QKeySequence::Save, this,
                  &MainWindow::saveBridgeNow);
  file->addSeparator();
  file->addAction(QStringLiteral("E&xit"), QKeySequence(QStringLiteral("Ctrl+Q")), this,
                  &QWidget::close);
  auto* tools = menuBar()->addMenu(QStringLiteral("&Tools"));
  tools->addAction(QStringLiteral("&Settings…"), this, &MainWindow::openSettings);

  engineLabel_ = new QLabel(QStringLiteral("Engine: stopped"), this);
  rateLabel_ = new QLabel(QStringLiteral("out 0.0 Mbps"), this);
  rateLabel_->setObjectName("mono");
  statusBar()->addWidget(engineLabel_);
  statusBar()->addPermanentWidget(rateLabel_);

  updatePrimary();
  updateStatus();
}

QString MainWindow::exePath() const {
  return QCoreApplication::applicationDirPath() + QStringLiteral("/omt-bridge-engine.exe");
}

QString MainWindow::engineArgs() const {
  return QStringLiteral("--port %1 --token %2 --discovery \"%3\" --bridge \"%4\"")
      .arg(port_)
      .arg(token_)
      .arg(QDir::toNativeSeparators(discoveryPath_), QDir::toNativeSeparators(bridgePath_));
}

void MainWindow::startOrAdopt() {
  const auto found = ob::readDiscovery(discoveryPath_.toStdWString());
  if (found && ob::isEngineProcess(found->pid)) {
    port_ = found->port;
    token_ = QString::fromStdString(found->token);
    launcher_ = std::make_unique<ProcessLauncher>(exePath().toStdWString(),
                                                  engineArgs().toStdWString());
    launcher_->adopt(found->pid);
    supervisor_ = std::make_unique<Supervisor>(*launcher_);
    supervisorTimer_->start();
    connectClient();
    updateStatus();
    return;
  }
  std::error_code ignored;
  std::filesystem::remove(discoveryPath_.toStdWString(), ignored);

  bool autoStart = false;
  if (const auto saved = loadBridge(bridgePath_.toStdWString()))
    autoStart = saved->settings.startEngineWithApp;
  if (autoStart) launchEngine();
  updatePrimary();
  updateStatus();
}

void MainWindow::launchEngine() {
  if (!QFile::exists(exePath())) {
    showError(QStringLiteral("omt-bridge-engine.exe was not found beside the app."));
    return;
  }
  if (token_.isEmpty()) {
    try {
      token_ = QString::fromStdString(ob::randomToken());
    } catch (...) {
      showError(QStringLiteral("Could not generate a control token."));
      return;
    }
  }
  if (!launcher_) {
    launcher_ = std::make_unique<ProcessLauncher>(exePath().toStdWString(),
                                                  engineArgs().toStdWString());
    supervisor_ = std::make_unique<Supervisor>(*launcher_);
  }
  if (!launcher_->isRunning()) launcher_->launch();
  supervisorTimer_->start();
  lastConnectAttempt_ = QDateTime::currentMSecsSinceEpoch();
  QTimer::singleShot(350, this, [this] { connectClient(); });
  updatePrimary();
  updateStatus();
}

void MainWindow::connectClient() {
  if (token_.isEmpty() || !launcher_ || !launcher_->isRunning()) return;
  client_->connectTo(
      QStringLiteral("ws://127.0.0.1:%1/?token=%2").arg(port_).arg(token_));
}

void MainWindow::send(const QJsonObject& command) { client_->send(command); }

void MainWindow::requestState() { send(QJsonObject{{QStringLiteral("cmd"), "state"}}); }

void MainWindow::onMessage(const QJsonObject& reply) {
  if (supervisor_) supervisor_->heartbeat(Supervisor::Clock::now());

  if (discoveryHandler_ && reply.contains("omt") && reply.contains("ndi")) {
    discoveryHandler_(reply);
    return;
  }
  if (!reply.value("ok").toBool()) {
    showError(reply.value("error").toString(QStringLiteral("The engine refused the command.")));
    return;
  }
  clearError();
  if (reply.contains("sources") && reply.contains("outputs")) {
    applyState(reply);
    return;
  }
  if (reply.contains("running")) running_ = reply.value("running").toBool();
  dirty_ = true;  // the model changed; the next snapshot is saved
  requestState();
  updatePrimary();
}

void MainWindow::onBinaryMessage(const QByteArray& payload) {
  if (payload.size() < 6 || std::memcmp(payload.constData(), "OBT1", 4) != 0) return;
  const int headerSize = static_cast<unsigned char>(payload[4]) |
                         (static_cast<unsigned char>(payload[5]) << 8);
  if (payload.size() < 6 + headerSize) return;
  const QJsonDocument doc = QJsonDocument::fromJson(payload.mid(6, headerSize));
  if (!doc.isObject()) return;
  const QJsonObject header = doc.object();
  const QString id = header.value("sourceId").toString();
  const int width = header.value("width").toInt();
  const int height = header.value("height").toInt();
  const qsizetype pixels = static_cast<qsizetype>(width) * height * 4;
  if (id.isEmpty() || width < 2 || height < 2 || payload.size() != 6 + headerSize + pixels)
    return;

  const QImage image(reinterpret_cast<const uchar*>(payload.constData() + 6 + headerSize),
                     width, height, width * 4, QImage::Format_ARGB32);
  thumbnails_.insert(id, image.copy());

  const int row = sourceIds_.indexOf(id);
  if (row < 0) return;  // the next state reply paints it from thumbnails_
  if (auto* label = qobject_cast<QLabel*>(sources_->cellWidget(row, 0))) {
    label->setPixmap(
        QPixmap::fromImage(image).scaled(92, 52, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    label->setText(QString());
  }
}

void MainWindow::applyState(const QJsonObject& state) {
  snapshot_ = state;
  running_ = state.value("running").toBool(false);
  const QString name = state.value("name").toString();
  setWindowTitle(name.isEmpty() ? QStringLiteral("OMT Bridge")
                                : QStringLiteral("OMT Bridge — %1").arg(name));

  QStringList sids;
  for (const QJsonValue& v : state.value("sources").toArray())
    sids << v.toObject().value("id").toString();
  sourceIds_ = sids;
  for (auto it = thumbnails_.begin(); it != thumbnails_.end();)
    it = sids.contains(it.key()) ? std::next(it) : thumbnails_.erase(it);
  rebuildSources();

  QStringList oids;
  for (const QJsonValue& v : state.value("outputs").toArray())
    oids << v.toObject().value("id").toString();
  if (oids != outputIds_ || sids != comboSourceIds_) {
    outputIds_ = oids;
    comboSourceIds_ = sids;
    rebuildOutputs();
  } else {
    updateOutputRuntime();
  }

  rebuildMatrix();
  updateBands();
  if (dirty_) saveBridgeNow();
  updatePrimary();
  updateStatus();
}

void MainWindow::rebuildSources() {
  const QJsonArray sources = snapshot_.value("sources").toArray();
  sources_->setRowCount(sources.size());
  for (int r = 0; r < sources.size(); ++r) {
    const QJsonObject s = sources[r].toObject();
    const QString id = s.value("id").toString();
    const QString address = s.value("address").toString();

    auto* thumb = new QLabel(sources_);
    thumb->setFixedSize(92, 52);
    thumb->setAlignment(Qt::AlignCenter);
    thumb->setStyleSheet(QStringLiteral(
        "background:#0B0E11;border:1px solid #26323D;border-radius:3px;color:#46525E;"));
    const auto image = thumbnails_.constFind(id);
    if (image != thumbnails_.constEnd() && !image->isNull()) {
      thumb->setPixmap(QPixmap::fromImage(*image).scaled(92, 52, Qt::KeepAspectRatio,
                                                         Qt::SmoothTransformation));
    } else {
      thumb->setText(QStringLiteral("—"));
    }
    sources_->setCellWidget(r, 0, thumb);

    auto* name = new QTableWidgetItem(address);
    name->setToolTip(address);
    sources_->setItem(r, 1, name);
    sources_->setItem(r, 2, new QTableWidgetItem(s.value("kind").toString()));
    auto* formatItem = new QTableWidgetItem(formatOf(s));
    bool hasOutput = false;
    for (const QJsonValue& ov : snapshot_.value("outputs").toArray())
      if (ov.toObject().value("sourceId").toString() == id) {
        hasOutput = true;
        break;
      }
    if (s.value("kind").toString() == QStringLiteral("NDI") && !hasOutput)
      formatItem->setToolTip(QStringLiteral(
          "NDI low-bandwidth preview; an output opens the full-resolution stream."));
    sources_->setItem(r, 3, formatItem);
    sources_->setItem(
        r, 4, new QTableWidgetItem(s.value("codec").toString(QStringLiteral("—"))));

    const bool program = s.value("program").toBool();
    const bool preview = s.value("preview").toBool();
    auto* tally = new QTableWidgetItem(
        program ? QStringLiteral("PGM") : (preview ? QStringLiteral("PVW") : QStringLiteral("—")));
    tally->setTextAlignment(Qt::AlignCenter);
    if (program)
      tally->setForeground(QColor("#EF4444"));
    else if (preview)
      tally->setForeground(QColor("#FBBF24"));
    sources_->setItem(r, 5, tally);

    auto* pill = new SignalPill(sources_);
    pill->setState(s.value("state").toString(QStringLiteral("ready")));
    sources_->setCellWidget(r, 6, pill);

    auto* remove = new QPushButton(QStringLiteral("Remove"), sources_);
    remove->setObjectName("danger");
    connect(remove, &QPushButton::clicked, this,
            [this, id, address] { onRemoveSource(id, address); });
    sources_->setCellWidget(r, 7, remove);
  }
}

void MainWindow::rebuildOutputs() {
  const QJsonArray outputs = snapshot_.value("outputs").toArray();
  const QJsonArray sources = snapshot_.value("sources").toArray();
  outputs_->setRowCount(outputs.size());

  for (int r = 0; r < outputs.size(); ++r) {
    const QJsonObject o = outputs[r].toObject();
    const QString id = o.value("id").toString();

    auto* name = new QLineEdit(o.value("name").toString(), outputs_);
    name->setPlaceholderText(QStringLiteral("Output name"));

    auto* source = new QComboBox(outputs_);
    source->addItem(QStringLiteral("(none)"), QString());
    for (const QJsonValue& v : sources) {
      const QJsonObject s = v.toObject();
      source->addItem(s.value("address").toString(), s.value("id").toString());
    }
    const int sourceIndex = source->findData(o.value("sourceId").toString());
    source->setCurrentIndex(sourceIndex >= 0 ? sourceIndex : 0);

    auto* target = new QComboBox(outputs_);
    target->addItems({QStringLiteral("OMT"), QStringLiteral("OMT-HX"), QStringLiteral("NDI")});
    target->setCurrentText(o.value("target").toString());

    auto* codec = new QComboBox(outputs_);
    codec->addItems({QStringLiteral("HEVC"), QStringLiteral("H.264")});
    codec->setCurrentText(o.value("codec").toString());

    auto* quality = new QComboBox(outputs_);
    quality->addItems({QStringLiteral("Low"), QStringLiteral("Medium"), QStringLiteral("High")});
    quality->setCurrentText(o.value("quality").toString());

    auto* alpha = new QComboBox(outputs_);
    alpha->addItems({QStringLiteral("Auto"), QStringLiteral("Off")});
    alpha->setCurrentText(o.value("alpha").toString());

    auto* pill = new SignalPill(outputs_);
    pill->setState(o.value("state").toString(QStringLiteral("ready")));

    auto* rate = new QLabel(rateText(o), outputs_);
    rate->setObjectName("mono");
    rate->setToolTip(o.value("note").toString());

    auto* note = new QTableWidgetItem(o.value("note").toString());
    note->setToolTip(o.value("note").toString());

    auto* remove = new QPushButton(QStringLiteral("Remove"), outputs_);
    remove->setObjectName("danger");

    outputs_->setCellWidget(r, 0, name);
    outputs_->setCellWidget(r, 1, source);
    outputs_->setCellWidget(r, 2, target);
    outputs_->setCellWidget(r, 3, codec);
    outputs_->setCellWidget(r, 4, quality);
    outputs_->setCellWidget(r, 5, alpha);
    outputs_->setCellWidget(r, 6, pill);
    outputs_->setCellWidget(r, 7, rate);
    outputs_->setItem(r, 8, note);
    outputs_->setCellWidget(r, 9, remove);

    connect(name, &QLineEdit::editingFinished, this, [this, id, name] {
      const QString text = name->text().trimmed();
      if (!text.isEmpty()) sendSetOutput(id, QStringLiteral("name"), text);
    });
    connect(source, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this, id, source](int) {
              sendSetOutput(id, QStringLiteral("sourceId"), source->currentData().toString());
            });
    connect(target, &QComboBox::currentTextChanged, this, [this, id](const QString& text) {
      sendSetOutput(id, QStringLiteral("target"), text);
    });
    connect(codec, &QComboBox::currentTextChanged, this, [this, id](const QString& text) {
      sendSetOutput(id, QStringLiteral("codec"), text);
    });
    connect(quality, &QComboBox::currentTextChanged, this, [this, id](const QString& text) {
      sendSetOutput(id, QStringLiteral("quality"), text);
    });
    connect(alpha, &QComboBox::currentTextChanged, this, [this, id](const QString& text) {
      sendSetOutput(id, QStringLiteral("alpha"), text);
    });
    connect(remove, &QPushButton::clicked, this,
            [this, id, o] { onRemoveOutput(id, o.value("name").toString()); });
  }
}

void MainWindow::updateOutputRuntime() {
  const QJsonArray outputs = snapshot_.value("outputs").toArray();
  for (const QJsonValue& v : outputs) {
    const QJsonObject o = v.toObject();
    const int r = rowOf(outputIds_, o.value("id").toString());
    if (r < 0) continue;
    if (auto* pill = qobject_cast<SignalPill*>(outputs_->cellWidget(r, 6)))
      pill->setState(o.value("state").toString(QStringLiteral("ready")));
    if (auto* rate = qobject_cast<QLabel*>(outputs_->cellWidget(r, 7))) {
      rate->setText(rateText(o));
      rate->setToolTip(o.value("note").toString());
    }
    if (auto* note = outputs_->item(r, 8)) {
      note->setText(o.value("note").toString());
      note->setToolTip(o.value("note").toString());
    }
  }
}

void MainWindow::rebuildMatrix() {
  const QJsonArray sources = snapshot_.value("sources").toArray();
  const QJsonArray outputs = snapshot_.value("outputs").toArray();
  QStringList rows, columns;
  for (const QJsonValue& v : sources) rows << v.toObject().value("address").toString();
  for (const QJsonValue& v : outputs) columns << v.toObject().value("name").toString();

  matrix_->clear();
  matrix_->setRowCount(rows.size());
  matrix_->setColumnCount(columns.size());
  matrix_->setVerticalHeaderLabels(rows);
  matrix_->setHorizontalHeaderLabels(columns);

  for (int c = 0; c < outputs.size(); ++c) {
    const QString sourceId = outputs[c].toObject().value("sourceId").toString();
    for (int r = 0; r < sources.size(); ++r) {
      if (sources[r].toObject().value("id").toString() != sourceId) continue;
      auto* mark = new QTableWidgetItem(QStringLiteral("●"));
      mark->setTextAlignment(Qt::AlignCenter);
      matrix_->setItem(r, c, mark);
    }
  }
  matrix_->resizeColumnsToContents();
  matrix_->resizeRowsToContents();
}

void MainWindow::updateBands() {
  QStringList sourceLines;
  QStringList outputLines;
  for (const QJsonValue& v : snapshot_.value("sources").toArray()) {
    const QJsonObject s = v.toObject();
    const QString error = s.value("error").toString();
    if (!error.isEmpty())
      sourceLines << QStringLiteral("%1: %2").arg(s.value("address").toString(), error);
  }
  for (const QJsonValue& v : snapshot_.value("outputs").toArray()) {
    const QJsonObject o = v.toObject();
    const QString note = o.value("note").toString();
    // "alpha dropped" is a per-frame label (design 9.3), not a system error.
    if (!note.isEmpty() && !note.startsWith(QStringLiteral("alpha dropped")))
      outputLines << QStringLiteral("%1: %2").arg(o.value("name").toString(), note);
  }
  const auto apply = [](QLabel* label, const QStringList& lines) {
    label->setText(lines.join('\n'));
    label->setVisible(!lines.isEmpty());
  };
  apply(sourcesError_, sourceLines);
  apply(outputsError_, outputLines);
}

void MainWindow::updatePrimary() {
  if (connected_ && running_) {
    primary_->setText(QStringLiteral("Stop all outputs"));
    primary_->setEnabled(true);
  } else if (connected_) {
    primary_->setText(QStringLiteral("Start"));
    primary_->setEnabled(true);
  } else if (launcher_ && launcher_->isRunning()) {
    primary_->setText(QStringLiteral("Connecting…"));
    primary_->setEnabled(false);
  } else {
    primary_->setText(QStringLiteral("Start"));
    primary_->setEnabled(true);
  }
}

void MainWindow::updateStatus() {
  const auto now = Supervisor::Clock::now();
  QString engine;
  if (connected_) {
    engine = QStringLiteral("Engine: running");
  } else if (launcher_ && launcher_->isRunning()) {
    engine = supervisor_ && supervisor_->crashLooping(now)
                 ? QStringLiteral("Engine: restarting (slow)")
                 : QStringLiteral("Engine: starting");
  } else {
    engine = QStringLiteral("Engine: stopped");
  }
  engineLabel_->setText(engine);
  if (!connected_ && supervisor_ && launcher_ && !launcher_->isRunning() &&
      supervisor_->crashLooping(now))
    showError(QStringLiteral("The engine keeps exiting. Port %1 may be in use.").arg(port_));

  double mbps = 0;
  for (const QJsonValue& v : snapshot_.value("outputs").toArray())
    mbps += v.toObject().value("mbps").toDouble();
  rateLabel_->setText(QStringLiteral("out %1 Mbps").arg(mbps, 0, 'f', 1));
}

void MainWindow::showError(const QString& text) {
  error_->setText(text);
  error_->show();
}

void MainWindow::clearError() {
  if (error_->isVisible()) error_->hide();
}

void MainWindow::onPrimary() {
  if (!connected_) {
    if (launcher_ && launcher_->isRunning())
      connectClient();
    else
      launchEngine();
    return;
  }
  if (running_) {
    if (!confirm(QStringLiteral("Stop all outputs"),
                 QStringLiteral("Every output stops sending. The sources, the rows and the "
                                "bridge file are kept."),
                 QStringLiteral("Stop all outputs")))
      return;
    send(QJsonObject{{QStringLiteral("cmd"), "set-running"}, {QStringLiteral("on"), false}});
  } else {
    send(QJsonObject{{QStringLiteral("cmd"), "set-running"}, {QStringLiteral("on"), true}});
  }
}

void MainWindow::onAddSource() {
  if (!connected_) {
    showError(QStringLiteral("The engine is not running. Start it first."));
    return;
  }
  QDialog dialog(this);
  dialog.setWindowTitle(QStringLiteral("Add source"));
  auto* layout = new QVBoxLayout(&dialog);
  auto* form = new QFormLayout();
  auto* kind = new QComboBox(&dialog);
  kind->addItems({QStringLiteral("OMT"), QStringLiteral("OMT-HX"), QStringLiteral("NDI")});
  auto* address = new QLineEdit(&dialog);
  address->setPlaceholderText(QStringLiteral("HOSTNAME (name), or an NDI source name"));
  form->addRow(QStringLiteral("Kind"), kind);
  form->addRow(QStringLiteral("Address"), address);
  layout->addLayout(form);

  auto* discover = new QPushButton(QStringLiteral("Discover"), &dialog);
  layout->addWidget(discover);
  auto* found = new QListWidget(&dialog);
  found->setMinimumHeight(150);
  layout->addWidget(found);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

  connect(discover, &QPushButton::clicked, this, [this, found] {
    discoveryHandler_ = [found](const QJsonObject& reply) {
      found->clear();
      for (const QJsonValue& v : reply.value("omt").toArray()) {
        auto* item = new QListWidgetItem(QStringLiteral("OMT      %1").arg(v.toString()), found);
        item->setData(Qt::UserRole, QStringLiteral("OMT"));
        item->setData(Qt::UserRole + 1, v.toString());
      }
      for (const QJsonValue& v : reply.value("ndi").toArray()) {
        auto* item = new QListWidgetItem(QStringLiteral("NDI      %1").arg(v.toString()), found);
        item->setData(Qt::UserRole, QStringLiteral("NDI"));
        item->setData(Qt::UserRole + 1, v.toString());
      }
      if (found->count() == 0)
        new QListWidgetItem(QStringLiteral("No sources found. Type an address instead."), found);
    };
    send(QJsonObject{{QStringLiteral("cmd"), "discover"}});
  });
  connect(found, &QListWidget::itemClicked, this, [kind, address](QListWidgetItem* item) {
    const QString k = item->data(Qt::UserRole).toString();
    const QString a = item->data(Qt::UserRole + 1).toString();
    if (k.isEmpty() || a.isEmpty()) return;
    kind->setCurrentText(k);
    address->setText(a);
  });

  const int result = dialog.exec();
  discoveryHandler_ = nullptr;
  if (result != QDialog::Accepted) return;
  const QString a = address->text().trimmed();
  if (a.isEmpty()) {
    showError(QStringLiteral("A source needs an address."));
    return;
  }
  send(QJsonObject{{QStringLiteral("cmd"), "add-source"},
                   {QStringLiteral("kind"), kind->currentText()},
                   {QStringLiteral("address"), a}});
}

void MainWindow::onAddOutput() {
  if (!connected_) {
    showError(QStringLiteral("The engine is not running. Start it first."));
    return;
  }
  send(QJsonObject{{QStringLiteral("cmd"), "add-output"},
                   {QStringLiteral("name"),
                    QStringLiteral("Output %1").arg(outputIds_.size() + 1)}});
}

void MainWindow::onRemoveSource(const QString& id, const QString& label) {
  if (!confirm(QStringLiteral("Remove source"),
               QStringLiteral("Remove \"%1\" from the bridge? Outputs that read it are detached.")
                   .arg(label),
               QStringLiteral("Remove source")))
    return;
  send(QJsonObject{{QStringLiteral("cmd"), "remove-source"}, {QStringLiteral("id"), id}});
}

void MainWindow::onRemoveOutput(const QString& id, const QString& label) {
  if (!confirm(QStringLiteral("Remove output"),
               QStringLiteral("Remove \"%1\"? It stops sending immediately.").arg(label),
               QStringLiteral("Remove output")))
    return;
  send(QJsonObject{{QStringLiteral("cmd"), "remove-output"}, {QStringLiteral("id"), id}});
}

void MainWindow::openSettings() {
  if (!connected_) {
    showError(QStringLiteral("The engine is not running. Start it first."));
    return;
  }
  const QJsonObject settings = snapshot_.value("settings").toObject();
  QDialog dialog(this);
  dialog.setWindowTitle(QStringLiteral("Settings"));
  auto* layout = new QVBoxLayout(&dialog);
  auto* form = new QFormLayout();

  auto* target = new QComboBox(&dialog);
  target->addItems({QStringLiteral("OMT"), QStringLiteral("OMT-HX"), QStringLiteral("NDI")});
  target->setCurrentText(settings.value("defaultTarget").toString(QStringLiteral("OMT-HX")));

  auto* quality = new QComboBox(&dialog);
  quality->addItems({QStringLiteral("Low"), QStringLiteral("Medium"), QStringLiteral("High")});
  quality->setCurrentText(settings.value("defaultQuality").toString(QStringLiteral("Medium")));

  auto* interval = new QSpinBox(&dialog);
  interval->setRange(0, 60000);
  interval->setSingleStep(500);
  interval->setSuffix(QStringLiteral(" ms"));
  interval->setValue(settings.value("thumbnailIntervalMs").toInt(2000));

  auto* startWithApp = new QCheckBox(QStringLiteral("Start engine with the app"), &dialog);
  startWithApp->setChecked(settings.value("startEngineWithApp").toBool(false));

  form->addRow(QStringLiteral("Default target"), target);
  form->addRow(QStringLiteral("Default quality"), quality);
  form->addRow(QStringLiteral("Thumbnail refresh"), interval);
  layout->addLayout(form);
  layout->addWidget(startWithApp);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

  if (dialog.exec() != QDialog::Accepted) return;
  send(QJsonObject{{QStringLiteral("cmd"), "set-settings"},
                   {QStringLiteral("defaultTarget"), target->currentText()},
                   {QStringLiteral("defaultQuality"), quality->currentText()},
                   {QStringLiteral("thumbnailIntervalMs"), interval->value()},
                   {QStringLiteral("startEngineWithApp"), startWithApp->isChecked()}});
}

void MainWindow::saveBridgeNow() {
  if (!snapshot_.contains("sources") || !snapshot_.contains("outputs")) return;
  Bridge bridge;
  bridge.name = snapshot_.value("name").toString().toStdString();
  const QJsonObject settings = snapshot_.value("settings").toObject();
  if (const auto v = outputTargetFromString(
          settings.value("defaultTarget").toString().toStdString()))
    bridge.settings.defaultTarget = *v;
  if (const auto v =
          qualityFromString(settings.value("defaultQuality").toString().toStdString()))
    bridge.settings.defaultQuality = *v;
  bridge.settings.thumbnailIntervalMs = settings.value("thumbnailIntervalMs").toInt(2000);
  bridge.settings.startEngineWithApp = settings.value("startEngineWithApp").toBool(false);

  for (const QJsonValue& v : snapshot_.value("sources").toArray()) {
    const QJsonObject s = v.toObject();
    Source source;
    source.id = s.value("id").toString().toStdString();
    source.address = s.value("address").toString().toStdString();
    const auto kind = sourceKindFromString(s.value("kind").toString().toStdString());
    if (!kind) continue;
    source.kind = *kind;
    if (s.contains("codec")) {
      const auto codec = hxCodecFromString(s.value("codec").toString().toStdString());
      if (!codec) continue;
      source.codec = *codec;
    }
    bridge.sources.push_back(std::move(source));
  }

  for (const QJsonValue& v : snapshot_.value("outputs").toArray()) {
    const QJsonObject o = v.toObject();
    Output output;
    output.id = o.value("id").toString().toStdString();
    output.sourceId = o.value("sourceId").toString().toStdString();
    output.name = o.value("name").toString().toStdString();
    const auto target = outputTargetFromString(o.value("target").toString().toStdString());
    const auto codec = hxCodecFromString(o.value("codec").toString().toStdString());
    const auto quality = qualityFromString(o.value("quality").toString().toStdString());
    const auto alpha = alphaModeFromString(o.value("alpha").toString().toStdString());
    if (!target || !codec || !quality || !alpha) continue;
    output.target = *target;
    output.codec = *codec;
    output.quality = *quality;
    output.alpha = *alpha;
    bridge.outputs.push_back(std::move(output));
  }

  try {
    saveBridge(bridge, bridgePath_.toStdWString());
    dirty_ = false;
  } catch (const std::exception& e) {
    showError(QStringLiteral("Could not save the bridge file: %1").arg(e.what()));
  }
}

void MainWindow::sendSetOutput(const QString& id, const QString& field, const QJsonValue& value) {
  send(QJsonObject{{QStringLiteral("cmd"), "set-output"},
                   {QStringLiteral("id"), id},
                   {field, value}});
}

bool MainWindow::confirm(const QString& title, const QString& text, const QString& verb) {
  QMessageBox box(this);
  box.setWindowTitle(title);
  box.setText(text);
  box.setIcon(QMessageBox::Question);
  auto* go = box.addButton(verb, QMessageBox::AcceptRole);
  auto* cancel = box.addButton(QMessageBox::Cancel);
  box.setDefaultButton(cancel);
  box.exec();
  return box.clickedButton() == go;
}

}  // namespace ob
