#pragma once
// The OMT Bridge shell (design 13): Sources and Outputs panes, a read-only matrix, the signal
// states, settings, the Bridge file and the confirm dialogs. It owns the engine process's
// lifecycle and talks to it only through the WebSocket control channel.
#include <QJsonObject>
#include <QJsonValue>
#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QMainWindow>
#include <QStringList>
#include <functional>
#include <memory>

class QAction;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTableWidget;
class QTimer;

namespace ob {

class ControlClient;
class ProcessLauncher;
class Supervisor;

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

 protected:
  void closeEvent(QCloseEvent* event) override;

 private:
  void buildUi();
  void startOrAdopt();
  void launchEngine();
  void connectClient();
  void send(const QJsonObject& command);
  void requestState();

  void onMessage(const QJsonObject& reply);
  void onBinaryMessage(const QByteArray& payload);
  void applyState(const QJsonObject& state);
  void rebuildSources();
  void rebuildOutputs();
  void updateOutputRuntime();
  void rebuildMatrix();
  void updateBands();
  void updatePrimary();
  void updateStatus();
  void showError(const QString& text);
  void clearError();

  void onPrimary();
  void onAddSource();
  void onAddOutput();
  void onRemoveSource(const QString& id, const QString& label);
  void onRemoveOutput(const QString& id, const QString& label);
  void openSettings();
  void saveBridgeNow();
  void sendSetOutput(const QString& id, const QString& field, const QJsonValue& value);
  bool confirm(const QString& title, const QString& text, const QString& verb);

  QString engineArgs() const;
  QString exePath() const;

  ControlClient* client_ = nullptr;
  std::unique_ptr<ProcessLauncher> launcher_;
  std::unique_ptr<Supervisor> supervisor_;
  QTimer* supervisorTimer_ = nullptr;
  QTimer* pollTimer_ = nullptr;

  QTableWidget* sources_ = nullptr;
  QTableWidget* outputs_ = nullptr;
  QTableWidget* matrix_ = nullptr;
  QStackedWidget* pages_ = nullptr;
  QAction* matrixAction_ = nullptr;
  QPushButton* primary_ = nullptr;
  QLabel* engineLabel_ = nullptr;
  QLabel* rateLabel_ = nullptr;
  QLabel* error_ = nullptr;
  QLabel* sourcesError_ = nullptr;
  QLabel* outputsError_ = nullptr;

  QJsonObject snapshot_;
  QHash<QString, QImage> thumbnails_;
  QStringList sourceIds_;
  QStringList outputIds_;
  QStringList comboSourceIds_;
  QString bridgePath_;
  QString discoveryPath_;
  QString token_;
  int port_ = 61715;
  bool running_ = false;
  bool dirty_ = false;
  bool connected_ = false;
  qint64 lastConnectAttempt_ = 0;
  std::function<void(const QJsonObject&)> discoveryHandler_;
};

}  // namespace ob
