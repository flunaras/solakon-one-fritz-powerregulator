#include "config.h"
#include "fritzapi.h"
#include "logger.h"
#include "mdnsapi.h"
#include "restapi.h"
#include "solakonapi.h"
#include "statestore.h"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <csignal>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <chrono>

// ---------------------------------------------------------------------------
// Signal handling
// ---------------------------------------------------------------------------

static std::atomic<bool> g_stop{false};

static void on_signal(int) {
    g_stop.store(true, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Device filters
// ---------------------------------------------------------------------------

// Case-insensitive substring search (ASCII only; locale-independent).
static bool containsIcase(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(),   needle.end(),
        [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        });
    return it != haystack.end();
}

// Returns true if the device matches the configured product-name and device-type filters.
// Product filter: case-insensitive substring match; device passes if its productName
// contains ANY entry in the list (empty list = no filter, accept all).
// Type filter: case-sensitive substring match; empty string accepts any value.
static bool matchesFilters(const FritzDevice&              dev,
                            const std::vector<std::string>& filter_products,
                            const std::string&              filter_type) {
    if (!filter_products.empty()) {
        bool product_match = false;
        for (const auto& p : filter_products) {
            if (containsIcase(dev.productName, p)) {
                product_match = true;
                break;
            }
        }
        if (!product_match)
            return false;
    }
    if (!filter_type.empty() && dev.deviceType.find(filter_type) == std::string::npos)
        return false;
    return true;
}

// ---------------------------------------------------------------------------
// Device suitability check
// ---------------------------------------------------------------------------

// Check whether a FRITZ!Box device is suitable as the power-meter input for run-loop.
//
// A suitable device must:
//   1. Match the product filter (fritz_filter_products), if one is configured.
//      The filter is a list of case-insensitive substrings matched against
//      FritzDevice::productName; a device passes if its name contains ANY entry.
//      The default list is {"FRITZ!Smart Energy 250"} — the only FRITZ!Smart Energy
//      device that supports bidirectional metering (distinguishing between consumption
//      and export).  All other models report only net consumption, which produces
//      incorrect setpoints.  Pass an empty list to disable.
//   2. Have an energy meter (hasEnergyMeter == true) — otherwise powerW is always 0.
//   3. Be currently reachable (present == true) — an offline device cannot be verified
//      and will stall the control loop.
//
// Returns true if the device passes all requirements.
// Emits log messages at the appropriate level for every condition found.
static bool checkDeviceSuitability(const FritzDevice& dev, const Config& cfg) {
    auto& log = Logger::instance();

    bool ok = true;

    if (!cfg.fritz_filter_products.empty()) {
        bool product_match = false;
        for (const auto& p : cfg.fritz_filter_products) {
            if (containsIcase(dev.productName, p)) {
                product_match = true;
                break;
            }
        }
        if (!product_match) {
            // Build a readable list of allowed substrings for the error message.
            std::string allowed;
            for (std::size_t i = 0; i < cfg.fritz_filter_products.size(); ++i) {
                if (i > 0) allowed += ", ";
                allowed += '\'' + cfg.fritz_filter_products[i] + '\'';
            }
            log.error("Device '" + dev.ain + "' (" + dev.name + ")"
                      " product '" + dev.productName + "'"
                      " does not match any entry in the product filter [" + allowed + "]."
                      "  Only devices whose product name contains one of these strings"
                      " are accepted as suitable for run-loop."
                      "  Use --fritz-filter-product to change the list"
                      " or pass no --fritz-filter-product arguments to disable the filter.");
            ok = false;
        }
    }

    if (!dev.hasEnergyMeter) {
        log.error("Device '" + dev.ain + "' (" + dev.name + ") has no energy meter."
                  "  Only devices with an energy meter report real power values."
                  "  Use 'list-fritz-devices' to find a suitable device.");
        ok = false;
    }

    if (!dev.present) {
        log.error("Device '" + dev.ain + "' (" + dev.name + ") is offline."
                  "  An offline device cannot supply power readings for the control loop.");
        ok = false;
    }

    return ok;
}

// ---------------------------------------------------------------------------
// Action implementations
// ---------------------------------------------------------------------------

// list-fritz-devices: list all FRITZ!Box Smart Home devices.
static int actionListFritzDevices(const Config& cfg) {
    auto& log = Logger::instance();

    FritzApi fritz;
    fritz.setHost(cfg.fritz_host);
    fritz.setScheme(cfg.fritz_scheme);
    fritz.setCredentials(cfg.fritz_username, cfg.fritz_password);
    fritz.setIgnoreSsl(cfg.fritz_ignore_ssl);

    if (!fritz.login()) {
        log.error("FRITZ!Box login failed: " + fritz.lastError());
        return 1;
    }

    std::vector<FritzDevice> devices;
    if (!fritz.fetchDeviceList(devices)) {
        log.error("Failed to fetch FRITZ!Box device list: " + fritz.lastError());
        return 1;
    }

    // Apply product-name and device-type filters.
    std::vector<FritzDevice> filtered;
    for (const auto& d : devices)
        if (matchesFilters(d, cfg.fritz_filter_products, cfg.fritz_filter_type))
            filtered.push_back(d);

    if (filtered.empty()) {
        if (!cfg.fritz_filter_products.empty() || !cfg.fritz_filter_type.empty())
            std::cout << "(no devices match the configured filters)\n";
        else
            std::cout << "(no devices found)\n";
        return 0;
    }

    // Column header.
    std::cout << "AIN\t\tName\t\t\tProduct\t\t\tType\t\t\tPower\t\tStatus\n";
    std::cout << std::string(100, '-') << '\n';

    for (const auto& d : filtered) {
        std::cout << d.ain
                  << '\t' << d.name
                  << '\t' << (d.productName.empty() ? "-" : d.productName)
                  << '\t' << (d.deviceType.empty()  ? "-" : d.deviceType)
                  << '\t';
        if (d.hasEnergyMeter)
            std::cout << d.powerW << " W";
        else
            std::cout << "[no energy meter]";
        std::cout << '\t' << (d.present ? "online" : "offline");
        std::cout << '\n';
    }

    return 0;
}

// read-fritz-device: print details of the single device identified by --fritz-ain.
static int actionReadFritzDevice(const Config& cfg) {
    auto& log = Logger::instance();

    if (cfg.fritz_ain.empty()) {
        log.error("read-fritz-device requires --fritz-ain");
        return 1;
    }

    FritzApi fritz;
    fritz.setHost(cfg.fritz_host);
    fritz.setScheme(cfg.fritz_scheme);
    fritz.setCredentials(cfg.fritz_username, cfg.fritz_password);
    fritz.setIgnoreSsl(cfg.fritz_ignore_ssl);

    if (!fritz.login()) {
        log.error("FRITZ!Box login failed: " + fritz.lastError());
        return 1;
    }

    FritzDevice dev;
    if (!fritz.findDeviceByAin(cfg.fritz_ain, dev)) {
        log.error("FRITZ!Box device not found: " + fritz.lastError());
        return 1;
    }

    std::cout << dev.ain
              << '\t' << dev.name
              << '\t' << (dev.productName.empty() ? "-" : dev.productName)
              << '\t' << (dev.deviceType.empty()  ? "-" : dev.deviceType);
    if (dev.hasEnergyMeter)
        std::cout << '\t' << dev.powerW << " W";
    if (!dev.present)
        std::cout << "\t[offline]";
    std::cout << '\n';

    // Emit suitability diagnostics for informational purposes.
    // The return value is intentionally ignored here — read-fritz-device is an
    // informational command and does not enforce suitability as a hard requirement.
    checkDeviceSuitability(dev, cfg);

    return 0;
}

// check-fritz-device: verify that the device identified by --fritz-ain is suitable for run-loop.
static int actionCheckFritzDevice(const Config& cfg) {
    auto& log = Logger::instance();

    if (cfg.fritz_ain.empty()) {
        log.error("check-fritz-device requires --fritz-ain");
        return 1;
    }

    FritzApi fritz;
    fritz.setHost(cfg.fritz_host);
    fritz.setScheme(cfg.fritz_scheme);
    fritz.setCredentials(cfg.fritz_username, cfg.fritz_password);
    fritz.setIgnoreSsl(cfg.fritz_ignore_ssl);

    if (!fritz.login()) {
        log.error("FRITZ!Box login failed: " + fritz.lastError());
        return 1;
    }

    FritzDevice dev;
    if (!fritz.findDeviceByAin(cfg.fritz_ain, dev)) {
        log.error("FRITZ!Box device not found: " + fritz.lastError());
        return 1;
    }

    // Always print the device line so the user can confirm which device was checked.
    std::cout << dev.ain
              << '\t' << dev.name
              << '\t' << (dev.productName.empty() ? "-" : dev.productName)
              << '\t' << (dev.deviceType.empty()  ? "-" : dev.deviceType);
    if (dev.hasEnergyMeter)
        std::cout << '\t' << dev.powerW << " W";
    else
        std::cout << "\t[no energy meter]";
    std::cout << '\t' << (dev.present ? "online" : "offline");
    std::cout << '\n';

    if (!checkDeviceSuitability(dev, cfg)) {
        std::cout << "UNSUITABLE: device cannot be used as the power-meter input for run-loop.\n";
        return 1;
    }

    std::cout << "OK: device is suitable for use with run-loop.\n";
    return 0;
}

// discover-mdns: discover services on the local mDNS network.
static int actionDiscoverMdns(const Config& cfg) {
    auto& log = Logger::instance();

    MdnsApi mdns;

    // Default to Modbus TCP services (Solakon ONE).
    // Users can customize this in the future if needed.
    std::vector<MdnsService> services = mdns.discoverModbusTcpServices(3000);

    if (!services.empty()) {
        // Column header.
        std::cout << "Name\t\t\tHostname\t\tAddress\t\t\tPort\n";
        std::cout << std::string(100, '-') << '\n';

        for (const auto& svc : services) {
            std::cout << svc.name << '\t' << svc.hostname << '\t' << svc.address;
            if (svc.port > 0)
                std::cout << '\t' << svc.port;
            std::cout << '\n';
        }
    } else {
        std::cout << "No mDNS services found on the network.\n";
        if (!mdns.lastError().empty()) {
            log.info("Note: " + mdns.lastError());
        }
    }

    return 0;
}

// read-solakon: connect to Solakon ONE, print current exported power.
static int actionReadSolakon(const Config& cfg) {
    auto& log = Logger::instance();

    SolakonApi solakon;
    if (!solakon.connect(cfg.solakon_host, cfg.solakon_port, cfg.solakon_slave_id)) {
        log.error("Cannot connect to Solakon ONE: " + solakon.lastError());
        return 1;
    }

    int      solakon_grid_power_w  = 0;
    int      pv_w        = 0;
    int      battery_w   = 0;
    int      soc         = -1;
    int      max_soc     = -1;
    int      min_soc     = -1;
    int      work_mode   = -1;
    uint16_t inv_status  = 0;
    uint16_t grid_status = 0;
    uint16_t remote_ctrl = 0;

    bool ok = true;

    if (!solakon.readExportedPower(solakon_grid_power_w)) {
        log.error("Failed to read ACTIVE_POWER: " + solakon.lastError());
        ok = false;
    }
    if (!solakon.readPvPower(pv_w)) {
        log.error("Failed to read TOTAL_PV_POWER: " + solakon.lastError());
        ok = false;
    }
    if (!solakon.readBatteryPower(battery_w)) {
        log.error("Failed to read BATTERY_COMBINED_POWER: " + solakon.lastError());
        ok = false;
    }
    // The remaining reads are diagnostic — they fail soft so that a missing
    // register on an older firmware doesn't break the basic three-value output.
    bool soc_ok    = solakon.readBatterySoc(soc);
    bool max_ok    = solakon.readMaxSoc(max_soc);
    bool min_ok    = solakon.readMinSoc(min_soc);
    bool work_ok   = solakon.readWorkMode(work_mode);
    bool stat_ok   = solakon.readInverterStatus(inv_status);
    bool grid_ok   = solakon.readGridStatus(grid_status);
    bool rc_ok     = solakon.readRemoteControlBitfield(remote_ctrl);

    if (!ok)
        return 1;

    // Derive a human-readable battery state from the sign of battery_w.
    const char* battery_state = (battery_w > 0) ? "charging"
                              : (battery_w < 0) ? "discharging"
                              :                   "idle";

    // Decode work-mode enum.
    auto work_mode_name = [](int m) -> const char* {
        switch (m) {
            case 1: return "Self Use";
            case 2: return "Feed-in Priority";
            case 3: return "Backup";
            case 4: return "Peak Shaving";
            case 6: return "Force Charge";
            case 7: return "Force Discharge";
            default: return "unknown";
        }
    };

    // Decode the inverter status bitfield (39063).
    auto inv_state_name = [](uint16_t s) -> std::string {
        std::string parts;
        if (s & 0x0040u) parts += "Fault ";
        if (s & 0x0004u) parts += "Operation ";
        if (s & 0x0001u) parts += "Standby ";
        if (parts.empty()) parts = "(none) ";
        parts.pop_back();  // trim trailing space
        return parts;
    };

    std::cout << "active_power:    " << solakon_grid_power_w << " W"
              << "  (" << (solakon_grid_power_w >= 0 ? "exporting" : "importing") << ")\n"
              << "pv_power:        " << pv_w       << " W\n"
              << "battery_power:   " << battery_w  << " W"
              << "  (" << battery_state << ")\n";

    if (soc_ok)
        std::cout << "battery_soc:     " << soc << " %\n";
    else
        std::cout << "battery_soc:     [read failed]\n";

    if (max_ok)
        std::cout << "max_soc_limit:   " << max_soc << " %  (stop-charging limit)\n";
    else
        std::cout << "max_soc_limit:   [read failed]\n";

    if (min_ok)
        std::cout << "min_soc_limit:   " << min_soc << " %  (stop-discharging limit)\n";
    else
        std::cout << "min_soc_limit:   [read failed]\n";

    if (work_ok)
        std::cout << "work_mode:       " << work_mode << "  (" << work_mode_name(work_mode) << ")\n";
    else
        std::cout << "work_mode:       [read failed]\n";

    if (stat_ok) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "0x%04X", inv_status);
        std::cout << "inverter_status: " << buf << "  (" << inv_state_name(inv_status) << ")\n";
    } else {
        std::cout << "inverter_status: [read failed]\n";
    }

    if (grid_ok) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "0x%04X", grid_status);
        std::cout << "grid_status:     " << buf
                  << "  (" << ((grid_status & 0x0001u) ? "Off-Grid (EPS)" : "On-Grid") << ")\n";
    } else {
        std::cout << "grid_status:     [read failed]\n";
    }

    if (rc_ok) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "0x%04X", remote_ctrl);
        std::cout << "remote_control:  " << buf
                  << "  (" << ((remote_ctrl & 0x0001u) ? "engaged" : "released") << ")\n";
    } else {
        std::cout << "remote_control:  [read failed]\n";
    }

    return 0;
}

// write-solakon: write an explicit watt value to the Solakon ONE export limit.
static int actionWriteSolakon(const Config& cfg) {
    auto& log = Logger::instance();

    const int watts = std::min(cfg.write_watts, cfg.max_power);
    if (watts < cfg.write_watts)
        log.warn("--watts " + std::to_string(cfg.write_watts) + " W exceeds --max-power "
                 + std::to_string(cfg.max_power) + " W; clamping to "
                 + std::to_string(watts) + " W");

    if (cfg.dry_run) {
        log.info("dry-run: would write " + std::to_string(watts) + " W"
                 + "  timeout " + std::to_string(cfg.write_timeout) + " s to Solakon ONE");
        std::cout << "(dry-run) " << watts << " W"
                  << "  timeout " << cfg.write_timeout << " s\n";
        return 0;
    }

    SolakonApi solakon;
    if (!solakon.connect(cfg.solakon_host, cfg.solakon_port, cfg.solakon_slave_id)) {
        log.error("Cannot connect to Solakon ONE: " + solakon.lastError());
        return 1;
    }

    const auto timeout = static_cast<uint16_t>(cfg.write_timeout);
    if (!solakon.writeRemoteControl(watts, timeout)) {
        log.error("Failed to write Solakon ONE: " + solakon.lastError());
        return 1;
    }

    if (log.is_enabled(LogLevel::INFO))
        log.info("Wrote " + std::to_string(watts) + " W"
                 + "  timeout " + std::to_string(cfg.write_timeout) + " s to Solakon ONE");

    std::cout << watts << " W"
              << "  timeout " << cfg.write_timeout << " s\n";
    return 0;
}

