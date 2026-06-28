#include "restapi.h"
#include "logger.h"

#include <nlohmann/json.hpp>

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <chrono>
#include <cstdio>
#include <ctime>

using json = nlohmann::json;

namespace {

// Formats a system_clock time_point as an ISO-8601 UTC string, or an empty
// string for a default-constructed (never-set) time_point.
std::string isoTimestamp(const std::chrono::system_clock::time_point& tp) {
    if (tp.time_since_epoch().count() == 0)
        return "";
    const auto t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

json snapshotToJson(const ApiSnapshot& s) {
    json j;
    j["have_data"] = s.have_data;
    j["updated_at"] = isoTimestamp(s.updated_at);
    j["cycle_count"] = s.cycle_count;

    j["solakon_grid_power_w"]  = s.solakon_grid_power_ok ? json(s.solakon_grid_power_w) : json(nullptr);
    j["pv_power_w"]        = s.pv_ok        ? json(s.pv_w)       : json(nullptr);
    j["battery_power_w"]  = s.battery_ok   ? json(s.battery_w)  : json(nullptr);
    j["battery_soc"]       = (s.soc >= 0)        ? json(s.soc)          : json(nullptr);
    j["max_soc_limit"]     = (s.max_soc_limit >= 0) ? json(s.max_soc_limit) : json(nullptr);
    j["min_soc_limit"]     = (s.min_soc_limit >= 0) ? json(s.min_soc_limit) : json(nullptr);
    j["inverter_status"]   = s.inverter_status_ok ? json(s.inverter_status) : json(nullptr);
    j["grid_status"]       = s.grid_status_ok     ? json(s.grid_status)     : json(nullptr);

    j["grid_meter_power_w"] = s.grid_meter_power_ok ? json(s.grid_meter_power_w) : json(nullptr);

    j["remote_engaged"]   = s.remote_engaged;
    j["owned_by_us"]       = s.owned_by_us;
    j["ever_engaged"]      = s.ever_engaged;
    j["low_soc_hold"]      = s.low_soc_hold;
    j["low_soc_hold_since"] = isoTimestamp(s.low_soc_hold_since);
    j["last_written_w"]   = s.has_last_written ? json(s.last_written) : json(nullptr);

    return j;
}

json overrideToJson(const ManualOverride& o) {
    json j;
    j["active"]           = o.active;
    j["mode"]             = (o.mode == OverrideMode::Release) ? "release" : "setpoint";
    j["set_at"]           = isoTimestamp(o.set_at);
    j["duration_seconds"] = o.duration_s; // meaning depends on mode -- see ManualOverride

    // expires_at is derived from set_at + duration_seconds the same way
    // regardless of mode -- for Setpoint, duration_seconds is the Modbus
    // REMOTE_TIMEOUT_SET revert timeout rather than the override's own
    // lifetime (see ManualOverride::duration_s), so this does NOT mean the
    // override itself will actually clear itself at that time for Setpoint
    // mode; it is surfaced anyway so clients can show "how long until the
    // configured timeout" consistently across both modes rather than only
    // for Release.
    if (o.active && o.duration_s > 0) {
        j["expires_at"] = isoTimestamp(o.set_at + std::chrono::seconds(o.duration_s));
    } else {
        j["expires_at"] = nullptr; // indefinite (or using the loop's own timeout), or not active
    }

    if (o.mode == OverrideMode::Setpoint) {
        j["watts"] = o.watts;
    }

    return j;
}

json lowSocHoldToJson(const ApiSnapshot& s) {
    json j;
    j["active"] = s.low_soc_hold;
    j["since"]  = isoTimestamp(s.low_soc_hold_since);
    return j;
}

void sendJson(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

void sendError(httplib::Response& res, int status, const std::string& message) {
    sendJson(res, status, json{{"error", message}});
}

// Generates an RSA-2048, SHA-256 self-signed X.509 certificate entirely in
// memory (never touches disk).  On success, *pkey_out/*cert_out are set to
// newly allocated OpenSSL objects that the caller owns and must free with
// EVP_PKEY_free()/X509_free(); on failure, both are left untouched and false
// is returned with err set to a human-readable message.
//
// Used only when api_tls_enabled is set but no external certificate/key pair
// was configured -- convenient for local/LAN use, but API clients must either
// disable certificate verification or explicitly trust/pin the generated
// certificate; it is not suitable for a public-facing deployment (there is no
// CA behind it, and a new certificate is generated on every process restart).
bool generateSelfSignedCert(EVP_PKEY** pkey_out, X509** cert_out,
                            int valid_days, const std::string& common_name,
                            std::string& err) {
    EVP_PKEY* pkey = EVP_PKEY_new();
    if (!pkey) {
        err = "EVP_PKEY_new() failed";
        return false;
    }

    RSA* rsa = RSA_new();
    BIGNUM* e = BN_new();
    bool ok = rsa && e && BN_set_word(e, RSA_F4)
              && RSA_generate_key_ex(rsa, 2048, e, nullptr);
    BN_free(e);
    if (!ok) {
        err = "RSA key generation failed";
        if (rsa) RSA_free(rsa);
        EVP_PKEY_free(pkey);
        return false;
    }

    // EVP_PKEY_assign_RSA transfers ownership of rsa to pkey on success --
    // rsa must NOT be freed separately afterwards.
    if (!EVP_PKEY_assign_RSA(pkey, rsa)) {
        err = "EVP_PKEY_assign_RSA() failed";
        RSA_free(rsa);
        EVP_PKEY_free(pkey);
        return false;
    }

    X509* cert = X509_new();
    if (!cert) {
        err = "X509_new() failed";
        EVP_PKEY_free(pkey);
        return false;
    }

    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert),
                     static_cast<long>(60) * 60 * 24 * valid_days);
    X509_set_pubkey(cert, pkey);
    X509_set_version(cert, 2); // X.509 v3

    X509_NAME* name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(
        name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>(common_name.c_str()), -1, -1, 0);
    // Self-signed: issuer == subject.
    X509_set_issuer_name(cert, name);

    if (!X509_sign(cert, pkey, EVP_sha256())) {
        err = "X509_sign() failed";
        X509_free(cert);
        EVP_PKEY_free(pkey);
        return false;
    }

    *pkey_out = pkey;
    *cert_out = cert;
    return true;
}

} // namespace

