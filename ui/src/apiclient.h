#pragma once

#include "apistatus.h"

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QString>

#include <functional>

// ApiClient: thin asynchronous REST client for the powerregulator's own
// HTTP API (src/restapi.cpp/.h in the parent project). One instance per
// connection; reconfigure via configure() when the connection dialog
// changes host/port/scheme/key rather than creating a new instance, so any
// in-flight requests' QNetworkReply signal connections remain valid.
//
// Threading: all calls must happen on the GUI thread. QNetworkAccessManager
// dispatches replies via the event loop (no worker threads of our own).
class ApiClient : public QObject {
    Q_OBJECT
public:
    explicit ApiClient(QObject* parent = nullptr);

    // baseUrl: e.g. "https://192.168.1.50:8080" (no trailing slash, no path).
    // apiKey: presented as "Authorization: Bearer <apiKey>" on every request
    // except /health. Read from the platform secret store by the caller
    // (see SecretStore) -- never persisted by ApiClient itself.
    void configure(const QString& baseUrl, const QString& apiKey, bool ignoreSslErrors);

    [[nodiscard]] bool isConfigured() const { return !m_baseUrl.isEmpty(); }
    [[nodiscard]] QString baseUrl() const { return m_baseUrl; }

    // GET /api/v1/health -- unauthenticated liveness probe.
    void checkHealth();

    // GET /api/v1/status -- full snapshot, including embedded "override".
    void fetchStatus();

    // POST /api/v1/override {"watts": <int>, "duration_seconds": <int>}
    void setOverrideSetpoint(int watts, int durationSeconds);

    // POST /api/v1/override {"release": true, "duration_seconds": <int>}
    void setOverrideRelease(int durationSeconds);

    // DELETE /api/v1/override
    void clearOverride();

    // GET /api/v1/low-soc-hold -- current low-SoC hold state (also present
    // embedded in fetchStatus()'s ApiStatus as low_soc_hold/low_soc_hold_since).
    void fetchLowSocHold();

    // POST /api/v1/low-soc-hold {"active": <bool>} -- manually set/clear the
    // low-SoC hold. Setting true forces an immediate release of remote
    // control (if currently engaged); setting false only clears the hold --
    // the control loop's normal recover logic still decides re-engagement.
    void setLowSocHold(bool active);

signals:
    void healthOk();
    void statusReceived(const ApiStatus& status, const ApiOverride& override);
    void overrideApplied(const ApiOverride& override);
    void overrideCleared();
    // active/since reflect the value the server accepted (pending, per the
    // 202 Accepted response -- the control loop applies it on its next cycle).
    void lowSocHoldApplied(bool active);
    // httpStatus == 0 for a transport-level failure (no HTTP response at all).
    void requestFailed(const QString& endpoint, int httpStatus, const QString& message);

private:
    QNetworkAccessManager m_nam;
    QString m_baseUrl;
    QString m_apiKey;
    bool    m_ignoreSslErrors = false;

    [[nodiscard]] QNetworkRequest makeRequest(const QString& path, bool authenticated) const;
    void postOverride(const QByteArray& jsonBody);
    void handleReply(QNetworkReply* reply, const QString& endpoint,
                      const std::function<void(const QByteArray&)>& onSuccess);
};
