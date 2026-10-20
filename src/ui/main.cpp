#include <QApplication>

#include <ixwebsocket/IXNetSystem.h>

#include "ui/main_window.h"
#include "ui/tokens.h"

int main(int argc, char** argv) {
  // ixwebsocket does not initialise Winsock, and the UI's WebSocket client needs it just as the
  // engine's server does (engine_main does the same).
  if (!ix::initNetSystem()) return 1;

  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("BMSolutionsVN"));
  QCoreApplication::setApplicationName(QStringLiteral("OMT Bridge"));
  QApplication::setStyle(QStringLiteral("Fusion"));
  app.setStyleSheet(ob::ui::styleSheet());

  ob::MainWindow window;
  window.show();
  const int rc = app.exec();
  ix::uninitNetSystem();
  return rc;
}