RestApi::RestApi(ApiState& state, const Config& cfg)
    : m_state(state), m_cfg(cfg) {
}

RestApi::~RestApi() {
    stop();
}

bool RestApi::requireAuth(const httplib::Request& req, httplib::Response& res) const {
    // cfg.api_key is guaranteed non-empty by start() (which refuses to run
    // otherwise) -- so any comparison here is against a real configured secret.
    std::string presented;

    if (auto it = req.headers.find("Authorization"); it != req.headers.end()) {
        static constexpr char kBearerPrefix[] = "Bearer ";
        constexpr std::size_t kBearerPrefixLen = sizeof(kBearerPrefix) - 1;
        if (it->second.compare(0, kBearerPrefixLen, kBearerPrefix) == 0)
            presented = it->second.substr(kBearerPrefixLen);
    }
    if (presented.empty()) {
        if (auto it = req.headers.find("X-API-Key"); it != req.headers.end())
            presented = it->second;
    }

    if (presented.empty() || presented != m_cfg.api_key) {
        sendError(res, 401, "Missing or invalid API key. Provide it via "
                             "'Authorization: Bearer <key>' or 'X-API-Key: <key>'.");
        return false;
    }
    return true;
}

void RestApi::setupRoutes() {
    // Unauthenticated liveness probe -- reveals nothing about inverter/grid state.
    m_server->Get("/api/v1/health", [](const httplib::Request&, httplib::Response& res) {
        sendJson(res, 200, json{{"status", "ok"}});
    });

    m_server->Get("/api/v1/status", [this](const httplib::Request& req, httplib::Response& res) {
        if (!requireAuth(req, res)) return;
        std::lock_guard<std::mutex> lock(m_state.mutex);
        json j = snapshotToJson(m_state.snapshot);
        j["override"] = overrideToJson(m_state.override_state);
        sendJson(res, 200, j);
    });

    m_server->Get("/api/v1/override", [this](const httplib::Request& req, httplib::Response& res) {
        if (!requireAuth(req, res)) return;
        std::lock_guard<std::mutex> lock(m_state.mutex);
        sendJson(res, 200, overrideToJson(m_state.override_state));
    });

    m_server->Post("/api/v1/override", [this](const httplib::Request& req, httplib::Response& res) {
        if (!requireAuth(req, res)) return;

        json body;
        try {
            body = json::parse(req.body);
        } catch (const json::exception&) {
            sendError(res, 400, "Request body must be valid JSON.");
            return;
        }

        // Two mutually exclusive shapes are accepted:
        //   {"watts": <int>, "duration_seconds": <int, optional>}
        //     -- OverrideMode::Setpoint: command an explicit setpoint every cycle;
        //        duration_seconds sets the Modbus revert timeout for each write.
        //   {"release": true, "duration_seconds": <int, optional>}
        //     -- OverrideMode::Release: force-release remote control;
        //        duration_seconds sets how long to stay released.
        // duration_seconds is named identically in both shapes (see
        // ManualOverride::duration_s for the difference in meaning per mode).
        const bool wants_release = body.contains("release")
                                 && body["release"].is_boolean()
                                 && body["release"].get<bool>();

        if (wants_release && body.contains("watts")) {
            sendError(res, 400, "Request body must not contain both \"release\" and "
                                 "\"watts\" -- these are mutually exclusive override modes.");
            return;
        }

        ManualOverride ov;
        ov.active     = true;
        ov.set_at     = std::chrono::system_clock::now();
        ov.duration_s = 0;
        if (body.contains("duration_seconds")) {
            if (!body["duration_seconds"].is_number_integer()
                || body["duration_seconds"].get<int>() < 0) {
                sendError(res, 400, "\"duration_seconds\" must be a non-negative integer.");
                return;
            }
            ov.duration_s = body["duration_seconds"].get<int>();
        }

        if (wants_release) {
            ov.mode = OverrideMode::Release;
        } else {
            if (!body.contains("watts") || !body["watts"].is_number_integer()) {
                sendError(res, 400, "Request body must contain either an integer field "
                                     "\"watts\" (positive = export, negative = import) or "
                                     "\"release\": true to force-release remote control.");
                return;
            }
            ov.mode  = OverrideMode::Setpoint;
            ov.watts = body["watts"].get<int>();
        }

        {
            std::lock_guard<std::mutex> lock(m_state.mutex);
            m_state.override_state = ov;
        }

        auto& log = Logger::instance();
        if (log.is_enabled(LogLevel::INFO)) {
            if (ov.mode == OverrideMode::Release) {
                log.info("REST API: manual force-release requested"
                         + (ov.duration_s > 0
                                ? " for " + std::to_string(ov.duration_s) + " s"
                                : std::string(" (indefinite, until cleared)")));
            } else {
                log.info("REST API: manual override set to " + std::to_string(ov.watts)
                         + " W" + (ov.duration_s > 0
                                      ? " (timeout " + std::to_string(ov.duration_s) + " s)"
                                      : " (using loop's default timeout)"));
            }
        }

        sendJson(res, 200, overrideToJson(ov));
    });

    m_server->Delete("/api/v1/override", [this](const httplib::Request& req, httplib::Response& res) {
        if (!requireAuth(req, res)) return;

        {
            std::lock_guard<std::mutex> lock(m_state.mutex);
            m_state.override_state = ManualOverride{};
        }

        auto& log = Logger::instance();
        if (log.is_enabled(LogLevel::INFO))
            log.info("REST API: manual override cleared -- resuming normal control");

        sendJson(res, 200, json{{"cleared", true}});
    });

    m_server->Get("/api/v1/low-soc-hold", [this](const httplib::Request& req, httplib::Response& res) {
        if (!requireAuth(req, res)) return;
        std::lock_guard<std::mutex> lock(m_state.mutex);
        sendJson(res, 200, lowSocHoldToJson(m_state.snapshot));
    });

    m_server->Post("/api/v1/low-soc-hold", [this](const httplib::Request& req, httplib::Response& res) {
        if (!requireAuth(req, res)) return;

        json body;
        try {
            body = json::parse(req.body);
        } catch (const json::exception&) {
            sendError(res, 400, "Request body must be valid JSON.");
            return;
        }

        if (!body.contains("active") || !body["active"].is_boolean()) {
            sendError(res, 400, "Request body must contain a boolean field \"active\".");
            return;
        }

        LowSocHoldCommand cmd;
        cmd.pending      = true;
        cmd.active       = body["active"].get<bool>();
        cmd.requested_at = std::chrono::system_clock::now();

        {
            std::lock_guard<std::mutex> lock(m_state.mutex);
            m_state.low_soc_hold_command = cmd;
        }

        auto& log = Logger::instance();
        if (log.is_enabled(LogLevel::INFO))
            log.info(std::string("REST API: manual low-SoC hold command: ")
                      + (cmd.active ? "set (releasing remote control if engaged)"
                                    : "clear"));

        // The command is only *queued* here -- it takes effect once the
        // control loop consumes it on its next cycle (see runOnce in
        // main.cpp), which is when snapshot.low_soc_hold actually flips and
        // gets persisted to --state-file.  Echo back the requested value
        // immediately so the caller/UI can show it as pending.
        sendJson(res, 202, json{{"active", cmd.active}, {"pending", true}});
    });

    m_server->set_error_handler([](const httplib::Request&, httplib::Response& res) {
        if (res.body.empty())
            sendError(res, res.status, "Not found.");
    });
}

