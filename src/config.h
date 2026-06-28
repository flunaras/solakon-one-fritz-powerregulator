#pragma once

#include <string>
#include <vector>

// POD configuration struct populated by CLI11 from CLI args and/or INI config file.
// All fields have sensible defaults. CLI flags take precedence over the config file.
struct Config {
    // Solakon ONE (Modbus TCP)
    // solakon_host: IP address, DNS name, or mDNS name (e.g., "solakon-abc123.local").
    //   Use 'discover-mdns' subcommand to find available mDNS names on the network.
    std::string solakon_host     = "192.168.1.1";
    int         solakon_port     = 502;
    int         solakon_slave_id = 1;

    // FRITZ!Box Smart Home (REST API)
    std::string fritz_host       = "fritz.box";
    std::string fritz_scheme     = "http"; // "http" or "https"
    std::string fritz_username;
    std::string fritz_password;
    std::string fritz_ain;        // AIN of the device to read power from (spaces stripped)
    bool        fritz_ignore_ssl  = false;

    // FRITZ!Box device filters (applied by list-fritz-devices, check-fritz-device,
    // read-fritz-device, and run-loop suitability check).
    // Empty list = no filter (accept any product).
    //
    // fritz_filter_products defaults to {"FRITZ!Smart Energy 250"} because that is the
    // only FRITZ!Smart Energy device model that can distinguish between power consumption
    // and power export (bidirectional metering).  All other FRITZ!Smart Energy models only
    // report net consumption and will produce incorrect setpoints.
    // Each entry is a case-insensitive substring match against FritzDevice::productName;
    // a device passes if its productName contains ANY entry in the list (case-insensitive).
    // Pass an empty list to disable the product filter.
    std::vector<std::string> fritz_filter_products = {"FRITZ!Smart Energy 250"};
    std::string fritz_filter_type;    // substring match against FritzDevice::deviceType
                                      // (e.g. "energyMeter" matches any device whose
                                      //  capability list contains "energyMeter")

    // run-loop control
    int    interval           = 60;   // seconds between cycles; meaningful only for run-loop
    int    loop_timeout_extra = 30;   // seconds added to interval for the inverter revert timeout
    int    max_power          = 800;  // maximum export power setpoint in watts
    bool   dry_run            = false; // read values but do not write to Solakon ONE

    // Anti-oscillation / stability controls
    //
    // smoothing: EMA factor applied to the computed setpoint on each GENUINE
    //   update (i.e. skipped while apply_jitter holds — see fritz_stuck_cycles
    //   below — so a repeated/stale grid_meter_power_w reading never blends into the EMA):
    //     new_setpoint = alpha * raw + (1 - alpha) * prev_setpoint
    //   1.0 = no smoothing (current behaviour); lower values slow the response.
    //
    //   alpha is NOT simply cfg.smoothing itself — it is scaled by the ACTUAL
    //   elapsed wall-clock time (dt) since the previous genuine update,
    //   relative to `interval`:
    //     alpha = 1 - (1 - smoothing) ^ (dt / interval)
    //   smoothing is thus defined as "the fraction of the gap closed by a
    //   genuine update that arrives exactly `interval` seconds after the
    //   previous one" — the assumption a plain per-call EMA silently makes.
    //   The FRITZ!Box's own refresh cadence is not fixed (observed anywhere
    //   from ~10 s to ~110 s in the field, and varying over time on the same
    //   device), so genuine updates do not actually arrive once per interval;
    //   they arrive whenever grid_meter_power_w happens to change.  Scaling by dt makes
    //   smoothing mean the same thing in real time regardless of how sparse
    //   or dense genuine updates happen to be, with no need to measure or
    //   estimate "the current refresh rate" at all — equivalent to a
    //   continuous-time exponential (RC) filter with time constant
    //   tau = -interval / ln(1 - smoothing), just parameterised so existing
    //   --smoothing/--interval values keep their current meaning in the
    //   common case (dt == interval -> alpha == smoothing exactly) and
    //   self-correct automatically otherwise: a rare, hard-won sample after a
    //   long refresh gap is trusted close to fully (alpha -> 1) instead of
    //   only partially applied and left to wait a further full refresh
    //   interval for the rest; a suspiciously quick update is weighted less
    //   (alpha shrinks) as a guard against noise/transients.
    //   Recommended starting point: 0.5.  See max_ramp_w_per_s below for a
    //   separate, complementary bound on how fast the WRITTEN setpoint itself
    //   may move — smoothing governs how much a fresh reading is trusted, not
    //   how large a single write's step may be.
    double smoothing          = 1.0;

