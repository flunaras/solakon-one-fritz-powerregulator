#pragma once

#include <string>
#include <vector>

// Minimal FRITZ!Box Smart Home device descriptor.
struct FritzDevice {
    std::string ain;            // AIN with spaces stripped — primary key
    std::string name;           // human-readable device name
    std::string productName;    // product/model name (e.g. "FRITZ!DECT 200"); empty for groups
    std::string deviceType;     // comma-separated capability list derived from unit interfaces
                                // (e.g. "switch,energyMeter,temperatureSensor"); empty if unknown
    bool        present        = false; // reachable by FRITZ!Box
    double      powerW         = 0.0;   // current power draw in W (0 if no energy meter)
    bool        hasEnergyMeter = false; // true if a multimeterInterface is present
};

// Synchronous FRITZ!Box Smart Home REST API client.
// Uses cpp-httplib; no Qt, no event loop.
// All methods are safe to call from a single thread only.
class FritzApi {
public:
    FritzApi();
    ~FritzApi();

    // Not copyable (owns internal httplib client state).
    FritzApi(const FritzApi&)            = delete;
    FritzApi& operator=(const FritzApi&) = delete;

    // Configure the FRITZ!Box hostname or IP address (default: fritz.box).
    // thread-safe: call before login().
    void setHost(const std::string& host);

    // Set the URL scheme: "http" (default) or "https".
    // thread-safe: call before login().
    void setScheme(const std::string& scheme);

    // Set FRITZ!Box login credentials.
    // thread-safe: call before login().
    void setCredentials(const std::string& username, const std::string& password);

    // If true, TLS certificate errors are ignored (useful for self-signed certs).
    // thread-safe: call before login().
    void setIgnoreSsl(bool ignore);

    // Perform the FRITZ!Box SID challenge-response authentication.
    // Supports both PBKDF2-HMAC-SHA256 (modern) and legacy MD5 challenges.
    // Returns false and sets lastError() on failure.
    // thread-safe: must not be called concurrently with any other method.
    [[nodiscard]] bool login();

    // Fetch the full list of FRITZ!Box Smart Home devices.
    // Calls login() automatically if the SID has expired (HTTP 401).
    // Returns false and sets lastError() on failure.
    // thread-safe: must not be called concurrently with any other method.
    [[nodiscard]] bool fetchDeviceList(std::vector<FritzDevice>& devices_out);

    // Convenience wrapper: find one device by AIN (spaces stripped before comparison).
    // Returns false if not found or if fetchDeviceList() fails.
    // thread-safe: must not be called concurrently with any other method.
    [[nodiscard]] bool findDeviceByAin(const std::string& ain, FritzDevice& device_out);

    // Returns a human-readable description of the last error.
    [[nodiscard]] std::string lastError() const;

private:
    std::string m_host       = "fritz.box";
    std::string m_scheme     = "http";  // "http" or "https"
    std::string m_username;
    std::string m_password;
    std::string m_sid        = "0000000000000000";
    bool        m_ignore_ssl = false;
    std::string m_last_error;

    // Compute the login response string for a given challenge.
    // Detects PBKDF2 vs MD5 from the challenge format.
    [[nodiscard]] bool computeLoginResponse(const std::string& challenge,
                                            std::string&       response_out);

    // Parse the <SID> element from FRITZ!Box login XML.
    static std::string parseSid(const std::string& xml);

    // Parse the <Challenge> element from FRITZ!Box login XML.
    static std::string parseChallenge(const std::string& xml);

    // Strip spaces from an AIN string.
    static std::string stripSpaces(const std::string& ain);

    // Hex-encode a binary buffer.
    static std::string hexEncode(const unsigned char* data, size_t len);
};
