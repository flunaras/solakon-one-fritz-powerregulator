# Architecture — solakon-one-fritz-powerregulator

## Overview

`solakon-one-fritz-powerregulator` is a single-binary, single-threaded C++17 command-line tool.
It has no persistent daemon state, no database, and no event loop. Each run either executes
a control cycle once and exits, or loops at a fixed interval until interrupted.

```
┌────────────────────────────────────────────────────────────────┐
│                   solakon-one-fritz-powerregulator             │
│                                                                │
│  main()                                                        │
│   │                                                            │
│   ├─ CLI11 parsing (global opts + subcommand)                  │
│   │                                                            │
│   └─ dispatch to action function                               │
│        │                                                       │
│        ├─ actionRunLoop()        ◄── main production path      │
│        │    │                                                  │
│        │    ├─ SolakonApi::readExportedPower()   (Modbus TCP)  │
│        │    ├─ FritzApi::findDeviceByAin()       (HTTP/S REST) │
│        │    ├─ compute setpoint = clamp(A+B, 0, max_power)     │
│        │    └─ SolakonApi::writeRemoteControl()  (Modbus TCP)  │
│        │                                                       │
│        ├─ actionListFritzDevices()                             │
│        ├─ actionReadFritzDevice()                              │
│        ├─ actionCheckFritzDevice()                             │
│        ├─ actionReadSolakon()                                  │
│        └─ actionWriteSolakon()                                 │
└────────────────────────────────────────────────────────────────┘
         │                              │
         ▼                              ▼
  Solakon ONE inverter           FRITZ!Box router
  (Modbus TCP :502)              (HTTP/S REST API)
```

---

## Module breakdown

### `main.cpp` — entry point and CLI

- Builds the CLI11 application with global options and six subcommands.
- Resolves the effective log level (`--log-level` > `-v` count > config file default).
- Dispatches to one of six static action functions.
- Owns signal handling (`SIGINT` / `SIGTERM`) for the run-loop via `std::atomic<bool> g_stop`.

### `config.h` — `Config` struct

Plain-old-data struct populated entirely by CLI11. No methods. Fields map 1:1 to CLI option
names (underscores replace hyphens). Holds defaults for every option.

Key fields:

| Field | CLI flag | Default | Notes |
|---|---|---|---|
| `solakon_host` | `--solakon-host` | `192.168.1.1` | |
| `solakon_port` | `--solakon-port` | `502` | |
| `fritz_host` | `--fritz-host` | `fritz.box` | |
| `fritz_ain` | `--fritz-ain` | `""` | Required for run-loop and read/check-fritz-device |
| `interval` | `--interval` | `60` | Seconds; 0 = run once |
| `loop_timeout_extra` | `--loop-timeout-extra` | `30` | Added to interval for inverter revert timeout |
| `max_power` | `--max-power` | `800` | Watts; upper clamp for all setpoints |
| `smoothing` | `--smoothing` | `1.0` | EMA factor; time-scaled per `interval`, 1.0 = no smoothing |
| `max_ramp_w_per_s` | `--max-ramp-w-per-s` | `0` | Max W/s the *written* setpoint may move; 0 = disabled |
| `min_change` | `--min-change` | `1` | Dead-band threshold in watts; 0 = disabled |
| `dry_run` | `--dry-run` | `false` | Skip all writes |
| `log_level` | `--log-level` | `1` | 0–5 |

### `logger.h` / `logger.cpp` — `Logger` singleton

- Single instance via function-local `static Logger inst` (thread-safe since C++11).
- Output to `stderr` only; format: `[ISO-8601 timestamp] [LEVEL] message\n`.
- Level check (`is_enabled()`) before string construction to avoid format overhead.
- `std::mutex` guard only around the `std::cerr` write, not around level check.
- Convenience wrappers: `error()`, `warn()`, `info()`, `debug()`, `trace()`.

