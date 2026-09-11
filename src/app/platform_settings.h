#pragma once
#include "qmlbridge/collection_service.h"
#include "qmlbridge/controllers.h"
#include "qmlbridge/portable_session.h"
#include <QMediaDevices>
#include <QMenu>
#include <QQuickWindow>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QNetworkAccessManager>
#include <atomic>

namespace listenfree {
class WindowsMediaSession;
class PlatformSettings final : public QObject {
  Q_OBJECT
public:
  PlatformSettings(QQuickWindow *window, QObject *shell,
                   qmlbridge::SettingsController &settings,
                   qmlbridge::PortableSession &player,
                   qmlbridge::CollectionService &lists,
                   infrastructure::database::Database &db,
                   qmlbridge::LibraryController &library,
                   QObject *parent = nullptr);
  ~PlatformSettings() override;
  Q_INVOKABLE void action(const QString &key);
  QString diagnostics() const;

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  QQuickWindow *window_;
  QObject *shell_;
  qmlbridge::SettingsController &settings_;
  qmlbridge::PortableSession &player_;
  qmlbridge::CollectionService &lists_;
  infrastructure::database::Database &db_;
  qmlbridge::LibraryController &library_;
  QSystemTrayIcon tray_;
  QMenu menu_;
  QMediaDevices devices_;
  QTimer status_;
  QTimer mediaUpdate_;
  WindowsMediaSession* mediaSession_{};
  QNetworkAccessManager network_;
  bool checkingUpdates_{false};
  bool transparencyActive_{false};
  bool quitting_{false};
  void *taskbar_{nullptr};
  std::atomic<int> frameLimit_{60};
  void apply(const QString &key, const QVariant &value);
  void applyTheme();
  void applyTransparency();
  void updateTray();
  void updateStatus();
  void updateMediaSession();
};
} // namespace listenfree