    // max_ramp_w_per_s: maximum rate of change, in watts per second of REAL
    //   elapsed time, allowed for the WRITTEN setpoint — independent of, and
    //   applied after, the smoothing/jitter computation above.  Bounds how
    //   large a single write's step may be regardless of how far the
    //   underlying solakon_grid_power_w + grid_meter_power_w target has moved, protecting the inverter/battery
    //   from an abrupt commanded swing.  This guards against a different
    //   failure mode than smoothing: smoothing's time-based alpha (above)
    //   deliberately trusts a fresh sample close to fully once dt is large,
    //   which is correct for TRACKING the true target quickly — but says
    //   nothing about how fast the WRITTEN value may get there, so without
    //   this a big step is still applied in a single write.
    //   The elapsed time used is measured since the most recent write of ANY
    //   kind, including keep-alive refreshes that re-send an unchanged
    //   setpoint — so a long quiet spell (even one filled with keep-alive
    //   pings) never "banks" an oversized jump allowance; the limit stays a
    //   tight, predictable bound on every single write.
    //   Applied to every write while continuously engaged (both ordinary and
    //   jitter writes).  The very first write of a fresh engagement (cold
    //   start or post-release regain) is exempt: there is no previous
    //   commanded value yet to ramp from.
    //   Default: 0 = disabled (no rate limit, matching prior behaviour).
    int    max_ramp_w_per_s  = 0;

    // min_change: dead-band threshold in watts.
    //   OUTPUT side: the write is skipped if |new_setpoint - last_written| <
    //   min_change.  Suppresses writes caused by measurement noise or small
    //   transients.
    //   INPUT side: also used as the Δsolakon_grid_power_w dead band for the input-side guard
    //   (see fritz_min_change below), but ONLY as a fallback when
    //   fritz_min_change is 0 (disabled) — whenever the grid_meter_power_w guard is active it
    //   alone decides, and this Δsolakon_grid_power_w guard is not consulted.  solakon_grid_power_w's own
    //   cycle-to-cycle delta cannot be used to independently trigger a skip
    //   because solakon_grid_power_w only moves in response to our own previous write: once one
    //   cycle is skipped, solakon_grid_power_w stops moving and would otherwise read as
    //   "unchanged" forever, freezing the loop permanently.
    //   0 = disabled (always write; and never used as the input-side fallback).
    int    min_change         = 1;


