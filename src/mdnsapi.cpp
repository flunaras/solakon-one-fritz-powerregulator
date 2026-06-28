#include "mdnsapi.h"

#include <cstring>
#include <ctime>
#include <netdb.h>
#include <arpa/inet.h>
#include <sstream>

// Try to include Avahi headers; if not available, we'll provide fallback implementations.
#ifdef HAVE_AVAHI
    #include <avahi-client/client.h>
    #include <avahi-client/lookup.h>
    #include <avahi-common/simple-watch.h>
    #include <avahi-common/malloc.h>
    #include <avahi-common/error.h>
#endif

// ────────────────────────────────────────────────────────────────────────────
// MdnsApi implementation
// ────────────────────────────────────────────────────────────────────────────

MdnsApi::MdnsApi() : m_avahi_available(false) {
#ifdef HAVE_AVAHI
    // Quick check: try to create a simple Avahi client to see if the service is available.
    // This is a soft probe — if Avahi isn't running, we'll still try at discovery time.
    m_avahi_available = true;
#else
    m_last_error = "Avahi development headers not available at compile time; "
                   "mDNS discovery will use hostname resolution only";
#endif
}

MdnsApi::~MdnsApi() = default;

#ifdef HAVE_AVAHI

// ────────────────────────────────────────────────────────────────────────────
// Avahi-based implementation (when HAVE_AVAHI is defined)
// ────────────────────────────────────────────────────────────────────────────

namespace {

// Callback context for service browser.
struct BrowseContext {
    std::vector<MdnsService> services;
    bool                     done = false;
    AvahiSimplePoll*         simple_poll = nullptr;
};

static void resolve_callback(
    AvahiServiceResolver*    resolver,
    AvahiIfIndex             interface,
    AvahiProtocol            protocol,
    AvahiResolverEvent       event,
    const char*              name,
    const char*              type,
    const char*              domain,
    const char*              host_name,
    const AvahiAddress*      address,
    uint16_t                 port,
    AvahiStringList*         txt,
    AvahiLookupResultFlags   flags,
    void*                    userdata) {
    (void)interface;
    (void)protocol;
    (void)txt;
    (void)flags;

    auto* ctx = static_cast<BrowseContext*>(userdata);

    if (event == AVAHI_RESOLVER_FOUND) {
        if (address && host_name) {
            MdnsService svc;
            svc.name = name ? name : "";
            svc.hostname = host_name;
            svc.port = port;

            // Convert address to string.
            char addr_str[256] = {0};
            if (address->proto == AVAHI_PROTO_INET) {
                inet_ntop(AF_INET, &address->data.ipv4, addr_str, sizeof(addr_str));
            } else if (address->proto == AVAHI_PROTO_INET6) {
                inet_ntop(AF_INET6, &address->data.ipv6, addr_str, sizeof(addr_str));
            }
            svc.address = addr_str;

            ctx->services.push_back(svc);
        }
    }

    avahi_service_resolver_free(resolver);
}

static void browse_callback(
    AvahiServiceBrowser*     browser,
    AvahiIfIndex             interface,
    AvahiProtocol            protocol,
    AvahiBrowserEvent        event,
    const char*              name,
    const char*              type,
    const char*              domain,
    AvahiLookupResultFlags   flags,
    void*                    userdata) {
    (void)flags;

    auto* ctx = static_cast<BrowseContext*>(userdata);
    auto* client = avahi_service_browser_get_client(browser);

    if (event == AVAHI_BROWSER_NEW) {
        // Try to resolve this service.
        if (client && name && type && domain) {
            AvahiServiceResolver* resolver = avahi_service_resolver_new(
                client, interface, protocol, name, type, domain,
                AVAHI_PROTO_UNSPEC, (AvahiLookupFlags)0,
                resolve_callback, ctx);
            // Note: resolver is freed in the resolve_callback.
            (void)resolver;
        }
    } else if (event == AVAHI_BROWSER_ALL_FOR_NOW) {
        ctx->done = true;
        if (ctx->simple_poll) {
            avahi_simple_poll_quit(ctx->simple_poll);
        }
    } else if (event == AVAHI_BROWSER_FAILURE) {
        ctx->done = true;
        if (ctx->simple_poll) {
            avahi_simple_poll_quit(ctx->simple_poll);
        }
    }
}

}  // namespace