### `solakonapi.h` / `solakonapi.cpp` — `SolakonApi`

Raw POSIX-socket Modbus TCP client for the FoxESS Solakon ONE (H3/H3 Pro family).
No external Modbus library — the tool implements only the two function codes it needs:

| FC | Name | Used for |
|---|---|---|
| `0x03` | Read Holding Registers | Read `ACTIVE_POWER` (39134) |
| `0x10` | Write Multiple Registers | Write remote-control registers (46001–46005) |

**Modbus TCP ADU structure:**

```
[Transaction ID 2B][Protocol ID 2B = 0x0000][Length 2B][Unit ID 1B][FC 1B][Data NB]
```

**Register map (relevant subset):**

| Name | Address | Type | Scale | Direction | Purpose |
|---|---|---|---|---|---|
| `ACTIVE_POWER` | 39134 | i32 (2×u16, hi word first) | /1000 → kW | Read | Current grid active power |
| `REMOTE_CONTROL` | 46001 | u16 bitfield | — | Write | Enable/direction flags; written last |
| `REMOTE_TIMEOUT_SET` | 46002 | u16 | seconds | Write | Revert timeout |
| `REMOTE_ACTIVE_POWER` | 46003 | i32 | watts | Write | Active power setpoint |
| `REMOTE_REACTIVE_POWER` | 46005 | i32 | var | Write | Always 0 here |

**Write sequence for `writeRemoteControl(watts, timeout_s)`:** four separate `FC 0x10`
requests, in order (the inverter only activates the new setpoint once it sees the last one):
1. `REMOTE_TIMEOUT_SET` (46002, u16, seconds)
2. `REMOTE_ACTIVE_POWER` (46003, i32, watts)
3. `REMOTE_REACTIVE_POWER` (46005, i32, always 0)
4. `REMOTE_CONTROL` (46001, u16 bitfield: bit 0 = enable, bit 1 = 0 for inject) — activates the command

**Word order:** FoxESS uses big-endian word order (high word first) for 32-bit registers.
On a little-endian host the two 16-bit words must be byte-swapped when assembling/disassembling.

**Sign convention for `ACTIVE_POWER`:** positive = exporting to grid, negative = importing.

**Timeout safety:** A 5-second socket read/write timeout is set via `SO_RCVTIMEO` /
`SO_SNDTIMEO` to prevent hanging if the inverter is unreachable.

**Reconnect-on-failure:** A failed transaction (timeout, short response, Modbus exception, or
mismatched transaction ID) can leave the TCP byte stream misaligned — a response that arrives
just after the client gave up on it sits in the socket's receive buffer and corrupts the next
read, so every subsequent transaction ID mismatches forever without intervention. `transaction()`
handles this transparently: on any failure it reconnects (a fresh TCP connection) and retries
the exact same request once before giving up. The response-length field read from the wire is
also bounds-checked against the fixed-size receive buffer before use, rejecting a garbled
length (most likely on an already-misaligned stream) instead of reading past the buffer.

### `fritzapi.h` / `fritzapi.cpp` — `FritzApi`

Synchronous `cpp-httplib` HTTP/HTTPS client for the FRITZ!Box Smart Home REST API.

**Authentication flow:**

```
GET /login_sid.lua?version=2
  → parse <Challenge> and <BlockTime>
  → if challenge contains '$': PBKDF2-HMAC-SHA256 response
  → else: MD5(UTF-16LE(challenge + "-" + password))
GET /login_sid.lua?version=2&username=<u>&response=<r>
  → parse <SID>; "0000000000000000" = auth failed
```

**Device list endpoint:** `GET /api/v0/smarthome/overview` with
`Authorization: AVM-SID <sid>`

**Power field:** `multimeterInterface.power` in **milliwatts** → divide by 1000 for watts.

**Session expiry:** If a request returns HTTP 401 or the SID becomes zeroed, `login()` is
called again transparently before retrying.

**`FritzDevice` struct:**

