#pragma once

#include <cstdint>
#include <string>

// Synchronous Modbus TCP client for the FoxESS Solakon ONE (H3/H3 Pro family).
//
// Implements only the Modbus function codes needed by this tool:
//   FC 0x03  Read Holding Registers
//   FC 0x10  Write Multiple Registers
//
// The Modbus TCP ADU is a 6-byte MBAP header followed by the PDU:
//   Transaction ID  2 bytes  (echoed back by server)
//   Protocol ID     2 bytes  (always 0x0000)
//   Length          2 bytes  (number of bytes following, i.e. PDU length)
//   Unit ID         1 byte   (Modbus slave / unit ID)
//   Function code   1 byte
//   Data            N bytes
//
// FoxESS uses big-endian word order for 32-bit registers (high word first).
//
// All public methods are safe to call from a single thread only.
class SolakonApi {
public:
    SolakonApi();
    ~SolakonApi();

    SolakonApi(const SolakonApi&)            = delete;
    SolakonApi& operator=(const SolakonApi&) = delete;

    // Open a TCP connection to host:port and set the Modbus unit ID.
    // Returns false and sets lastError() on failure.
    // thread-safe: must not be called concurrently with any other method.
    [[nodiscard]] bool connect(const std::string& host, int port, int slave_id);

    // Close the socket.  Safe to call even if not connected.
    // thread-safe: must not be called concurrently with any other method.
    void disconnect();

    // Read the current grid active power from register 39134 (i32, /1000 → kW).
    // watts_out is set to the raw register value (positive = exporting to grid).
    // Returns false and sets lastError() on failure.
    // thread-safe: must not be called concurrently with any other method.
    [[nodiscard]] bool readExportedPower(int& watts_out) const;

    // Read the total PV generation power from register 39118 (i32, /1000 → kW).
    // watts_out is set to the raw register value (always >= 0; zero at night).
    // Returns false and sets lastError() on failure.
    // thread-safe: must not be called concurrently with any other method.
    [[nodiscard]] bool readPvPower(int& watts_out) const;

    // Read the current battery combined power from register 39237 (i32, watts).
    // Positive = battery is charging (absorbing PV/grid power).
    // Negative = battery is discharging (supplying loads).
    // Zero     = battery is idle.
    // Returns false and sets lastError() on failure.
    // thread-safe: must not be called concurrently with any other method.
    [[nodiscard]] bool readBatteryPower(int& watts_out) const;

    // Read the BMS1 battery state of charge from register 37612 (u16, %).
    // Range: 0–100.  The value reflects the BMS-reported SoC of the primary battery.
    // (BMS2, if present, would be read from register 38310 — not exposed here as the
    // Solakon ONE typically has a single battery, and combined power 39237 already
    // aggregates both batteries.)
    // Returns false and sets lastError() on failure.
    // thread-safe: must not be called concurrently with any other method.
    [[nodiscard]] bool readBatterySoc(int& percent_out) const;

    // Read the configured maximum SoC charging limit from register 46610 (u16, %).
    // This is the user-set "stop charging at X%" threshold configured via the
    // Solakon ONE web UI / FoxESS app.  Range: typically 10–100; many users set it
    // to 95–100% to avoid stressing LiFePO4 cells at the top of charge.
    // Returns false and sets lastError() on failure.
    // thread-safe: must not be called concurrently with any other method.
    [[nodiscard]] bool readMaxSoc(int& percent_out) const;

    // Read the configured minimum SoC discharging limit from register 46609 (u16, %).
    // This is the user-set "stop discharging at X%" threshold; the battery will not
    // discharge below this value (it is reserved for grid outages or backup).
    // Returns false and sets lastError() on failure.
    // thread-safe: must not be called concurrently with any other method.
    [[nodiscard]] bool readMinSoc(int& percent_out) const;

    // Read the current work mode from register 49203 (u16, enum).
    // Known values on H3 PRO / Solakon ONE:
    //   1 = Self Use         — cover loads from PV/battery, export only surplus
    //   2 = Feed-in Priority — prefer exporting PV to the grid
    //   3 = Back-up          — reserve battery for grid outage
    //   4 = Peak Shaving     — uses configured peak-shaving threshold
    //   6 = Force Charge     — runtime override: battery is being charged
    //   7 = Force Discharge  — runtime override: battery is being discharged
    // Values 6 and 7 appear only when the FoxESS app or our REMOTE_CONTROL writes
    // have temporarily put the inverter into a force-charge/discharge state.
    // Returns false and sets lastError() on failure.
    // thread-safe: must not be called concurrently with any other method.
    [[nodiscard]] bool readWorkMode(int& mode_out) const;

