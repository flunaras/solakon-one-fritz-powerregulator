#include "fritzapi.h"
#include "logger.h"

// cpp-httplib is header-only; CPPHTTPLIB_OPENSSL_SUPPORT is set by its CMake target.
#include <httplib.h>

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string stripSpacesImpl(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        if (c != ' ') out += c;
    return out;
}

// Hex-encode a binary buffer.
std::string FritzApi::hexEncode(const unsigned char* data, size_t len) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i)
        ss << std::setw(2) << static_cast<unsigned>(data[i]);
    return ss.str();
}

// Parse the first occurrence of <Tag>value</Tag> from xml.
static std::string parseXmlTag(const std::string& xml, const std::string& tag) {
    const std::string open  = "<" + tag + ">";
    const std::string close = "</" + tag + ">";
    auto s = xml.find(open);
    if (s == std::string::npos) return {};
    s += open.size();
    auto e = xml.find(close, s);
    if (e == std::string::npos) return {};
    return xml.substr(s, e - s);
}

std::string FritzApi::parseSid(const std::string& xml) {
    return parseXmlTag(xml, "SID");
}

std::string FritzApi::parseChallenge(const std::string& xml) {
    return parseXmlTag(xml, "Challenge");
}

std::string FritzApi::stripSpaces(const std::string& ain) {
    return stripSpacesImpl(ain);
}

// ---------------------------------------------------------------------------
// FritzApi implementation
// ---------------------------------------------------------------------------

FritzApi::FritzApi()  = default;
FritzApi::~FritzApi() = default;

void FritzApi::setHost(const std::string& host)       { m_host       = host; }
void FritzApi::setScheme(const std::string& scheme)   { m_scheme     = scheme; }
void FritzApi::setIgnoreSsl(bool ignore)               { m_ignore_ssl = ignore; }
void FritzApi::setCredentials(const std::string& username,
                               const std::string& password) {
    m_username = username;
    m_password = password;
}

std::string FritzApi::lastError() const { return m_last_error; }

// ---------------------------------------------------------------------------
// Internal: build an httplib client for the configured scheme + host.
// httplib::Client's URL-based constructor handles both http:// and https://.
// ---------------------------------------------------------------------------
static httplib::Client makeClient(const std::string& scheme,
                                  const std::string& host,
                                  bool               ignore_ssl) {
    httplib::Client cli(scheme + "://" + host);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(15);
    cli.enable_server_certificate_verification(!ignore_ssl);
    return cli;
}

// ---------------------------------------------------------------------------
// Authentication
// ---------------------------------------------------------------------------

// Hex-decode a hex string into a byte vector.  Returns false on invalid input.
static bool hexDecode(const std::string& hex, std::vector<unsigned char>& out) {
    if (hex.size() % 2 != 0) return false;
    out.resize(hex.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        const char hi = hex[2 * i];
        const char lo = hex[2 * i + 1];
        auto hexVal = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int h = hexVal(hi);
        const int l = hexVal(lo);
        if (h < 0 || l < 0) return false;
        out[i] = static_cast<unsigned char>((h << 4) | l);
    }
    return true;
}

