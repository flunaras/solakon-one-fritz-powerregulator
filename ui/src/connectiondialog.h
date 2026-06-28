#pragma once

#include <QDialog>

class QLineEdit;
class QSpinBox;
class QComboBox;
class QCheckBox;
class QDialogButtonBox;
class SecretStore;

// ConnectionSettings: everything needed to talk to a powerregulator
// instance's REST API. Deliberately does NOT include the API key -- that is
// loaded/saved separately via SecretStore so it never round-trips through
// QSettings or any plaintext config.
struct ConnectionSettings {
    QString host = "127.0.0.1";
    int     port = 8080;
    QString scheme = "http";   // "http" or "https"
    bool    ignoreSslErrors = false;
    int     pollIntervalS = 10;
    // autoConnectOnStartup: if true, MainWindow connects to this saved
    // connection automatically on the next launch instead of waiting for
    // the user to open Connect... (a CLI --host still takes priority when
    // given). The API key itself still always comes from the secret store,
    // never from this (QSettings-persisted) struct.
    bool    autoConnectOnStartup = false;
};

// ConnectionDialog: prompts for the REST API endpoint and (optionally) a new
// API key to store. Mirrors solakon-one-ui's connection dialog in spirit
// (same fields persisted via QSettings, shown on startup unless a
// connection was supplied on the command line) but targets the HTTP REST
// API of this project instead of Modbus TCP directly.
//
// The API key field is intentionally handled out-of-band: on accept(), if
// the user typed a new key, it is asynchronously written to the platform
// secret store via SecretStore before the dialog is considered done; the
// dialog does not expose the key value to callers at all.
class ConnectionDialog : public QDialog {
    Q_OBJECT
public:
    explicit ConnectionDialog(SecretStore* secretStore, QWidget* parent = nullptr);

    void setSettings(const ConnectionSettings& settings);
    [[nodiscard]] ConnectionSettings settings() const;

    // Pre-fills the API key field (called after an async SecretStore::load()
    // completes for the currently displayed host/port). Never called with
    // the key already typed by the user mid-edit -- only right after the
    // dialog is shown for a given host/port.
    void setApiKeyField(const QString& apiKey);

    // True if the user changed the API key field's contents since it was
    // last set via setApiKeyField() (i.e. there is something new to save).
    [[nodiscard]] bool apiKeyChanged() const;
    [[nodiscard]] QString apiKey() const;

private slots:
    void onHostOrPortChanged();

private:
    SecretStore* m_secretStore;
    QLineEdit*   m_hostEdit;
    QSpinBox*    m_portSpin;
    QComboBox*   m_schemeCombo;
    QCheckBox*   m_ignoreSslCheck;
    QSpinBox*    m_intervalSpin;
    QLineEdit*   m_apiKeyEdit;
    QCheckBox*   m_autoConnectCheck;
    QString      m_lastLoadedApiKey;
    QDialogButtonBox* m_buttons;
};