    // Read the inverter operational status from register 39063 (u16, bitfield).
    // Bit 0 = Standby, bit 2 = Operation (running), bit 6 = Fault.
    // The convenience getters below decode these bits.
    // Returns false and sets lastError() on failure.
    // thread-safe: must not be called concurrently with any other method.
    [[nodiscard]] bool readInverterStatus(uint16_t& status_out) const;

    // Read the grid status from register 39065 (u16, bitfield).
    // Bit 0 = Off-Grid / EPS (1 = currently in island/backup mode, 0 = grid-tied).
    // Returns false and sets lastError() on failure.
    // thread-safe: must not be called concurrently with any other method.
    [[nodiscard]] bool readGridStatus(uint16_t& status_out) const;

    // Read back the REMOTE_CONTROL bitfield from register 46001 (u16).
    // Used at startup to detect whether the inverter is currently under remote control
    // (e.g. left engaged by a previous process that did not exit cleanly, or by the
    // FoxESS app's strategy-period feature).  Bit 0 = enabled.
    // Returns false and sets lastError() on failure.
    // thread-safe: must not be called concurrently with any other method.
    [[nodiscard]] bool readRemoteControlBitfield(uint16_t& bitfield_out) const;

    // Write the remote-control setpoint to the Solakon ONE using the four-register
    // remote-control mechanism (Mechanism B):
    //   46002  REMOTE_TIMEOUT_SET    u16  seconds  — inverter reverts when no refresh arrives
    //   46003  REMOTE_ACTIVE_POWER   i32  watts    — active power setpoint
    //   46005  REMOTE_REACTIVE_POWER i32  var      — reactive power (always 0 here)
    //   46001  REMOTE_CONTROL        u16  bitfield — written last to activate the command
    //            bit 0 = enable (1 = remote control active)
    //            bit 1 = direction (0 = generate/inject, 1 = consume/absorb)
    //            bits 3:2 = target (00 = AC, 01 = Battery, 10 = Grid)
    // Negative watt values are clamped to 0 before writing.
    // Returns false and sets lastError() on failure.
    // thread-safe: must not be called concurrently with any other method.
    [[nodiscard]] bool writeRemoteControl(int watts, uint16_t timeout_seconds) const;

    // Write a remote-control setpoint that instructs the inverter to IMPORT
    // (consume/absorb) power from the grid, using the same four-register
    // mechanism as writeRemoteControl() above but with the REMOTE_CONTROL
    // direction bit (bit 1) set to 1 (consume/absorb) instead of 0
    // (generate/inject):
    //   46002  REMOTE_TIMEOUT_SET    u16  seconds  — inverter reverts when no refresh arrives
    //   46003  REMOTE_ACTIVE_POWER   i32  watts    — magnitude of power to draw from the grid
    //   46005  REMOTE_REACTIVE_POWER i32  var      — reactive power (always 0 here)
    //   46001  REMOTE_CONTROL        u16  bitfield — written last to activate the command
    //            bit 0 = enable (1)
    //            bit 1 = direction (1 = consume/absorb)
    //            bits 3:2 = target (10 = Grid)
    // watts is the magnitude (>= 0) of power to import from the grid; negative
    // values are clamped to 0 before writing (same convention as
    // writeRemoteControl()).
    // Used by the run-loop when the A + B setpoint would otherwise go negative
    // (the inverter would need to import from the grid to cover the load) and
    // grid-import is enabled via Config::enable_grid_import — instead of
    // clamping the setpoint to 0 and letting the inverter fall back to its own
    // work mode, this actively commands the requested import amount.
    // Returns false and sets lastError() on failure.
    // thread-safe: must not be called concurrently with any other method.
    [[nodiscard]] bool writeRemoteControlImport(int watts, uint16_t timeout_seconds) const;

    // Disengage remote control by writing REMOTE_CONTROL = 0x00 (bit 0 = 0, disabled).
    // After this call the inverter reverts to its default behaviour (free export of all
    // available PV/battery power).  Should be called whenever the load the tool is
    // compensating for drops to zero or below the release threshold so that surplus
    // solar energy is not wasted by a stale setpoint.
    // Returns false and sets lastError() on failure.
    // thread-safe: must not be called concurrently with any other method.
    [[nodiscard]] bool releaseRemoteControl() const;

    // Returns a human-readable description of the last error.
    [[nodiscard]] std::string lastError() const;

private:
    int         m_sock{-1};
    std::string m_host;                // remembered for reconnect(); empty until connect() succeeds
    int         m_port{0};             // remembered for reconnect()
    int         m_slave_id{1};
    uint16_t    m_transaction_id{0};
    std::string m_last_error;

