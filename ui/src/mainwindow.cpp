#include "mainwindow.h"
#include "apiclient.h"
#include "chartwidget.h"
#include "connectiondialog.h"
#include "overridewidget.h"
#include "secretstore.h"
#include "statuswidget.h"

#include <QCloseEvent>
#include <QAction>
#include <QDockWidget>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>

#include <algorithm>
#include <memory>

namespace {
constexpr char kOrg[] = "flunaras";
constexpr char kApp[] = "solakon-one-fritz-powerregulator-ui";
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(tr("Solakon ONE / FRITZ!Box Power Regulator"));

    m_apiClient = new ApiClient(this);
    m_secretStore = new SecretStore(this);
    m_pollTimer = new QTimer(this);

    createMenus();
    setupDocks();

    m_connectionLabel = new QLabel(tr("Not connected"), this);
    statusBar()->addPermanentWidget(m_connectionLabel);

    connect(m_secretStore, &SecretStore::apiKeyError, this, [this](const QString& msg) {
        statusBar()->showMessage(tr("Secret store error: %1").arg(msg), 5000);
    });

    connect(m_pollTimer, &QTimer::timeout, this, &MainWindow::onPollTimer);

    connect(m_apiClient, &ApiClient::statusReceived, this, &MainWindow::onStatusReceived);
    connect(m_apiClient, &ApiClient::requestFailed, this, &MainWindow::onRequestFailed);
    connect(m_apiClient, &ApiClient::overrideApplied, this, &MainWindow::onOverrideApplied);
    connect(m_apiClient, &ApiClient::overrideCleared, this, &MainWindow::onOverrideCleared);

    connect(m_overrideWidget, &OverrideWidget::applySetpointRequested, m_apiClient,
            &ApiClient::setOverrideSetpoint);
    connect(m_overrideWidget, &OverrideWidget::applyReleaseRequested, m_apiClient,
            &ApiClient::setOverrideRelease);
    connect(m_overrideWidget, &OverrideWidget::clearRequested, m_apiClient,
            &ApiClient::clearOverride);
    connect(m_overrideWidget, &OverrideWidget::lowSocHoldRequested, m_apiClient,
            &ApiClient::setLowSocHold);
    connect(m_apiClient, &ApiClient::lowSocHoldApplied, this, &MainWindow::onLowSocHoldApplied);

    restoreLayout();

    QSettings settings(kOrg, kApp);
    m_pendingSettings.host = settings.value("connection/host", "127.0.0.1").toString();
    m_pendingSettings.port = settings.value("connection/port", 8080).toInt();
    m_pendingSettings.scheme = settings.value("connection/scheme", "http").toString();
    m_pendingSettings.ignoreSslErrors = settings.value("connection/ignoreSslErrors", false).toBool();
    m_pendingSettings.pollIntervalS = settings.value("connection/pollIntervalS", 10).toInt();
    m_pendingSettings.autoConnectOnStartup =
        settings.value("connection/autoConnectOnStartup", false).toBool();
}

MainWindow::~MainWindow() = default;

void MainWindow::setupDocks() {
    m_statusWidget = new StatusWidget(this);
    auto* statusDock = new QDockWidget(tr("Status"), this);
    statusDock->setObjectName("statusDock");
    statusDock->setWidget(m_statusWidget);
    addDockWidget(Qt::LeftDockWidgetArea, statusDock);

    m_chartWidget = new ChartWidget(this);
    auto* chartDock = new QDockWidget(tr("Live Charts"), this);
    chartDock->setObjectName("chartDock");
    chartDock->setWidget(m_chartWidget);
    addDockWidget(Qt::RightDockWidgetArea, chartDock);

    m_overrideWidget = new OverrideWidget(this);
    auto* overrideDock = new QDockWidget(tr("Override"), this);
    overrideDock->setObjectName("overrideDock");
    overrideDock->setWidget(m_overrideWidget);
    addDockWidget(Qt::LeftDockWidgetArea, overrideDock);

    // ── View menu: one toggle action per dock ───────────────────────────────
    // Built here (rather than in createMenus()) so the docks are in scope --
    // same approach as solakon-one-ui's MainWindow::createDockWidgets().
    // QDockWidget::toggleViewAction() returns a checkable QAction owned by
    // the dock whose checked state is bound to the dock's visibility -- no
    // manual wiring needed, and it stays in sync even if the user closes the
    // dock via its title-bar X button.
    auto* viewMenu = menuBar()->addMenu(tr("&View"));
    for (QDockWidget* dock : {statusDock, chartDock, overrideDock})
        viewMenu->addAction(dock->toggleViewAction());
}

