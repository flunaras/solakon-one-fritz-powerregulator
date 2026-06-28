#include "apiclient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSslConfiguration>
#include <QUrl>

namespace {

QDateTime parseIso8601(const QJsonValue& v) {
    if (!v.isString() || v.toString().isEmpty())
        return QDateTime();
    return QDateTime::fromString(v.toString(), Qt::ISODate);
}

ApiOverride parseOverride(const QJsonObject& o) {
    ApiOverride ov;
    ov.active = o.value("active").toBool();
    const QString mode = o.value("mode").toString();
    ov.mode = (mode == "release") ? ApiOverrideMode::Release
            : (mode == "setpoint") ? ApiOverrideMode::Setpoint
                                    : ApiOverrideMode::None;
    ov.set_at = parseIso8601(o.value("set_at"));
    ov.duration_seconds = o.value("duration_seconds").toInt();
    ov.expires_at = parseIso8601(o.value("expires_at"));
    if (ov.mode == ApiOverrideMode::Setpoint)
        ov.watts = o.value("watts").toInt();
    return ov;
}

ApiStatus parseStatus(const QJsonObject& o) {
    ApiStatus s;
    s.have_data = o.value("have_data").toBool();
    s.updated_at = parseIso8601(o.value("updated_at"));
    s.cycle_count = static_cast<qint64>(o.value("cycle_count").toDouble());

    auto readOptInt = [&o](const char* key, int& out, bool& ok) {
        const QJsonValue v = o.value(key);
        ok = v.isDouble();
        if (ok) out = v.toInt();
    };
    readOptInt("solakon_grid_power_w", s.solakon_grid_power_w, s.solakon_grid_power_ok);
    readOptInt("pv_power_w", s.pv_power_w, s.pv_power_ok);
    readOptInt("battery_power_w", s.battery_power_w, s.battery_power_ok);
    readOptInt("grid_meter_power_w", s.grid_meter_power_w, s.grid_meter_power_ok);

    bool ignored_ok;
    readOptInt("battery_soc", s.battery_soc, ignored_ok);
    if (!ignored_ok) s.battery_soc = -1;
    readOptInt("max_soc_limit", s.max_soc_limit, ignored_ok);
    if (!ignored_ok) s.max_soc_limit = -1;
    readOptInt("min_soc_limit", s.min_soc_limit, ignored_ok);
    if (!ignored_ok) s.min_soc_limit = -1;

    int tmp;
    readOptInt("inverter_status", tmp, s.inverter_status_ok);
    s.inverter_status = s.inverter_status_ok ? static_cast<quint16>(tmp) : 0;
    readOptInt("grid_status", tmp, s.grid_status_ok);
    s.grid_status = s.grid_status_ok ? static_cast<quint16>(tmp) : 0;

    s.remote_engaged = o.value("remote_engaged").toBool();
    s.owned_by_us = o.value("owned_by_us").toBool();
    s.ever_engaged = o.value("ever_engaged").toBool();
    s.low_soc_hold = o.value("low_soc_hold").toBool();
    s.low_soc_hold_since = parseIso8601(o.value("low_soc_hold_since"));

    readOptInt("last_written_w", s.last_written_w, s.has_last_written);

    return s;
}

} // namespace

ApiClient::ApiClient(QObject* parent) : QObject(parent) {
}

void ApiClient::configure(const QString& baseUrl, const QString& apiKey, bool ignoreSslErrors) {
    m_baseUrl = baseUrl;
    while (m_baseUrl.endsWith('/'))
        m_baseUrl.chop(1);
    m_apiKey = apiKey;
    m_ignoreSslErrors = ignoreSslErrors;
}

QNetworkRequest ApiClient::makeRequest(const QString& path, bool authenticated) const {
    QNetworkRequest req(QUrl(m_baseUrl + path));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (authenticated && !m_apiKey.isEmpty())
        req.setRawHeader("Authorization", ("Bearer " + m_apiKey).toUtf8());
    if (m_ignoreSslErrors) {
        QSslConfiguration ssl = req.sslConfiguration();
        ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
        req.setSslConfiguration(ssl);
    }
    return req;
}