// Compute PBKDF2-HMAC-SHA256 based challenge response.
// FRITZ!Box challenge format (modern): "<version>$<iter1>$<salt1_hex>$<iter2>$<salt2_hex>"
// Response: "<salt2_hex>$<hex(PBKDF2(PBKDF2(password, salt1, iter1), salt2, iter2))>"
static bool pbkdf2Response(const std::string& challenge,
                            const std::string& password,
                            std::string&       response_out) {
    // Split by '$': version, iter1, salt1_hex, iter2, salt2_hex
    std::vector<std::string> parts;
    std::istringstream ss(challenge);
    std::string token;
    while (std::getline(ss, token, '$'))
        parts.push_back(token);

    // Accept both the 5-part modern format (version$iter1$salt1$iter2$salt2)
    // and the legacy 4-part format without a version prefix (iter1$salt1$iter2$salt2).
    if (parts.size() == 5) {
        // Drop the leading version field so the rest of the indices are uniform.
        parts.erase(parts.begin());
    }
    if (parts.size() != 4) return false;

    const int         iter1    = std::stoi(parts[0]);
    const std::string salt1_hex = parts[1];
    const int         iter2    = std::stoi(parts[2]);
    const std::string salt2_hex = parts[3];

    // The salts are transmitted as hex strings; decode them to raw bytes.
    std::vector<unsigned char> salt1_bytes;
    std::vector<unsigned char> salt2_bytes;
    if (!hexDecode(salt1_hex, salt1_bytes)) return false;
    if (!hexDecode(salt2_hex, salt2_bytes)) return false;

    // First PBKDF2 pass: password + salt1 + iter1 → intermediate key.
    std::array<unsigned char, 32> key1{};
    if (!PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()),
                             salt1_bytes.data(), static_cast<int>(salt1_bytes.size()),
                             iter1, EVP_sha256(), 32, key1.data()))
        return false;

    // Second PBKDF2 pass: key1 + salt2 + iter2 → final key.
    std::array<unsigned char, 32> key2{};
    if (!PKCS5_PBKDF2_HMAC(reinterpret_cast<const char*>(key1.data()), 32,
                             salt2_bytes.data(), static_cast<int>(salt2_bytes.size()),
                             iter2, EVP_sha256(), 32, key2.data()))
        return false;

    // Response: "<salt2_hex>$<hex(key2)>"
    std::ostringstream out;
    out << salt2_hex << '$' << std::hex << std::setfill('0');
    for (unsigned char b : key2)
        out << std::setw(2) << static_cast<unsigned>(b);
    response_out = out.str();
    return true;
}

// Compute legacy MD5 challenge response.
// Challenge: 32-char hex string.
// Response: MD5(challenge + "-" + password), where password is encoded as UTF-16LE.
static bool md5Response(const std::string& challenge,
                         const std::string& password,
                         std::string&       response_out) {
    // Build the UTF-16LE string: challenge '-' password encoded as UTF-16LE.
    // For ASCII passwords this is just inserting a NUL byte after each char.
    const std::string sep   = challenge + "-";
    std::string input_utf16;
    // Encode separator as UTF-16LE (ASCII range only).
    for (char c : sep) {
        input_utf16 += c;
        input_utf16 += '\0';
    }
    // Encode password as UTF-16LE.  Non-BMP characters are not handled (rare in practice).
    for (unsigned char c : password) {
        if (c < 0x80) {
            input_utf16 += static_cast<char>(c);
            input_utf16 += '\0';
        } else {
            // Replace characters > 0x7F with '.' (same as AVM reference implementation).
            input_utf16 += '.';
            input_utf16 += '\0';
        }
    }

    // Compute MD5 via EVP (avoids the deprecated MD5() one-shot function in OpenSSL 3.0+).
    unsigned char digest[16]{};
    unsigned int  digest_len = sizeof(digest);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return false;
    const bool ok =
        EVP_DigestInit_ex(ctx, EVP_md5(), nullptr) == 1 &&
        EVP_DigestUpdate(ctx,
                         reinterpret_cast<const unsigned char*>(input_utf16.data()),
                         input_utf16.size()) == 1 &&
        EVP_DigestFinal_ex(ctx, digest, &digest_len) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok) return false;

    // Response: "<challenge>-<hex(md5)>"
    std::ostringstream out;
    out << challenge << '-' << std::hex << std::setfill('0');
    for (unsigned char b : digest)
        out << std::setw(2) << static_cast<unsigned>(b);
    response_out = out.str();
    return true;
}

bool FritzApi::computeLoginResponse(const std::string& challenge,
                                     std::string&       response_out) {
    // PBKDF2 challenges contain '$'; legacy challenges are plain 32-char hex.
    if (challenge.find('$') != std::string::npos)
        return pbkdf2Response(challenge, m_password, response_out);
    return md5Response(challenge, m_password, response_out);
}

