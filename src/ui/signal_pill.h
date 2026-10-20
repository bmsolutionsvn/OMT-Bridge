#pragma once
// The one custom widget in the shell (design 13.3): a 999 px pill, mono uppercase label and a
// solid dot, coloured by the signal state. Static under reduced motion is not detectable through
// Qt 6's public API, so it never animates.
#include <QWidget>

namespace ob {

class SignalPill : public QWidget {
  Q_OBJECT
 public:
  explicit SignalPill(QWidget* parent = nullptr);

  void setState(const QString& state);  // the wire names: ready, connecting, live, offline
  QSize sizeHint() const override;

 protected:
  void paintEvent(QPaintEvent*) override;

 private:
  QString state_ = QStringLiteral("ready");
};

}  // namespace ob