void MainWindow::createMenus() {
    // ── File menu ────────────────────────────────────────────────────────────
    auto* fileMenu = menuBar()->addMenu(tr("&File"));

    m_connectAction = new QAction(tr("&Connect..."), this);
    m_connectAction->setShortcut(QKeySequence(tr("Ctrl+K")));
    connect(m_connectAction, &QAction::triggered, this, &MainWindow::showConnectionDialog);
    fileMenu->addAction(m_connectAction);

    m_disconnectAction = new QAction(tr("&Disconnect"), this);
    m_disconnectAction->setEnabled(false);
    connect(m_disconnectAction, &QAction::triggered, this, &MainWindow::disconnectFromServer);
    fileMenu->addAction(m_disconnectAction);

    fileMenu->addSeparator();
    auto* quitAction = new QAction(tr("&Quit"), this);
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &MainWindow::close);
    fileMenu->addAction(quitAction);

    // The View menu itself is built in setupDocks(), where the dock widgets
    // it toggles are in scope.
}

void MainWindow::restoreLayout() {
    QSettings settings(kOrg, kApp);
    const QByteArray geometry = settings.value("window/geometry").toByteArray();
    if (!geometry.isEmpty() && restoreGeometry(geometry)) {
        // Successfully restored a previous session's size/position.
    } else {
        // First run (or corrupt/incompatible saved geometry) -- fall back to
        // a sensible default size instead of whatever Qt's own default would
        // be. Must happen here, not in main.cpp, or an unconditional
        // resize() there would clobber whatever was just restored above.
        resize(1200, 800);
    }
    restoreState(settings.value("window/state").toByteArray());
}

void MainWindow::saveLayout() {
    QSettings settings(kOrg, kApp);
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/state", saveState());
    settings.setValue("connection/host", m_activeSettings.host);
    settings.setValue("connection/port", m_activeSettings.port);
    settings.setValue("connection/scheme", m_activeSettings.scheme);
    settings.setValue("connection/ignoreSslErrors", m_activeSettings.ignoreSslErrors);
    settings.setValue("connection/pollIntervalS", m_activeSettings.pollIntervalS);
    settings.setValue("connection/autoConnectOnStartup", m_activeSettings.autoConnectOnStartup);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveLayout();
    QMainWindow::closeEvent(event);
}

void MainWindow::showConnectionDialog() {
    ConnectionDialog dlg(m_secretStore, this);
    dlg.setSettings(m_connected ? m_activeSettings : m_pendingSettings);
    // Pre-load whatever key is already stored for the currently displayed
    // host/port; the dialog updates itself if the user edits host/port.
    m_secretStore->load(m_pendingSettings.host, m_pendingSettings.port);
    connect(m_secretStore, &SecretStore::apiKeyLoaded, &dlg, &ConnectionDialog::setApiKeyField);

    if (dlg.exec() != QDialog::Accepted)
        return;

    const ConnectionSettings s = dlg.settings();
    const QString apiKey = dlg.apiKey();

    if (dlg.apiKeyChanged() && !apiKey.isEmpty())
        m_secretStore->save(s.host, s.port, apiKey);

    applyConnection(s, apiKey);
}

void MainWindow::connectWithCliArgs(const ConnectionSettings& settings) {
    connectUsingSecretStore(settings);
}

void MainWindow::autoConnectIfEnabled() {
    if (m_connected || !m_pendingSettings.autoConnectOnStartup)
        return;
    connectUsingSecretStore(m_pendingSettings);
}

