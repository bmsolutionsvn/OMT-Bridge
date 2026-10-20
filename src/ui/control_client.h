#pragma once
// A WebSocket client for the engine's control channel (design 6.1). ixwebsocket runs its own
// thread; every callback is marshalled onto the Qt thread this object lives on.
#include <QJsonObject>
#include <QByteArray>
#include <QObject>
#include <memory>

namespace ix {
class WebSocket;
}

namespace ob {

class ControlClient : public QObject {
  Q_OBJECT
 public:
  explicit ControlClient(QObject* parent = nullptr);
  ~ControlClient() override;

  void connectTo(const QString& url);
  void disconnect();
  void send(const QJsonObject& command);
  bool connected() const { return connected_; }

 signals:
  void connectedChanged();
  void message(const QJsonObject& reply);
  void binaryMessage(const QByteArray& data);

 private:
  std::unique_ptr<ix::WebSocket> ws_;
  bool connected_ = false;
};

}  // namespace ob
