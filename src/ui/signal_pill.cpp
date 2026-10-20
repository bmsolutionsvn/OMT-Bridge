#include "ui/signal_pill.h"

#include <QFont>
#include <QFontMetrics>
#include <QPainter>

namespace ob {
namespace {

QColor colourFor(const QString& state) {
  if (state == "live") return QColor("#34D399");
  if (state == "connecting") return QColor("#FBBF24");
  if (state == "offline") return QColor("#F87171");
  return QColor("#8B9AA8");
}

}  // namespace

SignalPill::SignalPill(QWidget* parent) : QWidget(parent) {
  setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

void SignalPill::setState(const QString& state) {
  if (state_ == state) return;
  state_ = state;
  update();
}

QSize SignalPill::sizeHint() const {
  QFont f = font();
  f.setPointSizeF(7.5);
  f.setBold(true);
  const QFontMetrics fm(f);
  return QSize(fm.horizontalAdvance(state_.toUpper()) + 30, 22);
}

void SignalPill::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const QColor c = colourFor(state_);
  QColor wash = c;
  wash.setAlphaF(0.15f);
  p.setPen(Qt::NoPen);
  p.setBrush(wash);
  p.drawRoundedRect(rect(), height() / 2.0, height() / 2.0);
  p.setBrush(c);
  p.drawEllipse(QPointF(11, height() / 2.0), 3, 3);
  QFont f = font();
  f.setPointSizeF(7.5);
  f.setBold(true);
  f.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
  p.setFont(f);
  p.setPen(c);
  p.drawText(QRect(20, 0, width() - 26, height()), Qt::AlignVCenter | Qt::AlignLeft,
             state_.toUpper());
}

}  // namespace ob
