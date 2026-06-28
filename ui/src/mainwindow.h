#pragma once

#include "apistatus.h"
#include "connectiondialog.h"

#include <QMainWindow>

class QTimer;
class QLabel;
class QAction;
class ApiClient;
class SecretStore;
class StatusWidget;
class ChartWidget;
class OverrideWidget;

// MainWindow: top-level window tying together ApiClient (REST polling),
// SecretStore (API key persistence), and the three dockable panels
// (status, chart, override) -- same "dockable panels persisted via
// QSettings" pattern as solakon-one-ui, but polling a REST endpoint instead
// of Modbus registers directly. Also mirrors solakon-one-ui's menu layout:
// a File menu with Connect.../Disconnect/Quit, and a View menu with one
// auto-syncing toggleViewAction() per dock (see setupDocks()).
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Applies connection settings supplied on the command line (if any) and
    // connects immediately instead of showing the dialog on startup.
    void connectWithCliArgs(const ConnectionSettings& settings);

    // Connects automatically using the last-saved connection settings if
    // ConnectionSettings::autoConnectOnStartup is set (configured via the
    // connection dialog's "Automatically connect..." checkbox). Called from
    // main.cpp only when no --host was given on the command line -- an
    // explicit CLI connection always takes priority over this. No-op if
    // auto-connect isn't enabled or a connection is already active.
    void autoConnectIfEnabled();

private slots:
    void showConnectionDialog();
    void disconnectFromServer();
    void onPollTimer();
    void onStatusReceived(const ApiStatus& status, const ApiOverride& override);
    void onRequestFailed(const QString& endpoint, int httpStatus, const QString& message);
    void onOverrideApplied(const ApiOverride& override);
    void onOverrideCleared();
    void onLowSocHoldApplied(bool active);

private:
    ApiClient*      m_apiClient;
    SecretStore*    m_secretStore;
    StatusWidget*   m_statusWidget;
    ChartWidget*    m_chartWidget;
    OverrideWidget* m_overrideWidget;
    QTimer*         m_pollTimer;
    QLabel*         m_connectionLabel;
    QAction*        m_connectAction;
    QAction*        m_disconnectAction;

    ConnectionSettings m_pendingSettings; // set by dialog, applied once the key finishes loading
    ConnectionSettings m_activeSettings;
    bool m_connected = false;

    void setupDocks();
    void createMenus();
    void restoreLayout();
    void saveLayout();
    void applyConnection(const ConnectionSettings& settings, const QString& apiKey);
    void setConnectedState(bool connected);

    // Shared by connectWithCliArgs() and autoConnectIfEnabled(): looks up
    // the API key for `settings` in the secret store and, once loaded (or
    // confirmed absent), connects. Uses a one-shot connection so it never
    // fires for unrelated SecretStore::apiKeyLoaded emissions triggered
    // later by the connection dialog.
    void connectUsingSecretStore(const ConnectionSettings& settings);

    void closeEvent(QCloseEvent* event) override;
};