    // Modbus holding register addresses (0-based wire addresses as used in the
    // FoxESS/Solakon ONE register map; these are passed directly in the PDU).
    static constexpr uint16_t kRegTotalPvPower      = 39118; // i32 /1000 kW  read-only  (always >= 0)
    static constexpr uint16_t kRegActivePower       = 39134; // i32 /1000 kW  read-only  (+export/-import)
    static constexpr uint16_t kRegBatteryCombPower  = 39237; // i32 W          read-only  (+charge/-discharge)
    static constexpr uint16_t kRegInverterStatus    = 39063; // u16 bitfield   read-only  (Standby/Operation/Fault)
    static constexpr uint16_t kRegGridStatus        = 39065; // u16 bitfield   read-only  (bit 0 = Off-Grid/EPS)
    static constexpr uint16_t kRegBms1Soc           = 37612; // u16 %          read-only  (BMS1 state of charge)
    static constexpr uint16_t kRegMinSoc            = 46609; // u16 %          read/write (stop-discharging limit)
    static constexpr uint16_t kRegMaxSoc            = 46610; // u16 %          read/write (stop-charging limit)
    static constexpr uint16_t kRegWorkMode          = 49203; // u16 enum       read/write (1=SelfUse, 2=FeedIn, ...)
    static constexpr uint16_t kRegRemoteControl     = 46001; // u16 bitfield   read/write
    static constexpr uint16_t kRegRemoteTimeout     = 46002; // u16 seconds    write
    static constexpr uint16_t kRegRemoteActivePow   = 46003; // i32 watts      write
    static constexpr uint16_t kRegRemoteReactivePow = 46005; // i32 var        write

    // Socket read/write timeout in seconds.
    static constexpr int kTimeoutSec = 5;

    // Maximum Modbus TCP response PDU size accepted into the fixed-size stack
    // buffer used by transactionOnce().  The Modbus spec caps a PDU at 253
    // bytes, so 256 leaves a little headroom.  A response whose header claims
    // a longer PDU is rejected instead of being read into an undersized
    // buffer (see transactionOnce()).
    static constexpr int kMaxPduLen = 256;

    // Perform the actual resolve/socket/connect using m_host/m_port/m_slave_id
    // (already populated by connect() or reconnect()).  Shared by both so the
    // connection-establishment logic lives in one place.
    [[nodiscard]] bool doConnect();

    // Close and re-open the connection using the same host/port/slave_id most
    // recently passed to connect().  A failed Modbus exchange (timeout, short
    // response, mismatched transaction ID, ...) can leave the TCP byte stream
    // misaligned: a response that arrives just after we gave up waiting for it
    // is still sitting in the kernel receive buffer, and the next read on the
    // same socket will misinterpret those stale bytes as part of a new
    // message — every transaction from that point on will keep failing.
    // Reconnecting (a fresh TCP connection) is the only way to guarantee a
    // clean, byte-aligned stream again; transaction() calls this automatically.
    // Returns false if connect() was never called successfully, or if the
    // fresh connection attempt itself fails.
    // thread-safe: must not be called concurrently with any other method.
    [[nodiscard]] bool reconnect();

    // Attempt a Modbus exchange; on failure, reconnect once and retry the same
    // request before giving up — see reconnect() for why retrying reads on the
    // same (possibly misaligned) socket cannot recover from that failure mode.
    // pdu_out must be sized to hold the expected response PDU.
    // Returns false on error (including when the reconnect-and-retry also fails).
    [[nodiscard]] bool transaction(const uint8_t* req_pdu, int req_pdu_len,
                                   uint8_t* resp_pdu, int expected_resp_pdu_len) const;

    // Single, non-retrying attempt at a Modbus transaction.  Used internally by
    // transaction(); see there for the retry/reconnect policy.
    [[nodiscard]] bool transactionOnce(const uint8_t* req_pdu, int req_pdu_len,
                                       uint8_t* resp_pdu, int expected_resp_pdu_len) const;

    // Shared implementation for writeRemoteControl() / writeRemoteControlImport():
    // writes REMOTE_TIMEOUT_SET, REMOTE_ACTIVE_POWER, REMOTE_REACTIVE_POWER, then
    // the REMOTE_CONTROL bitfield (control_bitfield) last.  watts is clamped to
    // >= 0 before writing.  action_desc is used only for the debug log line.
    [[nodiscard]] bool writeRemoteControlInternal(int watts, uint16_t timeout_seconds,
                                                  uint16_t control_bitfield,
                                                  const char* action_desc) const;

    // Low-level socket helpers.
    [[nodiscard]] bool sendAll(const uint8_t* buf, int len) const;
    [[nodiscard]] bool recvAll(uint8_t* buf, int len) const;
};