bool RestApi::createServer() {
    auto& log = Logger::instance();

    if (!m_cfg.api_tls_enabled) {
        m_server = std::make_unique<httplib::Server>();
        return true;
    }

    const bool have_cert = !m_cfg.api_tls_cert_file.empty();
    const bool have_key  = !m_cfg.api_tls_key_file.empty();
    if (have_cert != have_key) {
        log.error("REST API: --api-tls-cert-file and --api-tls-key-file must both be "
                  "set together, or both left empty to auto-generate a self-signed "
                  "certificate.");
        return false;
    }

    if (have_cert) {
        auto ssl_server = std::make_unique<httplib::SSLServer>(
            m_cfg.api_tls_cert_file.c_str(), m_cfg.api_tls_key_file.c_str());
        if (!ssl_server->is_valid()) {
            log.error("REST API: failed to load TLS certificate/key from '"
                      + m_cfg.api_tls_cert_file + "' / '" + m_cfg.api_tls_key_file + "'");
            return false;
        }
        if (log.is_enabled(LogLevel::INFO))
            log.info("REST API: using TLS certificate '" + m_cfg.api_tls_cert_file + "'");
        m_server = std::move(ssl_server);
        return true;
    }

    // Neither file configured: generate a self-signed certificate in memory.
    EVP_PKEY* pkey = nullptr;
    X509*     cert = nullptr;
    std::string err;
    if (!generateSelfSignedCert(&pkey, &cert, /*valid_days=*/825,
                                 "solakon-one-fritz-powerregulator", err)) {
        log.error("REST API: failed to generate self-signed TLS certificate: " + err);
        return false;
    }

    auto ssl_server = std::make_unique<httplib::SSLServer>(cert, pkey);
    // SSL_CTX_use_certificate()/SSL_CTX_use_PrivateKey() (called internally by
    // the SSLServer constructor above) increment OpenSSL's own reference
    // counts on cert/pkey, so our copies must still be freed here regardless
    // of whether construction succeeded.
    X509_free(cert);
    EVP_PKEY_free(pkey);

    if (!ssl_server->is_valid()) {
        log.error("REST API: failed to initialize TLS context with the generated "
                  "self-signed certificate");
        return false;
    }

    if (log.is_enabled(LogLevel::WARN))
        log.warn("REST API: --api-tls-enabled is set but no --api-tls-cert-file/"
                 "--api-tls-key-file was configured -- using an auto-generated, "
                 "in-memory self-signed certificate. Clients must disable certificate "
                 "verification or explicitly trust/pin this certificate; a new one is "
                 "generated on every restart. Not suitable for public-facing deployments.");

    m_server = std::move(ssl_server);
    return true;
}

bool RestApi::start() {
    auto& log = Logger::instance();

    if (m_cfg.api_key.empty()) {
        log.error("REST API: --api-key must be set (non-empty) when --api-enabled is used; "
                  "refusing to start the API with no authentication configured.");
        return false;
    }

    if (!createServer())
        return false;

    setupRoutes();

    if (!m_server->bind_to_port(m_cfg.api_host.c_str(), m_cfg.api_port)) {
        log.error("REST API: failed to bind to " + m_cfg.api_host + ":"
                  + std::to_string(m_cfg.api_port));
        m_server.reset();
        return false;
    }

    m_running = true;
    m_thread = std::thread([this]() {
        m_server->listen_after_bind();
    });

    if (log.is_enabled(LogLevel::INFO))
        log.info(std::string("REST API: listening on ")
                 + (m_cfg.api_tls_enabled ? "https://" : "http://")
                 + m_cfg.api_host + ":" + std::to_string(m_cfg.api_port)
                 + " (endpoints under /api/v1)");

    return true;
}

void RestApi::stop() {
    if (!m_running) return;
    m_server->stop();
    if (m_thread.joinable())
        m_thread.join();
    m_running = false;
}
