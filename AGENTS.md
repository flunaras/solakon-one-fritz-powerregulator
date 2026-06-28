# AGENTS.md — solakon-one-fritz-powerregulator

## Project Overview

`solakon-one-fritz-powerregulator` is a **command-line C++17 tool** that automatically adjusts the
exported power of a **FoxESS Solakon ONE** battery inverter based on the net grid power measured
by a **FRITZ!Box Smart Home** electricity meter (e.g. a FRITZ!Smart Energy 250 installed at the
grid connection point).

### Control logic

1. Read `A` — the current grid active power exported by the Solakon ONE (via Modbus TCP).
2. Read `B` — the current power drawn by a configured FRITZ!Box Smart Home device (via FRITZ!Box
   REST API).
3. Compute `A + B`. If the result is negative (the inverter would need to *consume* from the grid),
   clamp it to 0. Write the result back to the Solakon ONE as the new export power setpoint.
4. **Remote-control release** — a two-part decision, using direct behavioural testing
   instead of proxy conditions wherever possible.  On startup the tool reads `MAX_SOC`
   (register 46610) — the user-configured "stop charging at X%" limit from the FoxESS app
   / web UI — and treats the battery as "full" when `SoC ≥ MAX_SOC − release_soc_hysteresis`.

   **Release eligibility** (ENGAGED → RELEASED) is checked fresh every cycle; the instant
   **all** of the following hold on the *same* cycle, remote control is released
   **immediately** — this is a real-world behavioural test, not gated by a debounce:

   - **Ownership** — we own the remote-control session (our own `writeRemoteControl` succeeded
     since startup).  On cold start, if `REMOTE_CONTROL` (46001) bit 0 is already set, the
     session belongs to a previous run or to the FoxESS app's "strategy periods" feature;
     release is inhibited until our own engagement cycle takes ownership.
   - **SoC** — `SoC ≥ MAX_SOC − release_soc_hysteresis` (registers 37612, 46610).
   - **Battery not charging** — `BATTERY_COMBINED_POWER ≤ battery_dead_band` (default 20 W).
     A charging battery can still absorb the PV surplus the setpoint is clipping, so
     releasing gains nothing.  Discharging (negative) does NOT block release — it is the
     expected state right after the battery reaches `MAX_SOC`.  The threshold absorbs
     LiFePO4 cell-balancing trickle reads of `+5..+20 W` that would otherwise read as
     "charging" indefinitely.
   - **PV headroom over current export** — `TOTAL_PV_POWER > ACTIVE_POWER (A) + release_pv_margin`
     (default margin 50 W), **bypassed (forced true) once `SoC ≥ MAX_SOC` itself** (the
     user's configured ceiling, not merely the `release_soc_hysteresis`-relaxed threshold
     `cond_soc` above uses).  While engaged, `A` is approximately whatever we last commanded
     the inverter to export.  Releasing can only ever increase the export up to what PV can
     supply once the battery stops absorbing more, so unless PV is comfortably above what is
     already being exported there is no PV surplus for a release to unlock — testing one
     would have no possible upside, only the risk of a spurious release the recover decision
     immediately has to undo.  That reasoning silently assumes `TOTAL_PV_POWER` is an
     independent measurement of available sunlight, which only holds while the battery still
     has room to climb toward `MAX_SOC`: some inverters/firmware have been observed to
     curtail PV harvest down to match whatever `ACTIVE_POWER` is currently being commanded
     once the battery is genuinely full (no spare capacity left to buffer the difference) —
     at that point PV can no longer exceed `A` regardless of how much sunlight is actually
     available, permanently vetoing a release that should happen (our own throttling
     poisons the very signal this condition depends on).  Once `SoC` has actually reached
     `MAX_SOC` there is nothing left to lose by testing a release anyway: the recover
     debounce below already safely undoes one that turns out to have no real surplus behind
     it, exactly as it does for every other release-eligibility condition.
   - **Surplus / growing-export trend** — `B < 0` OR `A >` (`A` when the current engagement
     started).  A second, complementary signal on top of the PV-headroom snapshot above.
     `B < 0` means the household is, right now, net-exporting even under our own capped
     setpoint — direct proof of supply beyond what the setpoint alone accounts for.  `A`
     higher than it was when this engagement began means the control loop itself has, net,
     been pushing the setpoint upward — a trend suggesting the situation keeps improving.
     Compared against a FIXED starting point (captured once, when we last transitioned into
     the engaged state) rather than the immediately preceding cycle, because `A` is not
     guaranteed to rise on every single cycle even while the underlying trend is upward — a
     rolling comparison could flap false on an ordinary one-cycle dip.  Without a captured
     baseline yet (true cold start, or the very first cycle of a fresh engagement) this
     condition falls back to the `B < 0` half alone.
   - **Inverter operational state** — `STATUS_1` (39063) shows Operation set, Fault clear;
     `STATUS_3` (39065) shows Off-Grid/EPS clear.
   - **A + B safety net** — `A + B ≤ release_threshold` (default 0 = disabled).  Optional
     additional condition.

   If any required Modbus read fails, the corresponding condition is treated as "not safe"
   and release is inhibited — safe-fail.

   Two earlier conditions — work mode must be Self Use, and PV power must exceed a
   threshold — have been **removed**: both proved unreliable in the field (e.g. some
   inverters/firmware leave `WORK_MODE` stuck at a non-Self-Use value indefinitely,
   permanently blocking release even with a genuinely full, idle battery).  The PV
   headroom condition above is a different, *relative* check (PV vs. our own current
   export) added afterwards — not a reintroduction of that flat PV threshold.

   **Recover decision** (RELEASED → ENGAGED) is purely behavioural: once released, the tool
   watches only `B`, the FRITZ!Box grid reading.  `B ≤ 0` (not importing) → remain released,
   reset the regain counter to 0.  `B > 0` (importing) for `release_debounce_cycles`
   consecutive cycles → regain control (resume `A + B` setpoint writes).  SoC dropping back
   down or the battery resuming charging do **not**, by themselves, force a regain — those
   conditions already had their say during release eligibility.  The very first engagement
   after process start bypasses this test entirely (nothing has been released yet to
   recover from) and takes control on the first cycle.

   **Why SoC instead of `A + B ≤ threshold`.** The previous proxy rule (`A + B ≤ 10 W`)
   essentially never fired in normal daytime operation: with any household load, the inverter
   tracks the load exactly so `A ≈ load`, `B ≈ 0`, and `A + B ≈ load` — typically 100–500 W,
   far above 10 W.  Reading SoC directly answers the question we actually care about: "is the
   battery full?"  The `A + B` threshold remains as an optional belt-and-braces condition for
   users who want it.

   **Why ownership tracking.** The FoxESS app's "strategy periods" feature uses the same
   `REMOTE_CONTROL` register internally.  Releasing a setpoint we didn't write would silently
   cancel an active app schedule.  The tool only releases what it engaged itself.

   **Why immediate, testable release instead of a pre-release debounce.** Debouncing the
   release side gained nothing once the recover side is behavioural: a wrong release (from a
   one-cycle SoC or battery-power blip) is corrected within `release_debounce_cycles` cycles
   regardless, so there is no need to also delay the release itself.  This removed the old
   `engage_cycles` option entirely — `release_debounce_cycles` now covers both what used to be
   two separate debounce knobs, because there is only one debounce left in the state machine
   (the recover side).

   **Why settling delay.** After every `writeRemoteControl` OR `releaseRemoteControl` the tool
   sleeps `settle_delay_ms` (default 2000 ms) before the next cycle's reads so the inverter has
   time to ramp/settle before A/B are measured again.  Without this, a reading taken partway
   through the transition feeds back as stale data — for writes this causes systematic
   overcorrection; for releases it would corrupt the very first B reading the recover decision
   depends on.

   **Clean shutdown.** On SIGINT/SIGTERM the tool issues a final `releaseRemoteControl()` if
   it owns the session, so the inverter is not left under a stale cap waiting for its revert
   timeout to expire after the process is gone.
