#pragma once

#include "config.h"

#include <httplib.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

// ---------------------------------------------------------------------------
// Shared state between the run-loop control cycle and the REST API.
// ---------------------------------------------------------------------------

// ApiSnapshot: a read-only-from-the-API-side copy of the most recent values
// the control loop observed/decided.  Updated by the control loop (runOnce in
// main.cpp) once per cycle, under ApiState::mutex; read by RestApi's HTTP
// handlers under the same mutex.  Deliberately a plain snapshot (not a live
// view into the control loop's own locals) so the API never blocks or races
// with an in-progress Modbus/FRITZ!Box transaction.
struct ApiSnapshot {
    bool     have_data          = false;  // false until the first cycle has completed

    // Solakon ONE readings (see solakonapi.h for register details).
    // solakon_grid_power_w: the inverter's own current grid power reading
    // (ACTIVE_POWER, register 39134; +export/-import).  Previously referred to
    // internally as "A".
    int      solakon_grid_power_w    = 0;
    bool     solakon_grid_power_ok   = false;
    int      pv_w              = 0;      // TOTAL_PV_POWER (39118)
    bool     pv_ok             = false;
    int      battery_w         = 0;      // BATTERY_COMBINED_POWER (39237); +charge/-discharge
    bool     battery_ok        = false;
    int      soc                = -1;    // BMS1_SOC (37612), %; -1 = unknown
    int      max_soc_limit      = -1;    // MAX_SOC (46610), %; -1 = unknown
    int      min_soc_limit      = -1;    // MIN_SOC (46609), %; -1 = unknown
    uint16_t inverter_status    = 0;      // STATUS_1 (39063) bitfield
    bool     inverter_status_ok = false;
    uint16_t grid_status        = 0;      // STATUS_3 (39065) bitfield
    bool     grid_status_ok     = false;

    // FRITZ!Box smart-meter reading.
    // grid_meter_power_w: the household's independent grid-connection meter
    // reading from the configured FRITZ!Box device (FRITZ!Smart Energy 250).
    // Previously referred to internally as "B".
    int      grid_meter_power_w      = 0;
    bool     grid_meter_power_ok     = false;

    // Control-loop / remote-control state.
    bool     remote_engaged      = false;  // we currently hold the remote-control session
    bool     owned_by_us         = false;  // the held session (if any) was engaged by us
    bool     ever_engaged        = false;  // remote control has been engaged at least once
    bool     low_soc_hold        = false;  // withheld/released due to --min-control-soc
    std::chrono::system_clock::time_point low_soc_hold_since{};  // last time low_soc_hold
                                             // toggled (either direction); default-constructed
                                             // (epoch) means it has never toggled this run
    bool     has_last_written    = false;
    int      last_written        = 0;      // last commanded setpoint, watts (signed; see config.h)
    long long cycle_count         = 0;      // increments on every snapshot publish (not exactly
                                             // once per control-loop cycle -- a cycle may publish
                                             // more than once as its state evolves); useful mainly
                                             // to notice that the loop is still alive and updating

    std::chrono::system_clock::time_point updated_at{};
};

// ManualOverride: an operator-requested override of the normal
// solakon_grid_power_w + grid_meter_power_w control logic, set via
// POST /api/v1/override and cleared via DELETE /api/v1/override (or, for
// OverrideMode::Release with a nonzero duration_s, automatically once that
// duration elapses).  While active, the control loop bypasses the entire
// release/recover state machine -- see the "Manual override" block in
// actionRunLoop (main.cpp) for the exact behaviour of each mode.
enum class OverrideMode {
    // Command an explicit setpoint every cycle (positive = export watts,
    // negative = import watts), exactly like the original override.
    Setpoint,

    // Force-release the Solakon ONE's remote-control session -- as if the
    // normal release-eligibility state machine had decided to release, but
    // triggered directly by the operator instead.  No setpoint is written
    // while this mode is active; the inverter runs under its own configured
    // work mode.  Automatically expires after duration_s seconds (0 = stays
    // released until explicitly cleared via DELETE /api/v1/override), at
    // which point the normal release/recover state machine resumes exactly
    // as it would after any other release.
    Release,
};

struct ManualOverride {
    bool         active   = false;
    OverrideMode mode     = OverrideMode::Setpoint;

    // watts: OverrideMode::Setpoint only.  Signed: positive = export, negative = import.
    int      watts       = 0;

    // duration_s: named identically (and exposed as the same
    // "duration_seconds" JSON field) in both modes, and behaves identically
    // as an auto-expiry: once duration_s > 0 seconds have elapsed since
    // set_at, the override itself is cleared automatically (see the
    // "Manual override" block in actionRunLoop, main.cpp) -- exactly as if
    // DELETE /api/v1/override had been called.  0 = stays active
    // indefinitely, until explicitly cleared via DELETE.  For
    // OverrideMode::Setpoint specifically, duration_s is ALSO passed as the
    // Modbus REMOTE_TIMEOUT_SET revert timeout for each write (0 there means
    // use the loop's own interval + loop_timeout_extra instead), so the
    // inverter's own safety-net timeout never lags behind the override's own
    // auto-expiry.
    int      duration_s = 0;

    std::chrono::system_clock::time_point set_at{};
};

