#pragma once

#include <string>
#include <vector>

// Avahi-based mDNS (Multicast DNS) discovery for finding Solakon ONE inverters
// and other services on the local network.
//
// Queries the local mDNS network for services (e.g., Modbus TCP) and returns
// a list of discovered services with their hostnames and resolved IP addresses.
//
// This module wraps Avahi's D-Bus API to provide simple synchronous discovery.
// All public methods are single-threaded and block until complete.
//
// Dependencies: libavahi-client and libavahi-common (standard on modern Linux
// distributions). If Avahi is not available, discovery will fail gracefully.

struct MdnsService {
    std::string name;          // Service instance name (e.g., "My Inverter")
    std::string hostname;      // Fully qualified hostname (e.g., "inverter.local")
    std::string address;       // Resolved IP address (IPv4 or IPv6 as string)
    int         port = 0;      // Port number (if available)
};

class MdnsApi {
public:
    MdnsApi();
    ~MdnsApi();

    MdnsApi(const MdnsApi&)            = delete;
    MdnsApi& operator=(const MdnsApi&) = delete;

    // Discover services of a given type on the local mDNS network.
    // service_type: e.g., "_modbus-tcp._tcp" or "_ssh._tcp"
    // timeout_ms: how long to wait for responses (default 3000 ms)
    // Returns a list of discovered services.
    // thread-safe (in the sense that results are only returned when discovery completes):
    // do not call concurrently with any other method.
    [[nodiscard]] std::vector<MdnsService> discoverServices(
        const std::string& service_type,
        int timeout_ms = 3000);

    // Convenience function to discover Modbus TCP services specifically.
    [[nodiscard]] std::vector<MdnsService> discoverModbusTcpServices(int timeout_ms = 3000) {
        return discoverServices("_modbus-tcp._tcp", timeout_ms);
    }

    // Convenience function to discover Solakon ONE inverters.
    // Attempts to discover services named with common Solakon/FoxESS patterns
    // and returns any that resolve.
    [[nodiscard]] std::vector<MdnsService> discoverSolakonInverters(int timeout_ms = 3000);

    std::string lastError() const;

private:
    std::string m_last_error;
    bool        m_avahi_available = false;

    // Helper to resolve a hostname to an IP address.
    [[nodiscard]] bool resolveHostname(const std::string& hostname, std::string& address_out);
};