    // fritz_min_change: minimum |Δgrid_meter_power_w| (change in the FRITZ!Box reading since the
    //   previous cycle) considered a real change.  Whenever this guard is
    //   enabled, grid_meter_power_w ALONE decides whether the input-side check skips the write
    //   — i.e. the write is skipped when |Δgrid_meter_power_w| < fritz_min_change, regardless
    //   of whether solakon_grid_power_w has moved.  solakon_grid_power_w is deliberately NOT consulted while this
    //   guard is active (see min_change above for why: solakon_grid_power_w's own quiet state is
    //   not independent evidence, and would deadlock the loop if allowed to
    //   veto on its own).
    //
    //   This grid_meter_power_w-only rule also prevents a compounding bug: if grid_meter_power_w is quiet
    //   (including "stuck" on a stale FRITZ!Box cache value not yet caught by
    //   fritz_stuck_cycles) while solakon_grid_power_w has just moved to catch up with a
    //   PREVIOUS write that already contains that same stale/noisy grid_meter_power_w, writing
    //   again would re-add the same grid_meter_power_w on top of an solakon_grid_power_w that already absorbed
    //   it — a setpoint (and battery charge/discharge power) that creeps
    //   upward every interval despite no real change in load.
    //
    //   Rationale for the threshold itself: the FRITZ!Smart Energy 250 polls
    //   its internal energy data on a ~2-minute interval and rounds its REST
    //   output from mW to W.  Successive reads at our cycle rate (8–60 s)
    //   therefore exhibit ±3..±10 W noise even when the household load is
    //   stable.  Without this guard each spike would push the solakon_grid_power_w+grid_meter_power_w setpoint,
    //   the inverter would ramp to match, and on the next cycle the new
    //   (higher) solakon_grid_power_w plus the noise-reverted grid_meter_power_w would drive yet another setpoint
    //   change — producing a slow upward drift of the setpoint even though
    //   the actual load is constant.
    //
    //   Default: 3 W — above typical mW-rounding noise but well below a single
    //   meaningful household appliance (a 5 W LED still triggers the loop).
    //   Set to 0 to disable this guard; min_change's Δsolakon_grid_power_w guard then becomes the
    //   sole (fallback) input-side check.  Disabling both means the
    //   input-side check never skips a write.
    int    fritz_min_change   = 3;

    // fritz_stuck_cycles: number of consecutive cycles the FRITZ!Box may return
    //   the EXACT same grid_meter_power_w value before the tool gives up and fully suppresses
    //   writes.  A real-world load fluctuates at least by ±1 W from second to
    //   second; the FRITZ device returning the same exact byte-equal value for
    //   many cycles in a row almost always indicates that its internal
    //   energy-data cache has not been refreshed.  Acting on stale data as if it
    //   were fresh would drive the inverter to an incorrect setpoint.
    //
    //   Rather than going silent the instant grid_meter_power_w repeats, cycles BEFORE this
    //   threshold still write — but with a small alternating +-1/+-2 W jitter
    //   added to the setpoint instead of the unchanged solakon_grid_power_w + grid_meter_power_w value, so the loop
    //   stays visibly alive and the inverter gets a small nudge without the tool
    //   pretending the stale reading justifies a real setpoint change.  Only once
    //   the counter REACHES this threshold does the tool log a warning and skip
    //   the write entirely until grid_meter_power_w changes to a different value.  The counter
    //   resets the moment grid_meter_power_w does change (regardless of direction or magnitude),
    //   so a single fresh reading immediately re-enables normal writes.
    //
    //   Default: 3 cycles (matching the release_debounce_cycles debounce style).
    //   Set to 0 to disable stuck-data detection (and its jitter) entirely.
    int    fritz_stuck_cycles = 3;

    // fritz_baseline_w: the target value grid_meter_power_w (the FRITZ!Box grid-power reading) should
    //   be held at, in watts, instead of 0.  All control-decision uses of grid_meter_power_w are shifted
    //   by this baseline before being consulted — i.e. the tool actually computes and
    //   holds:
    //     setpoint = solakon_grid_power_w + (grid_meter_power_w - fritz_baseline_w)
    //   so the household is steered towards importing/exporting fritz_baseline_w W at
    //   the FRITZ!Box meter rather than towards net zero.  A positive value keeps a
    //   small standing import (useful if, say, another circuit not visible to the
    //   Solakon ONE also needs to draw from the grid); a negative value keeps a small
    //   standing export.
    //   This baseline shift applies uniformly to every other grid_meter_power_w-based decision as well,
    //   so the whole control loop consistently treats "at baseline" as the new zero:
    //     - the release-eligibility "export growing" trend check (grid_meter_power_w < baseline instead
    //       of grid_meter_power_w < 0)
    //     - the post-release recover decision (regain control when grid_meter_power_w > baseline,
    //       instead of grid_meter_power_w > 0)
    //   The raw (unshifted) FRITZ!Box reading is still what fritz_min_change and
    //   fritz_stuck_cycles operate on above — those guard against noise/staleness in
    //   the raw measurement itself and are independent of where the target baseline is.
    //   Default: 0 (matches the original "hold grid_meter_power_w at zero" behavior).
    int    fritz_baseline_w  = 0;