// LowSocHoldCommand: an operator-requested override of the low-SoC hold flag
// (see ApiSnapshot::low_soc_hold / Config::min_control_soc), set via
// POST /api/v1/low-soc-hold and consumed once by the control loop on its next
// cycle (main.cpp's runOnce), which then clears `pending`. Unlike
// ManualOverride above, this does not stay "active" indefinitely -- it is a
// one-shot command that directly sets the persisted low_soc_hold flag to
// `active` and, if `active` is true and remote control is currently engaged,
// forces an immediate release exactly as the automatic --min-control-soc
// cutoff would. Setting `active` to false simply clears the hold; the normal
// recover/setpoint logic in the next cycle(s) decides whether and when to
// actually re-engage (a manual clear does not itself force re-engagement,
// mirroring how the automatic min_control_soc_recover threshold only clears
// the hold rather than also bypassing the usual recover checks).
struct LowSocHoldCommand {
    bool pending = false;
    bool active  = false;
    std::chrono::system_clock::time_point requested_at{};
};

// ApiState: the single point of synchronization between the control loop
// thread and the RestApi's HTTP handler thread(s) (cpp-httplib serves each
// connection on its own thread by default).  All access to `snapshot` and
// `override` MUST hold `mutex`.
struct ApiState {
    mutable std::mutex mutex;
    ApiSnapshot         snapshot;
    ManualOverride       override_state;
    LowSocHoldCommand    low_soc_hold_command;
};

// ---------------------------------------------------------------------------
// RestApi: optional HTTP REST API exposing current parameters and accepting a
// manual grid import/export override.  Runs entirely in-process alongside
// run-loop (not a separate daemon) and shares ApiState with the control loop.
//
// Endpoints (all under /api/v1, all requiring the configured API key except
// /api/v1/health):
//   GET    /api/v1/health     - unauthenticated liveness probe: {"status":"ok"}
//   GET    /api/v1/status     - full snapshot of current parameters
//   GET    /api/v1/override   - current manual override state
//   POST   /api/v1/override   - set a manual override; JSON body is EITHER:
//                                 {"watts": <int>, "duration_seconds": <int, optional>}
//                                   -- command an explicit setpoint every cycle;
//                                      duration_seconds auto-expires the override
//                                      after that many seconds (0 = indefinite) and
//                                      is ALSO used as the Modbus revert timeout for
//                                      each write
//                                 OR
//                                 {"release": true, "duration_seconds": <int, optional>}
//                                   -- force-release remote control; duration_seconds
//                                      omitted or 0 = stays released until explicitly
//                                      cleared, otherwise auto-resumes normal control
//                                      after that many seconds
//   DELETE /api/v1/override   - clear the manual override, resuming normal
//                                solakon_grid_power_w + grid_meter_power_w control
//   GET    /api/v1/low-soc-hold  - current low-SoC hold state (also present as
//                                  "low_soc_hold"/"low_soc_hold_since" in
//                                  GET /api/v1/status)
//   POST   /api/v1/low-soc-hold  - manually set the low-SoC hold state; JSON
//                                  body: {"active": <bool>}. Setting true
//                                  forces an immediate release of remote
//                                  control (if currently engaged and owned),
//                                  exactly like the automatic --min-control-soc
//                                  cutoff. Setting false only clears the hold;
//                                  it does not force re-engagement, which the
//                                  normal recover/setpoint logic still governs.
//                                  Persisted to --state-file so the state
//                                  survives a process/service restart.
//
// Authentication: every protected endpoint requires the configured API key,
// presented as EITHER:
//   Authorization: Bearer <api_key>
//   X-API-Key: <api_key>
// A missing or mismatched key returns 401 Unauthorized with a JSON error body.
//
// TLS: when cfg.api_tls_enabled is set, the API is served over HTTPS instead
// of plain HTTP, using either an externally provided certificate/key pair
// (cfg.api_tls_cert_file / cfg.api_tls_key_file) or, if both are left empty,
// an RSA-2048 self-signed certificate generated in memory at start() time
// (never written to disk; regenerated on every process restart).
class RestApi {
public:
    RestApi(ApiState& state, const Config& cfg);
    ~RestApi();

    RestApi(const RestApi&)            = delete;
    RestApi& operator=(const RestApi&) = delete;

    // Validate configuration, bind the listening socket, and start serving in
    // a background thread.  Returns false (and logs an error) if:
    //   - cfg.api_key is empty (refuses to start with no authentication at all)
    //   - cfg.api_tls_enabled is set and exactly one of api_tls_cert_file /
    //     api_tls_key_file is set (both or neither are required)
    //   - the certificate/key files (if given) cannot be loaded, or the
    //     in-memory self-signed certificate could not be generated
    //   - binding to cfg.api_host:cfg.api_port fails
    // Safe to call at most once per instance.
    [[nodiscard]] bool start();

    // Stop the listener and join the background thread.  Safe to call even if
    // start() was never called or failed; safe to call more than once.
    void stop();

private:
    ApiState&      m_state;
    const Config&  m_cfg;
    std::unique_ptr<httplib::Server> m_server;  // httplib::Server or httplib::SSLServer
    std::thread     m_thread;
    bool            m_running{false};

    void setupRoutes();

    // Constructs m_server as either a plain httplib::Server or an
    // httplib::SSLServer, depending on cfg.api_tls_enabled/api_tls_cert_file/
    // api_tls_key_file.  Returns false (and logs an error) on any failure;
    // m_server is left null in that case.
    [[nodiscard]] bool createServer();

    // Checks the request's API key against cfg.api_key.  On failure, populates
    // res with a 401 JSON error body and returns false; the caller must return
    // immediately without doing any further work.
    [[nodiscard]] bool requireAuth(const httplib::Request& req, httplib::Response& res) const;
};