| Field | Source JSON | Notes |
|---|---|---|
| `ain` | `ain` (spaces stripped) | Primary key |
| `name` | `name` | Human-readable |
| `productName` | `productName` | Model string; empty for groups |
| `deviceType` | derived from `units[].interfaces` | Comma-separated capability list |
| `present` | `present` | `true` = reachable |
| `powerW` | `multimeterInterface.power / 1000` | Watts; 0 if no energy meter |
| `hasEnergyMeter` | `multimeterInterface` presence | |

---

## Control algorithm

```
A = SolakonApi::readExportedPower()     // watts, positive = exporting
B = FritzApi::findDeviceByAin().powerW  // watts, positive = consuming

raw = clamp(A + B, 0, max_power)

// Stuck-data detection: the FRITZ!Box's own refresh cadence is not fixed
// (observed ~10 s to ~110 s in the field) and far slower than the poll
// interval, so B commonly repeats byte-for-byte across many cycles between
// genuine refreshes.  fritz_stuck_counter counts consecutive exact repeats.
apply_jitter    = fritz_stuck_cycles > 0 and B == last_B and counter < fritz_stuck_cycles
fully_stuck     = fritz_stuck_cycles > 0 and counter >= fritz_stuck_cycles

if fully_stuck:
    refreshKeepAlive()   // re-send last_written unchanged — see "Keep-alive" below
    return               // no setpoint recomputation this cycle

// EMA smoothing — frozen entirely while apply_jitter holds.  A only moves in
// response to our own previous write, so recomputing raw = A + B from a
// repeated/stale B would re-add that same B on top of an A that already
// reflects it: a feedback loop, not smoothing.  ema is only ever touched on
// a genuine (non-repeated) B reading.
if !apply_jitter:
    if ema < 0:
        ema = raw                                    // seed on first genuine update
    else:
        dt    = now - ema_last_update                // real elapsed seconds, monotonic clock
        n     = dt / max(interval, 1)
        alpha = 1 - (1 - smoothing) ^ n               // time-scaled — see rationale below
        ema   = alpha * raw + (1 - alpha) * ema
    ema_last_update = now

setpoint_base = round(ema)                             // clamped to max_power
jitter        = apply_jitter ? {1,-1,2,-2}[(counter-1) % 4] : 0
setpoint      = clamp(setpoint_base + jitter, 0, max_power)

// Ramp limit: bounds the WRITTEN step independently of smoothing above.
// Measured since the most recent write of ANY kind (including keep-alive
// refreshes), so a long quiet spell never "banks" an oversized allowance.
if max_ramp_w_per_s > 0 and last_written >= 0:
    max_step = max_ramp_w_per_s * (now - last_write_time)
    setpoint = clamp(setpoint, last_written - max_step, last_written + max_step)

// Dead band (bypassed while jittering — jittering IS the write for that cycle)
if !apply_jitter and min_change > 0 and last_written >= 0 and |setpoint - last_written| < min_change:
    refreshKeepAlive()
    return   // change too small; suppress to avoid hunting, but keep the session alive

if !dry_run:
    timeout = interval + loop_timeout_extra   // clamped to uint16_t
    SolakonApi::writeRemoteControl(setpoint, timeout)
    last_written = setpoint
    last_write_time = now
```

**Rationale:** When the household (B) consumes power from the grid, increasing the inverter's
export setpoint lets the inverter cover that load from the battery/solar rather than pulling
from the grid. Clamping to `[0, max_power]` ensures the inverter never imports and respects
the configured export cap (e.g. 800 W German Balkonkraftwerk limit).

**Revert timeout:** The inverter automatically reverts to its normal operating mode if no
new remote-control setpoint arrives within `timeout` seconds. This means the tool crashing
or being killed between cycles has a bounded impact: the inverter recovers on its own within
`interval + loop_timeout_extra` seconds.