    // release_threshold: combined setpoint (solakon_grid_power_w + grid_meter_power_w) below which remote control is released.
    //   Acts as an OPTIONAL additional release condition on top of the primary,
    //   SoC-based trigger (see release_soc_hysteresis below) — release becomes
    //   eligible whenever the battery is "full enough" relative to the
    //   user-configured MAX_SOC limit, the battery is not charging, and the
    //   inverter is healthy.  This solakon_grid_power_w + grid_meter_power_w threshold remains as a safety net for
    //   users who want an extra guarantee that load is genuinely near zero.
    //   Remote control is tested for release (see release_debounce_cycles below
    //   for what "tested" means) when ALL of the following hold on the SAME cycle:
    //     - we own the remote-control session (owned_by_us)
    //     - SoC has reached MAX_SOC - release_soc_hysteresis  (primary trigger)
    //     - the battery is not charging (battery_w <= battery_dead_band)
    //     - the inverter is in normal Operation state (not standby, fault, off-grid)
    //     - the combined setpoint solakon_grid_power_w + grid_meter_power_w is at or below release_threshold
    //       (only checked when release_threshold > 0)
    //   With release_threshold = 0 the solakon_grid_power_w + grid_meter_power_w condition is disabled and release
    //   eligibility depends only on SoC, battery-charging state, and inverter
    //   health.  Default: 0 (disabled).
    int    release_threshold    = 0;

    // release_soc_hysteresis: percentage points below MAX_SOC at which release becomes
    //   eligible (in %).  The tool reads MAX_SOC (register 46610) once on startup —
    //   this is the user-configured "stop charging at X%" limit from the FoxESS app
    //   or web UI — and treats the battery as "full" when SoC ≥ (MAX_SOC - hysteresis).
    //   Example: MAX_SOC = 95 %, hysteresis = 2 → release allowed when SoC ≥ 93 %.
    //   The hysteresis prevents flapping when SoC oscillates around MAX_SOC due to
    //   BMS top-off behaviour and brief loads pulling SoC down by a percent or two.
    //   Default: 2 %.  Set to 0 to require SoC to reach the full MAX_SOC limit.
    int    release_soc_hysteresis = 2;

    // release_debounce_cycles: NOT a pre-release debounce — the release-eligibility
    //   conditions above are tested on a single cycle, and the moment they all hold,
    //   remote control is released immediately as a real-world BEHAVIOURAL TEST (see
    //   the "Alternative release/recover detection" note in main.cpp for the full
    //   rationale).  This field instead governs the OPPOSITE direction: once remote
    //   control has been released, the tool watches the FRITZ!Box reading grid_meter_power_w every
    //   cycle.  If grid_meter_power_w > 0 (the household is net-importing from the grid — the release
    //   was premature, or conditions changed) for this many CONSECUTIVE cycles,
    //   remote control is regained (the tool resumes writing solakon_grid_power_w + grid_meter_power_w setpoints).  Any
    //   cycle where grid_meter_power_w <= 0 (still exporting / not importing — the release is working)
    //   resets the counter to 0, so remote control stays released.
    //   This replaces the old proxy-condition-based re-engage debounce (which used
    //   to require work mode, PV production, etc.) with a direct behavioural signal:
    //   a wrong release decision shows up immediately as grid import, so there is no
    //   need to re-derive "should we still be released" from indirect proxies.
    //   Default: 2 cycles.  Set to 0 or 1 to regain control on the first cycle grid_meter_power_w is
    //   positive (no debounce).
    int    release_debounce_cycles = 2;