bool FritzApi::login() {
    auto& log = Logger::instance();

    auto cli = makeClient(m_scheme, m_host, m_ignore_ssl);

    // Step 1: fetch challenge.
    if (log.is_enabled(LogLevel::DBG))
        log.debug("FritzApi::login() GET /login_sid.lua?version=2");

    auto res = cli.Get("/login_sid.lua?version=2");
    if (!res || res->status != 200) {
        m_last_error = "login: failed to fetch challenge";
        if (res) m_last_error += " (HTTP " + std::to_string(res->status) + ")";
        return false;
    }

    const std::string challenge = parseChallenge(res->body);
    if (challenge.empty()) {
        m_last_error = "login: no <Challenge> in response";
        return false;
    }

    if (log.is_enabled(LogLevel::TRACE))
        log.trace("FritzApi::login() challenge=" + challenge);

    // Step 2: compute response.
    std::string response;
    if (!computeLoginResponse(challenge, response)) {
        m_last_error = "login: failed to compute challenge response";
        return false;
    }

    // Step 3: submit response.
    const std::string url = "/login_sid.lua?version=2&username=" + m_username
                          + "&response=" + response;

    if (log.is_enabled(LogLevel::DBG))
        log.debug("FritzApi::login() GET " + url);

    auto res2 = cli.Get(url.c_str());
    if (!res2 || res2->status != 200) {
        m_last_error = "login: auth request failed";
        if (res2) m_last_error += " (HTTP " + std::to_string(res2->status) + ")";
        return false;
    }

    const std::string sid = parseSid(res2->body);
    if (sid == "0000000000000000" || sid.empty()) {
        m_last_error = "login: authentication failed (wrong credentials?)";
        return false;
    }

    m_sid = sid;
    if (log.is_enabled(LogLevel::INFO))
        log.info("FritzApi: logged in successfully");

    return true;
}

// ---------------------------------------------------------------------------
// Device list
// ---------------------------------------------------------------------------