void ApiClient::handleReply(QNetworkReply* reply, const QString& endpoint,
                            const std::function<void(const QByteArray&)>& onSuccess) {
    connect(reply, &QNetworkReply::sslErrors, reply,
            [this, reply](const QList<QSslError>&) {
                if (m_ignoreSslErrors)
                    reply->ignoreSslErrors();
            });
    connect(reply, &QNetworkReply::finished, this, [this, reply, endpoint, onSuccess]() {
        reply->deleteLater();
        const int httpStatus =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();

        if (reply->error() != QNetworkReply::NoError && httpStatus == 0) {
            emit requestFailed(endpoint, 0, reply->errorString());
            return;
        }
        if (httpStatus < 200 || httpStatus >= 300) {
            QString message = reply->errorString();
            const QJsonDocument doc = QJsonDocument::fromJson(body);
            if (doc.isObject() && doc.object().contains("error"))
                message = doc.object().value("error").toString();
            emit requestFailed(endpoint, httpStatus, message);
            return;
        }
        onSuccess(body);
    });
}

void ApiClient::checkHealth() {
    auto* reply = m_nam.get(makeRequest("/api/v1/health", /*authenticated=*/false));
    handleReply(reply, "health", [this](const QByteArray&) { emit healthOk(); });
}

void ApiClient::fetchStatus() {
    auto* reply = m_nam.get(makeRequest("/api/v1/status", /*authenticated=*/true));
    handleReply(reply, "status", [this](const QByteArray& body) {
        const QJsonObject o = QJsonDocument::fromJson(body).object();
        const ApiStatus status = parseStatus(o);
        const ApiOverride ov = o.contains("override")
                                  ? parseOverride(o.value("override").toObject())
                                  : ApiOverride{};
        emit statusReceived(status, ov);
    });
}

void ApiClient::postOverride(const QByteArray& jsonBody) {
    auto* reply = m_nam.post(makeRequest("/api/v1/override", /*authenticated=*/true), jsonBody);
    handleReply(reply, "override", [this](const QByteArray& body) {
        const ApiOverride ov = parseOverride(QJsonDocument::fromJson(body).object());
        emit overrideApplied(ov);
    });
}

void ApiClient::setOverrideSetpoint(int watts, int durationSeconds) {
    QJsonObject body;
    body["watts"] = watts;
    body["duration_seconds"] = durationSeconds;
    postOverride(QJsonDocument(body).toJson(QJsonDocument::Compact));
}

void ApiClient::setOverrideRelease(int durationSeconds) {
    QJsonObject body;
    body["release"] = true;
    body["duration_seconds"] = durationSeconds;
    postOverride(QJsonDocument(body).toJson(QJsonDocument::Compact));
}

void ApiClient::clearOverride() {
    auto* reply = m_nam.deleteResource(makeRequest("/api/v1/override", /*authenticated=*/true));
    handleReply(reply, "override", [this](const QByteArray&) { emit overrideCleared(); });
}

void ApiClient::fetchLowSocHold() {
    auto* reply = m_nam.get(makeRequest("/api/v1/low-soc-hold", /*authenticated=*/true));
    handleReply(reply, "low-soc-hold", [this](const QByteArray& body) {
        const QJsonObject o = QJsonDocument::fromJson(body).object();
        emit lowSocHoldApplied(o.value("active").toBool());
    });
}

void ApiClient::setLowSocHold(bool active) {
    QJsonObject body;
    body["active"] = active;
    auto* reply = m_nam.post(makeRequest("/api/v1/low-soc-hold", /*authenticated=*/true),
                              QJsonDocument(body).toJson(QJsonDocument::Compact));
    handleReply(reply, "low-soc-hold", [this](const QByteArray& respBody) {
        const QJsonObject o = QJsonDocument::fromJson(respBody).object();
        emit lowSocHoldApplied(o.value("active").toBool());
    });
}
