#pragma once
#include <QMainWindow>
#include <QLabel>

namespace ob {
class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  explicit MainWindow(QWidget* parent = nullptr);
 private:
  QLabel* engine_state_ = nullptr;
};
}  // namespace ob