    // battery_dead_band: threshold in watts above which the battery is considered
    //   to be actively CHARGING (absorbing power) — release is refused while charging,
    //   because a charging battery can still absorb whatever PV surplus the remote-
    //   control setpoint is currently clipping, so there is nothing to gain (and
    //   potential SoC-limit overshoot to lose) by releasing yet.  Battery readings at
    //   or below this threshold — idling OR discharging — are treated as "not
    //   charging" and do not block release; discharging is in fact the expected
    //   state immediately after the battery reaches MAX_SOC (see BATTERY_COMBINED_POWER
    //   sign convention below).
    //   The threshold (rather than a strict battery_w <= 0 test) absorbs LiFePO4
    //   trickle/float charge current (typically +5..+20 W of BMS cell-balancing
    //   current) that would otherwise indefinitely read as "charging" and lock
    //   release out once SoC has already hit MAX_SOC.
    //   Default: 20 W.  Set to 0 to require the strict battery_w <= 0 behaviour.
    int    battery_dead_band      = 20;

    // release_pv_margin: minimum margin in watts by which TOTAL_PV_POWER must exceed
    //   the current ACTIVE_POWER (solakon_grid_power_w) before a release is even attempted.  Gates
    //   whether a release test can start at all — independent of, and evaluated
    //   alongside, the other release-eligibility conditions above.
    //   While engaged, solakon_grid_power_w is (approximately) whatever we last commanded the inverter
    //   to export.  If PV is not comfortably above that — i.e. PV production is at
    //   or barely over what we are already forcing to the grid — then the battery
    //   is not absorbing (see battery_dead_band above) for the simple reason that
    //   there is no PV surplus left to absorb: releasing cannot possibly make the
    //   inverter export MORE than it already does under our setpoint, because PV is
    //   the ultimate ceiling once the battery stops accepting charge.  Testing a
    //   release in that situation has no potential upside and only risks a spurious
    //   release/regain cycle (a brief release immediately corrected by the recover
    //   decision once grid_meter_power_w goes positive again).
    //   Bypassed entirely once SoC has actually reached MAX_SOC itself (not merely the
    //   release_soc_hysteresis-relaxed threshold cond_soc uses) — see cond_pv_headroom
    //   in main.cpp for the full rationale: the "PV is the ultimate ceiling" assumption
    //   above only holds while the battery still has room to climb, because some
    //   inverters/firmware curtail PV harvest down to match whatever ACTIVE_POWER is
    //   currently being commanded once the battery is genuinely full and has no spare
    //   capacity left to buffer the difference. Left unbypassed, that curtailment would
    //   permanently suppress the very PV reading this margin depends on, deadlocking
    //   release forever regardless of how much sunlight is actually available.
    //   Default: 10 W — above typical Modbus register noise but well below a
    //   meaningful PV surplus.  A larger margin requires more headroom before
    //   bothering to test; a negative value relaxes (or, sufficiently negative,
    //   effectively disables) the guard.
    int    release_pv_margin     = 10;

    // recover_remote_on_start: on startup (before the first cycle), read the
    //   REMOTE_CONTROL register (46001) to detect whether the inverter is already
    //   under remote control.  If it is, but we did not engage it ourselves
    //   (e.g. a previous instance crashed or the FoxESS app's "strategy periods"
    //   feature is currently active), the tool will NOT release remote control —
    //   it would otherwise silently cancel the strategy period or hand control to
    //   a non-Self-Use mode.  Instead it waits until it has engaged remote control
    //   itself (its own writeRemoteControl succeeded) before any release becomes
    //   eligible — ownership-based release.
    //   Default: true.  Disable only for diagnostic runs where the original
    //   "release whatever is engaged" behaviour is desired.
    bool   recover_remote_on_start = true;

