#include "ui/tokens.h"

namespace ob::ui {

QString styleSheet() {
  return QStringLiteral(R"qss(
* { font-family: "Segoe UI", "Inter", sans-serif; font-size: 10pt; }
QMainWindow, QDialog { background: #0B0E11; }
QWidget { color: #E6EDF3; }
QToolBar { background: #12171C; border: none; border-bottom: 1px solid #26323D; spacing: 6px; padding: 6px 12px; }
QToolButton { background: transparent; border: 1px solid transparent; border-radius: 4px; padding: 6px 10px; color: #C7D2DC; }
QToolButton:hover { background: #171E25; border-color: #26323D; }
QToolButton:checked { background: #17323A; border-color: #22D3EE; color: #E6EDF3; }
QGroupBox { background: #12171C; border: 1px solid #26323D; border-radius: 6px; margin-top: 14px; padding: 10px; font-weight: 600; }
QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 4px; color: #8B9AA8; font-size: 9pt; }
QTableWidget { background: #0F1418; alternate-background-color: #12171C; border: 1px solid #26323D; border-radius: 4px; gridline-color: #1D262F; selection-background-color: #17323A; selection-color: #E6EDF3; }
QHeaderView::section { background: #12171C; color: #8B9AA8; border: none; border-bottom: 1px solid #26323D; padding: 6px; font-size: 9pt; font-weight: 600; }
QLineEdit, QComboBox, QSpinBox { background: #0F1418; border: 1px solid #26323D; border-radius: 4px; padding: 4px 6px; min-height: 20px; }
QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border-color: #22D3EE; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox QAbstractItemView { background: #171E25; border: 1px solid #26323D; selection-background-color: #17323A; }
QPushButton { background: #1D262F; border: 1px solid #26323D; border-radius: 4px; padding: 5px 10px; }
QPushButton:hover { background: #26323D; }
QPushButton#primary { background: #22D3EE; color: #061014; border: none; border-radius: 999px; padding: 7px 18px; font-weight: 700; }
QPushButton#primary:hover { background: #67E8F9; }
QPushButton#danger { color: #F87171; }
QLabel#errorBlock { background: rgba(248, 113, 113, 0.12); border: 1px solid rgba(248, 113, 113, 0.4); border-radius: 4px; padding: 8px 10px; color: #FCA5A5; }
QLabel#muted { color: #8B9AA8; }
QLabel#mono { font-family: "Cascadia Mono", "JetBrains Mono", Consolas, monospace; font-size: 9pt; }
QStatusBar { background: #12171C; border-top: 1px solid #26323D; color: #8B9AA8; }
QStatusBar QLabel { color: #8B9AA8; }
QSplitter::handle { background: transparent; width: 12px; }
QToolTip { background: #171E25; color: #E6EDF3; border: 1px solid #26323D; }
QListWidget { background: #0F1418; border: 1px solid #26323D; border-radius: 4px; }
)qss");
}

}  // namespace ob::ui