// run-loop: full control cycle, optionally repeated.
//
// Release/recover decision model — direct behavioural test, not proxy conditions:
//
// Two earlier release conditions — "work mode must be Self Use" (require_self_use)
// and "PV power must exceed a threshold" (pv_release_threshold) — proved unreliable
// in the field.  In particular, WORK_MODE can read a non-Self-Use value (e.g. Force
// Discharge) indefinitely on some inverters/firmware, which permanently blocked
// release even when the battery was genuinely full.  Both conditions have been
// REMOVED.  Rather than replacing them with yet more indirect proxies, the release
// decision now tests the actual, ground-truth outcome directly:
//
//   1. RELEASE ELIGIBILITY (gates the ENGAGED -> RELEASED transition).
//      Evaluated fresh every cycle while remote control is engaged.  ALL of the
//      following must hold on the SAME cycle:
//        - cond_owned:        we own the remote-control session (owned_by_us).
//                              A boolean set true whenever our own writeRemoteControl
//                              succeeds and false after a successful release.  On cold
//                              start we read back REMOTE_CONTROL (46001); if it is
//                              already engaged (bit 0 set) we set owned_by_us = false
//                              and refuse the release path until our own engagement
//                              cycle sets the flag to true.  This prevents the tool
//                              from silently cancelling a FoxESS-app strategy period
//                              or a setpoint left over from a previous run.
//        - cond_soc:          SoC ≥ MAX_SOC − release_soc_hysteresis.
//                              The primary signal that the battery cannot absorb more
//                              surplus.  MAX_SOC is the user-configured "stop charging
//                              at X%" limit, read once at startup from register 46610.
//        - cond_not_charging: BATTERY_COMBINED_POWER ≤ battery_dead_band.
//                              A battery that is still charging can still absorb
//                              whatever PV surplus the remote-control setpoint is
//                              currently clipping, so releasing would gain nothing.
//                              Discharging (negative) does NOT block release — it is
//                              the expected state immediately after the battery
//                              reaches MAX_SOC.  The dead band (default 20 W) absorbs
//                              LiFePO4 trickle/float charge current so a few watts of
//                              residual "charging" don't lock release out forever.
//                              Safe-fail: if the Modbus read itself fails, this
//                              condition is false regardless of the stale battery_w
//                              value still sitting in the variable.
//        - cond_pv_headroom:  TOTAL_PV_POWER > ACTIVE_POWER (solakon_grid_power_w) + release_pv_margin,
//                              UNLESS SoC has already reached MAX_SOC itself (not just
//                              the release_soc_hysteresis-relaxed cond_soc threshold),
//                              in which case this condition is bypassed (forced true).
//                              While engaged, solakon_grid_power_w is (approximately) whatever we last
//                              commanded the inverter to export.  Releasing can only
//                              ever increase the export up to what PV can supply once
//                              the battery stops absorbing more — so unless PV is
//                              comfortably above what we are already exporting, there
//                              is no PV surplus left for a release to unlock, and
//                              testing one has no possible upside, only the risk of a
//                              spurious release immediately undone by the recover
//                              decision.  Default margin: 50 W.
//                              The MAX_SOC bypass exists because that reasoning silently
//                              assumes TOTAL_PV_POWER is an independent measurement of
//                              available sunlight — true while the battery still has
//                              room to climb, but some inverters/firmware curtail PV
//                              harvest down to match whatever ACTIVE_POWER we are
//                              currently commanding once the battery is genuinely full
//                              (no spare capacity left to buffer the difference).  When
//                              that happens PV can no longer exceed solakon_grid_power_w regardless of how
//                              much sunlight is actually available, permanently vetoing
//                              a release that should happen — our own throttling
//                              poisons the very signal this condition depends on.  Once
//                              SoC has actually reached MAX_SOC there is nothing left to
//                              lose by testing a release anyway: the existing grid_meter_power_w-driven
//                              recover debounce below already safely undoes one that
//                              turns out to have no real surplus behind it.
//        - cond_export_growing: grid_meter_power_w < 0 OR solakon_grid_power_w > engage_baseline_solakon_grid_power_w (solakon_grid_power_w as it was when the
//                              current engaged streak started — see engage_baseline_solakon_grid_power_w
//                              above runOnce).
//                              A second, complementary signal that the export
//                              situation is already favourable or actively improving,
//                              on top of the static PV-vs-solakon_grid_power_w snapshot above.  grid_meter_power_w < 0
//                              means the household is, right now, net-exporting even
//                              under our own capped setpoint — direct proof that
//                              there is already more supply than the setpoint alone
//                              accounts for.  solakon_grid_power_w higher than it was at the start of
//                              this engagement means the control loop itself has
//                              been pushing the setpoint up net overall (growing
//                              load or growing PV) — a trend suggesting the
//                              situation keeps improving.  Deliberately compared
//                              against a FIXED starting point rather than the
//                              immediately preceding cycle: solakon_grid_power_w is not guaranteed to
//                              rise on every single cycle even while the underlying
//                              trend is upward, so a rolling comparison could flap
//                              false on an ordinary one-cycle dip; comparing against
//                              where solakon_grid_power_w started only cares about net progress since
//                              engagement began, however many cycles that took.
//                              Safe-fail: without a captured baseline yet (true cold
//                              start, or the very first cycle of a fresh engagement)
//                              this condition falls back to the grid_meter_power_w < 0 half alone.
//        - cond_inv_state:    the inverter is in normal Operation, on-grid, and not
//                              in Fault.  STATUS_1 (39063) bit 2 set, bit 6 clear;
//                              STATUS_3 (39065) bit 0 (Off-Grid/EPS) clear.
//        - cond_combined:     solakon_grid_power_w + grid_meter_power_w ≤ release_threshold, only checked when
//                              release_threshold > 0 (optional extra safety net;
//                              disabled — 0 — by default).
//
//      The MOMENT all of these hold, remote control is released IMMEDIATELY, as a
//      real-world behavioural test — NOT after a further debounce.  There is nothing
//      to debounce on this side any more: the test is inherently self-correcting (see
//      point 2), so a one-cycle blip in SoC or battery power costs at most
//      release_debounce_cycles cycles of being wrongly released before recovery
//      kicks in — see config.h for the full rationale.
//
//   2. RECOVER DECISION (gates the RELEASED -> ENGAGED transition).
//      Once released, the tool stops asking indirect questions ("is the work mode
//      right?", "is PV still producing?") and instead watches the one number that
//      actually answers whether the release was a good idea: grid_meter_power_w, the FRITZ!Box
//      grid-connection reading.
//        - grid_meter_power_w ≤ 0 (exporting to / not importing from the grid): the release is
//          working — the inverter's own configured work mode is covering the load
//          without our help.  Remain released; reset the regain counter to 0.
//        - grid_meter_power_w >  0 (importing from the grid) for release_debounce_cycles CONSECUTIVE
//          cycles: the release was premature, or conditions changed since — regain
//          control by falling through to the normal solakon_grid_power_w + grid_meter_power_w setpoint path below.
//      This is the ONLY trigger for regaining control after a release; SoC dropping
//      back down or the battery resuming charging do not, by themselves, force a
//      regain — the household not importing is the only outcome that matters once
//      released.  The very first engagement after process start (nothing has ever
//      been released yet) bypasses this grid_meter_power_w-based test entirely and takes control
//      immediately — see "ever_engaged" below.
//
//   3. LOW-SOC HARD CUTOFF (a THIRD, entirely independent release/recover pair).
//      cfg.min_control_soc (see config.h) is a minimum battery SoC required for the
//      tool to hold or take remote control at all, protecting the battery from being
//      driven to discharge further by our own solakon_grid_power_w+grid_meter_power_w setpoint once it is already low.
//      Unlike points 1/2 above:
//        - Release is a hard cutoff gated on nothing but SoC and session ownership
//          (owned_by_us) — none of battery_dead_band / release_pv_margin /
//          cond_export_growing / inverter health apply; those answer "is there PV
//          surplus a release could unlock", which is not the question here.
//        - Recovery (regaining control) is driven purely by SoC reaching
//          min_control_soc_recover_effective — no debounce cycles, no dependency on
//          grid_meter_power_w.  The instant SoC crosses back up, control resumes immediately.
//        - Applies even before the very first engagement: if SoC already reads below
//          cfg.min_control_soc at a true cold start, the tool withholds control from
//          cycle one (tracked by low_soc_hold) instead of taking control
//          unconditionally the way a normal cold start does.
//      Because full-battery and low-battery are mutually exclusive states, this
//      cutoff and the release-when-full mechanism never compete in practice; the
//      low-SoC check is simply evaluated first, on its own terms, every cycle.
//
// Settling: after every writeRemoteControl OR releaseRemoteControl the tool sleeps
// settle_delay_ms before the next cycle's reads.  This now matters on both sides:
// the very next grid_meter_power_w reading after a release is the input the recover test above
// relies on, so it must reflect the inverter's settled free-run state, not a
// mid-ramp transient.
//
// Atomic read window: all Solakon ONE registers are read back-to-back at the
// start of each cycle (solakon_grid_power_w, battery_w, SoC, status), then the FRITZ!Box HTTP read
// happens last.  This keeps the inverter-side measurements within tens of
// milliseconds of each other and confines the cross-component time skew (Modbus
// vs FRITZ) to a single boundary.
//
// Clean shutdown: when the loop exits due to SIGINT/SIGTERM, the tool issues a
// final releaseRemoteControl() if it owns the session, so the inverter is not
// left under a stale cap waiting for its revert timeout to expire.
static int actionRunLoop(const Config& cfg) {
    auto& log = Logger::instance();

    if (cfg.fritz_ain.empty()) {
        log.error("run-loop requires --fritz-ain");
        return 1;
    }

    // ---- Optional REST API -------------------------------------------------
    // ApiState is shared (under its own mutex) between this control-loop thread
    // and the RestApi's HTTP handler thread(s); see restapi.h.  RestApi's
    // destructor stops the listener and joins its background thread, so simply
    // letting `restApi` go out of scope on any return path from actionRunLoop
    // cleanly shuts the API down -- no explicit stop() calls needed below.
    ApiState apiState;
    RestApi  restApi(apiState, cfg);
    if (cfg.api_enabled) {
        if (!restApi.start())
            return 1;
    }

    // Validate / clamp the low-SoC recover threshold once at startup (see min_control_soc
    // and min_control_soc_recover in config.h).  min_control_soc_recover must be
    // >= min_control_soc to form a non-inverted hysteresis band; a lower (or default 0)
    // value is clamped up rather than silently producing a release/recover flap.
    int min_control_soc_recover_effective = cfg.min_control_soc_recover;
    if (cfg.min_control_soc > 0 && min_control_soc_recover_effective < cfg.min_control_soc) {
        log.warn("--min-control-soc-recover (" + std::to_string(cfg.min_control_soc_recover)
                 + " %) is below --min-control-soc (" + std::to_string(cfg.min_control_soc)
                 + " %) — clamping the recover threshold up to "
                 + std::to_string(cfg.min_control_soc) + " % (zero-width hysteresis band)");
        min_control_soc_recover_effective = cfg.min_control_soc;
    }

    // Restore the low-SoC hold state persisted by a previous run (see
    // --state-file / statestore.h). Only meaningful while the low-SoC cutoff
    // itself is enabled -- if it is disabled (cfg.min_control_soc == 0) a
    // stale "held" flag from an earlier run (made with a different config)
    // must not silently withhold control forever, since the maintenance logic
    // that would normally clear it is itself gated on min_control_soc > 0.
    bool low_soc_hold_restored = false;
    if (cfg.min_control_soc > 0 && !cfg.state_file.empty()) {
        PersistedState persisted;
        if (loadState(cfg.state_file, persisted) && persisted.low_soc_hold) {
            low_soc_hold_restored = true;
            log.info("Restored low-SoC hold state from '" + cfg.state_file
                     + "' — remote control will remain withheld until SoC reaches "
                     + std::to_string(min_control_soc_recover_effective) + " %");
        }
    }

    if (log.is_enabled(LogLevel::INFO)) {
        log.info("Starting run-loop"
                 "  solakon=" + cfg.solakon_host + ":" + std::to_string(cfg.solakon_port)
                 + "  slave=" + std::to_string(cfg.solakon_slave_id));
        log.info("  fritz=" + cfg.fritz_scheme + "://" + cfg.fritz_host
                 + "  ain=" + cfg.fritz_ain
                 + "  user=" + cfg.fritz_username);
        log.info("  interval=" + std::to_string(cfg.interval) + " s"
                 + "  timeout-extra=" + std::to_string(cfg.loop_timeout_extra) + " s"
                 + "  max-power=" + std::to_string(cfg.max_power) + " W"
                 + "  smoothing=" + std::to_string(cfg.smoothing) + " (time-scaled per --interval)"
                 + "  max-ramp=" + (cfg.max_ramp_w_per_s > 0
                                     ? std::to_string(cfg.max_ramp_w_per_s) + " W/s"
                                     : std::string("disabled"))
                 + "  min-change=" + std::to_string(cfg.min_change) + " W"
                 + "  fritz-min-change=" + std::to_string(cfg.fritz_min_change) + " W"
                 + "  fritz-stuck-cycles=" + std::to_string(cfg.fritz_stuck_cycles)
                 + "  fritz-baseline=" + std::to_string(cfg.fritz_baseline_w) + " W"
                 + (cfg.dry_run ? "  [dry-run]" : ""));
        log.info("  release-soc-hysteresis=" + std::to_string(cfg.release_soc_hysteresis) + " %"
                 + "  release-debounce=" + std::to_string(cfg.release_debounce_cycles) + " (regain-control side only)"
                 + "  battery-dead-band=" + std::to_string(cfg.battery_dead_band) + " W (charging threshold)"
                 + "  release-pv-margin=" + std::to_string(cfg.release_pv_margin) + " W"
                 + "  settle-delay=" + std::to_string(cfg.settle_delay_ms) + " ms"
                 + "  recover-on-start=" + (cfg.recover_remote_on_start ? "yes" : "no"));
        log.info("  release-threshold=" + std::to_string(cfg.release_threshold) + " W (solakon_grid_power_w+grid_meter_power_w safety net)");
        if (cfg.min_control_soc > 0)
            log.info("  min-control-soc=" + std::to_string(cfg.min_control_soc) + " %"
                     + "  min-control-soc-recover=" + std::to_string(min_control_soc_recover_effective) + " %"
                     + "  (independent low-SoC safety cutoff, SoC-driven recovery only)");
    }

    SolakonApi solakon;
    if (!solakon.connect(cfg.solakon_host, cfg.solakon_port, cfg.solakon_slave_id)) {
        log.error("Cannot connect to Solakon ONE: " + solakon.lastError());
        return 1;
    }

    FritzApi fritz;
    fritz.setHost(cfg.fritz_host);
    fritz.setScheme(cfg.fritz_scheme);
    fritz.setCredentials(cfg.fritz_username, cfg.fritz_password);
    fritz.setIgnoreSsl(cfg.fritz_ignore_ssl);

    if (!fritz.login()) {
        log.error("FRITZ!Box login failed: " + fritz.lastError());
        return 1;
    }

    // Verify the configured FRITZ!Box device is suitable before entering the loop.
    // A device without an energy meter or that is currently offline cannot supply
    // meaningful power readings.  Abort rather than silently writing wrong setpoints.
    {
        FritzDevice probe;
        if (!fritz.findDeviceByAin(cfg.fritz_ain, probe)) {
            log.error("FRITZ!Box device not found: " + fritz.lastError());
            return 1;
        }
        if (!checkDeviceSuitability(probe, cfg))
            return 1;
    }

    // Read the configured MAX_SOC limit (the user's "stop charging at X%" setting).
    // This is the reference point against which SoC is compared for release decisions.
    // Read once at startup — the user does not change this often, and re-reading on
    // every cycle would add a Modbus round-trip for no benefit.  If the read fails,
    // the SoC-based release trigger is disabled (treated as "always at threshold")
    // but the tool continues to run, falling back to the solakon_grid_power_w+grid_meter_power_w safety-net trigger.
    int max_soc_limit = -1;  // -1 = unknown, SoC release trigger disabled
    if (!solakon.readMaxSoc(max_soc_limit)) {
        log.warn("Could not read MAX_SOC limit (register 46610): " + solakon.lastError()
                 + " — SoC-based release will be disabled this run");
        max_soc_limit = -1;
    } else {
        if (max_soc_limit < 10 || max_soc_limit > 100) {
            log.warn("MAX_SOC limit reads out-of-range (" + std::to_string(max_soc_limit)
                     + " %) — SoC-based release will be disabled this run");
            max_soc_limit = -1;
        } else if (log.is_enabled(LogLevel::INFO)) {
            log.info("MAX_SOC limit = " + std::to_string(max_soc_limit) + " %"
                     + "  → release-eligible at SoC ≥ " + std::to_string(
                         std::max(0, max_soc_limit - cfg.release_soc_hysteresis)) + " %");
        }
    }

    // Read the configured MIN_SOC limit once, purely for informational purposes on the
    // REST API's status endpoint (not used by any control decision in run-loop).
    // Best-effort: failure here does not affect the control loop, only the reported value.
    int min_soc_limit = -1;
    if (!solakon.readMinSoc(min_soc_limit))
        min_soc_limit = -1;

    // Cold-start ownership detection.
    // If REMOTE_CONTROL bit 0 is already set when we start, we are NOT the owner.
    // The setpoint may belong to a previous instance that did not exit cleanly, or
    // to a FoxESS app "strategy period" that is currently active.  In either case
    // we must not release it: doing so could cancel an active app schedule, or
    // discard work the user wants preserved.  We will only become the owner once
    // our own writeRemoteControl call has succeeded — at which point the previous
    // owner's setpoint has already been overwritten by our own, and release becomes
    // safe.
    bool owned_by_us = false;
    if (cfg.recover_remote_on_start) {
        uint16_t rc_bitfield = 0;
        if (!solakon.readRemoteControlBitfield(rc_bitfield)) {
            log.warn("Could not read REMOTE_CONTROL state on start (register 46001): "
                     + solakon.lastError()
                     + " — assuming inverter is not under remote control");
        } else if ((rc_bitfield & 0x0001u) != 0) {
            if (log.is_enabled(LogLevel::INFO))
                log.info("REMOTE_CONTROL is already engaged on start (bitfield=0x"
                         + [&]{ char b[8]; std::snprintf(b, sizeof(b), "%04X", rc_bitfield); return std::string(b); }()
                         + ") — not our session, release inhibited until we engage it ourselves");
        } else if (log.is_enabled(LogLevel::DBG)) {
            log.debug("REMOTE_CONTROL bit 0 = 0 on start — inverter is free, normal operation");
        }
    }

    // Anti-oscillation and release state (persists across cycles).
    //
    // ema_setpoint    : the smoothed setpoint carried forward; -1 = unset.
    // last_written    : the last value actually sent to the inverter (has_last_written
    //                   indicates whether it holds a real value yet; can be negative,
    //                   meaning an active grid-import command, when enable_grid_import
    //                   is set).
    // remote_engaged  : whether remote control was engaged by our most recent action.
    //                   Set true after writeRemoteControl; false after releaseRemoteControl.
    // owned_by_us     : whether we are the owner of the current remote-control session.
    //                   Set true on a successful writeRemoteControl; false after release.
    //                   Initially false (we did not engage it yet).  The release path
    //                   only fires when owned_by_us is true.
    // ever_engaged    : whether writeRemoteControl has EVER succeeded since process
    //                   start (set once, never reset back to false — unlike
    //                   owned_by_us, a release does not undo this).  Distinguishes a
    //                   true cold start (nothing to recover from — take control on
    //                   the first cycle) from a post-release recovery decision (must
    //                   pass the grid_meter_power_w-driven regain-control test below).  Without this
    //                   distinction, the regain-control test would also gate the very
    //                   first engagement, and the tool would never take control at
    //                   all if the household happened to be net-exporting (grid_meter_power_w <= 0)
    //                   at startup — even with room in the battery to charge.
    // regain_counter  : counts consecutive cycles where grid_meter_power_w > 0 (net grid import)
    //                   while remote control is released.  Resets to 0 whenever
    //                   grid_meter_power_w <= 0.  Regain-control fires when this reaches
    //                   cfg.release_debounce_cycles.  There is deliberately no
    //                   equivalent counter on the release side: release fires the
    //                   instant all release-eligibility conditions hold on a single
    //                   cycle — the release itself IS the test, nothing to debounce
    //                   beforehand.
    // last_solakon_grid_power_w : the inverter's ACTIVE_POWER reading from the previous cycle,
    //                   used to compute |Δsolakon_grid_power_w| for the input-side dead band.
    //                   INT_MIN = unset (first cycle, or just released).
    // last_grid_meter_power_w    : the FRITZ!Box grid_meter_power_w reading from the previous cycle, used both to
    //                   compute |Δgrid_meter_power_w| and to detect stuck-data (exact-equal repeats).
    //                   INT_MIN = unset.
    // grid_meter_stuck_counter : count of consecutive cycles where grid_meter_power_w exactly equals
    //                   last_grid_meter_power_w.  Reaches cfg.fritz_stuck_cycles → the FRITZ
    //                   reading is treated as stale and the write is suppressed
    //                   until grid_meter_power_w genuinely changes.
    // engage_baseline_solakon_grid_power_w : the ACTIVE_POWER (solakon_grid_power_w) reading captured on the cycle we most
    //                   recently transitioned INTO the engaged state (cold-start
    //                   engagement or post-release regain) — then held FIXED for the
    //                   rest of that continuous engaged streak.  Used by
    //                   cond_export_growing as a stable reference point: "is solakon_grid_power_w higher
    //                   now than it was when we started this engagement", rather than
    //                   a rolling "higher than last cycle" comparison.  solakon_grid_power_w rolling
    //                   comparison would be fooled by an ordinary one-cycle dip (solakon_grid_power_w is
    //                   not guaranteed to rise monotonically cycle-over-cycle even
    //                   while the underlying trend is upward), spuriously resetting
    //                   the "growing" signal on noise.  Comparing against a fixed
    //                   starting point only cares about net progress since engagement
    //                   began, however many cycles that took.  INT_MIN = unset (not
    //                   currently engaged, or engaged but the baseline has not been
    //                   captured yet).
    // low_soc_hold      : true while remote control is being deliberately withheld, or
    //                   held released, because BMS1_SOC is below cfg.min_control_soc (an
    //                   independent safety cutoff — see config.h).  Recovery from this
    //                   hold is driven purely by SoC reaching min_control_soc_recover_
    //                   effective — completely independent of ever_engaged/regain_counter
    //                   and the grid_meter_power_w-driven recover test used for the release-when-full
    //                   mechanism.  Also consulted at true cold start, before remote
    //                   control has ever been engaged: if SoC already reads below the
    //                   minimum on the very first cycle, this is set true before any
    //                   setpoint is ever written — "minimum charge required to control"
    //                   applies from the first cycle onward.  Invariant: low_soc_hold and
    //                   remote_engaged are never true at the same time (this flag is only
    //                   ever set within an "is not currently engaged" context).
    // ema_last_update : wall-clock time (steady_clock, monotonic) at which
    //                   ema_setpoint was last genuinely updated (seeded or
    //                   blended — never touched during apply_jitter cycles,
    //                   which freeze ema_setpoint).  Used to scale the EMA
    //                   blend factor by ACTUAL elapsed time rather than by
    //                   call count, so smoothing behaves consistently in
    //                   real time regardless of how sparsely or densely the
    //                   FRITZ!Box happens to refresh — see cfg.smoothing.
    // last_write_time : wall-clock time (steady_clock) of the most recent
    //                   successful Modbus write to the remote-control
    //                   registers — ANY write, including keep-alive refreshes
    //                   that re-send an unchanged setpoint.  Used to bound
    //                   cfg.max_ramp_w_per_s against real elapsed time since
    //                   the inverter last heard from us at all, so a long
    //                   quiet spell (even one filled with keep-alive pings)
    //                   never "banks" an oversized jump allowance — the
    //                   ramp limit stays a tight, predictable bound on every
    //                   single write, which is the point of having it.
    double ema_setpoint        = -1.0;
    int    last_written        = -1;
    bool   has_last_written    = false;  // true once last_written holds a real commanded value
                                          // (last_written itself can legitimately be negative
                                          // when cfg.enable_grid_import is set, so -1 alone can
                                          // no longer serve as the "unset" sentinel)
    bool   remote_engaged      = false;
    bool   ever_engaged        = false;
    int    regain_counter      = 0;
    int    last_solakon_grid_power_w     = std::numeric_limits<int>::min();
    int    last_grid_meter_power_w        = std::numeric_limits<int>::min();
    int    grid_meter_stuck_counter = 0;
    int    engage_baseline_solakon_grid_power_w   = std::numeric_limits<int>::min();
    bool   low_soc_hold        = low_soc_hold_restored;
    auto   low_soc_hold_since  = low_soc_hold_restored ? std::chrono::system_clock::now()
                                                        : std::chrono::system_clock::time_point{};
    auto   ema_last_update     = std::chrono::steady_clock::now();
    auto   last_write_time     = std::chrono::steady_clock::now();
    long long cycle_count      = 0;  // exposed via the REST API's status endpoint only

    // Persists the current low_soc_hold value to --state-file (best-effort;
    // logs a warning and otherwise continues on failure -- persistence is a
    // convenience for surviving restarts, not something a write failure
    // should ever abort the control loop over). Called every time
    // low_soc_hold actually changes (both automatic transitions and manual
    // ones via POST /api/v1/low-soc-hold), never on every cycle.
    auto persistLowSocHold = [&]() {
        low_soc_hold_since = std::chrono::system_clock::now();
        if (cfg.state_file.empty())
            return;
        PersistedState st;
        st.low_soc_hold = low_soc_hold;
        if (!saveState(cfg.state_file, st) && log.is_enabled(LogLevel::WARN))
            log.warn("Failed to persist low-SoC hold state to '" + cfg.state_file + "'");
    };

    // Publishes the current cycle's readings and control-loop state to ApiState for
    // the REST API's GET /api/v1/status endpoint to read.  Reads the various
    // persistent locals above (remote_engaged, owned_by_us, ...) at CALL time, so
    // calling this again later in the same cycle picks up any changes made by the
    // release/recover/setpoint logic since the previous call.  A no-op (cheap lock +
    // copy) when the REST API is disabled.
    auto syncApiSnapshot = [&](int solakon_w, bool solakon_ok,
                               int pv_w_, bool pv_ok_,
                               int bat_w, bool bat_ok,
                               int soc_,
                               uint16_t inv_st, bool inv_st_ok,
                               uint16_t grid_st, bool grid_st_ok,
                               int meter_w, bool meter_ok) {
        std::lock_guard<std::mutex> lock(apiState.mutex);
        ApiSnapshot& snap = apiState.snapshot;
        snap.have_data          = true;
        snap.solakon_grid_power_w = solakon_w; snap.solakon_grid_power_ok = solakon_ok;
        snap.pv_w               = pv_w_;    snap.pv_ok            = pv_ok_;
        snap.battery_w          = bat_w;    snap.battery_ok       = bat_ok;
        snap.soc                = soc_;
        snap.max_soc_limit      = max_soc_limit;
        snap.min_soc_limit      = min_soc_limit;
        snap.inverter_status    = inv_st;   snap.inverter_status_ok = inv_st_ok;
        snap.grid_status        = grid_st;  snap.grid_status_ok     = grid_st_ok;
        snap.grid_meter_power_w  = meter_w; snap.grid_meter_power_ok = meter_ok;
        snap.remote_engaged     = remote_engaged;
        snap.owned_by_us        = owned_by_us;
        snap.ever_engaged       = ever_engaged;
        snap.low_soc_hold       = low_soc_hold;
        snap.low_soc_hold_since = low_soc_hold_since;
        snap.has_last_written   = has_last_written;
        snap.last_written       = last_written;
        snap.cycle_count        = ++cycle_count;
        snap.updated_at         = std::chrono::system_clock::now();
    };

    // Sleep for cfg.settle_delay_ms in 100 ms increments so the wait stays responsive
    // to signals (a SIGTERM during settling should not block shutdown).  Shared by
    // both the writeRemoteControl and releaseRemoteControl transitions — see
    // settle_delay_ms in config.h for why both need it.
    auto settleDelay = [&]() {
        if (cfg.settle_delay_ms <= 0) return;
        const int steps = cfg.settle_delay_ms / 100;
        for (int i = 0; i < steps && !g_stop.load(std::memory_order_relaxed); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const int rem = cfg.settle_delay_ms % 100;
        if (rem > 0 && !g_stop.load(std::memory_order_relaxed))
            std::this_thread::sleep_for(std::chrono::milliseconds(rem));
    };

    // Refresh the Solakon's remote-control revert timeout WITHOUT changing the
    // commanded setpoint.  The anti-oscillation guards below (stuck-data
    // detection, input/output dead bands) deliberately skip re-writing an
    // unchanged setpoint on many consecutive cycles to avoid needless churn —
    // but every write also resets REMOTE_TIMEOUT_SET (46002), and a skip streak
    // longer than (interval + loop_timeout_extra) seconds lets the inverter
    // silently revert to its own work mode mid-streak while this tool's state
    // still believes remote control is engaged.  Re-sending the identical
    // last-written setpoint keeps the session alive without touching the
    // inverter's actual output (same value in, same value out), so — unlike a
    // real setpoint change — it needs no settle delay: nothing is transitioning.
    // No-op when not currently engaged, in dry-run, or before any setpoint has
    // ever been written (nothing to keep alive yet).
    auto refreshKeepAlive = [&]() -> bool {
        if (cfg.dry_run || !remote_engaged || !has_last_written)
            return true;
        const int raw_timeout = cfg.interval + cfg.loop_timeout_extra;
        const uint16_t timeout_s = static_cast<uint16_t>(
            std::min(raw_timeout, static_cast<int>(UINT16_MAX)));
        // last_written may be negative (an active grid-import command, only
        // possible when cfg.enable_grid_import is set) — re-send it through
        // the matching API so the direction bit is refreshed identically.
        const bool ok = (last_written >= 0)
            ? solakon.writeRemoteControl(last_written, timeout_s)
            : solakon.writeRemoteControlImport(-last_written, timeout_s);
        if (!ok) {
            log.error("Failed to refresh Solakon ONE remote-control keep-alive: "
                      + solakon.lastError());
            return false;
        }
        // Counts as a write for max_ramp_w_per_s purposes even though the
        // value is unchanged — see last_write_time above for why.
        last_write_time = std::chrono::steady_clock::now();
        if (log.is_enabled(LogLevel::DBG))
            log.debug("Keep-alive: re-sent setpoint " + std::to_string(last_written)
                      + " W to refresh the remote-control revert timeout");
        return true;
    };

    // One control cycle.  Returns true on success (logical: cycle completed, regardless
    // of whether a write/release was actually performed); false if a hard error means
    // the cycle could not make any decision (typically a Solakon Modbus read failure).
    auto runOnce = [&]() -> bool {
        // --- Atomic read window (Solakon ONE) -----------------------------------
        // All Solakon registers are read back-to-back to keep the inverter-side
        // measurements as close to a single instant as possible.  PV/battery/SoC/
        // status are needed only for the release decision; we read them unconditionally
        // because most cycles will at least evaluate the release conditions, and the
        // marginal cost (a few Modbus transactions, each <50 ms on a healthy LAN) is
        // small compared to the FRITZ!Box HTTP round-trip that follows.

        int      solakon_grid_power_w = 0;
        int      pv_w       = 0;
        bool     pv_ok      = true;
        int      battery_w  = 0;
        bool     battery_ok = true;
        int      soc        = -1;       // -1 = read failed
        uint16_t inv_status = 0;
        uint16_t grid_status = 0;
        bool     inv_status_ok  = true;
        bool     grid_status_ok = true;

        if (!solakon.readExportedPower(solakon_grid_power_w)) {
            log.error("Failed to read Solakon ONE ACTIVE_POWER: " + solakon.lastError());
            return false;
        }
        if (!solakon.readPvPower(pv_w)) {
            log.warn("Failed to read TOTAL_PV_POWER: " + solakon.lastError());
            pv_ok = false;
        }
        if (!solakon.readBatteryPower(battery_w)) {
            log.warn("Failed to read BATTERY_COMBINED_POWER: " + solakon.lastError());
            battery_ok = false;
        }
        if (!solakon.readBatterySoc(soc)) {
            log.warn("Failed to read BMS1_SOC: " + solakon.lastError());
            soc = -1;
        }
        if (!solakon.readInverterStatus(inv_status)) {
            log.warn("Failed to read inverter STATUS_1: " + solakon.lastError());
            inv_status_ok = false;
        }
        if (!solakon.readGridStatus(grid_status)) {
            log.warn("Failed to read GRID_STATUS: " + solakon.lastError());
            grid_status_ok = false;
        }

        // --- grid_meter_power_w: current power draw of the FRITZ!Box device ----------------------
        // Done last so the slow HTTP round-trip is the only cross-component time skew.
        FritzDevice dev;
        if (!fritz.findDeviceByAin(cfg.fritz_ain, dev)) {
            log.error("FRITZ!Box device not found or error: " + fritz.lastError());
            // The Solakon ONE reads above (solakon_grid_power_w/pv/battery/soc/status) already
            // succeeded this cycle — only the FRITZ!Box side failed, and there is no
            // fresh grid_meter_power_w to compute a new setpoint from.  Still refresh the Solakon
            // remote-control keep-alive (re-sending the unchanged last_written value)
            // so a transient/slow/erroring FRITZ!Box endpoint does not, by itself,
            // starve REMOTE_TIMEOUT_SET and cause the inverter to silently revert
            // remote control out from under us while we are otherwise perfectly
            // healthy.  If FRITZ!Box stays unreachable for longer than
            // interval + loop_timeout_extra, the inverter's own revert timeout is
            // still the ultimate fail-safe (see settle_delay_ms/REMOTE_TIMEOUT_SET
            // notes above) — this only prevents that fail-safe from being triggered
            // needlessly by a problem confined entirely to the FRITZ!Box side.
            if (!refreshKeepAlive()) return false;
            syncApiSnapshot(solakon_grid_power_w, true, pv_w, pv_ok, battery_w, battery_ok, soc,
                             inv_status, inv_status_ok, grid_status, grid_status_ok,
                             0, false);
            return false;
        }
        const int grid_meter_power_w = static_cast<int>(dev.powerW);
        // grid_meter_power_eff_w: grid_meter_power_w shifted by the configured baseline (default 0, i.e. grid_meter_power_eff_w == grid_meter_power_w).
        // Every control decision below that cares about "is grid_meter_power_w positive/negative" or
        // "what should the setpoint be" uses grid_meter_power_eff_w so the loop holds grid_meter_power_w at
        // cfg.fritz_baseline_w instead of always driving it towards 0. Anti-oscillation
        // guards on the raw reading itself (fritz_min_change, fritz_stuck_cycles) still
        // operate on grid_meter_power_w directly further below — they guard against noise/staleness
        // in the measurement, independent of where the target baseline sits.
        const int grid_meter_power_eff_w = grid_meter_power_w - cfg.fritz_baseline_w;
        const int combined = solakon_grid_power_w + grid_meter_power_eff_w;

        // Publish this cycle's raw inputs to the REST API immediately -- independent
        // of whatever the release/recover/setpoint logic below decides to do with them.
        syncApiSnapshot(solakon_grid_power_w, true, pv_w, pv_ok, battery_w, battery_ok, soc,
                         inv_status, inv_status_ok, grid_status, grid_status_ok,
                         grid_meter_power_w, true);

        // --- Manual override (REST API) -----------------------------------------
        // Set via POST /api/v1/override, cleared via DELETE /api/v1/override (see
        // restapi.h/.cpp).  Takes ABSOLUTE priority over the entire release/recover
        // state machine below.  This is a deliberate, explicit operator action, so
        // none of the release-eligibility conditions, the low-SoC hard cutoff, or
        // the post-release recover debounce are consulted while an override is
        // active.  Two modes are supported (ManualOverride::mode), both of which
        // share the same duration_s auto-expiry mechanism: once duration_s > 0
        // seconds have elapsed since the override was set, it is cleared
        // automatically and this cycle falls through to the normal
        // release/recover state machine below, exactly as if the operator had
        // just called DELETE /api/v1/override themselves.  duration_s == 0 means
        // the override stays active indefinitely, until explicitly cleared.
        //
        //   OverrideMode::Setpoint  -- every cycle simply (re)writes the operator-
        //     specified wattage, subject only to the --max-power clamp (and
        //     --enable-grid-import for negative/import values).  duration_s is
        //     ALSO passed as the Modbus REMOTE_TIMEOUT_SET revert timeout for
        //     each write, so the inverter's own safety-net timeout never lags
        //     behind the override's own auto-expiry.
        //
        //   OverrideMode::Release  -- force-releases the remote-control session
        //     (as if the release-eligibility state machine had decided to
        //     release, but triggered directly by the operator) and writes
        //     nothing while active.
        ManualOverride ov;
        {
            std::lock_guard<std::mutex> lock(apiState.mutex);
            ov = apiState.override_state;
        }

        // --- Manual low-SoC hold command (REST API) -----------------------------
        // Set via POST /api/v1/low-soc-hold, consumed here once (pending is
        // cleared immediately so a subsequent cycle does not re-apply a
        // stale request). Independent of, and processed before, the manual
        // setpoint/release override above -- this is a safety-oriented
        // control, so an operator asking to withhold control takes effect
        // even if a setpoint/release override happens to also be active.
        // Setting active=true forces an immediate release (mirroring the
        // automatic --min-control-soc cutoff); setting active=false only
        // clears the hold flag -- it does not itself force re-engagement,
        // which the normal recover/setpoint logic further below still
        // decides on its own terms.
        {
            LowSocHoldCommand cmd;
            {
                std::lock_guard<std::mutex> lock(apiState.mutex);
                cmd = apiState.low_soc_hold_command;
                apiState.low_soc_hold_command.pending = false;
            }
            if (cmd.pending && cmd.active != low_soc_hold) {
                if (log.is_enabled(LogLevel::INFO))
                    log.info(std::string("[manual low-soc-hold command: ")
                             + (cmd.active ? "set" : "clear") + "]");

                if (cmd.active && remote_engaged && owned_by_us) {
                    if (!cfg.dry_run) {
                        if (!solakon.releaseRemoteControl()) {
                            log.error("Failed to release Solakon ONE remote control "
                                      "for manual low-soc-hold command: " + solakon.lastError());
                            return false;
                        }
                        remote_engaged      = false;
                        owned_by_us         = false;
                        last_written        = -1;
                        has_last_written    = false;
                        ema_setpoint        = -1.0;
                        ema_last_update     = std::chrono::steady_clock::now();
                        regain_counter      = 0;
                        last_solakon_grid_power_w   = std::numeric_limits<int>::min();
                        last_grid_meter_power_w     = std::numeric_limits<int>::min();
                        grid_meter_stuck_counter    = 0;
                        engage_baseline_solakon_grid_power_w = std::numeric_limits<int>::min();
                    } else if (log.is_enabled(LogLevel::INFO)) {
                        log.info("[manual low-soc-hold command: would release remote control  [dry-run]]");
                    }
                }
                low_soc_hold = cmd.active;
                persistLowSocHold();

                syncApiSnapshot(solakon_grid_power_w, true, pv_w, pv_ok, battery_w, battery_ok, soc,
                                 inv_status, inv_status_ok, grid_status, grid_status_ok,
                                 grid_meter_power_w, true);

                if (cmd.active)
                    settleDelay();
            }
        }

        if (ov.active) {
            const double elapsed_s = std::chrono::duration<double>(
                std::chrono::system_clock::now() - ov.set_at).count();
            const bool expired = (ov.duration_s > 0) && (elapsed_s >= ov.duration_s);

            if (expired) {
                // Clear the override (only if it hasn't already been replaced by a
                // newer request in the meantime) and fall through to the normal
                // state machine below.
                {
                    std::lock_guard<std::mutex> lock(apiState.mutex);
                    if (apiState.override_state.mode == ov.mode
                        && apiState.override_state.set_at == ov.set_at) {
                        apiState.override_state = ManualOverride{};
                    }
                }
                if (log.is_enabled(LogLevel::INFO)) {
                    if (ov.mode == OverrideMode::Release)
                        log.info("[manual force-release expired after " + std::to_string(ov.duration_s)
                                 + " s — resuming normal release/recover control]");
                    else
                        log.info("[manual override expired after " + std::to_string(ov.duration_s)
                                 + " s — resuming normal solakon_grid_power_w+grid_meter_power_w control]");
                }
                // Falls through to the release/recover state machine below --
                // remote_engaged still reflects whatever this override left it as
                // (false for Release, true for Setpoint), so the usual logic picks
                // up exactly as it would after any other transition.
            } else if (ov.mode == OverrideMode::Release) {
                if (remote_engaged && owned_by_us) {
                    if (log.is_enabled(LogLevel::INFO))
                        log.info("[manual force-release requested"
                                 + (ov.duration_s > 0
                                        ? " for " + std::to_string(ov.duration_s) + " s"
                                        : std::string(" (indefinite)"))
                                 + " — releasing remote control]"
                                 + (cfg.dry_run ? "  [dry-run]" : ""));

                    if (!cfg.dry_run) {
                        if (!solakon.releaseRemoteControl()) {
                            log.error("Failed to release Solakon ONE remote control "
                                      "for manual force-release: " + solakon.lastError());
                            return false;
                        }
                        remote_engaged      = false;
                        owned_by_us         = false;  // we released, we no longer own it
                        last_written        = -1;
                        has_last_written    = false;
                        ema_setpoint        = -1.0;
                        ema_last_update     = std::chrono::steady_clock::now();
                        regain_counter      = 0;
                        last_solakon_grid_power_w = std::numeric_limits<int>::min();
                        last_grid_meter_power_w   = std::numeric_limits<int>::min();
                        grid_meter_stuck_counter  = 0;
                        engage_baseline_solakon_grid_power_w = std::numeric_limits<int>::min();

                        syncApiSnapshot(solakon_grid_power_w, true, pv_w, pv_ok, battery_w, battery_ok, soc,
                                         inv_status, inv_status_ok, grid_status, grid_status_ok,
                                         grid_meter_power_w, true);

                        settleDelay();
                    }
                } else if (log.is_enabled(LogLevel::DBG)) {
                    log.debug("[manual force-release active — remote control already released]");
                }
                return true;
            } else { // ov.mode == OverrideMode::Setpoint, not yet expired
                const int lower = (cfg.enable_grid_import || ov.watts < 0) ? -cfg.max_power : 0;
                const int setpoint = std::min(std::max(lower, ov.watts), cfg.max_power);

                if (log.is_enabled(LogLevel::INFO))
                    log.info("[manual override active — commanding " + std::to_string(setpoint)
                             + " W, bypassing normal solakon_grid_power_w+grid_meter_power_w control and release/recover logic]"
                             + (cfg.dry_run ? "  [dry-run]" : ""));

                last_solakon_grid_power_w = solakon_grid_power_w;
                last_grid_meter_power_w    = grid_meter_power_w;

                if (!cfg.dry_run) {
                    const int raw_timeout = (ov.duration_s > 0) ? ov.duration_s
                                                                : (cfg.interval + cfg.loop_timeout_extra);
                    const uint16_t timeout_s = static_cast<uint16_t>(
                        std::min(raw_timeout, static_cast<int>(UINT16_MAX)));
                    const bool write_ok = (setpoint >= 0)
                        ? solakon.writeRemoteControl(setpoint, timeout_s)
                        : solakon.writeRemoteControlImport(-setpoint, timeout_s);
                    if (!write_ok) {
                        log.error("Failed to write manual override to Solakon ONE: "
                                  + solakon.lastError());
                        return false;
                    }
                    if (!remote_engaged)
                        engage_baseline_solakon_grid_power_w = solakon_grid_power_w;
                    last_written      = setpoint;
                    has_last_written  = true;
                    last_write_time   = std::chrono::steady_clock::now();
                    remote_engaged    = true;
                    owned_by_us       = true;
                    ever_engaged      = true;
                    regain_counter    = 0;
                    if (low_soc_hold) { low_soc_hold = false; persistLowSocHold(); }

                    syncApiSnapshot(solakon_grid_power_w, true, pv_w, pv_ok, battery_w, battery_ok, soc,
                                     inv_status, inv_status_ok, grid_status, grid_status_ok,
                                     grid_meter_power_w, true);

                    settleDelay();
                }
                return true;
            }
        }

        // --- Decode inverter operational state ---------------------------------
        // STATUS_1 (39063): bit 0 = Standby, bit 2 = Operation, bit 6 = Fault.
        // STATUS_3 (39065): bit 0 = Off-Grid/EPS active.
        // For the release decision we require: in Operation, not in Fault, not Off-Grid.
        // If any status read failed we treat that condition as "unknown / unsafe" and
        // block release (inv_state_ok = false).
        const bool inv_in_operation = inv_status_ok && ((inv_status & 0x0004u) != 0);
        const bool inv_in_fault     = inv_status_ok && ((inv_status & 0x0040u) != 0);
        const bool inv_off_grid     = grid_status_ok && ((grid_status & 0x0001u) != 0);
        const bool inv_state_ok     = inv_status_ok && grid_status_ok
                                   && inv_in_operation && !inv_in_fault && !inv_off_grid;

        // --- Low-SoC hard safety cutoff -----------------------------------------
        // Deliberately independent of the release-when-full eligibility bundle below:
        // the tool must stop commanding additional export (and therefore additional
        // battery discharge) the instant BMS1_SOC drops below cfg.min_control_soc,
        // regardless of PV headroom, charging state, export trend, or inverter health —
        // none of those answer the question that matters here ("should we stop forcing
        // more discharge right now?").  Gated only by cfg.min_control_soc itself and (at
        // the point of use below) session ownership.
        // Safe-fail: an invalid SoC read (soc < 0) never triggers this cutoff, and never
        // clears an existing low_soc_hold either (see the recover-path handling below) —
        // a missing reading is not evidence of anything either way.
        // cfg.min_control_soc <= 0 disables this feature entirely: SoC can
        // never be negative, so the comparison can never hold for any value <= 0.
        const bool cond_low_soc_release = (cfg.min_control_soc > 0)
                                       && (soc >= 0)
                                       && (soc < cfg.min_control_soc);

        // --- Evaluate release-eligibility conditions ----------------------------
        // ALL must hold, on the SAME cycle, to trigger the immediate test-release
        // below.  See the "RELEASE ELIGIBILITY" note above actionRunLoop for why
        // work-mode and the old flat PV-threshold proxies were removed rather than
        // kept alongside these; cond_pv_headroom below is a different, relative
        // check (PV vs. our own current export) rather than a reintroduction of
        // the old absolute PV threshold.
        const int  charge_threshold  = std::max(cfg.battery_dead_band, 0);
        const bool cond_owned        = owned_by_us;
        const bool cond_soc          = (soc >= 0) && (max_soc_limit > 0)
                                    && (soc >= max_soc_limit - cfg.release_soc_hysteresis);
        const bool cond_not_charging = battery_ok && (battery_w <= charge_threshold);
        // cond_pv_headroom is bypassed once SoC has reached MAX_SOC itself (the user's
        // configured ceiling), as opposed to merely the hysteresis-relaxed cond_soc
        // threshold above.  TOTAL_PV_POWER is only a meaningful, independent "is there
        // real surplus" signal while the battery still has room left to climb toward
        // MAX_SOC.  Once SoC is genuinely AT that ceiling, some inverters/firmware have
        // been observed to curtail PV harvest down to match whatever ACTIVE_POWER we are
        // currently commanding — the battery has zero spare capacity left to buffer the
        // difference — so PV no longer independently exceeds solakon_grid_power_w no matter how much
        // sunlight is actually available, permanently vetoing a release that otherwise
        // should happen (a feedback deadlock: our own throttling suppresses the very
        // signal the headroom check is waiting for).  At soc >= max_soc_limit there is
        // nothing left to lose by testing a release anyway: the existing grid_meter_power_w-driven
        // recover debounce (release_debounce_cycles) already safely undoes a release
        // that turns out to have no real surplus behind it, exactly as it does for
        // every other release-eligibility condition.
        const bool cond_at_max_soc   = (soc >= 0) && (max_soc_limit > 0) && (soc >= max_soc_limit);
        const bool cond_pv_headroom  = cond_at_max_soc
                                    || (pv_ok && (pv_w > solakon_grid_power_w + cfg.release_pv_margin));
        const bool have_engage_baseline = (engage_baseline_solakon_grid_power_w != std::numeric_limits<int>::min());
        const bool cond_export_growing = (grid_meter_power_eff_w < 0)
                                      || (have_engage_baseline && (solakon_grid_power_w > engage_baseline_solakon_grid_power_w));
        const bool cond_inv_state    = inv_state_ok;
        const bool cond_combined     = (cfg.release_threshold <= 0)
                                    || (combined <= cfg.release_threshold);

        const bool all_release_conds = cond_owned
                                    && cond_soc
                                    && cond_not_charging
                                    && cond_pv_headroom
                                    && cond_export_growing
                                    && cond_inv_state
                                    && cond_combined;

        // --- Detailed logging of the release-decision inputs (INFO) ------------
        if (log.is_enabled(LogLevel::INFO)) {
            const char* battery_state = !battery_ok                    ? "??"
                                       : (battery_w > charge_threshold) ? "charging"
                                       : (battery_w < 0)                ? "discharging"
                                       :                                  "idle";
            std::string msg = "solakon_grid_power_w=" + std::to_string(solakon_grid_power_w)
                            + " W  grid_meter_power_w=" + std::to_string(grid_meter_power_w)
                            + (cfg.fritz_baseline_w != 0
                               ? " W (baseline=" + std::to_string(cfg.fritz_baseline_w)
                                 + " W, grid_meter_power_w-baseline=" + std::to_string(grid_meter_power_eff_w) + " W)"
                               : " W")
                            + "  solakon_grid_power_w+grid_meter_power_w=" + std::to_string(combined) + " W"
                            + "  pv=" + (pv_ok ? std::to_string(pv_w) + " W" : std::string("?? W"))
                            + "  bat=" + std::to_string(battery_w) + " W (" + battery_state + ")"
                            + "  soc=" + (soc >= 0 ? std::to_string(soc) + "%" : std::string("??"))
                            + (max_soc_limit > 0 ? "/" + std::to_string(max_soc_limit) + "%" : "")
                            + "  owned=" + (owned_by_us ? "y" : "n")
                            + "  surplus=" + (cond_export_growing ? "y" : "n")
                            + (have_engage_baseline ? "(A0=" + std::to_string(engage_baseline_solakon_grid_power_w) + " W)" : "")
                            + "  inv=" + (inv_state_ok ? "ok" :
                                          inv_off_grid ? "off-grid" :
                                          inv_in_fault ? "fault" :
                                          inv_in_operation ? "ok" : "standby")
                            + "  remote=" + (remote_engaged ? "engaged"
                                            : low_soc_hold   ? "released(low-soc-hold)"
                                                             : "released");
            if (cfg.min_control_soc > 0)
                msg += "  min-ctrl-soc=" + std::to_string(cfg.min_control_soc) + "%"
                     + (cond_low_soc_release ? "(LOW!)" : "(ok)");
            log.info(msg);
        }

        // --- Per-condition release-eligibility breakdown (INFO, while engaged) --
        // Surfaces each individual cond_* flag by name, plus which one(s) (if any)
        // are currently blocking a release, so a "why didn't it release" question
        // can be answered directly from the log instead of manually re-deriving
        // every condition from the solakon_grid_power_w/grid_meter_power_w/pv/bat/soc summary line above.  Only
        // emitted while engaged: these conditions gate the ENGAGED -> RELEASED
        // transition (see all_release_conds above) and have nothing to act on
        // otherwise — while released/recovering, cond_owned alone would read
        // "n" and drown out the rest with a foregone conclusion on every cycle.
        if (remote_engaged && log.is_enabled(LogLevel::INFO)) {
            const std::string soc_detail = (soc >= 0 ? std::to_string(soc) : std::string("??"))
                                          + (max_soc_limit > 0
                                             ? ">=" + std::to_string(std::max(0, max_soc_limit - cfg.release_soc_hysteresis))
                                             : std::string(""));
            const std::string charge_detail = std::to_string(battery_w) + "<=" + std::to_string(charge_threshold);
            const std::string pv_detail = cond_at_max_soc
                                         ? std::string("bypassed, soc>=max")
                                         : (pv_ok ? std::to_string(pv_w) : std::string("??"))
                                           + ">" + std::to_string(solakon_grid_power_w + cfg.release_pv_margin);
            const std::string combined_detail = (cfg.release_threshold > 0)
                                               ? std::to_string(combined) + "<=" + std::to_string(cfg.release_threshold)
                                               : std::string("disabled");

            // name -> (currently met?, human-readable detail shown in parens)
            struct ReleaseCond { const char* name; bool ok; std::string detail; };
            const ReleaseCond conds[] = {
                {"owned",          cond_owned,          ""},
                {"soc",            cond_soc,            soc_detail},
                {"not-charging",   cond_not_charging,   charge_detail},
                {"pv-headroom",    cond_pv_headroom,    pv_detail},
                {"export-growing", cond_export_growing, ""},
                {"inv-state",      cond_inv_state,      ""},
                {"a+b",            cond_combined,       combined_detail},
            };

            std::string cmsg = "release-check:";
            std::string blockers;
            for (const auto& c : conds) {
                cmsg += std::string(" ") + c.name + "=" + (c.ok ? "y" : "n");
                if (!c.detail.empty())
                    cmsg += "(" + c.detail + ")";
                if (!c.ok)
                    blockers += (blockers.empty() ? "" : ", ") + std::string(c.name);
            }
            cmsg += all_release_conds ? "  => ALL MET (releasing this cycle)"
                                      : "  => blocked by: " + blockers;
            log.info(cmsg);
        }

        // --- Low-SoC hard cutoff: takes priority over the release-eligibility bundle ---
        // Checked BEFORE the normal release-eligibility bundle below and independent of
        // it: a battery that has dropped below cfg.min_control_soc must stop absorbing
        // additional forced-discharge commands regardless of whether the "release when
        // full" conditions also happen to hold (they cannot, in practice, hold at the
        // same time as this one — full and low-SoC are mutually exclusive states — but
        // even so this check is evaluated first on its own terms).  Still gated on
        // owned_by_us: never release a session this process did not itself engage (same
        // ownership principle as every other release path here).
        if (remote_engaged && cond_low_soc_release && owned_by_us) {
            if (log.is_enabled(LogLevel::WARN))
                log.warn("BMS1_SOC (" + std::to_string(soc) + " %) dropped below --min-control-soc ("
                         + std::to_string(cfg.min_control_soc) + " %)"
                         + " — releasing remote control immediately to stop forcing further"
                         " battery discharge; will not resume until SoC reaches "
                         + std::to_string(min_control_soc_recover_effective) + " %"
                         + (cfg.dry_run ? "  [dry-run]" : ""));

            if (!cfg.dry_run) {
                if (!solakon.releaseRemoteControl()) {
                    log.error("Failed to release Solakon ONE remote control: "
                              + solakon.lastError());
                    return false;
                }
                remote_engaged      = false;
                owned_by_us         = false;  // we released, we no longer own it
                last_written        = -1;     // next engagement starts fresh
                has_last_written    = false;
                ema_setpoint        = -1.0;
                ema_last_update     = std::chrono::steady_clock::now();
                regain_counter      = 0;
                last_solakon_grid_power_w     = std::numeric_limits<int>::min();
                last_grid_meter_power_w        = std::numeric_limits<int>::min();
                grid_meter_stuck_counter = 0;
                engage_baseline_solakon_grid_power_w   = std::numeric_limits<int>::min();
                low_soc_hold        = true;   // hold released until SoC recovers (see below)
                persistLowSocHold();

                syncApiSnapshot(solakon_grid_power_w, true, pv_w, pv_ok, battery_w, battery_ok, soc,
                                 inv_status, inv_status_ok, grid_status, grid_status_ok,
                                 grid_meter_power_w, true);

                // Let the inverter settle into its free-run state — same rationale as
                // every other release/write transition; see settle_delay_ms.
                settleDelay();
            }
            return true;
        }

        // --- Release path: immediate behavioural test ---------------------------
        // No debounce here: the release-eligibility conditions are evaluated fresh
        // every cycle, and the instant they all hold, remote control is released
        // right away.  The release itself doubles as the test; the recover path
        // below is what detects and corrects a wrong call.
        if (remote_engaged && all_release_conds) {
            if (log.is_enabled(LogLevel::INFO))
                log.info("Release conditions met (owned, SoC, not-charging, PV headroom, surplus"
                         " trend, inverter healthy"
                         + std::string(cfg.release_threshold > 0 ? ", solakon_grid_power_w+grid_meter_power_w" : "") + ")"
                         + " — releasing remote control to test behaviour"
                         + (cfg.dry_run ? "  [dry-run]" : ""));

            if (!cfg.dry_run) {
                if (!solakon.releaseRemoteControl()) {
                    log.error("Failed to release Solakon ONE remote control: "
                              + solakon.lastError());
                    return false;
                }
                remote_engaged      = false;
                owned_by_us         = false;  // we released, we no longer own it
                last_written        = -1;     // next engagement starts fresh
                has_last_written    = false;
                ema_setpoint        = -1.0;
                ema_last_update     = std::chrono::steady_clock::now();
                regain_counter      = 0;
                last_solakon_grid_power_w     = std::numeric_limits<int>::min();
                last_grid_meter_power_w        = std::numeric_limits<int>::min();
                grid_meter_stuck_counter = 0;
                engage_baseline_solakon_grid_power_w   = std::numeric_limits<int>::min();

                syncApiSnapshot(solakon_grid_power_w, true, pv_w, pv_ok, battery_w, battery_ok, soc,
                                 inv_status, inv_status_ok, grid_status, grid_status_ok,
                                 grid_meter_power_w, true);

                // Let the inverter settle into its free-run state before the next
                // cycle's grid_meter_power_w reading feeds the recover test below — see settle_delay_ms.
                settleDelay();
            }
            return true;
        }

        // --- Recover path: regain control if released ---------------------------
        // Only reached while remote control is not currently engaged.  Three distinct
        // situations share this branch:
        //   - Low-SoC hold (low_soc_hold, or SoC already below cfg.min_control_soc at a
        //     true cold start): governed purely by SoC crossing back up to
        //     min_control_soc_recover_effective — see the dedicated block immediately
        //     below.  Deliberately independent of ever_engaged and of the grid_meter_power_w-driven
        //     regain_counter test used for the other two situations, per config.h.
        //   - True cold start (!ever_engaged, no low-SoC hold): there is no prior release
        //     decision to recover from, so the tool simply takes control on this cycle.
        //     The tool's default state is "engaged"; the release paths above are the only
        //     things that deliberately and temporarily leave that state, so there is
        //     nothing to protect against before the first engagement.
        //   - Post-release recovery (ever_engaged, no low-SoC hold): watch grid_meter_power_w only — see
        //     the "RECOVER DECISION" note above actionRunLoop.  SoC/battery/inverter-state
        //     are deliberately NOT re-checked here; they already had their say when
        //     release was tested.  The only question left is the ground truth the release
        //     was supposed to achieve: did the household actually stop importing from the
        //     grid?
        if (!remote_engaged) {
            // --- Low-SoC hold maintenance ---------------------------------------
            // Independent of the ever_engaged/regain_counter machinery below.  Applies
            // uniformly to a true cold start and to any already-released state, because
            // "minimum charge required to control" (cfg.min_control_soc) must hold before
            // the very first engagement too, not only after one has already happened.
            // Safe-fail: an invalid SoC read (soc < 0) neither raises nor clears the hold
            // this cycle — a missing reading is not evidence of anything either way, and
            // an existing hold must never be cleared on uncertain data.
            bool just_recovered_from_low_soc = false;
            if (cfg.min_control_soc > 0 && soc >= 0) {
                if (soc < cfg.min_control_soc) {
                    if (!low_soc_hold && log.is_enabled(LogLevel::WARN))
                        log.warn("BMS1_SOC (" + std::to_string(soc) + " %) is below"
                                 " --min-control-soc (" + std::to_string(cfg.min_control_soc)
                                 + " %) — withholding remote control until SoC reaches "
                                 + std::to_string(min_control_soc_recover_effective) + " %");
                    if (!low_soc_hold) { low_soc_hold = true; persistLowSocHold(); }
                } else if (low_soc_hold && soc >= min_control_soc_recover_effective) {
                    if (log.is_enabled(LogLevel::INFO))
                        log.info("BMS1_SOC (" + std::to_string(soc) + " %) reached"
                                 " --min-control-soc-recover ("
                                 + std::to_string(min_control_soc_recover_effective) + " %)"
                                 + " — resuming remote control");
                    low_soc_hold                 = false;
                    persistLowSocHold();
                    just_recovered_from_low_soc  = true;
                }
            }

            if (low_soc_hold) {
                if (log.is_enabled(LogLevel::INFO))
                    log.info("[low-soc-hold — soc="
                             + (soc >= 0 ? std::to_string(soc) + "%" : std::string("??"))
                             + "  need " + std::to_string(min_control_soc_recover_effective)
                             + "% to resume — holding released]");
                return true;
            }

            // Recovery from a low-SoC hold falls straight through to the setpoint path
            // below: it is SoC-driven and immediate, with no additional grid_meter_power_w-based debounce
            // (unlike the ever_engaged/regain_counter test it deliberately bypasses here).
            if (just_recovered_from_low_soc) {
                if (log.is_enabled(LogLevel::INFO))
                    log.info("[low-soc-hold cleared — taking control of remote setpoint]");
            } else if (!ever_engaged) {
                if (log.is_enabled(LogLevel::INFO))
                    log.info("[cold start — taking control of remote setpoint]");
                // Fall through to the setpoint path unconditionally.
            } else {
                if (grid_meter_power_eff_w > 0)
                    regain_counter++;
                else
                    regain_counter = 0;

                const int regain_needed = std::max(1, cfg.release_debounce_cycles);

                if (log.is_enabled(LogLevel::INFO))
                    log.info("[released — grid_meter_power_w=" + std::to_string(grid_meter_power_w) + " W"
                             + (cfg.fritz_baseline_w != 0
                                ? " (grid_meter_power_w-baseline=" + std::to_string(grid_meter_power_eff_w) + " W)"
                                : "")
                             + "  regain-control: " + std::to_string(regain_counter)
                             + "/" + std::to_string(regain_needed)
                             + (regain_counter < regain_needed ? " cycles — holding released]"
                                                                : " cycles — regaining control]"));

                if (regain_counter < regain_needed)
                    return true;
                // Threshold reached — fall through to the setpoint path to regain
                // control.  regain_counter is reset by the successful write below.
            }
        }

        // --- Setpoint path ------------------------------------------------------
        //
        // Input-side stale/noise guards on grid_meter_power_w (and solakon_grid_power_w).  These fire BEFORE the EMA
        // update so a suppressed reading does not pollute the smoothed state.
        //
        // Stuck-data detection: the FRITZ!Smart Energy 250 polls its internal
        // energy data on a ~2-minute interval and rounds its REST output to whole
        // watts.  Occasionally the REST endpoint returns the same exact byte-equal
        // value for several of our cycles in a row even though the real-world load
        // has changed — its cache simply has not refreshed yet.  Acting on stale
        // data as if it were fresh would drive the inverter to an incorrect
        // setpoint, but going completely silent the instant grid_meter_power_w repeats is also not
        // ideal: it can freeze the loop (and the log) for a long stretch on data
        // that may turn out to be genuine.
        //
        // We detect repeats by counting consecutive cycles where grid_meter_power_w equals the
        // previous cycle's grid_meter_power_w exactly.  For counts BELOW cfg.fritz_stuck_cycles, the
        // write still goes ahead, but with a small alternating +-1/+-2 W jitter
        // added to the setpoint (see "Apply jitter" below) instead of the normal
        // solakon_grid_power_w + grid_meter_power_w value — this keeps the loop visibly alive and gives the inverter a
        // small nudge rather than freezing outright, without pretending the stale grid_meter_power_w
        // reading justifies a real setpoint change.  Only once the count REACHES
        // cfg.fritz_stuck_cycles does the tool give up and skip the write entirely
        // until grid_meter_power_w changes to a different value; the counter resets on the first
        // changed reading either way.
        //
        // Note: a real-world household load fluctuates by at least ±1 W from
        // second to second, so a byte-equal repeat across multiple cycles at
        // 8–60 s intervals is a strong signal that the data is stale, not that
        // the load is genuinely constant.
        if (last_grid_meter_power_w != std::numeric_limits<int>::min() && grid_meter_power_w == last_grid_meter_power_w) {
            grid_meter_stuck_counter++;
        } else {
            grid_meter_stuck_counter = 0;
        }

        const bool grid_meter_unchanged   = (last_grid_meter_power_w != std::numeric_limits<int>::min())
                                     && (grid_meter_power_w == last_grid_meter_power_w);
        const bool grid_meter_fully_stuck = (cfg.fritz_stuck_cycles > 0)
                                     && (grid_meter_stuck_counter >= cfg.fritz_stuck_cycles);

        if (grid_meter_fully_stuck) {
            if (log.is_enabled(LogLevel::WARN))
                log.warn("FRITZ!Box reading stuck at " + std::to_string(grid_meter_power_w)
                         + " W for " + std::to_string(grid_meter_stuck_counter)
                         + " cycles — withholding keep-alive, letting remote control"
                           " lapse on the inverter's own revert timeout until grid_meter_power_w changes");
            last_solakon_grid_power_w = solakon_grid_power_w;
            last_grid_meter_power_w    = grid_meter_power_w;

            // Deliberately do NOT call refreshKeepAlive() here, unlike the other
            // skip paths below (inputs_unchanged, below_min_change).  Those skip
            // paths mean "nothing changed enough to warrant a new value, but the
            // data is still trustworthy" — refreshing the keep-alive there is
            // correct, exactly per the "keep-alive during skipped writes" note.
            // grid_meter_fully_stuck is a different claim entirely: we have decided the
            // data itself is stale/untrustworthy ("skipping write until value
            // changes").  Continuing to refresh REMOTE_TIMEOUT_SET indefinitely
            // while explicitly distrusting the only input that would justify a new
            // setpoint defeats the whole point of giving up — it just props the
            // session up forever on data we no longer believe.  Instead, let the
            // timeout set by our last real write (or keep-alive) run out naturally,
            // exactly like an explicit release.

            const int      timeout_s = cfg.interval + cfg.loop_timeout_extra;
            const long long elapsed_s = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - last_write_time).count();

            // Once that timeout has had time to elapse, the inverter has already
            // reverted to its own work mode on its own — resync our software state
            // to match instead of continuing to believe we still hold the session.
            // This lets the existing recover-path logic (regain_counter / grid_meter_power_w-driven
            // test) take over correctly the next time grid_meter_power_w changes, exactly as it
            // would after any other release.
            if (remote_engaged && has_last_written && elapsed_s >= timeout_s) {
                if (log.is_enabled(LogLevel::INFO))
                    log.info("Remote-control revert timeout (" + std::to_string(timeout_s)
                             + " s) has elapsed since our last write to the Solakon ONE —"
                               " the inverter has reverted to its own work mode on its own;"
                               " releasing ownership in our state to match");
                remote_engaged    = false;
                owned_by_us       = false;   // the inverter dropped it, not us — no longer ours
                last_written      = -1;
                has_last_written  = false;
                ema_setpoint      = -1.0;
                ema_last_update   = std::chrono::steady_clock::now();
                regain_counter    = 0;
                engage_baseline_solakon_grid_power_w = std::numeric_limits<int>::min();
            }
            return true;
        }

        // grid_meter_power_w is exactly unchanged, but not yet stuck long enough to give up on
        // entirely — jitter the setpoint instead of skipping (see the write block
        // below).  Only meaningful while stuck-data detection itself is enabled;
        // with cfg.fritz_stuck_cycles == 0 (disabled), an unchanged grid_meter_power_w falls through
        // to the ordinary input-side dead band below exactly as before.
        const bool apply_jitter = (cfg.fritz_stuck_cycles > 0) && grid_meter_unchanged;

        // Input-side dead band: skip the write if it would not respond to any
        // real-world change.  grid_meter_power_w (the FRITZ!Box reading) is the ground-truth
        // signal here, and it — ALONE — decides whether this cycle's write is
        // redundant, whenever its guard (--fritz-min-change) is enabled.  solakon_grid_power_w's
        // own cycle-to-cycle delta must NOT independently gate this decision,
        // for two reasons:
        //
        //   1. solakon_grid_power_w only moves in response to OUR OWN previous write (the
        //      inverter tracks whatever we last commanded).  So "solakon_grid_power_w unchanged"
        //      is not independent evidence of anything — it is trivially true
        //      on every cycle following a skip, since nothing is driving solakon_grid_power_w to
        //      move.  If solakon_grid_power_w's own quiet state could ALSO independently trigger
        //      a skip (i.e. skip whenever EITHER solakon_grid_power_w or grid_meter_power_w is quiet), the very
        //      first skipped cycle would latch solakon_grid_power_w "unchanged" forever, and the
        //      loop would never write again regardless of what grid_meter_power_w does
        //      afterwards — a permanent freeze, not just a missed cycle.
        //   2. Letting a quiet grid_meter_power_w skip on its own also fixes a compounding bug:
        //      if grid_meter_power_w is quiet (including "stuck" on a stale FRITZ!Box cache
        //      value not yet caught by --fritz-stuck-cycles) while solakon_grid_power_w has just
        //      moved to catch up with a PREVIOUS write that already contains
        //      that same stale/noisy grid_meter_power_w, writing again would re-add the same grid_meter_power_w
        //      on top of a solakon_grid_power_w that already absorbed it — a setpoint (and
        //      battery charge/discharge power) that creeps upward every
        //      interval despite no real change in load.
        //
        // solakon_grid_power_w's guard (--min-change) only takes over, on its own, as a fallback
        // when grid_meter_power_w's guard is disabled (--fritz-min-change 0).  If both guards
        // are disabled, this check never skips.  The first cycle (sentinel)
        // and any cycle after a release bypass the check, because there is no
        // previous value to compare against.  An exact-match grid_meter_power_w that is being
        // jittered (apply_jitter) also bypasses this check entirely: jittering
        // IS the write for this cycle, not a candidate for suppression.
        const bool have_prev_inputs = (last_solakon_grid_power_w != std::numeric_limits<int>::min())
                                   && (last_grid_meter_power_w   != std::numeric_limits<int>::min());
        const bool a_guard_active = cfg.min_change > 0;
        const bool b_guard_active = cfg.fritz_min_change > 0;
        const bool a_within_band  = a_guard_active
                                 && (std::abs(solakon_grid_power_w - last_solakon_grid_power_w) < cfg.min_change);
        const bool b_within_band  = b_guard_active
                                 && (std::abs(grid_meter_power_w - last_grid_meter_power_w) < cfg.fritz_min_change);
        const bool inputs_unchanged = !apply_jitter
                                   && have_prev_inputs
                                   && (b_guard_active ? b_within_band : a_within_band);

        if (inputs_unchanged) {
            if (log.is_enabled(LogLevel::INFO)) {
                std::string msg = "Inputs unchanged since last cycle: ";
                if (b_guard_active)
                    msg += "|Δgrid_meter_power_w|=" + std::to_string(std::abs(grid_meter_power_w - last_grid_meter_power_w))
                         + "<" + std::to_string(cfg.fritz_min_change)
                         + "  (grid_meter_power_w guard — Δsolakon_grid_power_w ignored while grid_meter_power_w guard is active)";
                else
                    msg += "|Δsolakon_grid_power_w|=" + std::to_string(std::abs(solakon_grid_power_w - last_solakon_grid_power_w))
                         + "<" + std::to_string(cfg.min_change)
                         + "  (solakon_grid_power_w guard fallback — grid_meter_power_w guard disabled)";
                msg += "  — skipping write";
                log.info(msg);
            }
            last_solakon_grid_power_w = solakon_grid_power_w;
            last_grid_meter_power_w    = grid_meter_power_w;
            if (!refreshKeepAlive()) return false;
            return true;
        }

        // Record this cycle's inputs as the reference for the next cycle's
        // delta computation.  Done before the write so that a later write-failure
        // still updates the inputs (the failure itself is logged elsewhere).
        last_solakon_grid_power_w = solakon_grid_power_w;
        last_grid_meter_power_w    = grid_meter_power_w;

        // Lower bound for the setpoint: 0 by default (negative solakon_grid_power_w+grid_meter_power_w is clamped away,
        // matching the original "never import" behaviour), or -max_power when
        // cfg.enable_grid_import is set — a negative setpoint in that range means
        // "actively import this many watts from the grid" (see the signed write
        // dispatch below).  Symmetric with the upper bound (max_power) used for
        // export.
        const int setpoint_lower_bound = cfg.enable_grid_import ? -cfg.max_power : 0;

        // Raw setpoint: solakon_grid_power_w + grid_meter_power_w, clamped to [setpoint_lower_bound, max_power].
        const int raw_setpoint = std::min(std::max(setpoint_lower_bound, solakon_grid_power_w + grid_meter_power_eff_w), cfg.max_power);

        // EMA smoothing: blend raw with the previous smoothed value — but ONLY
        // when grid_meter_power_w is not a stale repeat (!apply_jitter).  While apply_jitter is
        // true, solakon_grid_power_w is itself just the inverter tracking whatever
        // setpoint we most recently commanded — it is NOT independent evidence
        // that the load has grown.  Recomputing raw = solakon_grid_power_w + grid_meter_power_w here would re-add
        // the same (unchanged, possibly stale) grid_meter_power_w on top of a solakon_grid_power_w that already
        // reflects last cycle's write, and blend that inflated value into
        // ema_setpoint.  Repeated over consecutive jitter cycles this is a
        // feedback loop, not smoothing: each cycle's setpoint becomes next
        // cycle's solakon_grid_power_w, which plus the same grid_meter_power_w produces yet another increase of
        // roughly grid_meter_power_w (or smoothing * grid_meter_power_w) — a real, substantial climb (or fall)
        // dressed up as "jitter", not the small +-1/+-2 W probe the jitter
        // mechanism is supposed to be.  Freezing ema_setpoint while
        // apply_jitter is true keeps the jitter self-contained: a nudge on
        // top of the last genuine (non-stale-grid_meter_power_w) setpoint, exactly as the
        // "instead of the unchanged solakon_grid_power_w + grid_meter_power_w value" rationale in config.h
        // describes — not a full re-application of that unchanged value with
        // a cosmetic +-1/+-2 W dressed on top.
        // On the very first cycle (ema_setpoint < 0) seed the accumulator with
        // the raw value so we don't start from 0 and ramp up slowly; this can
        // never coincide with apply_jitter, which requires a previous grid_meter_power_w
        // reading to compare against.
        //
        // Time-based blend factor: cfg.smoothing is defined as the fraction of
        // the gap closed by a genuine update that arrives exactly cfg.interval
        // seconds after the previous one — the assumption a plain per-call EMA
        // silently makes.  The FRITZ!Box's own refresh cadence is not fixed,
        // though (observed anywhere from ~10 s to ~110 s in the field), so
        // genuine updates do not actually arrive once per interval — they
        // arrive whenever grid_meter_power_w happens to change, however long that takes.
        // Scaling by ACTUAL elapsed wall-clock time (dt) since the last
        // genuine update, rather than by call count, makes cfg.smoothing mean
        // the same thing in real time no matter how sparse or dense genuine
        // updates happen to be — equivalent to a continuous-time exponential
        // (RC) filter with time constant tau = -interval / ln(1 - smoothing),
        // just parameterised so existing --smoothing/--interval values keep
        // their current meaning in the common case and self-correct
        // automatically otherwise:
        //   dt == interval (the assumption baked into the old formula)
        //     -> alpha == smoothing, unchanged from before.
        //   dt >> interval (FRITZ was slow to refresh this time)
        //     -> alpha -> 1: a rare, hard-won sample is trusted close to
        //     fully instead of applying only "smoothing" of it and leaving
        //     the rest to wait for another full refresh interval.
        //   dt <  interval (FRITZ refreshed unusually fast this time)
        //     -> alpha shrinks proportionally: extra caution on a
        //     suspiciously quick change, which is weighted less heavily
        //     against noise/transients than a change confirmed by a longer
        //     wait would be.
        // No rate estimation is needed — dt is a direct, exact measurement of
        // whatever gap actually just occurred, so this adapts automatically
        // to a refresh cadence that itself varies over time.
        if (!apply_jitter) {
            if (ema_setpoint < 0.0) {
                ema_setpoint = static_cast<double>(raw_setpoint);
            } else {
                const double dt_seconds = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - ema_last_update).count();
                const double interval_s = static_cast<double>(std::max(cfg.interval, 1));
                const double n     = std::max(0.0, dt_seconds) / interval_s;
                const double alpha = 1.0 - std::pow(1.0 - cfg.smoothing, n);
                ema_setpoint = alpha * raw_setpoint + (1.0 - alpha) * ema_setpoint;
            }
            ema_last_update = std::chrono::steady_clock::now();
        }

        // Round to the nearest watt.  While apply_jitter is true this is simply
        // the last genuine (non-stale-grid_meter_power_w) setpoint, unchanged, since ema_setpoint
        // was frozen above; jitter is applied afterwards and deliberately kept
        // OUT of ema_setpoint itself, so a run of stuck cycles doesn't leave the
        // smoothing accumulator permanently offset once grid_meter_power_w starts moving again.
        const int setpoint_base = std::max(setpoint_lower_bound, std::min(
            static_cast<int>(ema_setpoint + 0.5),
            cfg.max_power));

        // Apply jitter: a small alternating +-1/+-2 W nudge, cycling with
        // grid_meter_stuck_counter so consecutive stuck cycles don't all push the same
        // direction (which would just look like a slow, deliberate ramp rather than
        // a probe).  Re-clamped to [setpoint_lower_bound, max_power] same as any
        // other setpoint.
        int jitter = 0;
        if (apply_jitter) {
            static constexpr int kJitterPattern[4] = {1, -1, 2, -2};
            jitter = kJitterPattern[(grid_meter_stuck_counter - 1) % 4];
        }
        int setpoint = std::min(std::max(setpoint_lower_bound, setpoint_base + jitter), cfg.max_power);

        // Ramp limit: bound how far THIS write may move the commanded setpoint
        // away from last_written, in watts, relative to REAL elapsed time since
        // the most recent write of any kind — including keep-alive refreshes,
        // see last_write_time above — regardless of how large the underlying
        // EMA/jitter target above has moved.  This guards against a DIFFERENT
        // failure mode than smoothing: smoothing's time-based alpha (above)
        // deliberately trusts a fresh sample close to fully once dt is large,
        // which is correct for TRACKING the true target — but says nothing
        // about how fast the WRITTEN value is allowed to get there, so a big
        // step is still fully applied in one write unless bounded here too.
        // Skipped for the very first write of a fresh engagement
        // (last_written < 0): there is no previous commanded value yet to ramp
        // from, so there is nothing to protect against on that write.
        // Default (cfg.max_ramp_w_per_s <= 0): disabled, matching pre-existing
        // behaviour exactly.
        const int ramp_pre_limit = setpoint;
        bool      ramp_limited   = false;
        if (cfg.max_ramp_w_per_s > 0 && has_last_written) {
            const double dt_seconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - last_write_time).count();
            const int max_step = static_cast<int>(
                std::max(0.0, dt_seconds) * cfg.max_ramp_w_per_s);
            const int lower = std::max(setpoint_lower_bound, last_written - max_step);
            const int upper = std::min(cfg.max_power, last_written + max_step);
            const int limited = std::min(std::max(setpoint, lower), upper);
            if (limited != setpoint) {
                ramp_limited = true;
                setpoint     = limited;
            }
        }

        // Dead band: skip the write if the change is below the threshold.
        // Bypassed entirely while jittering — the jitter's whole purpose is to
        // force a write despite the setpoint otherwise looking unchanged.
        // Evaluated against the final (possibly ramp-limited) setpoint, so a
        // step ramp-limited down to something tiny can still be suppressed.
        const bool below_min_change = !apply_jitter
                                   && (cfg.min_change > 0)
                                   && has_last_written
                                   && (std::abs(setpoint - last_written) < cfg.min_change);

        if (log.is_enabled(LogLevel::INFO)) {
            std::string msg = "raw=" + std::to_string(raw_setpoint)
                            + " W  ema=" + std::to_string(setpoint_base)
                            + " W  (max=" + std::to_string(cfg.max_power) + " W)";
            if (apply_jitter)
                msg += "  [grid_meter_power_w unchanged " + std::to_string(grid_meter_stuck_counter)
                     + "/" + std::to_string(cfg.fritz_stuck_cycles)
                     + " cycles — jitter " + (jitter > 0 ? "+" : "") + std::to_string(jitter)
                     + " W -> " + std::to_string(ramp_pre_limit) + " W]";
            if (ramp_limited)
                msg += "  [ramp-limited: " + std::to_string(ramp_pre_limit) + " -> "
                     + std::to_string(setpoint) + " W (max "
                     + std::to_string(cfg.max_ramp_w_per_s) + " W/s)]";
            if (below_min_change)
                msg += "  [skip: |" + std::to_string(setpoint) + "-"
                     + std::to_string(last_written) + "|<" + std::to_string(cfg.min_change) + "]";
            if (cfg.dry_run)
                msg += "  [dry-run]";
            log.info(msg);
        }

        if (below_min_change) {
            if (!refreshKeepAlive()) return false;
            return true; // not an error — just no write needed
        }

        if (!cfg.dry_run) {
            // Timeout = interval + loop_timeout_extra so the inverter reverts automatically
            // if the loop crashes or is killed before the next cycle.
            // For run-once (interval == 0) use loop_timeout_extra alone as the timeout.
            const int raw_timeout = cfg.interval + cfg.loop_timeout_extra;
            const uint16_t timeout_s = static_cast<uint16_t>(
                std::min(raw_timeout, static_cast<int>(UINT16_MAX)));
            // A negative setpoint (only possible when cfg.enable_grid_import is set;
            // see setpoint_lower_bound above) means "import this many watts from the
            // grid" — dispatch to writeRemoteControlImport() with the magnitude
            // instead of writeRemoteControl().
            const bool write_ok = (setpoint >= 0)
                ? solakon.writeRemoteControl(setpoint, timeout_s)
                : solakon.writeRemoteControlImport(-setpoint, timeout_s);
            if (!write_ok) {
                log.error("Failed to write Solakon ONE: " + solakon.lastError());
                return false;
            }
            if (!remote_engaged) {
                // Transitioning INTO the engaged state this cycle (cold start or
                // post-release regain) — capture the fixed solakon_grid_power_w baseline that
                // cond_export_growing will compare against for the rest of this
                // continuous engaged streak.  Deliberately NOT refreshed on later
                // writes while already engaged; see engage_baseline_solakon_grid_power_w above.
                engage_baseline_solakon_grid_power_w = solakon_grid_power_w;
            }
            last_written     = setpoint;
            has_last_written = true;
            last_write_time  = std::chrono::steady_clock::now();
            remote_engaged   = true;
            owned_by_us      = true;     // we engaged, we now own the session
            ever_engaged     = true;     // no longer a "true cold start" from here on
            regain_counter   = 0;        // regain complete (harmless no-op if already engaged)

            syncApiSnapshot(solakon_grid_power_w, true, pv_w, pv_ok, battery_w, battery_ok, soc,
                             inv_status, inv_status_ok, grid_status, grid_status_ok,
                             grid_meter_power_w, true);

            // Let the inverter ramp to the new setpoint before the next cycle's
            // reads; otherwise solakon_grid_power_w reads partway through the ramp and feeds back as
            // a stale measurement, causing systematic overcorrection.
            settleDelay();
        }
        return true;
    };

    // Helper: clean shutdown — release remote control if we currently own it.
    // Called after the main loop exits (whether by signal or by --interval 0
    // completion).  This prevents the inverter from being left under a stale
    // setpoint waiting up to (interval + loop_timeout_extra) seconds for its
    // revert timeout to expire after our process is gone.
    //
    // In dry-run mode we skip the release write (consistent with the rest of
    // the dry-run policy: never modify inverter state).
    auto cleanShutdownRelease = [&]() {
        if (!owned_by_us) {
            if (log.is_enabled(LogLevel::DBG))
                log.debug("Clean shutdown: no remote-control session owned by us, nothing to release");
            return;
        }
        if (cfg.dry_run) {
            if (log.is_enabled(LogLevel::INFO))
                log.info("Clean shutdown: would release remote control [dry-run]");
            return;
        }
        if (log.is_enabled(LogLevel::INFO))
            log.info("Clean shutdown: releasing remote control");
        if (!solakon.releaseRemoteControl()) {
            log.error("Clean shutdown: failed to release Solakon ONE remote control: "
                      + solakon.lastError());
        }
    };

    // --interval 0: run a single cycle and exit.  Still release on the way out
    // if we engaged remote control, so a one-shot test invocation does not
    // leave the inverter capped for up to a minute waiting for the revert
    // timeout to expire.
    if (cfg.interval == 0) {
        const bool ok = runOnce();
        cleanShutdownRelease();
        return ok ? 0 : 1;
    }

    // Continuous loop with graceful signal handling.
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    while (!g_stop.load(std::memory_order_relaxed)) {
        runOnce();

        if (g_stop.load(std::memory_order_relaxed)) break;

        if (log.is_enabled(LogLevel::INFO))
            log.info("Waiting " + std::to_string(cfg.interval) + " s until next cycle");

        // Sleep in 100 ms increments so we stay responsive to signals.
        for (int i = 0; i < cfg.interval * 10 && !g_stop.load(std::memory_order_relaxed); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (log.is_enabled(LogLevel::INFO))
        log.info("Shutting down");

    cleanShutdownRelease();

    return 0;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    Config cfg;
    auto& log = Logger::instance();

    // ---- Top-level app ----
    CLI::App app{
        "Controls the export power of a FoxESS Solakon ONE battery inverter\n"
        "based on the current load of a FRITZ!Box Smart Home device.\n"
        "\n"
        "Usage:\n"
        "  solakon-one-fritz-powerregulator [global-options] <subcommand> [subcommand-options]\n"
        "\n"
        "Global options must appear BEFORE the subcommand name.\n"
        "Subcommand options must appear AFTER the subcommand name.\n"
        "\n"
        "Examples:\n"
        "  solakon-one-fritz-powerregulator -f fritz.box -u admin -p secret list-fritz-devices\n"
        "  solakon-one-fritz-powerregulator -H 192.168.1.148 read-solakon\n"
        "  solakon-one-fritz-powerregulator -H 192.168.1.148 write-solakon --watts 3500 --timeout 120\n"
        "  solakon-one-fritz-powerregulator --config /etc/solakon-one-fritz-powerregulator/solakon-one-fritz-powerregulator.conf"
            " run-loop --interval 30\n"
        "\n"
        "All options can also be set in the INI config file (CLI flags take precedence).\n"
        "Run 'solakon-one-fritz-powerregulator <subcommand> --help' for subcommand-specific options.\n"
    };
    app.set_version_flag("--version", "1.0.0");
    app.set_config("-c,--config",
                   "/etc/solakon-one-fritz-powerregulator/solakon-one-fritz-powerregulator.conf",
                   "Config file (INI format; CLI flags take precedence)");
    app.require_subcommand(1);

    // ---- Global options (available to all subcommands) ----

    // Solakon ONE connection.
    app.add_option("-H,--solakon-host",     cfg.solakon_host,     "Solakon ONE hostname or IP")
       ->default_val(cfg.solakon_host);
    app.add_option("-P,--solakon-port",     cfg.solakon_port,     "Modbus TCP port")
       ->default_val(cfg.solakon_port);
    app.add_option("-s,--solakon-slave-id", cfg.solakon_slave_id, "Modbus slave/unit ID")
       ->default_val(cfg.solakon_slave_id);

    // FRITZ!Box connection.
    app.add_option("-f,--fritz-host",     cfg.fritz_host,     "FRITZ!Box hostname or IP")
       ->default_val(cfg.fritz_host);
    app.add_option("--fritz-scheme",     cfg.fritz_scheme,   "Connection scheme: http or https")
       ->default_val(cfg.fritz_scheme)
       ->check(CLI::IsMember({"http", "https"}));
    app.add_option("-u,--fritz-username", cfg.fritz_username, "FRITZ!Box username");
    app.add_option("-p,--fritz-password", cfg.fritz_password, "FRITZ!Box password");
    app.add_option("-a,--fritz-ain",      cfg.fritz_ain,      "AIN of the FRITZ!Box device");
    app.add_flag  ("--fritz-ignore-ssl",  cfg.fritz_ignore_ssl,
                   "Ignore FRITZ!Box TLS certificate errors");
    app.add_option("--fritz-filter-product", cfg.fritz_filter_products,
                   "Restrict devices to those whose product name contains this substring\n"
                   "(case-insensitive).  Repeat the flag to allow multiple products;\n"
                   "a device passes if its name matches ANY entry.\n"
                   "Applied by list-fritz-devices, read-fritz-device, check-fritz-device,\n"
                   "and the run-loop suitability check.\n"
                   "Default: \"FRITZ!Smart Energy 250\" — the only FRITZ!Smart Energy device\n"
                   "that supports bidirectional metering (distinguishing between consumption\n"
                   "and export).  All other models report only net consumption and will\n"
                   "produce incorrect setpoints when used with run-loop.\n"
                   "Pass no --fritz-filter-product arguments to disable the filter entirely.\n"
                   "Examples:\n"
                   "  --fritz-filter-product 'FRITZ!Smart Energy 250'\n"
                   "  --fritz-filter-product 'FRITZ!Smart Energy 250' --fritz-filter-product 'FRITZ!Smart Energy 350'")
       ->default_str("FRITZ!Smart Energy 250")
       ->allow_extra_args(false);
    app.add_option("--fritz-filter-type", cfg.fritz_filter_type,
                   "Filter devices by capability type (case-sensitive substring match\n"
                   "against the comma-separated capability list).\n"
                   "Known capabilities: switch, energyMeter, temperatureSensor, thermostat,\n"
                   "  dimmer, colorBulb, blind, humiditySensor, alarm.\n"
                   "Example: --fritz-filter-type energyMeter");

    // Common control flags.
    app.add_flag("-n,--dry-run", cfg.dry_run,
                 "Read values but do not write to Solakon ONE");
    app.add_option("-m,--max-power", cfg.max_power,
                   "Maximum export power setpoint in watts (default: 800).\n"
                   "The computed setpoint (run-loop) and the explicit value (write-solakon)\n"
                   "are both clamped to this limit before being sent to the inverter.")
       ->check(CLI::Range(0, std::numeric_limits<int>::max()));
     // Hidden global options: allow interval and loop-timeout-extra to be set via the
     // INI config file (bare keys, no section header).  The run-loop subcommand exposes
     // these as visible options; a CLI value there overwrites whatever the config file set.
    app.add_option("--interval", cfg.interval)->group("");
    app.add_option("--loop-timeout-extra", cfg.loop_timeout_extra)->group("")
       ->check(CLI::Range(0, 65535));
    app.add_option("--smoothing", cfg.smoothing)->group("")
       ->check(CLI::Range(0.0, 1.0));
    app.add_option("--max-ramp-w-per-s", cfg.max_ramp_w_per_s)->group("")
       ->check(CLI::Range(0, std::numeric_limits<int>::max()));
    app.add_option("--min-change", cfg.min_change)->group("")
       ->check(CLI::Range(0, std::numeric_limits<int>::max()));
    app.add_option("--fritz-min-change", cfg.fritz_min_change)->group("")
       ->check(CLI::Range(0, std::numeric_limits<int>::max()));
    app.add_option("--fritz-stuck-cycles", cfg.fritz_stuck_cycles)->group("")
       ->check(CLI::Range(0, std::numeric_limits<int>::max()));
    app.add_option("--fritz-baseline-w", cfg.fritz_baseline_w)->group("");
    app.add_option("--release-threshold", cfg.release_threshold)->group("")
       ->check(CLI::Range(0, std::numeric_limits<int>::max()));
    app.add_option("--release-soc-hysteresis", cfg.release_soc_hysteresis)->group("")
       ->check(CLI::Range(0, 100));
    app.add_option("--release-debounce-cycles", cfg.release_debounce_cycles)->group("")
       ->check(CLI::Range(0, std::numeric_limits<int>::max()));
    app.add_option("--battery-dead-band", cfg.battery_dead_band)->group("")
       ->check(CLI::Range(0, std::numeric_limits<int>::max()));
    app.add_option("--release-pv-margin", cfg.release_pv_margin)->group("");
    app.add_flag  ("--recover-remote-on-start,!--no-recover-remote-on-start",
                   cfg.recover_remote_on_start)->group("");
    app.add_option("--settle-delay-ms", cfg.settle_delay_ms)->group("")
       ->check(CLI::Range(0, std::numeric_limits<int>::max()));
    app.add_option("--min-control-soc", cfg.min_control_soc)->group("")
       ->check(CLI::Range(0, 100));
    app.add_option("--min-control-soc-recover", cfg.min_control_soc_recover)->group("")
       ->check(CLI::Range(0, 100));
    app.add_option("--state-file", cfg.state_file)->group("");
    app.add_flag  ("--enable-grid-import,!--no-enable-grid-import",
                   cfg.enable_grid_import)->group("");

    // REST API (hidden global options; visible copies added on run-loop below).
    app.add_flag  ("--api-enabled,!--no-api-enabled", cfg.api_enabled)->group("");
    app.add_option("--api-host", cfg.api_host)->group("");
    app.add_option("--api-port", cfg.api_port)->group("")
       ->check(CLI::Range(1, 65535));
    app.add_option("--api-key", cfg.api_key)->group("");
    app.add_flag  ("--api-tls-enabled,!--no-api-tls-enabled", cfg.api_tls_enabled)->group("");
    app.add_option("--api-tls-cert-file", cfg.api_tls_cert_file)->group("");
    app.add_option("--api-tls-key-file", cfg.api_tls_key_file)->group("");

    // Logging.
    int verbose_count    = 0;
    int log_level_override = -1;
    app.add_flag ("-v,--verbose",   verbose_count,      "Increase verbosity (repeatable: -vvvv)")
       ->multi_option_policy(CLI::MultiOptionPolicy::Sum);
    app.add_option("--log-level",   log_level_override, "Log level 0-5 (overrides -v)")
       ->check(CLI::Range(0, 5));

    // ---- Subcommands ----

    // run-loop
    auto* scLoop = app.add_subcommand("run-loop",
        "Run the control cycle, writing the computed setpoint to the Solakon ONE.\n"
        "Reads the Solakon ONE's own grid power (solakon_grid_power_w) and the FRITZ!Box\n"
        "device's grid-meter power (grid_meter_power_w), then writes\n"
        "solakon_grid_power_w + grid_meter_power_w (clamped to >= 0) as the new export setpoint.\n"
        "Repeats every --interval seconds; use --interval 0 to run once and exit.\n"
        "\n"
        "Anti-oscillation options:\n"
        "  --smoothing applies an exponential moving average to the setpoint each cycle;\n"
        "  --min-change suppresses writes when the setpoint barely changed.\n"
        "\n"
        "Usage: solakon-one-fritz-powerregulator [global-options] run-loop [options]");
    scLoop->add_option("-i,--interval", cfg.interval,
                       "Poll interval in seconds; 0 = run once and exit (default: 60)");
    scLoop->add_option("--loop-timeout-extra", cfg.loop_timeout_extra,
                       "Extra seconds added to --interval for the inverter revert timeout\n"
                       "(inverter reverts to normal if no new setpoint arrives within\n"
                       "interval + loop-timeout-extra seconds; default: 30)")
          ->check(CLI::Range(0, 65535));
    scLoop->add_option("--smoothing", cfg.smoothing,
                       "EMA smoothing factor applied to each GENUINE update, i.e. one that\n"
                       "  arrives with a real (non-repeated) FRITZ!Box reading (0.0-1.0;\n"
                       "  default: 1.0).  1.0 = no smoothing (tracks measurements exactly).\n"
                       "  Lower values reduce oscillation by slowing the response, BUT the\n"
                       "  blend factor is scaled by actual elapsed time since the previous\n"
                       "  genuine update relative to --interval, not by call count:\n"
                       "    alpha = 1 - (1 - smoothing) ^ (dt / interval)\n"
                       "  so smoothing keeps meaning the same thing in real time no matter\n"
                       "  how often the FRITZ!Box actually refreshes (observed to vary\n"
                       "  between ~10 s and ~110 s in the field) — a rare update after a\n"
                       "  long gap is trusted close to fully; a suspiciously quick one is\n"
                       "  trusted less.  See --max-ramp-w-per-s for a separate bound on how\n"
                       "  fast the WRITTEN setpoint itself may move.\n"
                       "  Suggested starting point: 0.5")
          ->check(CLI::Range(0.0, 1.0));
    scLoop->add_option("--max-ramp-w-per-s", cfg.max_ramp_w_per_s,
                       "Maximum rate of change, in watts per second of real elapsed time,\n"
                       "  allowed for the WRITTEN setpoint (default: 0 = disabled).\n"
                       "  Applied after smoothing/jitter, bounding how large a single\n"
                       "  write's step may be regardless of how far the underlying target\n"
                       "  has moved — protects the inverter/battery from an abrupt\n"
                       "  commanded swing (e.g. right after a long FRITZ!Box refresh gap,\n"
                       "  when --smoothing correctly trusts the fresh reading almost fully).\n"
                       "  Measured since the most recent write of ANY kind, including\n"
                       "  keep-alive refreshes, so a long quiet spell never \"banks\" an\n"
                       "  oversized jump allowance.  The very first write of a fresh\n"
                       "  engagement (cold start or post-release regain) is exempt: there\n"
                       "  is no previous commanded value yet to ramp from.")
          ->check(CLI::Range(0, std::numeric_limits<int>::max()));
    scLoop->add_option("--min-change", cfg.min_change,
                       "Output-side dead band in watts (default: 1).\n"
                       "  Skips the Solakon ONE write in TWO situations:\n"
                       "    1. Output-side: |new_setpoint - last_written| < min-change.\n"
                       "    2. Input-side (FALLBACK ONLY, used when --fritz-min-change is 0):\n"
                       "       |solakon_grid_power_w - last_solakon_grid_power_w| < min-change.\n"
                       "       Ignored on the input side whenever --fritz-min-change is\n"
                       "       nonzero (enabled) — see --fritz-min-change for why solakon_grid_power_w cannot\n"
                       "       independently veto a write while grid_meter_power_w's guard is active.\n"
                       "  The default of 1 suppresses redundant writes when the inverter\n"
                       "  export is byte-stable, in addition to filtering measurement noise.\n"
                       "  Set to 0 to disable (always write; also disables the input-side\n"
                       "  fallback role).")
          ->check(CLI::Range(0, std::numeric_limits<int>::max()));
    scLoop->add_option("--fritz-min-change", cfg.fritz_min_change,
                       "FRITZ!Box-side dead band in watts (default: 3).\n"
                       "  Minimum |Δgrid_meter_power_w| since the previous cycle that counts as a real\n"
                       "  change.  Whenever this guard is enabled, grid_meter_power_w ALONE decides the\n"
                       "  input-side skip: the write is skipped when |Δgrid_meter_power_w| < --fritz-min-\n"
                       "  change, regardless of what solakon_grid_power_w did.  solakon_grid_power_w is NOT consulted while this\n"
                       "  guard is active (see --min-change) because solakon_grid_power_w only moves in\n"
                       "  response to our own previous write — its own \"unchanged\" state\n"
                       "  is not independent evidence, and letting it also veto a write\n"
                       "  would freeze the loop permanently after the first skipped cycle.\n"
                       "  This also prevents a compounding bug: if grid_meter_power_w is quiet (including\n"
                       "  \"stuck\" on a stale FRITZ!Box cache value not yet caught by\n"
                       "  --fritz-stuck-cycles) while solakon_grid_power_w has just moved to catch up with a\n"
                       "  PREVIOUS write that already contains that same stale grid_meter_power_w, writing\n"
                       "  again would re-add the same grid_meter_power_w on top of a solakon_grid_power_w that already\n"
                       "  absorbed it, creeping the setpoint upward every interval despite\n"
                       "  no real change in load.\n"
                       "  Rationale for the threshold itself: the FRITZ!Smart Energy 250\n"
                       "  polls its internal energy data on a ~2-minute interval and rounds\n"
                       "  its REST output to whole watts.  Successive reads at our cycle\n"
                       "  rate (8–60 s) therefore show ±3..±10 W noise even when the\n"
                       "  household load is stable.\n"
                       "  The default of 3 W is above typical mW-rounding noise but well\n"
                       "  below a single meaningful household appliance.\n"
                       "  Set to 0 to disable; --min-change's Δsolakon_grid_power_w guard then becomes the\n"
                       "  sole (fallback) input-side check.")
          ->check(CLI::Range(0, std::numeric_limits<int>::max()));
    scLoop->add_option("--fritz-stuck-cycles", cfg.fritz_stuck_cycles,
                       "Consecutive cycles the FRITZ!Box may return the EXACT same grid_meter_power_w value\n"
                       "before the tool gives up and fully suppresses writes (default: 3).\n"
                       "  A real-world household load fluctuates by at least ±1 W from second\n"
                       "  to second; the FRITZ device returning the same byte-equal value\n"
                       "  for many cycles in a row strongly indicates that its internal\n"
                       "  energy-data cache has not refreshed.  Acting on stale data as if it\n"
                       "  were fresh would drive the inverter to an incorrect setpoint.\n"
                       "  Rather than going silent the instant grid_meter_power_w repeats, cycles BEFORE this\n"
                       "  threshold still write, but with a small alternating +-1/+-2 W jitter\n"
                       "  added to the setpoint instead of the unchanged solakon_grid_power_w + grid_meter_power_w value -- keeps\n"
                       "  the loop visibly alive without pretending the stale reading justifies\n"
                       "  a real setpoint change.  Only once the counter REACHES this threshold\n"
                       "  does the tool log a warning and skip the write entirely until grid_meter_power_w\n"
                       "  changes; the counter resets on the first changed reading either way.\n"
                       "  Set to 0 to disable stuck-data detection (and its jitter) entirely.")
          ->check(CLI::Range(0, std::numeric_limits<int>::max()));
    scLoop->add_option("--fritz-baseline-w", cfg.fritz_baseline_w,
                       "Target value (W) the FRITZ!Box grid-power reading grid_meter_power_w should be held at,\n"
                       "instead of 0 (default: 0, i.e. current/original behaviour). The tool\n"
                       "computes and holds setpoint = solakon_grid_power_w + (grid_meter_power_w - baseline), so the household is\n"
                       "steered towards importing/exporting --fritz-baseline-w W at the\n"
                       "FRITZ!Box meter rather than towards net zero. A positive value keeps a\n"
                       "small standing import (e.g. to leave headroom for another circuit not\n"
                       "visible to the Solakon ONE); a negative value keeps a small standing\n"
                       "export. This baseline shift also applies to the release-eligibility\n"
                       "export-growing check and the post-release recover decision (both treat\n"
                       "the baseline as the new zero for grid_meter_power_w). --fritz-min-change and\n"
                       "--fritz-stuck-cycles still operate on the raw grid_meter_power_w reading, independent of\n"
                       "the baseline.");
    scLoop->add_option("--release-threshold", cfg.release_threshold,
                       "Combined-setpoint threshold in watts at or below which the solakon_grid_power_w + grid_meter_power_w\n"
                       "safety-net release condition is satisfied (default: 0, i.e. disabled).\n"
                       "  The primary release trigger is SoC-based (see\n"
                       "  --release-soc-hysteresis).  This solakon_grid_power_w + grid_meter_power_w threshold is an additional\n"
                       "  condition the user can require: release fires only if solakon_grid_power_w + grid_meter_power_w <= this\n"
                       "  AND the SoC condition AND all other release conditions are met.\n"
                       "  When 0 (the default) the solakon_grid_power_w + grid_meter_power_w condition is disabled and release\n"
                       "  decisions are made purely on SoC, battery-charging state, and\n"
                       "  inverter operational state.")
          ->check(CLI::Range(0, std::numeric_limits<int>::max()));
    scLoop->add_option("--release-soc-hysteresis", cfg.release_soc_hysteresis,
                       "Percentage points below the configured MAX_SOC at which release\n"
                       "becomes eligible (default: 3 %).\n"
                       "  The tool reads MAX_SOC (register 46610) at startup — this is the\n"
                       "  user-configured 'stop charging at X%' limit from the FoxESS app or\n"
                       "  web UI — and treats the battery as 'full' when SoC >= MAX_SOC minus\n"
                       "  this value.  Example: MAX_SOC = 95 %, hysteresis = 2 → release\n"
                       "  allowed when SoC >= 93 %.\n"
                       "  The hysteresis prevents flap when SoC briefly oscillates around the\n"
                       "  MAX_SOC limit due to BMS top-off behaviour and small loads pulling\n"
                       "  SoC down by a percent or two.\n"
                       "  Set to 0 to require SoC to reach the full MAX_SOC limit.")
          ->check(CLI::Range(0, 100));
    scLoop->add_option("--release-debounce-cycles", cfg.release_debounce_cycles,
                       "Consecutive cycles grid_meter_power_w > 0 (net grid import) must hold, after remote\n"
                       "control has been released, before it is regained (default: 2).\n"
                       "  NOT a pre-release debounce: release itself fires the instant all\n"
                       "  release-eligibility conditions hold on a single cycle (see\n"
                       "  --release-soc-hysteresis / --battery-dead-band / --release-threshold)\n"
                       "  — that release IS the behavioural test.  This option governs only\n"
                       "  the opposite direction: once released, remote control is regained\n"
                       "  when the FRITZ!Box reading grid_meter_power_w is positive (importing) for this many\n"
                       "  CONSECUTIVE cycles.  Any cycle where grid_meter_power_w <= 0 resets the counter to 0.\n"
                       "  Set to 0 or 1 to regain control on the first cycle grid_meter_power_w is positive\n"
                       "  (no debounce).")
          ->check(CLI::Range(0, std::numeric_limits<int>::max()));
    scLoop->add_option("--battery-dead-band", cfg.battery_dead_band,
                       "Threshold in watts above which the battery is considered actively\n"
                       "CHARGING; release is refused while charging (default: 20 W).\n"
                       "  A charging battery can still absorb whatever PV surplus the remote-\n"
                       "  control setpoint is currently clipping, so releasing gains nothing.\n"
                       "  Battery readings at or below this threshold — idling OR discharging —\n"
                       "  are treated as 'not charging' and do not block release; discharging\n"
                       "  is the expected state immediately after the battery reaches MAX_SOC.\n"
                       "  The threshold (rather than a strict battery_w <= 0 test) absorbs\n"
                       "  LiFePO4 trickle/float charge current (+5..+20 W of BMS cell-balancing\n"
                       "  current) that would otherwise read as 'charging' indefinitely.\n"
                       "  Set to 0 to require the strict battery_w <= 0 behaviour.")
          ->check(CLI::Range(0, std::numeric_limits<int>::max()));
    scLoop->add_option("--release-pv-margin", cfg.release_pv_margin,
                       "Minimum margin in watts by which TOTAL_PV_POWER must exceed the\n"
                       "current ACTIVE_POWER (solakon_grid_power_w) before a release is even attempted\n"
                       "(default: 10 W).\n"
                       "  While engaged, solakon_grid_power_w is (approximately) whatever we last commanded the\n"
                       "  inverter to export.  Releasing can only ever increase the export up\n"
                       "  to what PV can supply once the battery stops absorbing more, so\n"
                       "  unless PV is comfortably above what is already being exported, there\n"
                       "  is no PV surplus left for a release to unlock -- testing one has no\n"
                       "  possible upside, only the risk of a spurious release immediately\n"
                       "  undone by the recover decision (--release-debounce-cycles).\n"
                       "  A larger margin requires more headroom before a release is tested;\n"
                       "  a negative value relaxes (or, sufficiently negative, effectively\n"
                       "  disables) the guard.");
    scLoop->add_flag  ("--recover-remote-on-start,!--no-recover-remote-on-start",
                       cfg.recover_remote_on_start,
                       "On startup, read REMOTE_CONTROL (register 46001) and refuse to\n"
                       "release until the tool has engaged remote control itself.\n"
                       "Default: enabled.\n"
                       "  If REMOTE_CONTROL bit 0 is already set when the tool starts, the\n"
                       "  setpoint belongs to someone else — a previous instance that did\n"
                       "  not exit cleanly, or the FoxESS app's 'strategy periods' feature.\n"
                       "  Releasing it would silently cancel an active app schedule or\n"
                       "  discard work the user wants preserved.  Ownership-based release\n"
                       "  fires only after our own writeRemoteControl has succeeded.\n"
                       "  Disable only for diagnostic runs where the original 'release\n"
                       "  whatever is engaged' behaviour is desired (--no-recover-remote-on-start).");
    scLoop->add_option("--settle-delay-ms", cfg.settle_delay_ms,
                       "Milliseconds to sleep after a successful writeRemoteControl OR\n"
                       "releaseRemoteControl before the next cycle's reads (default: 2000).\n"
                       "  The FoxESS inverter ramps its output over 1–3 s after either\n"
                       "  transition; reading solakon_grid_power_w/grid_meter_power_w immediately afterwards gives a value partway\n"
                       "  through the ramp.  For writes this causes systematic overcorrection;\n"
                       "  for releases it would pollute the very first grid_meter_power_w reading the recover\n"
                       "  test (--release-debounce-cycles) relies on with a transient rather\n"
                       "  than the inverter's settled free-run state.\n"
                       "  Set to 0 to disable the settling delay.")
          ->check(CLI::Range(0, std::numeric_limits<int>::max()));
    scLoop->add_option("--min-control-soc", cfg.min_control_soc,
                       "Minimum battery SoC (%) required to hold or take remote control of\n"
                       "the Solakon ONE (default: 10).\n"
                       "  An independent, hard safety cutoff — separate from the release-when-\n"
                       "  full mechanism above and NOT tied to the inverter's own MIN_SOC\n"
                       "  register.  The instant BMS1_SOC drops below this value, remote\n"
                       "  control is released immediately, gated only by SoC itself and\n"
                       "  session ownership — none of --battery-dead-band, --release-pv-margin,\n"
                       "  or the export-growing/inverter-health checks apply here.\n"
                       "  'Minimum charge required to control' is literal: if SoC is already\n"
                       "  below this value at cold start, the tool will not engage remote\n"
                       "  control at all until SoC first reaches --min-control-soc-recover.\n"
                       "  Set to 0 to disable (SoC can never be negative, so the check never\n"
                       "  triggers).")
          ->check(CLI::Range(0, 100));
    scLoop->add_option("--min-control-soc-recover", cfg.min_control_soc_recover,
                       "Absolute battery SoC (%) that must be reached before remote control\n"
                       "resumes after a --min-control-soc release (default: 15).\n"
                       "  A second, independently configured absolute value — not an offset —\n"
                       "  so the effective hysteresis band is\n"
                       "  (min-control-soc-recover - min-control-soc).  Recovery is decided\n"
                       "  purely by SoC: no debounce cycles, no dependency on the FRITZ!Box\n"
                       "  reading grid_meter_power_w (unlike the release-when-full recover decision).\n"
                       "  Must be >= --min-control-soc; if configured lower, it is clamped up to\n"
                       "  --min-control-soc at startup (zero-width band) and a warning is\n"
                       "  logged.")
          ->check(CLI::Range(0, 100));
    scLoop->add_option("--state-file", cfg.state_file,
                       "Path to a small JSON file used to persist the --min-control-soc\n"
                       "low-SoC hold state across restarts (default:\n"
                       "/var/lib/solakon-one-fritz-powerregulator/state.json).\n"
                       "  Without this, a restart (crash, package upgrade, reboot) would\n"
                       "  forget that the battery was too low to hold remote control and\n"
                       "  resume forcing export/discharge from a normal, unconditional cold\n"
                       "  start.  The parent directory is created if missing.  The hold state\n"
                       "  can also be queried/toggled live via GET/POST /api/v1/low-soc-hold\n"
                       "  (see --api-enabled).  Set to \"\" to disable persistence entirely.");
    scLoop->add_flag  ("--enable-grid-import,!--no-enable-grid-import", cfg.enable_grid_import,
                       "When the computed solakon_grid_power_w+grid_meter_power_w setpoint would be negative (the inverter would\n"
                       "need to import from the grid to cover the load), actively command the\n"
                       "inverter to IMPORT the shortfall from the grid instead of clamping the\n"
                       "setpoint to 0 (default: disabled). The import magnitude is bounded by\n"
                       "--max-power, same as export setpoints; all anti-oscillation guards\n"
                       "(smoothing, --max-ramp-w-per-s, --min-change, etc.) apply identically.");

    scLoop->add_flag  ("--api-enabled,!--no-api-enabled", cfg.api_enabled,
                       "Enable the optional HTTP REST API for reading current parameters and\n"
                       "setting a manual grid import/export override (default: disabled).\n"
                       "  The API runs in-process alongside run-loop, sharing its Solakon ONE\n"
                       "  connection state. Requires --api-key to be set (non-empty); the tool\n"
                       "  refuses to start the API otherwise. See --api-host/--api-port/--api-key.");
    scLoop->add_option("--api-host", cfg.api_host,
                       "Address the REST API listener binds to (default: 127.0.0.1).\n"
                       "  Use 0.0.0.0 to listen on all interfaces (e.g. to reach the API from\n"
                       "  elsewhere on the LAN); the default restricts access to the local host.");
    scLoop->add_option("--api-port", cfg.api_port,
                       "TCP port for the REST API listener (default: 8080).")
          ->check(CLI::Range(1, 65535));
    scLoop->add_option("--api-key", cfg.api_key,
                       "Shared-secret API key required on every REST API request (except\n"
                       "/api/v1/health), presented by clients as either:\n"
                       "  Authorization: Bearer <api-key>\n"
                       "  X-API-Key: <api-key>\n"
                       "  Required (non-empty) whenever --api-enabled is set.");
    scLoop->add_flag  ("--api-tls-enabled,!--no-api-tls-enabled", cfg.api_tls_enabled,
                       "Serve the REST API over HTTPS instead of plain HTTP (default: disabled).\n"
                       "  Use --api-tls-cert-file/--api-tls-key-file to provide a real\n"
                       "  certificate/key pair; if both are left empty, an RSA-2048 self-signed\n"
                       "  certificate is generated in memory at startup (never written to disk,\n"
                       "  regenerated on every restart) -- convenient for local/LAN use, but\n"
                       "  clients must disable certificate verification or explicitly trust/pin\n"
                       "  it. Not suitable for public-facing deployments.");
    scLoop->add_option("--api-tls-cert-file", cfg.api_tls_cert_file,
                       "Path to a PEM certificate (chain) file for the REST API's TLS listener.\n"
                       "  Must be set together with --api-tls-key-file, or both left empty to\n"
                       "  auto-generate a self-signed certificate. Only used when\n"
                       "  --api-tls-enabled is set.")
          ->check(CLI::ExistingFile);
    scLoop->add_option("--api-tls-key-file", cfg.api_tls_key_file,
                       "Path to the PEM private key file matching --api-tls-cert-file.\n"
                       "  Must be set together with --api-tls-cert-file, or both left empty to\n"
                       "  auto-generate a self-signed certificate. Only used when\n"
                       "  --api-tls-enabled is set.")
          ->check(CLI::ExistingFile);

    // list-fritz-devices
    auto* scListFritz = app.add_subcommand("list-fritz-devices",
        "List all FRITZ!Box Smart Home devices with their AIN, name,\n"
        "current power draw (W, if the device has an energy meter), and online status.\n"
        "Use this to find the AIN to pass to --fritz-ain.\n"
        "Does not require --fritz-ain or any Solakon ONE options.\n"
        "\n"
        "Usage: solakon-one-fritz-powerregulator -f <host> -u <user> -p <pass> list-fritz-devices");
    (void)scListFritz;

    // read-fritz-device
    auto* scReadFritzDevice = app.add_subcommand("read-fritz-device",
        "Read and print the current power draw of the FRITZ!Box Smart Home device\n"
        "identified by --fritz-ain (required).\n"
        "\n"
        "Usage: solakon-one-fritz-powerregulator -f <host> -u <user> -p <pass> -a <ain> read-fritz-device");
    (void)scReadFritzDevice;

    // check-fritz-device
    auto* scCheckFritzDevice = app.add_subcommand("check-fritz-device",
        "Check whether the FRITZ!Box Smart Home device identified by --fritz-ain is\n"
        "suitable for use as the power-meter input in run-loop.\n"
        "\n"
        "A suitable device must:\n"
        "  - Have an energy meter (otherwise power readings are always 0).\n"
        "  - Be currently reachable (online).\n"
        "\n"
        "Exits with status 0 if the device is suitable, 1 otherwise.\n"
        "Suitable for use in scripts and as a systemd ExecStartPre= check.\n"
        "\n"
        "Usage: solakon-one-fritz-powerregulator -f <host> -u <user> -p <pass> -a <ain> check-fritz-device");
    (void)scCheckFritzDevice;

    // discover-mdns
    auto* scDiscoverMdns = app.add_subcommand("discover-mdns",
        "Discover mDNS services on the local network, including Solakon ONE inverters.\n"
        "This command queries for Modbus TCP services and displays their hostnames,\n"
        "IP addresses, and port numbers.\n"
        "\n"
        "Use the discovered hostnames (e.g., \"solakon-abc123.local\") with\n"
        "the --solakon-host option to connect to the inverter without specifying\n"
        "a static IP address.\n"
        "\n"
        "Does not require any other options.\n"
        "\n"
        "Usage: solakon-one-fritz-powerregulator discover-mdns");
    (void)scDiscoverMdns;

    // read-solakon
    auto* scReadSolakon = app.add_subcommand("read-solakon",
        "Read and print all Solakon ONE values used by run-loop:\n"
        "  active_power     — current grid active power in W (positive = exporting)\n"
        "  pv_power         — total PV generation power in W (register 39118)\n"
        "  battery_power    — battery combined power in W (register 39237);\n"
        "                     positive = charging, negative = discharging\n"
        "  battery_soc      — BMS1 state of charge in % (register 37612)\n"
        "  max_soc_limit    — configured stop-charging limit in % (register 46610)\n"
        "  min_soc_limit    — configured stop-discharging limit in % (register 46609)\n"
        "  work_mode        — active work mode (register 49203):\n"
        "                     1=SelfUse 2=FeedIn 3=Backup 4=PeakShaving\n"
        "                     6=ForceCharge 7=ForceDischarge\n"
        "  inverter_status  — status bitfield (register 39063): Standby/Operation/Fault\n"
        "  grid_status      — status bitfield (register 39065): On-Grid/Off-Grid (EPS)\n"
        "  remote_control   — remote-control bitfield (register 46001); bit 0 = engaged\n"
        "Does not require any FRITZ!Box options.\n"
        "\n"
        "Usage: solakon-one-fritz-powerregulator -H <host> read-solakon");
    (void)scReadSolakon;

    // write-solakon
    auto* scWriteSolakon = app.add_subcommand("write-solakon",
        "Write an explicit remote-control setpoint to the Solakon ONE.\n"
        "Both --watts and --timeout are required and must appear AFTER the subcommand name.\n"
        "Does not require any FRITZ!Box options.\n"
        "\n"
        "Usage: solakon-one-fritz-powerregulator -H <host> write-solakon --watts <W> --timeout <s>\n"
        "\n"
        "Use the global --dry-run flag to print the values that would be written without\n"
        "sending them (--dry-run must appear BEFORE the subcommand name).");
    scWriteSolakon->add_option("-w,--watts", cfg.write_watts,
                               "Export power limit in watts (>= 0, required)")
                 ->required()
                 ->check(CLI::Range(0, std::numeric_limits<int>::max()));
    scWriteSolakon->add_option("-t,--timeout", cfg.write_timeout,
                               "Revert timeout in seconds (1-65535, required):\n"
                               "inverter returns to normal operation if no new setpoint\n"
                               "is received within this time")
                 ->required()
                 ->check(CLI::Range(1, 65535));

    CLI11_PARSE(app, argc, argv);

    // Strip any empty strings from the product filter list.
    // This handles the case where a user passes --fritz-filter-product '' to
    // explicitly clear/disable the filter from the command line.
    cfg.fritz_filter_products.erase(
        std::remove_if(cfg.fritz_filter_products.begin(),
                       cfg.fritz_filter_products.end(),
                       [](const std::string& s) { return s.empty(); }),
        cfg.fritz_filter_products.end());

    // ---- Resolve log level ----
    // Priority: --log-level > -v count > config file / default.
    if (log_level_override >= 0)
        cfg.log_level = log_level_override;
    else if (verbose_count > 0)
        cfg.log_level = std::min(verbose_count + 1, 5);
    // else: leave cfg.log_level as loaded from the config file (or its default of 1).
    log.setLevel(cfg.log_level);

    // ---- Dispatch ----
    if (app.got_subcommand("run-loop"))
        return actionRunLoop(cfg);
    if (app.got_subcommand("list-fritz-devices"))
        return actionListFritzDevices(cfg);
    if (app.got_subcommand("read-fritz-device"))
        return actionReadFritzDevice(cfg);
    if (app.got_subcommand("check-fritz-device"))
        return actionCheckFritzDevice(cfg);
    if (app.got_subcommand("discover-mdns"))
        return actionDiscoverMdns(cfg);
    if (app.got_subcommand("read-solakon"))
        return actionReadSolakon(cfg);
    if (app.got_subcommand("write-solakon"))
        return actionWriteSolakon(cfg);

    // Should be unreachable (require_subcommand(1) enforces this).
    std::cerr << app.help() << '\n';
    return 1;
}