**Keep-alive:** Every `writeRemoteControl` call refreshes that same revert timeout — but the
guards below all deliberately skip writes for many consecutive cycles, which would otherwise
let the timeout lapse and the inverter silently revert mid-streak while the tool still
believes it is in control. `refreshKeepAlive()` re-sends the unchanged `last_written` setpoint
whenever a cycle would otherwise write nothing at all (fully-stuck or dead-band skip), keeping
the session alive without touching the commanded value — no ramp limiting or settle delay
needed, since nothing is actually changing.

**Anti-oscillation:** The closed loop has a one-cycle measurement delay: the inverter
reacts to the written setpoint and changes A before the next reading, making the effective
loop gain >1 if left unattenuated. Independent mitigations are provided:

- **Stuck-data detection** (`--fritz-stuck-cycles`, default `3`): treats a byte-identical B
  repeated for many cycles as evidence of a stale FRITZ!Box cache rather than a genuinely
  unchanging load, and freezes the EMA accordingly (see above) rather than blending in a
  reading derived from it.
- **EMA smoothing** (`--smoothing`, default `1.0`): acts as a first-order low-pass filter on
  the setpoint sequence on each genuine (non-repeated) update, reducing the per-update
  correction magnitude. Scaled by actual elapsed time relative to `--interval`
  (`alpha = 1 - (1 - smoothing) ^ (dt / interval)`) so it means the same thing in real time
  regardless of how sparsely or densely genuine updates actually arrive — no rate estimation
  needed, since it is a direct function of measured elapsed time. A value of `0.5` halves the
  gain of a genuine update that arrives exactly one interval after the previous one; a longer
  gap trusts the new reading proportionally more.
- **Ramp limit** (`--max-ramp-w-per-s`, default `0` = disabled): independent of smoothing —
  bounds how fast the *written* setpoint may move in watts per second of real elapsed time,
  regardless of how far the underlying target has moved. Protects the inverter/battery from
  an abrupt commanded swing that smoothing's time-based trust (above) does not by itself
  prevent.
- **Dead band** (`--min-change`, default `1`): suppresses writes when the change is below
  the threshold. The default of `1` means a difference of exactly 0 W (no change in either
  reading) is filtered, which implicitly covers the stable-load case without a separate
  option. Setting it higher (e.g. `10`) widens the dead band to absorb larger fluctuations.

All are zero-overhead when left at their defaults (`smoothing=1.0`, `max-ramp-w-per-s=0`,
`min-change=0`).

---

## Build system

### CMake structure

Single `CMakeLists.txt` at the project root. No subdirectories.

- `project(solakon-one-fritz-powerregulator VERSION 1.0.0)` — sets `PROJECT_VERSION`
- `CMAKE_CXX_STANDARD 17`, `REQUIRED`, `EXTENSIONS OFF`
- `FetchContent` for: `cpp-httplib`, `nlohmann_json`, `CLI11` (all header-only)
- System `find_package`: `OpenSSL` (required), `Threads` (required)
- Single executable target `solakon-one-fritz-powerregulator`
- `GNUInstallDirs` for standard install paths
- `CPack` for RPM and DEB packaging

**Static linking option:**

```cmake
option(SOLAKON_FRITZ_POWERREGULATOR_STATIC
    "Link OpenSSL and C++ runtime statically; libc stays shared" OFF)
```

When `ON`, passes `-static-libstdc++ -static-libgcc` and links `OpenSSL::SSL`/`Crypto`
statically. `libc` is intentionally left shared (dynamic linking required for DNS resolution
at runtime).

### Docker build

`docker/build.sh` builds for all six target platforms in sequence (or one with `--distro`):

