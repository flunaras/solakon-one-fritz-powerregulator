#include "solakonapi.h"
#include "logger.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

SolakonApi::SolakonApi()  = default;

SolakonApi::~SolakonApi() {
    disconnect();
}

// ---------------------------------------------------------------------------
// connect / disconnect
// ---------------------------------------------------------------------------

bool SolakonApi::connect(const std::string& host, int port, int slave_id) {
    m_host     = host;
    m_port     = port;
    m_slave_id = slave_id;
    return doConnect();
}

bool SolakonApi::doConnect() {
    disconnect();

    // Resolve host and connect.
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* ai = nullptr;
    const std::string port_str = std::to_string(m_port);
    const int gai_err = getaddrinfo(m_host.c_str(), port_str.c_str(), &hints, &ai);
    if (gai_err != 0) {
        m_last_error = "getaddrinfo: ";
        m_last_error += gai_strerror(gai_err);
        return false;
    }

    int sock = -1;
    for (addrinfo* p = ai; p; p = p->ai_next) {
        sock = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock == -1) continue;

        // Apply timeout before connect so the initial TCP handshake can time out.
        timeval tv{kTimeoutSec, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (::connect(sock, p->ai_addr, p->ai_addrlen) == 0) break;  // success

        ::close(sock);
        sock = -1;
    }
    freeaddrinfo(ai);

    if (sock == -1) {
        m_last_error = "connect to " + m_host + ":" + port_str + " failed: ";
        m_last_error += std::strerror(errno);
        return false;
    }

    m_sock = sock;
    // Fresh TCP connection: any transaction ID the previous session was
    // waiting on is now moot, and starting the counter over keeps log output
    // readable across a reconnect (see reconnect()).
    m_transaction_id = 0;

    auto& log = Logger::instance();
    if (log.is_enabled(LogLevel::DBG))
        log.debug("SolakonApi: connected to " + m_host + ":" + port_str
                  + " slave_id=" + std::to_string(m_slave_id));
    return true;
}

bool SolakonApi::reconnect() {
    if (m_host.empty()) {
        m_last_error = "reconnect: connect() was never called successfully";
        return false;
    }

    auto& log = Logger::instance();
    if (log.is_enabled(LogLevel::WARN))
        log.warn("SolakonApi: reconnecting to " + m_host + ":" + std::to_string(m_port)
                  + " to recover from a failed Modbus transaction");

    return doConnect();
}

void SolakonApi::disconnect() {
    if (m_sock != -1) {
        ::close(m_sock);
        m_sock = -1;
    }
}

// ---------------------------------------------------------------------------
// Low-level socket I/O
// ---------------------------------------------------------------------------

bool SolakonApi::sendAll(const uint8_t* buf, int len) const {
    int sent = 0;
    while (sent < len) {
        const int n = static_cast<int>(::send(m_sock, buf + sent, len - sent, 0));
        if (n <= 0) {
            const_cast<SolakonApi*>(this)->m_last_error = "send: ";
            const_cast<SolakonApi*>(this)->m_last_error += std::strerror(errno);
            return false;
        }
        sent += n;
    }
    return true;
}