void MainWindow::connectUsingSecretStore(const ConnectionSettings& settings) {
    m_pendingSettings = settings;
    // The API key must still come from the secret store -- neither CLI args
    // nor the persisted auto-connect settings ever carry the secret itself,
    // only where/how to connect. Uses a one-shot connection (rather than a
    // permanently-wired slot) so it never fires for unrelated loads
    // triggered later by the connection dialog.
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = connect(m_secretStore, &SecretStore::apiKeyLoaded, this,
        [this, connection](const QString& apiKey) {
            disconnect(*connection);
            if (!apiKey.isEmpty())
                applyConnection(m_pendingSettings, apiKey);
            else
                statusBar()->showMessage(
                    tr("No API key found in the secret store for %1:%2 -- use Connect... to set one.")
                        .arg(m_pendingSettings.host).arg(m_pendingSettings.port), 8000);
        });
    m_secretStore->load(settings.host, settings.port);
}

void MainWindow::disconnectFromServer() {
    setConnectedState(false);
    statusBar()->showMessage(tr("Disconnected"), 3000);
}

void MainWindow::applyConnection(const ConnectionSettings& settings, const QString& apiKey) {
    m_activeSettings = settings;
    const QString baseUrl = QString("%1://%2:%3")
        .arg(settings.scheme, settings.host, QString::number(settings.port));
    m_apiClient->configure(baseUrl, apiKey, settings.ignoreSslErrors);
    m_pollTimer->start(std::max(settings.pollIntervalS, 2) * 1000);
    setConnectedState(true);
    onPollTimer(); // fetch immediately instead of waiting a full interval
}

void MainWindow::onPollTimer() {
    if (m_apiClient->isConfigured())
        m_apiClient->fetchStatus();
}

void MainWindow::onStatusReceived(const ApiStatus& status, const ApiOverride& override) {
    m_statusWidget->updateStatus(status);
    m_chartWidget->addSample(status);
    m_overrideWidget->updateOverride(override);
    m_overrideWidget->updateLowSocHold(status.low_soc_hold, status.low_soc_hold_since);
    m_connectionLabel->setText(tr("Connected: %1 (cycle %2)")
        .arg(m_apiClient->baseUrl())
        .arg(status.cycle_count));
}

void MainWindow::onRequestFailed(const QString& endpoint, int httpStatus, const QString& message) {
    if (endpoint == "status") {
        m_statusWidget->setDisconnected();
        m_connectionLabel->setText(tr("Connection error: %1").arg(message));
    }
    statusBar()->showMessage(
        tr("%1 request failed (HTTP %2): %3").arg(endpoint).arg(httpStatus).arg(message), 8000);

    if (httpStatus == 401) {
        QMessageBox::warning(this, tr("Authentication failed"),
            tr("The API key was rejected. Please check the key stored for this "
               "connection in Connect... and try again."));
    }
}

void MainWindow::onOverrideApplied(const ApiOverride& override) {
    m_overrideWidget->updateOverride(override);
    statusBar()->showMessage(tr("Override applied"), 3000);
}

void MainWindow::onOverrideCleared() {
    m_overrideWidget->updateOverride(ApiOverride{});
    statusBar()->showMessage(tr("Override cleared"), 3000);
}

void MainWindow::onLowSocHoldApplied(bool active) {
    // The response only reflects the requested (pending) value -- the
    // control loop applies it and reports the authoritative since-timestamp
    // on the next fetchStatus() poll.  Show it optimistically here in the
    // meantime with an invalid "since" (updateLowSocHold treats that as
    // "just now, timestamp not yet known").
    m_overrideWidget->updateLowSocHold(active, QDateTime());
    statusBar()->showMessage(active ? tr("Low-SoC hold set (pending)")
                                     : tr("Low-SoC hold cleared (pending)"), 3000);
}

void MainWindow::setConnectedState(bool connected) {
    m_connected = connected;
    m_overrideWidget->setControlsEnabled(connected);
    m_connectAction->setEnabled(!connected);
    m_disconnectAction->setEnabled(connected);
    if (!connected) {
        m_pollTimer->stop();
        m_statusWidget->setDisconnected();
        m_connectionLabel->setText(tr("Not connected"));
    }
}