    // settle_delay_ms: milliseconds to sleep after a successful writeRemoteControl
    //   OR releaseRemoteControl before the next cycle's reads.  The FoxESS inverter
    //   ramps its output over 1–3 s after either transition; reading solakon_grid_power_w/grid_meter_power_w immediately
    //   afterwards gives a value partway through the ramp, which feeds back into the
    //   next cycle as a stale measurement.  For writeRemoteControl this causes
    //   systematic overcorrection; for releaseRemoteControl it would pollute the very
    //   first grid_meter_power_w reading the release/recover behavioural test relies on (see
    //   release_debounce_cycles above) with a transient, not the inverter's settled
    //   free-run state.  A short settling delay lets the inverter reach the new state
    //   before we measure again.  The delay is applied only when a write or release
    //   actually occurred and only for the remainder of the current cycle — it does
    //   not extend the configured interval.  Default: 2000 ms.  Set to 0 to disable.
    int    settle_delay_ms        = 2000;

    // min_control_soc: minimum BMS1_SOC (%) required for the tool to hold or take remote
    //   control of the Solakon ONE — an independent, directly configured safety floor,
    //   separate from the release-when-full mechanism above and NOT tied to the inverter's
    //   own MIN_SOC register (46609, the user's "stop discharging" limit).  It exists
    //   because remote control appears to bypass that built-in floor the same way it
    //   appears to bypass MAX_SOC — see release_soc_hysteresis above.
    //
    //   Whenever the tool is engaged and BMS1_SOC drops below this value, remote control
    //   is released IMMEDIATELY, as a hard, independent safety cutoff.  Unlike the
    //   release-when-full conditions above, this check depends on nothing but SoC itself
    //   and session ownership (owned_by_us) — it is deliberately NOT gated by
    //   battery_dead_band, release_pv_margin, the export-growing trend, or inverter health,
    //   none of which answer the question that matters here ("should we stop forcing more
    //   export/discharge right now?").
    //
    //   "Minimum charge required to control" is read literally: if BMS1_SOC is already
    //   below this value when the tool starts, it will not engage remote control at all on
    //   the first cycle either — unlike the normal cold-start behaviour (which takes control
    //   unconditionally), a fresh process must first observe SoC at or above the minimum
    //   before ever writing a setpoint.
    //
    //   Default: 10.  Set to 0 to disable this cutoff.
    int    min_control_soc         = 10;

    // min_control_soc_recover: absolute BMS1_SOC (%) that the battery must rise back UP TO
    //   before the tool resumes control after a min_control_soc release.  This is a second,
    //   independently configured absolute value — not an offset — so the effective
    //   hysteresis band is simply (min_control_soc_recover - min_control_soc).  Recovery is
    //   evaluated purely on SoC: no debounce cycles, no dependency on the FRITZ!Box reading
    //   grid_meter_power_w (unlike the release-when-full mechanism's grid_meter_power_w-driven recover decision).  The instant
    //   BMS1_SOC reaches this value, control resumes.
    //
    //   Must be >= min_control_soc to form a valid (non-inverted) hysteresis band.  If
    //   configured below min_control_soc, it is clamped up to min_control_soc at startup
    //   (a zero-width band: control resumes the instant SoC is back at min_control_soc)
    //   and a warning is logged.
    //
    //   Default: 15.
    int    min_control_soc_recover = 15;

    // state_file: path to a small JSON file used to persist control-loop state
    // that must survive a service restart -- currently just the low-SoC hold
    // flag (see min_control_soc/min_control_soc_recover above). Without this,
    // a restart (crash, package upgrade, reboot) would forget that the battery
    // was below min_control_soc moments earlier and resume forcing export/
    // discharge on a still-too-low battery from a normal, unconditional cold
    // start. The parent directory is created (recursively, best-effort) if it
    // does not already exist. The file is rewritten only when the low-SoC hold
    // state actually changes (including via the REST API -- see
    // GET/POST /api/v1/low-soc-hold in restapi.h), not on every cycle.
    // Set to "" (empty) to disable persistence entirely -- the hold state then
    // always starts as "not held" on every process start, matching the
    // tool's original behaviour.
    // Default: "/var/lib/solakon-one-fritz-powerregulator/state.json".
    std::string state_file = "/var/lib/solakon-one-fritz-powerregulator/state.json";