bool SolakonApi::recvAll(uint8_t* buf, int len) const {
    int received = 0;
    while (received < len) {
        const int n = static_cast<int>(::recv(m_sock, buf + received, len - received, 0));
        if (n <= 0) {
            const_cast<SolakonApi*>(this)->m_last_error =
                (n == 0) ? "connection closed by peer"
                         : std::string("recv: ") + std::strerror(errno);
            return false;
        }
        received += n;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Modbus TCP transaction
// ---------------------------------------------------------------------------
//
// Request ADU layout (MBAP header + PDU):
//   [0-1]  Transaction ID   (big-endian uint16)
//   [2-3]  Protocol ID      (always 0x0000)
//   [4-5]  Length           (PDU length + 1 for unit ID, big-endian uint16)
//   [6]    Unit ID
//   [7..N] PDU (function code + data)
//
// Response ADU layout:
//   [0-1]  Transaction ID   (echoed)
//   [2-3]  Protocol ID      (0x0000)
//   [4-5]  Length           (response PDU length + 1)
//   [6]    Unit ID
//   [7..N] Response PDU
//
bool SolakonApi::transaction(const uint8_t* req_pdu, int req_pdu_len,
                              uint8_t* resp_pdu, int expected_resp_pdu_len) const {
    if (transactionOnce(req_pdu, req_pdu_len, resp_pdu, expected_resp_pdu_len))
        return true;

    // A failed exchange (timeout, short response, mismatched transaction ID,
    // Modbus exception, ...) can leave the TCP byte stream misaligned: if the
    // response we gave up on arrives moments later, it is still sitting in the
    // kernel receive buffer, and the next read on this same socket will
    // misinterpret those stale bytes as part of a new message.  From that
    // point every subsequent transaction ID mismatches — retrying reads on
    // the same socket cannot fix it, only a fresh, byte-aligned TCP
    // connection can.  Reconnect once and retry this exact request before
    // giving up for good; a plain read is naturally safe to repeat, and a
    // write repeated after a reconnect just re-sends the same values, which
    // is harmless since the inverter has not necessarily even acted on the
    // failed attempt.
    const std::string first_error = m_last_error;
    auto& log = Logger::instance();
    if (log.is_enabled(LogLevel::WARN))
        log.warn("SolakonApi: transaction failed (" + first_error
                  + ") — reconnecting and retrying once");

    if (!const_cast<SolakonApi*>(this)->reconnect()) {
        const_cast<SolakonApi*>(this)->m_last_error =
            first_error + "; reconnect failed: " + m_last_error;
        return false;
    }

    if (transactionOnce(req_pdu, req_pdu_len, resp_pdu, expected_resp_pdu_len))
        return true;

    const_cast<SolakonApi*>(this)->m_last_error =
        first_error + "; retry after reconnect also failed: " + m_last_error;
    return false;
}

bool SolakonApi::transactionOnce(const uint8_t* req_pdu, int req_pdu_len,
                                  uint8_t* resp_pdu, int expected_resp_pdu_len) const {
    if (m_sock == -1) {
        const_cast<SolakonApi*>(this)->m_last_error = "not connected";
        return false;
    }

    // Increment transaction ID for each request.
    const uint16_t tid = ++const_cast<SolakonApi*>(this)->m_transaction_id;

    // Build request ADU.
    // MBAP header is 6 bytes; PDU starts at byte 6; unit ID is byte 6.
    // Length field = unit_id (1 byte) + PDU length.
    const uint16_t length_field = static_cast<uint16_t>(1 + req_pdu_len);
    uint8_t adu[256];
    adu[0] = static_cast<uint8_t>(tid >> 8);
    adu[1] = static_cast<uint8_t>(tid & 0xFF);
    adu[2] = 0x00;  // Protocol ID high
    adu[3] = 0x00;  // Protocol ID low
    adu[4] = static_cast<uint8_t>(length_field >> 8);
    adu[5] = static_cast<uint8_t>(length_field & 0xFF);
    adu[6] = static_cast<uint8_t>(m_slave_id);
    std::memcpy(adu + 7, req_pdu, req_pdu_len);
    const int adu_len = 7 + req_pdu_len;

    auto& log = Logger::instance();
    if (log.is_enabled(LogLevel::TRACE)) {
        std::string hex;
        for (int i = 0; i < adu_len; ++i) {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%02X ", adu[i]);
            hex += buf;
        }
        log.trace("SolakonApi TX: " + hex);
    }

    if (!sendAll(adu, adu_len)) return false;

    // Read response MBAP header (6 bytes).
    uint8_t resp_header[7];  // 6 bytes MBAP + 1 byte unit ID
    if (!recvAll(resp_header, 7)) return false;

    // Validate transaction ID.
    const uint16_t resp_tid = (static_cast<uint16_t>(resp_header[0]) << 8) | resp_header[1];
    if (resp_tid != tid) {
        const_cast<SolakonApi*>(this)->m_last_error =
            "transaction ID mismatch (expected " + std::to_string(tid)
            + ", got " + std::to_string(resp_tid) + ")";
        return false;
    }

    // Length field tells us how many more bytes to read (unit ID already consumed).
    const uint16_t resp_length = (static_cast<uint16_t>(resp_header[4]) << 8) | resp_header[5];
    const int resp_pdu_len = static_cast<int>(resp_length) - 1;  // subtract unit ID byte

    // Reject anything that would not fit in raw_resp_pdu below rather than
    // passing an oversized length into recvAll() — a garbage length field
    // (most likely to occur on an already-misaligned stream; see transaction())
    // must not turn into an out-of-bounds write on the stack buffer.
    if (resp_pdu_len < 1 || resp_pdu_len > kMaxPduLen) {
        const_cast<SolakonApi*>(this)->m_last_error =
            "invalid response length (" + std::to_string(resp_pdu_len) + " bytes)";
        return false;
    }

    // Read the response PDU.
    uint8_t raw_resp_pdu[kMaxPduLen];
    if (!recvAll(raw_resp_pdu, resp_pdu_len)) return false;

    if (log.is_enabled(LogLevel::TRACE)) {
        std::string hex;
        for (int i = 0; i < resp_pdu_len; ++i) {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%02X ", raw_resp_pdu[i]);
            hex += buf;
        }
        log.trace("SolakonApi RX PDU: " + hex);
    }

    // Check for Modbus exception response (function code has bit 7 set).
    if (raw_resp_pdu[0] & 0x80) {
        const uint8_t exc = (resp_pdu_len >= 2) ? raw_resp_pdu[1] : 0;
        const_cast<SolakonApi*>(this)->m_last_error =
            "Modbus exception code " + std::to_string(exc);
        return false;
    }

    if (resp_pdu_len < expected_resp_pdu_len) {
        const_cast<SolakonApi*>(this)->m_last_error =
            "short response: expected " + std::to_string(expected_resp_pdu_len)
            + " PDU bytes, got " + std::to_string(resp_pdu_len);
        return false;
    }

    std::memcpy(resp_pdu, raw_resp_pdu, expected_resp_pdu_len);
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool SolakonApi::readExportedPower(int& watts_out) const {
    // FC 0x03 Read Holding Registers: start_addr (2), quantity (2)
    // Register 39134 is a 32-bit signed integer spanning 2 consecutive 16-bit
    // registers with FoxESS big-endian word order (high word first).
    // Scale: raw / 1000 = kW  →  raw value == watts (since 1 W = 0.001 kW).
    const uint16_t addr = kRegActivePower;
    const uint16_t qty  = 2;
    uint8_t req[5];
    req[0] = 0x03;                              // FC Read Holding Registers
    req[1] = static_cast<uint8_t>(addr >> 8);
    req[2] = static_cast<uint8_t>(addr & 0xFF);
    req[3] = static_cast<uint8_t>(qty >> 8);
    req[4] = static_cast<uint8_t>(qty & 0xFF);

    // Expected response PDU: FC(1) + byte_count(1) + 2 registers × 2 bytes = 6 bytes
    uint8_t resp[6];
    if (!transaction(req, 5, resp, 6)) return false;

    // resp[0] = 0x03, resp[1] = byte count (4), resp[2..5] = register values
    const uint16_t hi = (static_cast<uint16_t>(resp[2]) << 8) | resp[3];
    const uint16_t lo = (static_cast<uint16_t>(resp[4]) << 8) | resp[5];
    const int32_t raw = static_cast<int32_t>((static_cast<uint32_t>(hi) << 16)
                                             | static_cast<uint32_t>(lo));

    // Raw value is in units of 1 W (scale /1000 → kW; raw == watts).
    watts_out = static_cast<int>(raw);

    auto& log = Logger::instance();
    if (log.is_enabled(LogLevel::DBG))
        log.debug("SolakonApi: ACTIVE_POWER = " + std::to_string(watts_out) + " W");

    return true;
}

bool SolakonApi::readPvPower(int& watts_out) const {
    // FC 0x03 Read Holding Registers: start_addr (2), quantity (2)
    // Register 39118 is TOTAL_PV_POWER: a 32-bit signed integer spanning 2 consecutive
    // 16-bit registers with FoxESS big-endian word order (high word first).
    // Scale: raw / 1000 = kW  →  raw value == watts (same convention as ACTIVE_POWER).
    // In practice the value is always >= 0 (PV panels cannot consume power).
    const uint16_t addr = kRegTotalPvPower;
    const uint16_t qty  = 2;
    uint8_t req[5];
    req[0] = 0x03;
    req[1] = static_cast<uint8_t>(addr >> 8);
    req[2] = static_cast<uint8_t>(addr & 0xFF);
    req[3] = static_cast<uint8_t>(qty >> 8);
    req[4] = static_cast<uint8_t>(qty & 0xFF);

    uint8_t resp[6];
    if (!transaction(req, 5, resp, 6)) return false;

    const uint16_t hi = (static_cast<uint16_t>(resp[2]) << 8) | resp[3];
    const uint16_t lo = (static_cast<uint16_t>(resp[4]) << 8) | resp[5];
    const int32_t raw = static_cast<int32_t>((static_cast<uint32_t>(hi) << 16)
                                             | static_cast<uint32_t>(lo));
    watts_out = static_cast<int>(raw);
    if (watts_out < 0) watts_out = 0;  // defensive clamp; PV power is never negative

    auto& log = Logger::instance();
    if (log.is_enabled(LogLevel::DBG))
        log.debug("SolakonApi: TOTAL_PV_POWER = " + std::to_string(watts_out) + " W");

    return true;
}

bool SolakonApi::readBatteryPower(int& watts_out) const {
    // FC 0x03 Read Holding Registers: start_addr (2), quantity (2)
    // Register 39237 is a 32-bit signed integer spanning 2 consecutive 16-bit registers
    // with FoxESS big-endian word order (high word first).
    // Scale: 1 W per unit (no division required).
    // Sign convention: positive = charging, negative = discharging.
    const uint16_t addr = kRegBatteryCombPower;
    const uint16_t qty  = 2;
    uint8_t req[5];
    req[0] = 0x03;
    req[1] = static_cast<uint8_t>(addr >> 8);
    req[2] = static_cast<uint8_t>(addr & 0xFF);
    req[3] = static_cast<uint8_t>(qty >> 8);
    req[4] = static_cast<uint8_t>(qty & 0xFF);

    uint8_t resp[6];
    if (!transaction(req, 5, resp, 6)) return false;

    const uint16_t hi = (static_cast<uint16_t>(resp[2]) << 8) | resp[3];
    const uint16_t lo = (static_cast<uint16_t>(resp[4]) << 8) | resp[5];
    const int32_t raw = static_cast<int32_t>((static_cast<uint32_t>(hi) << 16)
                                             | static_cast<uint32_t>(lo));
    watts_out = static_cast<int>(raw);

    auto& log = Logger::instance();
    if (log.is_enabled(LogLevel::DBG))
        log.debug("SolakonApi: BATTERY_COMBINED_POWER = " + std::to_string(watts_out) + " W");

    return true;
}

// ---------------------------------------------------------------------------
// u16 holding-register reads
// ---------------------------------------------------------------------------
//
// All single-register reads share an identical PDU layout (FC 0x03, qty=1,
// 5-byte response with 2 register bytes).  A private helper keeps the public
// read methods short and avoids duplicating the byte-twiddling.

namespace {
// Build the FC 0x03 request PDU for a single u16 register at `addr`.
inline void buildReadU16Request(uint16_t addr, uint8_t (&req)[5]) {
    const uint16_t qty = 1;
    req[0] = 0x03;
    req[1] = static_cast<uint8_t>(addr >> 8);
    req[2] = static_cast<uint8_t>(addr & 0xFF);
    req[3] = static_cast<uint8_t>(qty >> 8);
    req[4] = static_cast<uint8_t>(qty & 0xFF);
}
} // namespace

bool SolakonApi::readBatterySoc(int& percent_out) const {
    // Register 37612: BMS1 SoC, u16, scale = 1 %.  Range 0–100.
    uint8_t req[5];
    buildReadU16Request(kRegBms1Soc, req);

    // Expected response PDU: FC(1) + byte_count(1) + 1 register × 2 bytes = 4 bytes.
    uint8_t resp[4];
    if (!transaction(req, 5, resp, 4)) return false;

    const uint16_t raw = (static_cast<uint16_t>(resp[2]) << 8) | resp[3];
    percent_out = static_cast<int>(raw);

    auto& log = Logger::instance();
    if (log.is_enabled(LogLevel::DBG))
        log.debug("SolakonApi: BMS1_SOC = " + std::to_string(percent_out) + " %");

    return true;
}

bool SolakonApi::readMaxSoc(int& percent_out) const {
    // Register 46610: configured maximum SoC (stop-charging limit), u16, scale = 1 %.
    uint8_t req[5];
    buildReadU16Request(kRegMaxSoc, req);

    uint8_t resp[4];
    if (!transaction(req, 5, resp, 4)) return false;

    const uint16_t raw = (static_cast<uint16_t>(resp[2]) << 8) | resp[3];
    percent_out = static_cast<int>(raw);

    auto& log = Logger::instance();
    if (log.is_enabled(LogLevel::DBG))
        log.debug("SolakonApi: MAX_SOC = " + std::to_string(percent_out) + " %");

    return true;
}

bool SolakonApi::readMinSoc(int& percent_out) const {
    // Register 46609: configured minimum SoC (stop-discharging limit), u16, scale = 1 %.
    uint8_t req[5];
    buildReadU16Request(kRegMinSoc, req);

    uint8_t resp[4];
    if (!transaction(req, 5, resp, 4)) return false;

    const uint16_t raw = (static_cast<uint16_t>(resp[2]) << 8) | resp[3];
    percent_out = static_cast<int>(raw);

    auto& log = Logger::instance();
    if (log.is_enabled(LogLevel::DBG))
        log.debug("SolakonApi: MIN_SOC = " + std::to_string(percent_out) + " %");

    return true;
}

bool SolakonApi::readWorkMode(int& mode_out) const {
    // Register 49203: WORK_MODE enum, u16.  Cross-referenced with solakon-one-ui's
    // WorkMode enum and the FoxESS H3 PRO Modbus protocol V1.05:
    //   1=SelfUse, 2=FeedinPriority, 3=BackUp, 4=PeakShaving,
    //   6=ForceCharge (active override), 7=ForceDischarge (active override).
    uint8_t req[5];
    buildReadU16Request(kRegWorkMode, req);

    uint8_t resp[4];
    if (!transaction(req, 5, resp, 4)) return false;

    const uint16_t raw = (static_cast<uint16_t>(resp[2]) << 8) | resp[3];
    mode_out = static_cast<int>(raw);

    auto& log = Logger::instance();
    if (log.is_enabled(LogLevel::DBG))
        log.debug("SolakonApi: WORK_MODE = " + std::to_string(mode_out));

    return true;
}

bool SolakonApi::readInverterStatus(uint16_t& status_out) const {
    // Register 39063: STATUS_1 bitfield, u16.
    //   bit 0 = Standby, bit 2 = Operation, bit 6 = Fault.
    uint8_t req[5];
    buildReadU16Request(kRegInverterStatus, req);

    uint8_t resp[4];
    if (!transaction(req, 5, resp, 4)) return false;

    status_out = (static_cast<uint16_t>(resp[2]) << 8) | resp[3];

    auto& log = Logger::instance();
    if (log.is_enabled(LogLevel::DBG))
        log.debug("SolakonApi: INVERTER_STATUS (39063) = 0x"
                  + [&]{ char b[8]; std::snprintf(b, sizeof(b), "%04X", status_out); return std::string(b); }());

    return true;
}

bool SolakonApi::readGridStatus(uint16_t& status_out) const {
    // Register 39065: STATUS_3 bitfield, u16.  bit 0 = Off-Grid/EPS.
    uint8_t req[5];
    buildReadU16Request(kRegGridStatus, req);

    uint8_t resp[4];
    if (!transaction(req, 5, resp, 4)) return false;

    status_out = (static_cast<uint16_t>(resp[2]) << 8) | resp[3];

    auto& log = Logger::instance();
    if (log.is_enabled(LogLevel::DBG))
        log.debug("SolakonApi: GRID_STATUS (39065) = 0x"
                  + [&]{ char b[8]; std::snprintf(b, sizeof(b), "%04X", status_out); return std::string(b); }());

    return true;
}

bool SolakonApi::readRemoteControlBitfield(uint16_t& bitfield_out) const {
    // Register 46001: REMOTE_CONTROL bitfield, u16.  Readable as a holding register.
    // bit 0 = enabled (1 = currently under remote control).
    uint8_t req[5];
    buildReadU16Request(kRegRemoteControl, req);

    uint8_t resp[4];
    if (!transaction(req, 5, resp, 4)) return false;

    bitfield_out = (static_cast<uint16_t>(resp[2]) << 8) | resp[3];

    auto& log = Logger::instance();
    if (log.is_enabled(LogLevel::DBG))
        log.debug("SolakonApi: REMOTE_CONTROL (46001) = 0x"
                  + [&]{ char b[8]; std::snprintf(b, sizeof(b), "%04X", bitfield_out); return std::string(b); }());

    return true;
}

bool SolakonApi::writeRemoteControlInternal(int watts, uint16_t timeout_seconds,
                                            uint16_t control_bitfield,
                                            const char* action_desc) const {
    if (watts < 0) watts = 0;

    // Write order matches solakon-one-ui's onApply():
    //   46002 timeout first, then power values, then the control bitfield last.
    // The inverter only activates the new setpoint when it sees the bitfield write,
    // so writing it last ensures it sees a consistent set of values.

    // Helper: FC 0x10 write a single u16 register.
    auto writeU16 = [&](uint16_t addr, uint16_t value) -> bool {
        uint8_t req[8];
        req[0] = 0x10;
        req[1] = static_cast<uint8_t>(addr >> 8);
        req[2] = static_cast<uint8_t>(addr & 0xFF);
        req[3] = 0x00; req[4] = 0x01;  // quantity = 1
        req[5] = 0x02;                  // byte count
        req[6] = static_cast<uint8_t>(value >> 8);
        req[7] = static_cast<uint8_t>(value & 0xFF);
        uint8_t resp[5];
        return transaction(req, 8, resp, 5);
    };

    // Helper: FC 0x10 write a big-endian i32 spanning two registers.
    auto writeI32 = [&](uint16_t addr, int32_t value) -> bool {
        const uint16_t hi = static_cast<uint16_t>((static_cast<uint32_t>(value) >> 16) & 0xFFFF);
        const uint16_t lo = static_cast<uint16_t>( static_cast<uint32_t>(value)        & 0xFFFF);
        uint8_t req[10];
        req[0] = 0x10;
        req[1] = static_cast<uint8_t>(addr >> 8);
        req[2] = static_cast<uint8_t>(addr & 0xFF);
        req[3] = 0x00; req[4] = 0x02;  // quantity = 2
        req[5] = 0x04;                  // byte count
        req[6] = static_cast<uint8_t>(hi >> 8);
        req[7] = static_cast<uint8_t>(hi & 0xFF);
        req[8] = static_cast<uint8_t>(lo >> 8);
        req[9] = static_cast<uint8_t>(lo & 0xFF);
        uint8_t resp[5];
        return transaction(req, 10, resp, 5);
    };

    // 1. REMOTE_TIMEOUT_SET (46002, u16, seconds)
    if (!writeU16(kRegRemoteTimeout, timeout_seconds)) return false;

    // 2. REMOTE_ACTIVE_POWER (46003, i32, watts, big-endian word order)
    if (!writeI32(kRegRemoteActivePow, static_cast<int32_t>(watts))) return false;

    // 3. REMOTE_REACTIVE_POWER (46005, i32, always 0)
    if (!writeI32(kRegRemoteReactivePow, 0)) return false;

    // 4. REMOTE_CONTROL bitfield (46001, u16) — written last to activate the command.
    if (!writeU16(kRegRemoteControl, control_bitfield)) return false;

    auto& log = Logger::instance();
    if (log.is_enabled(LogLevel::DBG)) {
        char bf[8];
        std::snprintf(bf, sizeof(bf), "0x%02X", control_bitfield);
        log.debug(std::string("SolakonApi: remote control (") + action_desc + ") set to "
                  + std::to_string(watts) + " W  timeout=" + std::to_string(timeout_seconds) + " s"
                  + "  bitfield=" + bf);
    }

    return true;
}

bool SolakonApi::writeRemoteControl(int watts, uint16_t timeout_seconds) const {
    // bit 0 = enable (1), bit 1 = direction (0 = generate/inject),
    // bits 3:2 = target (10 = Grid)  →  combined: 0b1001 = 0x09
    constexpr uint16_t kBitfieldEnableGenerateGrid = 0x09u;
    return writeRemoteControlInternal(watts, timeout_seconds, kBitfieldEnableGenerateGrid,
                                      "enable|generate|grid");
}

bool SolakonApi::writeRemoteControlImport(int watts, uint16_t timeout_seconds) const {
    // bit 0 = enable (1), bit 1 = direction (1 = consume/absorb),
    // bits 3:2 = target (10 = Grid)  →  combined: 0b1011 = 0x0B
    constexpr uint16_t kBitfieldEnableConsumeGrid = 0x0Bu;
    return writeRemoteControlInternal(watts, timeout_seconds, kBitfieldEnableConsumeGrid,
                                      "enable|consume|grid");
}

bool SolakonApi::releaseRemoteControl() const {
    // Disengage remote control by writing REMOTE_CONTROL = 0x00 (bit 0 clear = disabled).
    // The inverter reverts to its configured work mode (Self Use, Feed-in Priority, etc.).
    // In the typical Self Use mode this means: cover house loads first, then charge battery,
    // then export any remaining surplus — safe at any time when PV is producing.
    // Callers are responsible for ensuring this is only called when reverting to the
    // configured work mode is the correct behaviour (see run-loop release conditions).

    auto writeU16 = [&](uint16_t addr, uint16_t value) -> bool {
        uint8_t req[8];
        req[0] = 0x10;
        req[1] = static_cast<uint8_t>(addr >> 8);
        req[2] = static_cast<uint8_t>(addr & 0xFF);
        req[3] = 0x00; req[4] = 0x01;  // quantity = 1
        req[5] = 0x02;                  // byte count
        req[6] = static_cast<uint8_t>(value >> 8);
        req[7] = static_cast<uint8_t>(value & 0xFF);
        uint8_t resp[5];
        return transaction(req, 8, resp, 5);
    };

    if (!writeU16(kRegRemoteControl, 0x00u)) return false;

    auto& log = Logger::instance();
    if (log.is_enabled(LogLevel::DBG))
        log.debug("SolakonApi: remote control released (bitfield=0x00, inverter free)");

    return true;
}

std::string SolakonApi::lastError() const {
    return m_last_error;
}