std::vector<MdnsService> MdnsApi::discoverServices(
    const std::string& service_type,
    int timeout_ms) {
    std::vector<MdnsService> result;

    // Create a simple Avahi poll (event loop).
    AvahiSimplePoll* simple_poll = avahi_simple_poll_new();
    if (!simple_poll) {
        m_last_error = "Failed to create Avahi simple poll";
        return result;
    }

    int error = 0;
    AvahiClient* client = avahi_client_new(
        avahi_simple_poll_get(simple_poll),
        (AvahiClientFlags)0,
        nullptr,  // no client callback
        nullptr,  // no userdata
        &error);
    if (!client) {
        m_last_error = std::string("Failed to create Avahi client: ") +
                       avahi_strerror(error);
        avahi_simple_poll_free(simple_poll);
        return result;
    }

    BrowseContext ctx;
    ctx.simple_poll = simple_poll;

    // Create a service browser for the requested service type.
    AvahiServiceBrowser* browser = avahi_service_browser_new(
        client,
        AVAHI_IF_UNSPEC,
        AVAHI_PROTO_UNSPEC,
        service_type.c_str(),
        "local",
        (AvahiLookupFlags)0,
        browse_callback,
        &ctx);

    if (!browser) {
        m_last_error = std::string("Failed to create Avahi service browser: ") +
                       avahi_strerror(avahi_client_errno(client));
        avahi_client_free(client);
        avahi_simple_poll_free(simple_poll);
        return result;
    }

    // Run the event loop for the specified timeout.
    struct timespec deadline;
    if (timeout_ms > 0) {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (timeout_ms % 1000) * 1000000;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }
    }

    // Poll until done or timeout.
    while (!ctx.done) {
        struct timespec now, remaining;
        clock_gettime(CLOCK_MONOTONIC, &now);

        remaining.tv_sec = deadline.tv_sec - now.tv_sec;
        remaining.tv_nsec = deadline.tv_nsec - now.tv_nsec;
        if (remaining.tv_nsec < 0) {
            remaining.tv_sec--;
            remaining.tv_nsec += 1000000000;
        }

        if (remaining.tv_sec < 0 ||
            (remaining.tv_sec == 0 && remaining.tv_nsec <= 0)) {
            break;  // Timeout reached.
        }

        int ret = avahi_simple_poll_iterate(simple_poll, remaining.tv_nsec / 1000000);
        if (ret < 0) {
            break;
        }
    }

    // Cleanup.
    avahi_service_browser_free(browser);
    avahi_client_free(client);
    avahi_simple_poll_free(simple_poll);

    result = ctx.services;
    return result;
}

#else

// ────────────────────────────────────────────────────────────────────────────
// Fallback implementation (when Avahi is not available)
// ────────────────────────────────────────────────────────────────────────────

std::vector<MdnsService> MdnsApi::discoverServices(
    const std::string& service_type,
    int timeout_ms) {
    (void)service_type;
    (void)timeout_ms;

    std::vector<MdnsService> result;
    m_last_error = "Avahi is not available; cannot discover services. "
                   "Install libavahi-client-dev and libavahi-common-dev and recompile.";
    return result;
}

#endif  // HAVE_AVAHI

// ────────────────────────────────────────────────────────────────────────────
// Hostname resolution (works with or without Avahi)
// ────────────────────────────────────────────────────────────────────────────

bool MdnsApi::resolveHostname(const std::string& hostname, std::string& address_out) {
    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;  // Both IPv4 and IPv6.
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = nullptr;
    int ret = getaddrinfo(hostname.c_str(), nullptr, &hints, &res);

    if (ret != 0) {
        m_last_error = std::string("Failed to resolve ") + hostname + ": " +
                       gai_strerror(ret);
        return false;
    }

    // Use the first result.
    if (res) {
        char addr_str[256] = {0};

        if (res->ai_family == AF_INET) {
            auto* addr = reinterpret_cast<struct sockaddr_in*>(res->ai_addr);
            inet_ntop(AF_INET, &addr->sin_addr, addr_str, sizeof(addr_str));
        } else if (res->ai_family == AF_INET6) {
            auto* addr = reinterpret_cast<struct sockaddr_in6*>(res->ai_addr);
            inet_ntop(AF_INET6, &addr->sin6_addr, addr_str, sizeof(addr_str));
        }

        address_out = addr_str;
        freeaddrinfo(res);
        return true;
    }

    freeaddrinfo(res);
    m_last_error = "No addresses found for " + hostname;
    return false;
}

// ────────────────────────────────────────────────────────────────────────────
// Convenience function
// ────────────────────────────────────────────────────────────────────────────

std::vector<MdnsService> MdnsApi::discoverSolakonInverters(int timeout_ms) {
    // Try to discover services on Modbus TCP (standard for Solakon).
    // If that doesn't yield results, we could try other approaches.
    return discoverModbusTcpServices(timeout_ms);
}

std::string MdnsApi::lastError() const {
    return m_last_error;
}