    // enable_grid_import: when the computed solakon_grid_power_w + grid_meter_power_w setpoint is negative (the
    //   inverter would need to import from the grid to cover the load), the
    //   default behaviour clamps the setpoint to 0 and lets the inverter fall
    //   back to whatever it can supply from PV/battery, potentially leaving a
    //   shortfall uncovered by remote control at all.  Enabling this option
    //   instead actively commands the inverter to IMPORT the shortfall from
    //   the grid via SolakonApi::writeRemoteControlImport() (REMOTE_CONTROL
    //   direction bit = consume/absorb), so the same remote-control mechanism
    //   that pushes export up when there's a surplus can also pull import up
    //   when there's a deficit.
    //   The magnitude of an import command is bounded by max_power, the same
    //   limit used for export setpoints.
    //   All of the existing anti-oscillation guards (smoothing, max_ramp_w_per_s,
    //   min_change, fritz_min_change/fritz_stuck_cycles) apply identically to
    //   import commands — they operate on the signed setpoint value, which
    //   simply ranges down to -max_power instead of being floored at 0 when
    //   this option is enabled.
    //   Default: false (disabled — matches the original clamp-to-0 behaviour).
    bool   enable_grid_import = false;

    // write-solakon action: watts to write directly (not computed)
    int write_watts   = -1;
    int write_timeout = -1;  // seconds; -1 = not set (required for write-solakon)

    // ── REST API ──────────────────────────────────────────────────────────
    //
    // Optional HTTP REST API, active only while run-loop is running (it shares the
    // same process and the same SolakonApi connection as the control loop; it is
    // NOT a separate daemon).  Disabled by default so nothing new is exposed on the
    // network unless explicitly enabled.
    //
    // api_enabled: master switch.  Default: false.
    bool        api_enabled  = false;

    // api_host: address the HTTP listener binds to.  "0.0.0.0" listens on all
    // interfaces; use "127.0.0.1" to restrict the API to local access only (e.g.
    // when only intended to be reached through an SSH tunnel or a reverse proxy
    // running on the same host).  Default: "127.0.0.1" (safest default — an
    // operator must deliberately widen this to expose the API on the LAN).
    std::string api_host     = "127.0.0.1";

    // api_port: TCP port for the HTTP listener.  Default: 8080.
    int         api_port     = 8080;

    // api_key: shared-secret API key required on every request (see below for how
    // it is presented).  Empty by default; if api_enabled is true and api_key is
    // empty, the tool refuses to start the API and logs an error rather than
    // silently exposing every endpoint with no authentication at all.
    //
    // Clients must present the key on every request using EITHER:
    //   - Header:  Authorization: Bearer <api_key>
    //   - Header:  X-API-Key: <api_key>
    // A request presenting neither, or presenting a key that does not match,
    // receives 401 Unauthorized. Comparison is a plain string equality check
    // (not constant-time) — treat the key as a shared secret on a trusted network,
    // not as a defense against a sophisticated timing-attack adversary.
    std::string api_key;

    // api_tls_enabled: serve the REST API over HTTPS instead of plain HTTP.
    // Default: false.
    bool        api_tls_enabled   = false;

    // api_tls_cert_file / api_tls_key_file: paths to a PEM certificate (chain)
    // and matching private key to use for the TLS listener.  Both must be set
    // together, or both left empty.
    //
    // When both are empty (the default) and api_tls_enabled is true, the tool
    // generates a self-signed RSA-2048 certificate in memory at startup (never
    // written to disk) and uses it for the lifetime of the process -- a new
    // one is generated on every restart.  This is convenient for local/LAN use
    // without provisioning real certificates, but API clients must either
    // disable certificate verification or pin/trust the generated certificate
    // explicitly; it is not suitable for a public-facing deployment.
    //
    // Set both to use a real certificate instead, e.g. one issued by an
    // internal CA or a public CA (via a reverse proxy's ACME client, or
    // directly if the tool is reachable from the internet).
    std::string api_tls_cert_file;
    std::string api_tls_key_file;

    // Logging
    int log_level = 1;     // 0=silent 1=err 2=warn 3=info 4=dbg 5=trace
};
