#pragma once

#include <QObject>
#include <QString>

// SecretStore: stores/retrieves the REST API key in the platform secret
// store (GNOME Keyring / KWallet via Secret Service on Linux, Credential
// Manager on Windows, Keychain on macOS) using QtKeychain
// (https://github.com/frankosterfeld/qtkeychain). The API key is never
// written to QSettings, a config file, or anywhere else on disk in
// plaintext -- only the connection's host/port/scheme are persisted there;
// the secret itself lives exclusively in the OS secret store.
//
// One key per configured connection (keyed by "host:port") so multiple
// saved connections (e.g. several powerregulator instances) don't collide.
class SecretStore : public QObject {
    Q_OBJECT
public:
    explicit SecretStore(QObject* parent = nullptr);

    // Asynchronously reads the API key for the given connection. Emits
    // apiKeyLoaded() (possibly with an empty string, if none was ever saved)
    // or apiKeyError() on a genuine secret-store failure.
    void load(const QString& host, int port);

    // Asynchronously stores/overwrites the API key for the given connection.
    // Emits apiKeySaved() or apiKeyError().
    void save(const QString& host, int port, const QString& apiKey);

    // Asynchronously removes any stored API key for the given connection.
    void remove(const QString& host, int port);

signals:
    void apiKeyLoaded(const QString& apiKey);
    void apiKeySaved();
    void apiKeyError(const QString& message);

private:
    static QString keyFor(const QString& host, int port);
};
