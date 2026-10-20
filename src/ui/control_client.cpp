#include "ui/control_client.h"

#include <QJsonDocument>
#include <QMetaObject>
#include <QString>

#include <ixwebsocket/IXWebSocket.h>

namespace ob {

ControlClient::ControlClient(QObject* parent) : QObject(parent), ws_(std::make_unique<ix::WebSocket>()) {
  ws_->disableAutomaticReconnection();  // the window owns retry policy (supervisor + timer)
  ws_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
    if (msg->type == ix::WebSocketMessageType::Open) {
      QMetaObject::invokeMethod(
          this,
          [this] {
            connected_ = true;
            emit connectedChanged();
          },
          Qt::QueuedConnection);
    } else if (msg->type == ix::WebSocketMessageType::Message && !msg->binary) {
      const std::string text = msg->str;
      QMetaObject::invokeMethod(
          this,
          [this, text] {
            const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(text));
            if (doc.isObject()) emit message(doc.object());
          },
          Qt::QueuedConnection);
    } else if (msg->type == ix::WebSocketMessageType::Close ||
               msg->type == ix::WebSocketMessageType::Error) {
      QMetaObject::invokeMethod(
          this,
          [this] {
            if (!connected_) return;
            connected_ = false;
            emit connectedChanged();
          },
          Qt::QueuedConnection);
    }
  });
}

ControlClient::~ControlClient() {
  // The callbacks capture `this`; stop() joins the ix thread before the members go away.
  ws_->stop();
}

void ControlClient::connectTo(const QString& url) {
  if (connected_ || ws_->getReadyState() == ix::ReadyState::Open ||
      ws_->getReadyState() == ix::ReadyState::Connecting)
    return;
  ws_->setUrl(url.toStdString());
  ws_->start();
}

void ControlClient::disconnect() {
  ws_->stop();
  if (connected_) {
    connected_ = false;
    emit connectedChanged();
  }
}

void ControlClient::send(const QJsonObject& command) {
  if (!connected_) return;
  ws_->send(QJsonDocument(command).toJson(QJsonDocument::Compact).toStdString());
}

}  // namespace ob