bool FritzApi::fetchDeviceList(std::vector<FritzDevice>& devices_out) {
    auto& log = Logger::instance();

    if (log.is_enabled(LogLevel::DBG))
        log.debug("FritzApi::fetchDeviceList()");

    auto cli = makeClient(m_scheme, m_host, m_ignore_ssl);

    httplib::Headers headers{{"Authorization", "AVM-SID " + m_sid}};

    auto res = cli.Get("/api/v0/smarthome/overview", headers);
    if (!res) {
        m_last_error = "fetchDeviceList: network error";
        return false;
    }

    // Re-login on 401 and retry once.
    if (res->status == 401) {
        if (log.is_enabled(LogLevel::WARN))
            log.warn("FritzApi: session expired, re-logging in");
        if (!login()) return false;
        headers = {{"Authorization", "AVM-SID " + m_sid}};
        res = cli.Get("/api/v0/smarthome/overview", headers);
        if (!res || res->status != 200) {
            m_last_error = "fetchDeviceList: failed after re-login";
            if (res) m_last_error += " (HTTP " + std::to_string(res->status) + ")";
            return false;
        }
    }

    if (res->status != 200) {
        m_last_error = "fetchDeviceList: HTTP " + std::to_string(res->status);
        return false;
    }

    if (log.is_enabled(LogLevel::TRACE))
        log.trace("FritzApi: overview response: " + res->body);

    // Parse JSON.
    //
    // The FRITZ!Box REST overview response has three top-level arrays:
    //   "devices" — physical device objects  (ain, name, isConnected, unitUids[])
    //   "groups"  — FRITZ!Box group objects  (ain, name, isConnected, unitUid)
    //   "units"   — unit objects             (UID, deviceUid, interfaces{})
    //
    // Presence is "isConnected" (bool) on the device/group object.
    // Energy-meter capability and live power live in
    //   units[].interfaces.multimeterInterface.power  (mW → W).
    // There is no numeric "functionbitmask" field; capability is inferred from
    // the presence of the interface key in units[].interfaces.
    try {
        auto j = json::parse(res->body);
        devices_out.clear();

        // ---- Pass 1: build a map from unit UID → unit JSON node ----
        // We use this to resolve the device's unitUids to their interface data.
        std::unordered_map<std::string, const json*> unitByUid;
        // We need stable storage for the json nodes; just keep a reference into j.
        if (j.contains("units") && j["units"].is_array()) {
            for (const auto& u : j["units"]) {
                if (u.contains("UID") && u["UID"].is_string())
                    unitByUid[u["UID"].get<std::string>()] = &u;
            }
        }

        // ---- Helper: extract energy-meter data from a list of unit UIDs ----
        auto readUnitInterfaces = [&](const json& uidArr, bool isArray,
                                      bool& hasEM_out, double& powerW_out,
                                      std::string& deviceType_out) {
            // Collect UIDs to check (device has an array; group has a single string).
            std::vector<std::string> uids;
            if (isArray && uidArr.is_array()) {
                for (const auto& u : uidArr)
                    if (u.is_string()) uids.push_back(u.get<std::string>());
            } else if (uidArr.is_string()) {
                uids.push_back(uidArr.get<std::string>());
            }

            // Map of FRITZ!Box interface key → human-readable capability name.
            // Ordered to produce a stable, deterministic deviceType string.
            static const std::pair<const char*, const char*> kIfaceNames[] = {
                {"onOffInterface",        "switch"},
                {"multimeterInterface",   "energyMeter"},
                {"temperatureInterface",  "temperatureSensor"},
                {"thermostatInterface",   "thermostat"},
                {"levelControlInterface", "dimmer"},
                {"colorControlInterface", "colorBulb"},
                {"blindInterface",        "blind"},
                {"humidityInterface",     "humiditySensor"},
                {"alertInterface",        "alarm"},
            };

            for (const auto& uid : uids) {
                auto it = unitByUid.find(uid);
                if (it == unitByUid.end()) continue;
                const json& u = *it->second;
                if (!u.contains("interfaces")) continue;
                const auto& ifaces = u["interfaces"];

                // Energy-meter data (power reading).
                if (ifaces.contains("multimeterInterface")) {
                    hasEM_out = true;
                    const auto& mm = ifaces["multimeterInterface"];
                    if (mm.contains("power") && !mm["power"].is_null())
                        powerW_out = mm["power"].get<double>() / 1000.0;
                }

                // Build deviceType string from present interface keys.
                for (const auto& [ifaceKey, capName] : kIfaceNames) {
                    if (ifaces.contains(ifaceKey)) {
                        if (!deviceType_out.empty()) deviceType_out += ',';
                        deviceType_out += capName;
                    }
                }
            }
        };

        // ---- Pass 2: parse device objects ----
        auto parseDevices = [&](const json& arr, bool unitUidsIsArray) {
            for (const auto& d : arr) {
                FritzDevice dev;

                if (d.contains("ain") && d["ain"].is_string())
                    dev.ain = stripSpaces(d["ain"].get<std::string>());

                if (d.contains("name") && d["name"].is_string())
                    dev.name = d["name"].get<std::string>();

                if (d.contains("productName") && d["productName"].is_string())
                    dev.productName = d["productName"].get<std::string>();

                // Presence: "isConnected" (bool).
                if (d.contains("isConnected") && d["isConnected"].is_boolean())
                    dev.present = d["isConnected"].get<bool>();

                // Energy meter, live power, and capability list: resolved via unit interfaces.
                const std::string uidKey = unitUidsIsArray ? "unitUids" : "unitUid";
                if (d.contains(uidKey))
                    readUnitInterfaces(d[uidKey], unitUidsIsArray,
                                       dev.hasEnergyMeter, dev.powerW, dev.deviceType);

                if (!dev.ain.empty())
                    devices_out.push_back(std::move(dev));
            }
        };

        if (j.contains("devices") && j["devices"].is_array())
            parseDevices(j["devices"], /*unitUidsIsArray=*/true);
        if (j.contains("groups") && j["groups"].is_array())
            parseDevices(j["groups"],  /*unitUidsIsArray=*/false);

    } catch (const json::exception& e) {
        m_last_error = std::string("fetchDeviceList: JSON parse error: ") + e.what();
        return false;
    }

    if (log.is_enabled(LogLevel::DBG))
        log.debug("FritzApi: fetched " + std::to_string(devices_out.size()) + " device(s)");

    return true;
}

bool FritzApi::findDeviceByAin(const std::string& ain, FritzDevice& device_out) {
    const std::string needle = stripSpaces(ain);

    std::vector<FritzDevice> devices;
    if (!fetchDeviceList(devices)) return false;

    for (const auto& d : devices) {
        if (d.ain == needle) {
            device_out = d;
            return true;
        }
    }

    m_last_error = "device with AIN '" + needle + "' not found";
    return false;
}
