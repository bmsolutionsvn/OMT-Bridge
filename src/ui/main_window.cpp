#include "ui/main_window.h"

#include <QStatusBar>

namespace ob {
MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle(QStringLiteral("OMT Bridge"));
  engine_state_ = new QLabel(QStringLiteral("Engine: stopped"), this);
  statusBar()->addWidget(engine_state_);
  resize(900, 560);
}
}  // namespace ob