| Alias | Dockerfile | Arch | Package |
|---|---|---|---|
| `opensuse-tumbleweed-x86_64` | `Dockerfile.tumbleweed` | x86_64 | RPM |
| `opensuse-tumbleweed-aarch64` | `Dockerfile.tumbleweed-aarch64` | aarch64 | RPM |
| `debian-12-x86_64` | `Dockerfile.debian` | x86_64 | DEB |
| `raspbian-bookworm-aarch64` | `Dockerfile.raspbian-aarch64` | arm64 | DEB |
| `raspbian-bookworm-armhf` | `Dockerfile.raspbian-armhf` | armhf | DEB |
| `raspbian-bullseye-armhf` | `Dockerfile.raspbian-bullseye-armhf` | armhf | DEB |

Cross-compilation targets use CMake toolchain files from `cmake/`. The aarch64/armhf Debian
targets use multiarch (`dpkg --add-architecture`) to install target-architecture OpenSSL
headers without QEMU. The openSUSE aarch64 target downloads aarch64 RPMs from the ports
mirror and extracts them into the cross-compiler sysroot.

Output layout:

```
out/
├── opensuse/tumbleweed/{x86_64,aarch64}/
│   ├── solakon-one-fritz-powerregulator
│   └── solakon-one-fritz-powerregulator-1.0.0-1.<arch>.rpm
├── debian/12/amd64/
│   ├── solakon-one-fritz-powerregulator
│   └── solakon-one-fritz-powerregulator_1.0.0-1_amd64.deb
└── raspbian/{bookworm,bullseye}/{arm64,armhf}/
    ├── solakon-one-fritz-powerregulator
    └── solakon-one-fritz-powerregulator_1.0.0-1_<arch>.deb
```

---

## Deployment

### Installed file layout

| Path | Contents |
|---|---|
| `/usr/bin/solakon-one-fritz-powerregulator` | Binary |
| `/etc/solakon-one-fritz-powerregulator/solakon-one-fritz-powerregulator.conf` | INI config (mode 644) |
| `/usr/lib/systemd/system/solakon-one-fritz-powerregulator.service` | Systemd unit |

### systemd service

The unit uses `DynamicUser=yes` (systemd allocates a transient unprivileged UID at runtime;
no pre-created account needed) and a strict security sandbox
(`ProtectSystem=strict`, `ProtectHome=yes`, `PrivateTmp=yes`, etc.).

The service reads the config file and runs `run-loop` with its default `--interval 60`.
Override individual settings without editing files via a drop-in
(`systemctl edit solakon-one-fritz-powerregulator`).

---

## Dependencies summary

| Dependency | Version | How fetched | Role |
|---|---|---|---|
| `cpp-httplib` | v0.18.0 | FetchContent | FRITZ!Box HTTP/S client |
| `nlohmann_json` | v3.11.3 | FetchContent | FRITZ!Box JSON parsing |
| `CLI11` | v2.4.2 | FetchContent | CLI + INI config parsing |
| `OpenSSL` | system | `find_package` | TLS for FRITZ!Box HTTPS |
| `Threads` | system | `find_package` | `std::mutex` / `std::thread` |

(Raspbian bullseye uses `cpp-httplib v0.14.3` because OpenSSL 1.1.1 is the system version
there and v0.15.0+ of cpp-httplib requires OpenSSL ≥ 3.0.0.)

---

## Error handling conventions

- **No exceptions** for runtime errors. All API methods return `bool`; the error message is
  available via `lastError()` (a `std::string` getter).
- Callers log the error at `ERR` level and return a non-zero exit code.
- `[[nodiscard]]` on every function that returns a success/error status.
- On FRITZ!Box SID expiry (HTTP 401 or zeroed SID), `fetchDeviceList()` re-authenticates
  transparently before the next attempt.

## Coding conventions

- C++17: `std::string_view` for read-only string parameters, structured bindings.
- `#pragma once` in all headers.
- `enum class LogLevel` with `ERR` (not `ERROR`) to avoid platform macro collision.
- No raw `new`/`delete`: RAII everywhere; socket `fd` managed in `SolakonApi` destructor.
- Guard logging: `if (log.is_enabled(level))` before building the log string.