5. **Minimum-SoC control cutoff** — a second, entirely independent release/recover mechanism,
   symmetric in purpose to point 4 above but simpler in design: point 4 releases control when
   the battery is *full* (to stop wasting PV surplus); this one releases control when the
   battery is *low*, to stop the `A + B` setpoint from forcing further battery discharge once
   there is little charge left to give. `min_control_soc` (default 10 %, i.e. enabled out of
   the box as a conservative safety floor — set to 0 to disable) is a directly configured
   minimum SoC required for the tool to hold or take remote control at all — it is **not**
   tied to the inverter's own `MIN_SOC` register (46609, the user's "stop discharging" limit).

   - **Release** is a hard cutoff, not a bundle of conditions: the instant `BMS1_SOC` drops
     below `min_control_soc`, remote control is released immediately, gated only by SoC
     itself and session ownership (`owned_by_us`) — none of `battery_dead_band`,
     `release_pv_margin`, the export-growing trend, or inverter health from point 4 apply
     here; those all answer "is there PV surplus a release could unlock", which is not the
     question this cutoff is answering.
   - **Recovery** is purely SoC-driven and immediate: no debounce cycles, no dependency on
     `B` (unlike point 4's `B`-driven recover decision).  The instant `BMS1_SOC` rises back up
     to `min_control_soc_recover` (default 15 %; a second, independently configured
     **absolute** SoC value — not an offset from `min_control_soc`), control resumes.
   - **Cold start** is read literally: "minimum charge required to control" applies from the
     very first cycle.  If SoC already reads below `min_control_soc` when the process starts,
     the tool withholds control from cycle one instead of engaging unconditionally the way a
     normal cold start does.
   - `min_control_soc_recover` (default 15 %) must be `>= min_control_soc` to form a
     non-inverted hysteresis band.  If configured below `min_control_soc` — whether via an
     explicit lower value, or because `min_control_soc` itself is raised above the 15 %
     recover default — it is clamped up to `min_control_soc` at startup (a zero-width band)
     and a warning is logged.

   - Safe-fail: an invalid `BMS1_SOC` read neither triggers this cutoff nor clears an
     existing hold — a missing reading is not evidence of anything either way.
   - Because a battery cannot be both full and low at the same time, this cutoff and point
     4's release-when-full mechanism never compete in practice; the minimum-SoC check is
     simply evaluated first, on its own terms, every cycle.

The tool can run this cycle in a continuous loop (configurable interval) or execute a single
iteration for manual testing.

---

## Repository Layout

```
solakon-one-fritz-powerregulator/
├── CMakeLists.txt          — build definition (mirrors fritzhome-cache style)
├── AGENTS.md               — this file
├── README.md               — user-facing documentation
├── instructions.md         — original one-line spec note
├── LICENSE.txt             — GPL-3.0-or-later
├── .gitignore
│
├── src/                    — all C++ source (flat, no sub-directories)
│   ├── main.cpp            — entry point: CLI parsing, config, control loop
│   ├── config.h            — Config struct (POD, header-only)
│   ├── logger.h            — Logger singleton declaration (stderr only)
│   ├── logger.cpp          — Logger implementation
│   ├── solakonapi.h        — SolakonApi class declaration
│   ├── solakonapi.cpp      — Modbus TCP client for the Solakon ONE
│   ├── fritzapi.h          — FritzApi class declaration
│   ├── fritzapi.cpp        — FRITZ!Box REST API client
│   └── controller.h/.cpp  — ControlLoop: ties SolakonApi + FritzApi together
│
├── cmake/                  — cross-compilation toolchain files (copied from fritzhome-cache)
│   ├── toolchain-aarch64.cmake
│   ├── toolchain-raspbian-aarch64.cmake
│   ├── toolchain-raspbian-armhf.cmake
│   └── toolchain-raspbian-bullseye-armhf.cmake
│
├── docker/                 — Docker build scripts (same style as fritzhome-cache)
│   ├── build.sh
│   ├── Dockerfile.tumbleweed
│   ├── Dockerfile.tumbleweed-aarch64
│   ├── Dockerfile.debian
│   ├── Dockerfile.raspbian-aarch64
│   ├── Dockerfile.raspbian-armhf
│   └── Dockerfile.raspbian-bullseye-armhf
│
└── data/
    ├── solakon-one-fritz-powerregulator.conf    — fully commented INI config template
    └── solakon-one-fritz-powerregulator.service — systemd unit file
```

---

## Build System

Mirrors `fritzhome-cache` exactly:

- **CMake ≥ 3.20**, single flat `CMakeLists.txt`, Ninja generator inside Docker
- **C++17** (`CMAKE_CXX_STANDARD 17`, `CMAKE_CXX_STANDARD_REQUIRED ON`, `CMAKE_CXX_EXTENSIONS OFF`)
- **FetchContent** for all non-system dependencies (no system Qt required):
  - `nlohmann_json v3.11.3` — JSON parsing for FRITZ!Box REST responses
  - `CLI11 v2.4.2` — CLI argument parsing and INI config file loading
  - `cpp-httplib v0.18.0` — HTTP client for FRITZ!Box REST API (with `CPPHTTPLIB_OPENSSL_SUPPORT`)
- **Modbus TCP** — implemented directly over POSIX sockets (no libmodbus dependency)
- **OpenSSL** — system package, required by cpp-httplib TLS support
- **Target platforms** and Docker files: identical set to fritzhome-cache (openSUSE Tumbleweed
  x86_64/aarch64, Debian 12 x86_64, Raspbian bookworm arm64/armhf, Raspbian bullseye armhf)
- **CPack** packaging: RPM (openSUSE), DEB (Debian/Raspbian)
- Install prefix: `/usr`; config file: `/etc/solakon-one-fritz-powerregulator/solakon-one-fritz-powerregulator.conf`

### Build with `./docker/build.sh`

Preferred packaging/build path is the Docker wrapper script:

```bash
./docker/build.sh [--distro <alias>|all] [--build-type Release|Debug] [--static]
```

Prerequisites:

- Docker installed and running
- User can run Docker commands (for example via `docker` group)
- Run from repository root

What the script does per selected target:

1. Builds/reuses a distro-specific builder image from `docker/Dockerfile.*`.
2. Runs CMake + Ninja + CPack inside that container as your host UID/GID.
3. Writes full build trees to `build/<family>/<distro>/<arch>/`.
4. Copies final artifacts to `out/<family>/<distro>/<arch>/`.

Available distro aliases (`--distro`):

- `opensuse-tumbleweed-x86_64`
- `opensuse-tumbleweed-aarch64`
- `debian-12-x86_64`
- `raspbian-bookworm-aarch64`
- `raspbian-bookworm-armhf`
- `raspbian-bullseye-armhf`
- `all` (default)

Common commands:

```bash
# Build all targets (default)
./docker/build.sh

# Build one target only
./docker/build.sh --distro debian-12-x86_64

# Debug package build for one target
./docker/build.sh --distro opensuse-tumbleweed-x86_64 --build-type Debug

# Static OpenSSL/C++ runtime linking (libc remains shared)
./docker/build.sh --distro raspbian-bookworm-armhf --static
```

Artifacts:

- Binary: `out/<family>/<distro>/<arch>/solakon-one-fritz-powerregulator`
- Package:
  - openSUSE: `out/.../solakon-one-fritz-powerregulator-<version>-1.<arch>.rpm`
  - Debian/Raspbian: `out/.../solakon-one-fritz-powerregulator_<version>-1_<arch>.deb`

Notes:

- `--build-type` accepts `Release` (default) or `Debug`.
- `raspbian-bullseye-armhf` automatically pins `cpp-httplib` to `v0.14.3` inside the build.
- For usage help, run `./docker/build.sh --help`.

### CMake option

```cmake
option(SOLAKON_FRITZ_POWERREGULATOR_STATIC "Link OpenSSL and C++ runtime statically" OFF)
```

---

## Dependencies and Their Roles

### Solakon ONE — `solakonapi.h/.cpp`

Communicates with the Solakon ONE (FoxESS H3/H3 Pro family) via **Modbus TCP** using
`libmodbus`. This is a synchronous, blocking client (no Qt, no event loop).

**Key registers** (from `flunaras/solakon-one-ui` `feature/initial`, cross-checked with
`nathanmarlor/foxess_modbus` and the FoxESS H3 PRO Modbus protocol V1.05):

| Register | Address | Type | Scale | Unit | Direction | Purpose |
|----------|---------|------|-------|------|-----------|---------|
| `TOTAL_PV_POWER` | 39118 | i32 (2 regs, hi word first) | /1000 | kW | Read | Total PV generation power (always ≥ 0; 0 at night) |
| `ACTIVE_POWER` | 39134 | i32 (2 regs, hi word first) | /1000 | kW | Read | Current grid active power (+export, −import) |
| `BATTERY_COMBINED_POWER` | 39237 | i32 (2 regs, hi word first) | none | W | Read | Battery power: +charging, −discharging, 0=idle |
| `INVERTER_STATUS_1` | 39063 | bitfield16 | — | — | Read | Bit 0=Standby, bit 2=Operation, bit 6=Fault |
| `GRID_STATUS_3` | 39065 | bitfield16 | — | — | Read | Bit 0=Off-Grid/EPS active |
| `BMS1_SOC` | 37612 | u16 | 1 | % | Read | Battery state of charge (primary battery) |
| `MIN_SOC` | 46609 | u16 | 1 | % | Read/Write | User-configured stop-discharging limit |
| `MAX_SOC` | 46610 | u16 | 1 | % | Read/Write | User-configured stop-charging limit |
| `WORK_MODE` | 49203 | u16 enum | — | — | Read/Write | 1=SelfUse, 2=FeedIn, 3=Backup, 4=PeakShaving, 6=ForceCharge, 7=ForceDischarge |
| `REMOTE_CONTROL` | 46001 | bitfield16 | — | — | Read/Write | Bit 0=enabled; readable for ownership detection |
| `REMOTE_TIMEOUT_SET` | 46002 | u16 | — | s | Write | Timeout before inverter reverts to normal |
| `REMOTE_ACTIVE_POWER` | 46003 | i32 | — | W | Write | Dynamic active power setpoint |

**Approach for setting export power:**

The tool uses **Mechanism B (remote control)** via registers 46001–46003. This is a dynamic,
real-time override with an automatic revert timeout — if the process crashes or is killed before
the next cycle the inverter reverts to normal after `interval + loop_timeout_extra` seconds.

Write sequence (order matters — control bitfield last):
1. Write `REMOTE_TIMEOUT_SET` (46002, seconds, u16)
2. Write `REMOTE_ACTIVE_POWER` (46003, watts, i32)
3. Write `REMOTE_REACTIVE_POWER` (46005, var, i32) — always 0
4. Write `REMOTE_CONTROL` bitfield last (46001, `0x09`: bit0=1 enable, bit1=0 generate/inject,
   bits3:2=10 target Grid)

To **release** remote control (let the inverter revert to its configured work mode):
- Write `REMOTE_CONTROL` (46001) = `0x00` (bit0=0, disabled)

**Word order:** FoxESS uses big-endian word order for 32-bit registers (high word first).
`libmodbus` on little-endian hosts requires `MODBUS_GET_INT32_FROM_INT16` with swapped words, or
an explicit swap using `modbus_set_byte_order` / manual reassembly.

**Sign convention for ACTIVE_POWER:** positive = exporting to grid, negative = importing from grid.
Verify against the specific firmware version, as FoxESS sign conventions have been observed to
vary.

```cpp
class SolakonApi {
public:
    SolakonApi();
    ~SolakonApi();

    // Connect / disconnect
    bool connect(const std::string& host, int port, int slave_id);
    void disconnect();

    // Power and grid readings (i32, hi-word first)
    [[nodiscard]] bool readExportedPower(int& watts_out) const;     // 39134
    [[nodiscard]] bool readPvPower      (int& watts_out) const;     // 39118
    [[nodiscard]] bool readBatteryPower (int& watts_out) const;     // 39237

    // Battery and configuration registers (u16)
    [[nodiscard]] bool readBatterySoc(int& percent_out) const;      // 37612, BMS1 SoC
    [[nodiscard]] bool readMaxSoc    (int& percent_out) const;      // 46610, stop-charging limit
    [[nodiscard]] bool readMinSoc    (int& percent_out) const;      // 46609, stop-discharging limit

    // Work mode (u16 enum: 1=SelfUse, 2=FeedIn, 3=Backup, 4=PeakShaving,
    // 6=ForceCharge, 7=ForceDischarge)
    [[nodiscard]] bool readWorkMode(int& mode_out) const;           // 49203

    // Status bitfields (u16) for inverter operational and grid state
    [[nodiscard]] bool readInverterStatus(uint16_t& status_out) const;  // 39063: bit0=Standby, bit2=Operation, bit6=Fault
    [[nodiscard]] bool readGridStatus    (uint16_t& status_out) const;  // 39065: bit0=Off-Grid/EPS

    // REMOTE_CONTROL read-back (used for cold-start ownership detection)
    [[nodiscard]] bool readRemoteControlBitfield(uint16_t& bitfield_out) const;  // 46001

    // Write remote-control setpoint (registers 46002, 46003, 46005, 46001).
    // Negative watts are clamped to 0 before writing.
    [[nodiscard]] bool writeRemoteControl(int watts, uint16_t timeout_seconds) const;

    // Release remote control (write REMOTE_CONTROL = 0x00).
    // Inverter reverts to its configured work mode.
    [[nodiscard]] bool releaseRemoteControl() const;

    std::string lastError() const;

private:
    int         m_sock{-1};
    int         m_slave_id{1};
    uint16_t    m_transaction_id{0};
    std::string m_last_error;
};
```

### FRITZ!Box Smart Home — `fritzapi.h/.cpp`

Communicates with the FRITZ!Box REST API (`/api/v0/smarthome/overview`) using `cpp-httplib`.
This is a thin, synchronous HTTP client — no Qt, no event loop. Authentication uses the
FRITZ!Box SID mechanism (challenge/PBKDF2-HMAC-SHA256 or legacy MD5).

Key data from `flunaras/fritzhome` (`fritzdevice.h`, `fritzapi.h`):

- **Device list endpoint:** `GET /api/v0/smarthome/overview` with `Authorization: AVM-SID <sid>`
- **Live power field:** `multimeterInterface.power` (in mW → convert to W by dividing by 1000),
  present for devices with the `ENERGY_METER` function bit (`1 << 7`)
- **Login:** challenge-response via `GET /login_sid.lua?version=2`

```cpp
struct FritzDevice {
    std::string ain;       // AIN (spaces stripped) — primary key
    std::string name;      // human-readable name
    bool        present;   // reachable by FRITZ!Box
    double      powerW;    // current power in W (0 if no energy meter)
    bool        hasEnergyMeter; // true if ENERGY_METER bit set
};

class FritzApi {
public:
    FritzApi();

    void setHost(const std::string& host);
    void setCredentials(const std::string& username, const std::string& password);
    void setIgnoreSsl(bool ignore);

    // Login (synchronous). Returns false on error.
    [[nodiscard]] bool login();

    // Fetch device list (synchronous). Returns false on error.
    [[nodiscard]] bool fetchDeviceList(std::vector<FritzDevice>& devices_out);

    // Find a device by AIN. Returns false if not found.
    [[nodiscard]] bool findDeviceByAin(const std::string& ain, FritzDevice& device_out);

    std::string lastError() const;

private:
    std::string m_host;
    std::string m_username;
    std::string m_password;
    std::string m_sid{"0000000000000000"};
    bool        m_ignore_ssl{false};
    std::string m_last_error;

    [[nodiscard]] bool computeLoginResponse(const std::string& challenge,
                                            std::string& response_out);
};
```

**Authentication flow (synchronous):**
1. `GET /login_sid.lua?version=2` → parse `<Challenge>` and `<BlockTime>`
2. If PBKDF2 challenge (contains `$`): compute `PBKDF2-HMAC-SHA256` response
3. If legacy challenge (32 hex chars): compute `MD5(challenge + "-" + password)` (UTF-16LE)
4. `GET /login_sid.lua?version=2&username=<u>&response=<r>` → parse new `<SID>`
5. SID `"0000000000000000"` means auth failed

---

## Configuration

All options are configurable via a **central INI config file** (default path:
`/etc/solakon-one-fritz-powerregulator/solakon-one-fritz-powerregulator.conf`) and via **command-line flags**,
with CLI flags taking precedence. Uses **CLI11** `set_config()` — the INI format uses the bare
option name (without `--`) as the key.

### `config.h` — Config Struct

```cpp
struct Config {
    // Solakon ONE (Modbus TCP)
    std::string solakon_host    = "192.168.1.1";
    int         solakon_port    = 502;
    int         solakon_slave_id = 1;

    // FRITZ!Box Smart Home (REST API)
    std::string fritz_host      = "fritz.box";
    std::string fritz_scheme    = "http";  // "http" or "https"
    std::string fritz_username  = "";
    std::string fritz_password  = "";
    std::string fritz_ain       = "";     // AIN of the device to read power from
    bool        fritz_ignore_ssl = false;

    // Control loop
    int  interval           = 60;   // seconds; 0 = run once and exit
    int  loop_timeout_extra = 30;   // seconds added to interval for inverter revert timeout
    int  max_power          = 800;  // maximum export power setpoint in watts
    bool dry_run            = false; // read values but do not write to Solakon ONE

    // Anti-oscillation / stability
    double smoothing        = 1.0;  // EMA factor; time-scaled per --interval, 1.0 = no smoothing
    int    max_ramp_w_per_s   = 0;    // max watts/s the WRITTEN setpoint may move; 0 = disabled
    int    min_change         = 1;    // output-side dead-band + half of ΔA input guard
    int    fritz_min_change   = 3;    // input-side ΔB dead band (FRITZ noise filter)
    int    fritz_stuck_cycles = 3;    // jitter setpoint ±1/±2W for N-1 byte-equal B reads, then fully skip on Nth

    // Remote-control release (see "Control logic" §4 above)
    int  release_threshold       = 0;    // optional A+B safety-net (0 = disabled)
    int  release_soc_hysteresis  = 2;    // %-points below MAX_SOC at which release is eligible
    int  release_debounce_cycles = 2;    // cycles B>0 must hold post-release before regain (recover side only)
    int  battery_dead_band       = 20;   // battery_w above this counts as "charging"; release refused while charging
    int  release_pv_margin       = 10;   // PV must exceed A (current export) by this much before a release is tested
    bool recover_remote_on_start = true; // refuse release until we engaged it ourselves
    int  settle_delay_ms         = 2000; // sleep after writeRemoteControl OR releaseRemoteControl before next read

    // Minimum-SoC control cutoff (see "Control logic" §5 above) — independent of §4
    int  min_control_soc         = 10;    // minimum SoC required to hold/take control (0 = disabled)
    int  min_control_soc_recover = 15;    // absolute SoC required to resume control (not an offset)

    // Logging
    int log_level  = 1;     // 0=silent, 1=error, 2=warn, 3=info, 4=debug, 5=trace
};
```

### CLI — Subcommand Structure

The binary uses a **subcommand-based interface**:

```
solakon-one-fritz-powerregulator [global-options] <action> [action-options]
```

Actions:

| Action | Purpose |
|--------|---------|
| `run-loop` | Full control cycle: reads A and B, writes A+B to Solakon ONE; repeats on interval |
| `list-fritz-devices` | List all FRITZ!Box Smart Home devices with AIN, name, power, and online status |
| `read-fritz-device` | Read and print the current power of the device identified by `--fritz-ain` |
| `read-solakon` | Read and print the current grid active power from the Solakon ONE |
| `write-solakon` | Write an explicit watt value to the Solakon ONE export power limit |

```
solakon-one-fritz-powerregulator [options] <action> [action-options]

Global options (available to all actions):

  Connection — Solakon ONE:
    -H, --solakon-host <host>       Solakon ONE hostname or IP  (default: 192.168.1.1)
    -P, --solakon-port <port>       Modbus TCP port             (default: 502)
    -s, --solakon-slave-id <id>     Modbus slave/unit ID        (default: 1)

  Connection — FRITZ!Box:
    -f, --fritz-host <host>         FRITZ!Box hostname or IP    (default: fritz.box)
        --fritz-scheme <scheme>     http (default) or https
    -u, --fritz-username <user>     FRITZ!Box username
    -p, --fritz-password <pass>     FRITZ!Box password
    -a, --fritz-ain <ain>           AIN of the FRITZ!Box device to read power from
        --fritz-ignore-ssl          Ignore FRITZ!Box TLS certificate errors

  Control:
    -n, --dry-run                   Read values but do not write to Solakon ONE

  Logging:
    -v, --verbose                   Increase log verbosity (repeatable: -vvvv)
        --log-level <0-5>           Set log level directly (overrides -v)

  General:
    -c, --config <path>             Config file path
                                    (default: /etc/solakon-one-fritz-powerregulator/
                                               solakon-one-fritz-powerregulator.conf)
        --version
    -h, --help

Action: run-loop
  Run the A+B control cycle, writing the new setpoint to the Solakon ONE.
  -i, --interval <seconds>             Poll interval in seconds; 0 = run once (default: 60)
      --loop-timeout-extra <s>         Extra seconds for the inverter revert timeout (default: 30)
      --smoothing <0.0–1.0>            EMA factor, time-scaled per --interval; 1.0=no smoothing (default: 1.0)
      --max-ramp-w-per-s <W/s>         Max rate of change (W/s) for the WRITTEN setpoint (default: 0 = disabled)
      --min-change <W>                 Output-side dead band + half of ΔA input guard (default: 1)
      --fritz-min-change <W>           Input-side ΔB dead band                     (default: 3)
      --fritz-stuck-cycles <N>         Jitter setpoint ±1/±2W for N-1 byte-equal B reads, then skip on Nth (default: 3)
      --release-threshold <W>          Optional A+B safety-net release condition  (default: 0 = disabled)
      --release-soc-hysteresis <%>     %-points below MAX_SOC at which release is eligible (default: 2)
      --release-debounce-cycles <N>    Cycles B>0 must hold post-release before regain (recover side only) (default: 2)
      --battery-dead-band <W>          battery_w above this counts as "charging"; refuses release (default: 20)
      --release-pv-margin <W>          PV must exceed A (current export) by this much before release test (default: 50)
      --recover-remote-on-start        Refuse release until we engage ourselves    (default: on)
      --settle-delay-ms <ms>           Sleep after each write/release before next read (default: 2000)
      --min-control-soc <%>            Minimum SoC required to hold/take control  (default: 10)
      --min-control-soc-recover <%>    Absolute SoC required to resume control after a
                                       min-control-soc release (default: 15; clamped up to
                                       min-control-soc if configured lower)

Action: list-fritz-devices
  List all FRITZ!Box Smart Home devices.
  Output columns: AIN, name, power (W or [no energy meter]), online/offline status.
  Does not require --fritz-ain.

Action: read-fritz-device
  Print the current power draw of the FRITZ!Box device identified by --fritz-ain (required).
  Output: AIN <TAB> name [<TAB> powerW W] [<TAB> [offline]]

Action: read-solakon
  Read and print all Solakon ONE values used by run-loop:
    active_power     — current grid active power in W (positive = exporting, register 39134)
    pv_power         — total PV generation power in W (register 39118)
    battery_power    — battery combined power in W (register 39237);
                       positive = charging, negative = discharging
    battery_soc      — BMS1 state of charge in % (register 37612)
    max_soc_limit    — configured stop-charging limit in % (register 46610)
    min_soc_limit    — configured stop-discharging limit in % (register 46609)
    work_mode        — active work mode (register 49203):
                       1=SelfUse 2=FeedIn 3=Backup 4=PeakShaving 6=ForceCharge 7=ForceDischarge
    inverter_status  — STATUS_1 bitfield (39063): Standby/Operation/Fault
    grid_status      — STATUS_3 bitfield (39065): On-Grid / Off-Grid (EPS)
    remote_control   — REMOTE_CONTROL bitfield (46001); bit 0 = engaged

Action: write-solakon
  Write an explicit export power limit to the Solakon ONE.
  -w, --watts <W>                   Value in watts to write (>= 0, required)
  -t, --timeout <s>                 Revert timeout in seconds (1-65535, required):
                                    inverter returns to normal if no new setpoint arrives
```

### Log Levels

| Value | Name | Meaning |
|-------|------|---------|
| 0 | SILENT | No output |
| 1 | ERR | Errors only (default) |
| 2 | WARN | Warnings + errors |
| 3 | INFO | Normal operation messages |
| 4 | DBG | Per-cycle debug detail |
| 5 | TRACE | Full request/response tracing |

All logging goes to **stderr** only (same as fritzhome-cache). The `LogLevel` enum uses `ERR`
(not `ERROR`) to avoid platform macro conflicts.

---

## Main Control Flow (`main.cpp`)

`main()` registers all global options and four subcommands with CLI11, calls `CLI11_PARSE`,
resolves the log level, then dispatches to one of four static action functions.

```cpp
int main(int argc, char* argv[]) {
    // 1. Build CLI11 app with global options + four subcommands.
    Config cfg;
    CLI::App app{"Solakon ONE remote control"};
    app.set_config("-c,--config", "/etc/solakon-one-fritz-powerregulator/…");
    app.require_subcommand(1);

    // Global options: --solakon-host/port/slave-id, --fritz-host/username/password/ain,
    // --fritz-ignore-ssl, --dry-run, -v/--log-level.

    app.add_subcommand("run-loop",      "…")->add_option("-i,--interval", cfg.interval, …);
    app.add_subcommand("read-fritz",    "…");
    app.add_subcommand("read-solakon",  "…");
    app.add_subcommand("write-solakon", "…")->add_option("-w,--watts", cfg.write_watts, …)
                                            ->required();

    CLI11_PARSE(app, argc, argv);

    // 2. Resolve log level from -v count or --log-level, apply to Logger.
    Logger::instance().setLevel(cfg.log_level);

    // 3. Dispatch.
    if (app.got_subcommand("run-loop"))      return actionRunLoop(cfg);
    if (app.got_subcommand("read-fritz"))    return actionReadFritz(cfg);
    if (app.got_subcommand("read-solakon")) return actionReadSolakon(cfg);
    if (app.got_subcommand("write-solakon")) return actionWriteSolakon(cfg);
    return 1;
}
```

### `actionRunLoop`

```cpp
static int actionRunLoop(const Config& cfg) {
    // Requires --fritz-ain.
    // Connects to Solakon ONE + logs into FRITZ!Box.
    // Runs the A+B control cycle; repeats every cfg.interval seconds until SIGINT/SIGTERM.
    // cfg.interval == 0 → run once and exit.
    // cfg.dry_run  → skip writeRemoteControl() and releaseRemoteControl().
    //
    // Startup:
    //   - Reads MAX_SOC (46610) once; this is the "battery full" reference for releases.
    //     If the read fails or returns out-of-range, the SoC-based release trigger is
    //     disabled this run (falls back to A+B safety net if configured).
    //   - If cfg.recover_remote_on_start: reads REMOTE_CONTROL (46001).  If bit 0 is
    //     already set, sets owned_by_us = false → release path inhibited until our own
    //     writeRemoteControl succeeds (prevents stealing the FoxESS app's strategy
    //     periods or a leftover session from a previous instance).
    //
    // Per-cycle (atomic read window, Solakon first, FRITZ last):
    //   read ACTIVE_POWER (39134)        → exported_w
    //   read TOTAL_PV_POWER (39118)      → pv_w           [warn on failure, proceed]
    //   read BATTERY_COMBINED_POWER      → battery_w      [warn on failure, proceed]
    //   read BMS1_SOC (37612)            → soc            [warn on failure, soc=-1]
    //   read INVERTER_STATUS_1 (39063)   → inv_status     [warn on failure]
    //   read GRID_STATUS_3 (39065)       → grid_status    [warn on failure]
    //   read FRITZ!Box device power      → fritz_w        [hard error on failure]
    //   combined = exported_w + fritz_w
    //
    //   Evaluate the release-eligibility conditions (ENGAGED -> RELEASED trigger):
    //     cond_owned        = owned_by_us
    //     cond_soc          = (soc valid) AND (soc >= MAX_SOC - release_soc_hysteresis)
    //     cond_not_charging = battery_w <= max(battery_dead_band, 0)
    //     cond_pv_headroom  = pv_w > exported_w + release_pv_margin
    //     cond_export_growing = (fritz_w < 0) OR (exported_w > engage_baseline_a)
    //     cond_inv_state    = STATUS_1 shows Operation set, Fault clear; STATUS_3 shows
    //                         Off-Grid/EPS clear; both reads succeeded
    //     cond_combined     = (release_threshold <= 0) OR (combined <= release_threshold)
    //     (work-mode and flat PV-threshold conditions have been REMOVED — proved unreliable)
    //     cond_low_soc      = (min_control_soc > 0) AND (soc valid) AND (soc < min_control_soc)
    //                         — independent of all conditions above; see §5
    //
    //   if remote_engaged AND cond_low_soc AND owned_by_us:   // independent hard cutoff, checked FIRST
    //     releaseRemoteControl()                     // immediate, gated only by SoC + ownership
    //     remote_engaged = false; owned_by_us = false; low_soc_hold = true
    //     reset EMA + ema_last_update + last_written + last_write_time + regain_counter
    //           + last_exported_w/last_fritz_w/fritz_stuck_counter/engage_baseline_a
    //     sleep(cfg.settle_delay_ms)
    //     return
    //
    //   if remote_engaged AND ALL release conditions hold:
    //     releaseRemoteControl()                     // immediate — no pre-release debounce
    //     remote_engaged = false; owned_by_us = false
    //     reset EMA + ema_last_update + last_written + last_write_time + regain_counter
    //           + last_exported_w/last_fritz_w/fritz_stuck_counter/engage_baseline_a
    //     sleep(cfg.settle_delay_ms)                  // settle before the recover test's first B read
    //     return
    //
    //   if !remote_engaged:
    //     if min_control_soc > 0 AND soc valid:            // low-SoC hold maintenance (see §5)
    //       if soc < min_control_soc: low_soc_hold = true
    //       else if low_soc_hold AND soc >= min_control_soc_recover_effective:
    //         low_soc_hold = false; just_recovered_from_low_soc = true
    //     if low_soc_hold: hold released; return             // overrides cold-start/regain below
    //     if just_recovered_from_low_soc:                    // SoC-driven, immediate, no B debounce
    //       fall through to setpoint path unconditionally
    //     else if !ever_engaged:                           // true cold start — nothing to recover from
    //       fall through to setpoint path unconditionally
    //     else:                                       // post-release recovery: B-driven only
    //       regain_counter = (fritz_w > 0) ? regain_counter + 1 : 0
    //       if regain_counter < release_debounce_cycles: hold released; return
    //       else: fall through to setpoint path to regain control
    //
    //   // Setpoint path (engaged, or regaining/first-time engaging):
    //   //
    //   // Stuck-data detection: fritz_stuck_counter counts consecutive cycles where
    //   // fritz_w exactly repeats.  apply_jitter = (fritz_stuck_cycles>0) AND fritz_w
    //   // unchanged AND counter < fritz_stuck_cycles.  Once counter reaches
    //   // fritz_stuck_cycles: log a warning, refreshKeepAlive() (re-send last_written to
    //   // refresh REMOTE_TIMEOUT_SET WITHOUT changing the commanded value — see below),
    //   // and return without computing a new setpoint at all this cycle.
    //   raw_setpoint = clamp(exported_w + fritz_w, 0, max_power)
    //
    //   if !apply_jitter:                    // freeze ema_setpoint while B is a stale repeat —
    //                                         // recomputing it from the current A would re-add
    //                                         // the same B on top of an A that already reflects
    //                                         // last cycle's write, a feedback loop rather than
    //                                         // smoothing (see config.h smoothing comment)
    //     if ema_setpoint < 0: ema_setpoint = raw_setpoint          // seed on first genuine update
    //     else:
    //       dt    = now - ema_last_update                          // wall-clock, monotonic
    //       n     = dt / max(cfg.interval, 1)
    //       alpha = 1 - (1 - cfg.smoothing) ^ n    // time-scaled: alpha == smoothing when a
    //                                              // genuine update arrives exactly one
    //                                              // interval after the last one; alpha -> 1
    //                                              // as the gap grows (FRITZ refreshed
    //                                              // slowly this time); alpha shrinks for a
    //                                              // faster-than-usual gap — self-adjusts to
    //                                              // however sparsely/densely genuine updates
    //                                              // actually arrive, no rate estimation needed
    //       ema_setpoint = alpha * raw_setpoint + (1 - alpha) * ema_setpoint
    //     ema_last_update = now
    //
    //   setpoint_base = round(ema_setpoint), clamped to max_power
    //   jitter = apply_jitter ? kJitterPattern[(fritz_stuck_counter - 1) % 4] : 0  // {1,-1,2,-2}
    //   setpoint = clamp(setpoint_base + jitter, 0, max_power)
    //
    //   // Ramp limit: bound |setpoint - last_written| to
    //   // max_ramp_w_per_s * (time since the most recent write of ANY kind, including
    //   // keep-alive refreshes) — independent of smoothing above, which governs how much a
    //   // fresh reading is trusted, not how fast the WRITTEN value may move.  Measuring
    //   // since the last write of any kind (not just value-changing ones) means a long
    //   // quiet spell never "banks" an oversized jump allowance.  Skipped when
    //   // last_written < 0 (no previous commanded value yet to ramp from).
    //   if cfg.max_ramp_w_per_s > 0 AND last_written >= 0:
    //     max_step = cfg.max_ramp_w_per_s * (now - last_write_time)
    //     setpoint = clamp(setpoint, last_written - max_step, last_written + max_step)
    //
    //   if (!apply_jitter AND cfg.min_change > 0 AND last_written >= 0
    //       AND |setpoint - last_written| < cfg.min_change):
    //     refreshKeepAlive(); return   // change too small — skip, but still refresh keep-alive
    //
    //   writeRemoteControl(setpoint, cfg.interval + cfg.loop_timeout_extra)
    //   if !remote_engaged (i.e. just transitioned in): engage_baseline_a = exported_w
    //   last_written = setpoint; last_write_time = now
    //   remote_engaged = true; owned_by_us = true; ever_engaged = true
    //   sleep(cfg.settle_delay_ms)  // let inverter ramp before next read
    //
    // Clean shutdown:
    //   When the main loop exits (signal or --interval 0 completion), call
    //   cleanShutdownRelease(): if owned_by_us, issue releaseRemoteControl() so the
    //   inverter is not left under a stale cap waiting for its revert timeout.
}
```

### `actionListFritzDevices`

```cpp
static int actionListFritzDevices(const Config& cfg) {
    // Logs into FRITZ!Box, calls fetchDeviceList(), prints a table.
    // Output columns: AIN, name, power (W or [no energy meter]), online/offline.
    // Does not require --fritz-ain.
}
```

### `actionReadFritzDevice`

```cpp
static int actionReadFritzDevice(const Config& cfg) {
    // Requires --fritz-ain (validated before connecting).
    // Logs into FRITZ!Box, calls findDeviceByAin(), prints one device line.
    // Output: AIN <TAB> name [<TAB> powerW W] [<TAB> [offline]]
}
```

### `actionReadSolakon`

```cpp
static int actionReadSolakon(const Config& cfg) {
    // Connects to Solakon ONE.
    // Reads and prints all values used by run-loop:
    //   active_power     (readExportedPower,           register 39134)
    //   pv_power         (readPvPower,                  register 39118)
    //   battery_power    (readBatteryPower,             register 39237)
    //   battery_soc      (readBatterySoc,               register 37612)
    //   max_soc_limit    (readMaxSoc,                   register 46610)
    //   min_soc_limit    (readMinSoc,                   register 46609)
    //   work_mode        (readWorkMode,                 register 49203)
    //   inverter_status  (readInverterStatus,           register 39063)
    //   grid_status      (readGridStatus,               register 39065)
    //   remote_control   (readRemoteControlBitfield,   register 46001)
    // The three power readings are hard requirements (return 1 if any fail);
    // the remaining diagnostic reads fail soft and print "[read failed]" if missing.
}
```

### `actionWriteSolakon`

```cpp
static int actionWriteSolakon(const Config& cfg) {
    // cfg.write_watts is required (enforced by CLI11 ->required()).
    // cfg.dry_run → print without writing.
    // Otherwise connects to Solakon ONE, calls writeRemoteControl(cfg.write_watts, cfg.write_timeout).
}
```

---

## Logger (`logger.h` / `logger.cpp`)

Identical pattern to `fritzhome-cache`:

- **Singleton** via function-local `static Logger inst` (thread-safe since C++11)
- **Output to stderr only**
- **Log line format:** `[2026-06-27T14:03:01.123] [DEBUG] <message>\n`
  (no thread ID column needed — this tool is single-threaded)
- **Minimize lock contention:** assemble the full line as `std::string` before acquiring
  `std::lock_guard<std::mutex>` for the `std::cerr` write
- **Level check first:** `if (!is_enabled(level)) return;`
- Convenience wrappers: `log.error()`, `log.warn()`, `log.info()`, `log.debug()`, `log.trace()`
- `[[nodiscard]] bool is_enabled(LogLevel level) const`

```cpp
enum class LogLevel { SILENT=0, ERR=1, WARN=2, INFO=3, DBG=4, TRACE=5 };

class Logger {
public:
    static Logger& instance();
    void setLevel(int level);
    [[nodiscard]] bool is_enabled(LogLevel level) const;
    void log(LogLevel level, const std::string& msg);
    void error(const std::string& m) { log(LogLevel::ERR,   m); }
    void warn (const std::string& m) { log(LogLevel::WARN,  m); }
    void info (const std::string& m) { log(LogLevel::INFO,  m); }
    void debug(const std::string& m) { log(LogLevel::DBG,   m); }
    void trace(const std::string& m) { log(LogLevel::TRACE, m); }
private:
    Logger() = default;
    std::atomic<int> m_level{1};
    std::mutex       m_mutex;
};
```

---

## Coding Style

Follows `fritzhome-cache` conventions throughout:

- **C++17**: `std::string_view` for read-only string parameters, structured bindings, `if constexpr`
- **No raw `new`/`delete`**: RAII for all resources; `modbus_t*` wrapped in `SolakonApi`
  destructor with `modbus_close()` + `modbus_free()`
- **`[[nodiscard]]`** on every function returning a success/error status or a result the caller
  must use
- **`#pragma once`** in all headers (no traditional include guards)
- **`enum class LogLevel`** with `ERR` (not `ERROR`) to avoid platform macro collision
- **No exceptions** for runtime errors; return `bool` with a `lastError()` string getter
- **Guard logging:** always check `log.is_enabled(level)` before building the log string
- **`std::string_view`** parameters where strings are read-only
- **One-line thread-safety comment** per public API method (even though single-threaded, for clarity)

---

## Key Implementation Notes and Gotchas

1. **FRITZ!Box power sign:** `energyStats.power` from the FRITZ!Box electricity meter is
   **positive when the household is consuming from the grid** and negative when exporting to
   it. Value B in the control formula is typically positive (net consumption). The formula
   `A + B` increases the export setpoint when the household is consuming more, allowing the
   inverter to cover that load and keep the net grid draw near zero.

2. **Solakon ONE sign convention:** `ACTIVE_POWER` register (39134) is positive when exporting.
   Value A is thus the current export in watts. Clamp `A + B >= 0` before writing to prevent
   commanding the inverter to import.

3. **Modbus word order:** Register 39134 is a 32-bit signed integer spanning two consecutive
   16-bit registers. FoxESS uses **high word first** (big-endian word ordering). Use
   `MODBUS_GET_INT32_FROM_INT16(tab_reg, offset)` with the high word at `tab_reg[offset]` and
   low word at `tab_reg[offset+1]`.

4. **FRITZ!Box SID session expiry:** The FRITZ!Box SID can expire during a long-running loop.
   If a device-list request returns HTTP 401 or an empty/zeroed SID, call `login()` again
   before retrying.

5. **FRITZ!Box power unit:** The REST API returns `multimeterInterface.power` in **milliwatts**.
   Divide by 1000 to obtain watts. (`flunaras/fritzhome` does this internally; replicate it here.)

6. **`--fritz-ain` format:** The AIN should be provided without spaces (e.g. `"087610123456"`).
   Strip any spaces from user input before comparison.

7. **`--interval 0`:** Runs the control cycle once and exits. Useful for testing from cron.

8. **`--dry-run`:** Reads both values and computes the setpoint but does **not** call
   `writeRemoteControl()` or `releaseRemoteControl()`. Logs the would-be setpoint at INFO level.

9. **`read-fritz-device` requires `--fritz-ain`:** Use `list-fritz-devices` first to find the
   AIN. `list-fritz-devices` does not require `--fritz-ain` or any Solakon ONE options.
   Outputs one device per line: `AIN<TAB>name[<TAB>powerW W][<TAB>online/offline]`.

10. **Modbus socket timeout:** `SolakonApi` sets a 5-second `SO_RCVTIMEO`/`SO_SNDTIMEO` on the
    raw POSIX socket (no `libmodbus` dependency — this project implements FC 0x03/0x10 directly
    over sockets) to avoid hanging indefinitely if the inverter is slow or unreachable. A timed-
    out or otherwise failed transaction triggers the automatic reconnect described in note 14
    below rather than simply failing the cycle.

11. **Remote-control release safe-fail:** Before evaluating release eligibility each cycle,
    the tool reads `BMS1_SOC` (37612), `BATTERY_COMBINED_POWER` (39237), `TOTAL_PV_POWER`
    (39118), `INVERTER_STATUS_1` (39063), and `GRID_STATUS_3` (39065). If any of these reads
    fails, the corresponding condition (`cond_soc`, `cond_not_charging`, `cond_pv_headroom`,
    `cond_inv_state`) evaluates to `false` rather than silently falling back to a stale or
    default variable value — e.g. a failed `BATTERY_COMBINED_POWER` read must not be allowed
    to leave `battery_w` at its default `0` and be read as "not charging". It never releases
    on an uncertain read — failing safe means keeping the last known setpoint active (or
    holding the released state if already released).

12. **`BATTERY_COMBINED_POWER` sign convention:** Positive = battery charging (absorbing power),
    negative = discharging (supplying power), zero = idle. The release-eligibility guard uses
    `battery_w <= battery_dead_band` (not a symmetric `|battery_w| <= battery_dead_band` band)
    because discharging — however large — must NOT block release: the battery transitions
    directly from charging to discharging (via a brief idle state) right after reaching full
    SoC, and discharging is the normal post-full condition, not a reason to keep a setpoint
    engaged.

13. **`TOTAL_PV_POWER` is always non-negative:** The raw register value is an i32 but the
    physical quantity cannot be negative (PV panels cannot consume power). `readPvPower()`
    defensively clamps negative raw values to 0.  Used by `run-loop`'s `cond_pv_headroom`
    release-eligibility check (see "Control logic" §4) and by `read-solakon` for diagnostics.

14. **Modbus reconnect-on-failure:** A failed transaction (timeout, short response, Modbus
    exception, or a mismatched transaction ID) can leave the TCP byte stream misaligned: a
    response that arrives just after the client gave up on it is still sitting in the kernel
    receive buffer, and the next read on that same socket will misinterpret those stale bytes
    as part of a new message — every subsequent transaction ID then mismatches forever,
    since retrying reads on the same socket cannot re-align it. `SolakonApi::transaction()`
    therefore reconnects (a fresh TCP connection, discarding any stale bytes) and retries the
    exact same request exactly once before giving up; a plain read is safe to repeat, and a
    write repeated after a reconnect just re-sends the same values, which is harmless. As a
    related hardening measure, the response-length field read from the wire is bounds-checked
    against the fixed-size receive buffer before use, so a garbled length (most likely to
    occur on an already-misaligned stream) is rejected instead of read into an undersized
    buffer.

15. **Remote-control keep-alive during skipped writes:** Every `writeRemoteControl` call
    refreshes `REMOTE_TIMEOUT_SET` (46002), the countdown after which the inverter reverts to
    its own work mode. The anti-oscillation guards above (stuck-data detection, the input/
    output dead bands) deliberately skip re-writing an unchanged setpoint for many consecutive
    cycles — but if that skip streak outlasts `interval + loop_timeout_extra` seconds without
    any Modbus write at all, the inverter silently reverts mid-streak while the tool's own
    state still believes `remote_engaged` is true. Each of those three skip paths therefore
    calls `refreshKeepAlive()`, which re-sends the unchanged `last_written` setpoint (same
    value in, same value out — no ramp, no settle delay needed) purely to keep
    `REMOTE_TIMEOUT_SET` from lapsing.


---

## Example Usage

```bash
# List all FRITZ!Box Smart Home devices to find the AIN
solakon-one-fritz-powerregulator \
  --fritz-host fritz.box \
  --fritz-username admin \
  --fritz-password secret \
  list-fritz-devices

# Read a specific device by AIN
solakon-one-fritz-powerregulator \
  --fritz-host fritz.box \
  --fritz-username admin \
  --fritz-password secret \
  --fritz-ain 087610123456 \
  read-fritz-device

# Read current exported power from the Solakon ONE
solakon-one-fritz-powerregulator --solakon-host 192.168.1.148 read-solakon

# Write an explicit export limit directly
solakon-one-fritz-powerregulator --solakon-host 192.168.1.148 write-solakon --watts 3500 --timeout 120

# Run once (manual test, dry-run)
solakon-one-fritz-powerregulator \
  --solakon-host 192.168.1.148 \
  --fritz-host fritz.box \
  --fritz-username admin \
  --fritz-password secret \
  --fritz-ain 087610123456 \
  --dry-run \
  -vvv \
  run-loop --interval 0

# Run in a continuous loop every 30 seconds
solakon-one-fritz-powerregulator \
  --config /etc/solakon-one-fritz-powerregulator/solakon-one-fritz-powerregulator.conf \
  --log-level 3 \
  run-loop --interval 30
```

---

## Desktop UI (`ui/`)

`ui/` contains an optional Qt6 desktop application, `solakon-one-fritz-powerregulator-ui`, in the
style of `github.com/flunaras/solakon-one-ui` (Qt6 Widgets + Charts, dockable panels persisted via
`QSettings`, connection dialog with CLI flags to skip it). Unlike `solakon-one-ui`, it does **not**
talk Modbus TCP directly -- it is a client of this project's own REST API (`src/restapi.h/.cpp`),
polling `GET /api/v1/status` on a timer and issuing `POST`/`DELETE /api/v1/override` for manual
control.

Layout:

```
ui/
├── CMakeLists.txt          — Qt6 target; standalone project when configured directly
│                             (cmake -S ui -B build), subdirectory target when the
│                             parent project is configured with -DBUILD_UI=ON
├── data/
│   └── solakon-one-fritz-powerregulator-ui.desktop
├── docker/                 — Docker build scripts for the UI's own RPM/DEB packages,
│   ├── build.sh              in the style of solakon-one-ui's own docker/build.sh
│   ├── Dockerfile.tumbleweed
│   ├── Dockerfile.tumbleweed-aarch64
│   └── Dockerfile.ubuntu-24.04
├── cmake/
│   └── toolchain-aarch64.cmake — aarch64 cross-compile toolchain (see below)
└── src/
    ├── main.cpp             — QApplication + CLI parsing (host/port/scheme/interval only)
    ├── apistatus.h          — Qt-side mirror of ApiSnapshot/ManualOverride JSON (restapi.h)
    ├── apiclient.h/.cpp     — async REST client (QNetworkAccessManager); GET/POST/DELETE
    ├── secretstore.h/.cpp   — API key storage via QtKeychain (platform secret store)
    ├── connectiondialog.h/.cpp — host/port/scheme/interval + API key entry
    ├── mainwindow.h/.cpp    — QMainWindow; owns ApiClient/SecretStore, dockable panels
    ├── statuswidget.h/.cpp  — read-only status panel (grid/PV/battery/meter/SoC/remote state)
    ├── chartwidget.h/.cpp   — rolling Qt Charts time-series (grid/PV/battery/meter power)
    └── overridewidget.h/.cpp — setpoint/release override controls; current state shown as a
                                green/yellow/red status chip (none / auto-expiring / indefinite)
```

Key points:

- **The API key is never persisted in `QSettings` or any config file.** `SecretStore` wraps
  QtKeychain's `ReadPasswordJob`/`WritePasswordJob`/`DeletePasswordJob`, keyed by `"host:port"`
  under service name `solakon-one-fritz-powerregulator`, so the key lives only in the OS secret
  store (Secret Service/libsecret, KWallet, Credential Manager, or Keychain). All other
  connection settings (host, port, scheme, ignore-TLS-errors, poll interval, window/dock layout)
  are ordinary `QSettings` values.
- `ApiClient` is transport-agnostic Qt/JSON glue over the REST API's actual JSON shapes
  (`snapshotToJson()`/`overrideToJson()` in `restapi.cpp`) -- field names and optional/nullable
  semantics must be kept in sync with the server side if either changes.
- `ChartWidget` retains samples for up to 24 h (the longest selectable window) by trimming each
  `QLineSeries`' oldest points once they age past that fixed ceiling, independent of the
  currently *displayed* window -- see the "Window" combo below; widening it never comes up empty
  for history already collected. A parallel `m_history` vector (independent of the series' point
  data) backs a persistent hover tooltip -- crosshair (`QGraphicsLineItem`) + info box
  (`QGraphicsRectItem`/`QGraphicsSimpleTextItem`) driven by an `eventFilter` on the viewport,
  matching `solakon-one-ui`'s `PvChartWidget` pattern rather than `QToolTip` (which auto-hides on
  a timer). The X axis's visible *span* is selected via a "Window" combo box (5 min .. 24 h, same
  table as `solakon-one-ui`); its *position* within the retained history is controlled by a
  horizontal `QScrollBar` (also matching `PvChartWidget`: value = seconds from the first
  retained sample to the right edge of the window, `m_scrollAtEnd` tracks whether it's pinned to
  live data or scrolled back). The Y axis auto-scales -- to only the samples within the
  currently visible window, not the full 24 h retained -- to "nice" round numbers (Heckbert's
  algorithm) with 0 W always included as an exact tick; checking "Lock Y" (mirroring
  `solakon-one-ui`'s own control) simply freezes the axis at whatever range is currently
  displayed, so scrolling back through history to compare an earlier period doesn't also keep
  rescaling the Y axis out from under it. Window selection and lock state are persisted via
  `QSettings` under a `chart/` prefix.
- `MainWindow` polls on a `QTimer` at the configured interval; every REST call, including
  override actions, is asynchronous (no blocking network I/O on the GUI thread). An
  `autoConnectIfEnabled()` entry point (called from `main.cpp` only when no `--host` CLI option
  was given) reconnects automatically to the last-saved connection if the connection dialog's
  "Automatically connect..." checkbox (`ConnectionSettings::autoConnectOnStartup`) was set --
  the API key itself still always comes from the secret store, never from this persisted flag.
  Menu layout mirrors `solakon-one-ui`: `createMenus()` builds a **File** menu (Connect...
  /Disconnect/Quit; `m_connectAction`/`m_disconnectAction` enabled state tracks
  `setConnectedState()`), and `setupDocks()` builds a **View** menu using each
  `QDockWidget::toggleViewAction()` -- an auto-syncing checkable `QAction` owned by the dock
  itself, so no manual visibility bookkeeping is needed and it stays correct even if a panel is
  closed via its own title-bar X button. Disconnecting (`disconnectFromServer()`) just stops the
  poll timer and resets UI state via `setConnectedState(false)` -- it does not need to notify the
  server, since the REST API is stateless per-request (no server-side session to tear down).
- Built via `add_subdirectory(ui)` guarded by the top-level `BUILD_UI` CMake option (default
  `OFF`), so the headless CLI/service build never requires Qt to be installed.
- **Packaging is standalone, not bundled with the CLI/service package.** `ui/CMakeLists.txt`
  detects whether it is the top-level source directory (`CMAKE_SOURCE_DIR STREQUAL
  CMAKE_CURRENT_SOURCE_DIR`); only then does it declare its own `project()`, version, and
  `include(CPack)` block (package name `solakon-one-fritz-powerregulator-ui`). `ui/docker/build.sh`
  always configures `ui/` this way (`cmake /src -B /build` with `/src` bind-mounted to `ui/`,
  not the repo root), producing a separate RPM (openSUSE Tumbleweed x86_64/aarch64) or DEB
  (Ubuntu 24.04) under `ui/out/<family>/<distro>/<arch>/` -- independent of `docker/build.sh`'s
  own `out/<family>/<distro>/<arch>/` tree for the CLI/service binary. `CPACK_RPM_PACKAGE_ARCHITECTURE`
  is set explicitly from `CMAKE_SYSTEM_PROCESSOR`, since CPack's RPM generator otherwise tags the
  package with the *build host's* `uname -m` even when cross-compiling.
- **aarch64 cross-compilation** (`ui/docker/Dockerfile.tumbleweed-aarch64`,
  `ui/cmake/toolchain-aarch64.cmake`) reuses the CLI/service build's own `cross-aarch64-gcc14`
  toolchain, but needs a far larger target sysroot than OpenSSL alone: Qt6 (Core/Gui/Widgets/
  Network/Charts/DBus/OpenGL/OpenGLWidgets), libsecret, and glib2. A `zypper --root`-based
  cross-arch chroot turned out not to be viable -- this libzypp no longer honors any architecture
  override, so `zypper install` on an x86_64 host reports "no provider found" for every
  aarch64-only package regardless of `--root`. Proper aarch64 emulation would need
  qemu-user-static + binfmt_misc registration (a privileged-container change deliberately
  avoided here, same as the CLI/service build's own aarch64 target). Instead, each required
  package is looked up directly on the openSUSE ports mirror's flat per-arch directory listing
  and pulled with plain `curl`, then only its file *contents* are extracted with
  `rpm2cpio | cpio` (no RPM database, no install, no scriptlets -- nothing aarch64 ever executes
  on the x86_64 build host). Deliberately not using the `qt6-base-devel`/`qt6-gui-devel` *meta*
  packages -- on a real system those pull in the full X11/Wayland/Mesa/systemd dependency graph
  (~250 packages) purely so the platform QPA plugins have everything to build against, none of
  which this project's own code ever includes or needs; `-Wl,--allow-shlib-undefined` (already
  used for the same reason in the CLI build's OpenSSL sysroot) covers the rest. `QT_HOST_PATH` is
  set to the image's native (x86_64) Qt6 install so Qt's CMake build system can still run
  moc/uic/rcc during the build (those must be host binaries, never the aarch64 target
  libraries).

---

## Reference Projects

| Project | Role |
|---------|------|
| `github.com/flunaras/fritzhome` (`fritzapi.h`, `fritzdevice.h`) | Source of FRITZ!Box API design, data types, REST endpoint paths, JSON field names, and authentication flow |
| `github.com/flunaras/solakon-one-ui` (branch `feature/initial`) | Source of Modbus register map, `ModbusApi` design, `InverterSettings`, and sign conventions |
| `github.com/flunaras/fritzhome-cache` | Reference for build system (CMakeLists.txt, Docker, CPack), CLI11 usage, `Config` struct pattern, `Logger` singleton, coding style, and target platforms |
