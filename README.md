# solakon-one-fritz-powerregulator

A command-line C++17 tool that automatically adjusts the exported power of a
**FoxESS Solakon ONE** battery inverter based on the net grid power measured by a
**FRITZ!Box Smart Home** electricity meter (e.g. a FRITZ!Smart Energy 250 installed
at the grid connection point).

## How it works

Each control cycle:

1. Read **solakon_grid_power_w** — current grid active power exported by the Solakon ONE (Modbus TCP, register 39134).
2. Read **grid_meter_power_w** — current net grid power at the connection point, measured by a FRITZ!Box electricity
   meter (FRITZ!Box REST API). Positive = consuming from grid, negative = exporting to grid.
3. Compute `solakon_grid_power_w + grid_meter_power_w`, clamp to `[0, max_power]`, and write the result back to the Solakon ONE
   as a remote-control power setpoint. The inverter reverts automatically if no refresh
   arrives within the configured timeout (safety net for crashes).

The goal is to keep grid_meter_power_w at zero: the inverter emits exactly as much power as the household is
consuming at any given moment, so no energy is drawn from or exported to the grid. Surplus PV
power beyond what the household needs is stored in the battery first. Only when the battery is
full and can no longer absorb more surplus is the excess exported to the grid for environmental
reasons — the remote-control release mechanism handles this case (see below).

This loop runs continuously at a configurable interval (default: 60 s) or once on demand.

### Remote-control release

When the battery is full and the PV array is still producing surplus, the remote-control
setpoint stops being useful: it caps the inverter's export at `solakon_grid_power_w + grid_meter_power_w`, which is much less
than the available PV. Releasing remote control (writing `REMOTE_CONTROL = 0x00`) lets the
inverter revert to its configured work mode — typically Self Use, which exports all PV
surplus after covering loads — so the surplus reaches the grid instead of being wasted.

Release/recover is a two-part decision, and each part answers a different question:

- **Release eligibility** — "is it *plausible* that we no longer need to be in control?"
  A fixed set of conditions (below), evaluated fresh every cycle. The moment **all** of
  them hold, remote control is released **immediately**, as a real-world behavioural test.
  There is no debounce on this side any more — see "Why immediate, testable release" below.
- **Recover decision** — "were we *right*?" Once released, the tool watches the FRITZ!Box
  reading `grid_meter_power_w` alone. If the household starts net-importing from the grid, the release was
  wrong (or conditions changed), and control is regained within a bounded number of cycles.

#### Release eligibility

On startup the tool reads `MAX_SOC` (register 46610), which is the user-configured "stop
charging at X%" limit from the FoxESS app or web UI, and treats the battery as "full" once
SoC has reached `MAX_SOC − release-soc-hysteresis`. Remote control is released the instant
**all** of the following hold **on the same cycle**:

| # | Condition | Source | Default |
|---|---|---|---|
| 1 | We own the remote-control session | internal flag set on our first write | required |
| 2 | `SoC ≥ MAX_SOC − release-soc-hysteresis` | Modbus 37612 / 46610 | 2 %-point band |
| 3 | `BATTERY_COMBINED_POWER ≤ battery-dead-band` (not charging) | Modbus 39237 | 20 W |
| 4 | `TOTAL_PV_POWER > ACTIVE_POWER (solakon_grid_power_w) + release-pv-margin`, bypassed once `SoC ≥ MAX_SOC` | Modbus 39118, 39134 | 50 W |
| 5 | `grid_meter_power_w < 0` OR `solakon_grid_power_w >` solakon_grid_power_w when this engagement started | FRITZ + Modbus 39134 | required |
| 6 | Inverter is in Operation, not Fault, not Off-Grid | Modbus 39063, 39065 | required |
| 7 | `solakon_grid_power_w + grid_meter_power_w ≤ release-threshold` (only if `release-threshold > 0`) | Modbus 39134 + FRITZ | disabled (0) |

If any required Modbus read fails, the corresponding condition is treated as "not safe" and
release is inhibited — the tool never releases on uncertain data.

Two earlier conditions — "work mode must be Self Use" and "PV power must exceed a
threshold" — have been **removed**. Both proved unreliable in the field: some
inverters/firmware leave `WORK_MODE` stuck at a non-Self-Use value indefinitely (e.g. Force
Discharge), which permanently blocked release even when the battery was genuinely full and
idle. Rather than replace them with further indirect proxies, the recover side of the
decision now tests the actual outcome directly (next section).

#### Recover decision

Once released, the tool stops asking indirect questions ("is the work mode right?", "is PV
still producing?") and watches the one number that actually answers whether the release was
a good idea: `grid_meter_power_w`, the FRITZ!Box grid-connection reading.

- `grid_meter_power_w ≤ 0` (exporting to / not importing from the grid): the release is working — remain
  released, and reset the regain counter to 0.
- `grid_meter_power_w > 0` (importing from the grid) for `release-debounce-cycles` **consecutive** cycles:
  the release was premature, or conditions changed since — regain control by resuming
  `solakon_grid_power_w + grid_meter_power_w` setpoint writes.

SoC dropping back down or the battery resuming charging do **not**, by themselves, force a
regain — those conditions already had their say during release eligibility. The household
not importing is the only outcome that matters once released.

The very first engagement after the process starts is not subject to this test: with
nothing yet released to recover from, the tool simply takes control on its first cycle. The
regain test exists purely to validate a release decision the tool itself just made.

**Why immediate, testable release.** There is nothing to gain by debouncing the release
side any more: the release itself is a real-world test of eligibility, and a wrong call
(e.g. a one-cycle SoC or battery-power blip) costs at most `release-debounce-cycles` cycles
of being wrongly released before the recover decision above corrects it. This is simpler and
more robust than accumulating more proxy conditions to debounce against.

**Why SoC instead of `solakon_grid_power_w + grid_meter_power_w` for the primary trigger.** An earlier release rule required
`solakon_grid_power_w + grid_meter_power_w ≤ 10 W`, but in normal daytime operation with any household load the inverter
perfectly tracks the load (`solakon_grid_power_w ≈ load`, `grid_meter_power_w ≈ 0`), so `solakon_grid_power_w + grid_meter_power_w` settles at the load level —
often 100–500 W, far above 10 W. Release effectively never fired, and PV surplus that the
battery couldn't absorb stayed capped by the stale setpoint. Reading SoC directly answers
the question we actually care about: "is the battery full?"

**Why ownership tracking.** The FoxESS app's "strategy periods" feature (scheduled forced
charge/discharge windows) uses the same `REMOTE_CONTROL` register internally. If the tool
released a setpoint it didn't write, it would silently cancel an active strategy period.
The tool only releases what it engaged itself; a setpoint already engaged on startup is left
alone until our own engagement cycle takes ownership.

**Battery-charging threshold.** LiFePO4 cells often draw a few watts of balancing current
indefinitely after reaching full charge — the BMS reports `+5..+20 W` of "charging" even
though no useful energy is being absorbed. The threshold (default 20 W) treats readings at
or below it as "not charging" — idling **or discharging** both count — so float charge no
longer locks out release, and discharging (the expected state immediately after the battery
reaches `MAX_SOC`) doesn't either.

**Why PV headroom over the current export.** While engaged, `solakon_grid_power_w` is (approximately) whatever
the tool last commanded the inverter to export. Releasing can only ever *increase* the
export up to what PV can supply once the battery stops absorbing more — PV is the ultimate
ceiling. So unless PV production is comfortably above `solakon_grid_power_w`, there is no PV surplus left for a
release to unlock: releasing cannot possibly export more than the setpoint already does, and
testing one has no potential upside, only the risk of a spurious release that the recover
decision immediately has to undo once `grid_meter_power_w` turns positive again. Comparing PV against the
*current* export (rather than a flat threshold) directly answers the question that matters
— "would releasing actually help right now?" — rather than approximating it.

This condition is **bypassed (forced true) once `SoC ≥ MAX_SOC` itself** — the user's
configured ceiling, not merely the `release-soc-hysteresis`-relaxed threshold condition 2
uses. The "PV is the ultimate ceiling" reasoning above silently assumes `TOTAL_PV_POWER` is
an independent measurement of available sunlight, which only holds while the battery still
has room to climb toward `MAX_SOC`. Some inverters/firmware have been observed to curtail PV
harvest down to match whatever `ACTIVE_POWER` is currently being commanded once the battery
is genuinely full (no spare capacity left to buffer the difference) — at that point PV can no
longer exceed `solakon_grid_power_w` regardless of how much sunlight is actually available, permanently vetoing
a release that should happen: our own throttling poisons the very signal this condition
depends on. Once `SoC` has actually reached `MAX_SOC` there is nothing left to lose by
testing a release anyway — the recover decision already safely undoes one that turns out to
have no real surplus behind it, exactly as it does for every other release-eligibility
condition.

**Why the surplus/growing-export check.** `grid_meter_power_w < 0` or `solakon_grid_power_w` greater than it was when the current
engagement started is a second, complementary signal on top of the PV-headroom snapshot
above. `grid_meter_power_w < 0` means the household is, right now, net-exporting even under our own capped
setpoint — direct proof that there is already more supply available than the setpoint alone
accounts for. `solakon_grid_power_w` higher than its value when this engagement began means the control loop
itself has, net, been pushing the setpoint upward (growing load, or growing PV) — a trend
suggesting the situation keeps improving. The comparison is deliberately against a *fixed*
starting point rather than the immediately preceding cycle: `solakon_grid_power_w` is not guaranteed to rise on
every single cycle even while the underlying trend is upward, so a rolling comparison could
flap false on an ordinary one-cycle dip; comparing against where `solakon_grid_power_w` started only cares about
net progress since engagement began, however many cycles that took. Without a captured
baseline yet (true cold start, or the very first cycle of a fresh engagement) this condition
falls back to the `grid_meter_power_w < 0` half alone.

**Cold-start ownership recovery.** On startup the tool reads `REMOTE_CONTROL` (46001). If
bit 0 is set, the setpoint belongs to a previous process or the FoxESS app — the tool will
not release it until it has engaged remote control itself. This prevents a restart from
clobbering an active app-scheduled session.

**Clean shutdown.** On SIGINT/SIGTERM the tool issues a final `releaseRemoteControl()` if it
owns the session, so the inverter is not left under a stale cap waiting up to
`interval + loop-timeout-extra` seconds for its revert timeout to expire.

**Settling delay.** After every `writeRemoteControl` **or** `releaseRemoteControl`, the tool
sleeps `settle-delay-ms` (default 2000 ms) before the next cycle's reads so the inverter has
time to ramp before solakon_grid_power_w/grid_meter_power_w are measured again. This matters even more for release than it used
to: the very next `grid_meter_power_w` reading is the input the recover decision relies on, so it must
reflect the inverter's settled free-run state, not a mid-ramp transient.

### Minimum-SoC control cutoff

A second, entirely independent release/recover mechanism protects the *low* end of the
battery's charge, symmetric in purpose to the release-when-full mechanism above but simpler
in design. Where release-when-full stops the setpoint from wasting PV surplus once the
battery can't absorb any more, the minimum-SoC cutoff stops the setpoint from forcing
*further battery discharge* once the battery is already low — the `solakon_grid_power_w + grid_meter_power_w` setpoint can drive
the inverter to draw on the battery to cover household load, and below some point that's no
longer desirable.

- **Release** — a hard cutoff, not a bundle of conditions. The instant `BMS1_SOC` drops
  below `--min-control-soc`, remote control is released immediately. The only other
  requirement is session ownership (`owned_by_us`) — none of `--battery-dead-band`,
  `--release-pv-margin`, the export-growing trend, or inverter health apply, because those
  all answer "is there PV surplus a release could unlock", which isn't the question here.
- **Recovery** — purely SoC-driven and immediate: no debounce cycles, no dependency on the
  FRITZ!Box reading `grid_meter_power_w` (unlike the release-when-full recover decision). The instant
  `BMS1_SOC` rises back up to `--min-control-soc-recover`, control resumes.
- **Cold start** — read literally: "minimum charge required to control" applies from the
  very first cycle. If SoC is already below `--min-control-soc` when the process starts, the
  tool withholds control from cycle one instead of engaging unconditionally the way a normal
  cold start does.

This is independent of, and not tied to, the inverter's own `MIN_SOC` register (46609, the
user's "stop discharging" limit) — `--min-control-soc` is a directly configured value of its
own. Enabled by default at `10` %, a conservative safety floor; pass `--min-control-soc 0` to
disable the cutoff entirely (SoC can never be negative, so the check then never triggers).

`--min-control-soc-recover` is a second, independently configured **absolute** SoC value
(not an offset), defaulting to `15` %: the effective hysteresis band is simply
`min-control-soc-recover - min-control-soc`. It must be `>= --min-control-soc`; if configured
below that — whether via an explicit lower value, or because `--min-control-soc` itself is
raised above the `15` % recover default — it is clamped up to `--min-control-soc` at startup
(a zero-width band — control resumes the instant SoC is back at `--min-control-soc`) and a
warning is logged.

Because a battery cannot be both full and low at the same time, this cutoff and the
release-when-full mechanism never compete in practice — the minimum-SoC check is simply
evaluated first, on its own terms, every cycle.

## Requirements

- A FoxESS Solakon ONE (H3 / H3 Pro family) inverter with Modbus TCP enabled
- A FRITZ!Box router with a **FRITZ!Smart Energy 250** smart plug
  (see [Compatible devices](#compatible-devices) below)
- Network access from the host running this tool to both devices

## Compatible devices

### FRITZ!Smart Energy 250 (required)

The control loop reads the FRITZ!Box device's net power value at the grid connection point
and adds it to the inverter's current export to compute the new setpoint. For this to work
correctly, the FRITZ!Box device **must be able to distinguish between power consumed from the
grid and power exported to the grid** — i.e. it must support bidirectional metering.

The **FRITZ!Smart Energy 250** is currently the only FRITZ!Smart Energy device with this
capability. All other FRITZ!Smart Energy models (e.g. FRITZ!DECT 200, FRITZ!DECT 210) measure
only net consumption and cannot determine the direction of power flow. Using them with
`run-loop` would produce incorrect setpoints.

The tool enforces this by default: `--fritz-filter-product` defaults to
`"FRITZ!Smart Energy 250"`, and the suitability check (`check-fritz-device` and `run-loop`)
rejects any device whose product name does not contain any entry from the filter list.

The filter is a list — repeat `--fritz-filter-product` to allow multiple products:

```bash
solakon-one-fritz-powerregulator \
  --fritz-filter-product 'FRITZ!Smart Energy 250' \
  --fritz-filter-product 'FRITZ!Smart Energy 350' \
  list-fritz-devices
```

To disable the filter entirely (e.g. during development), omit all `--fritz-filter-product`
arguments or pass an empty string:

```bash
solakon-one-fritz-powerregulator --fritz-filter-product '' list-fritz-devices
```

## Installation

### Pre-built packages

Download the RPM (openSUSE) or DEB (Debian / Raspberry Pi OS) for your platform from the
[Releases](https://github.com/flunaras/solakon-one-fritz-powerregulator/releases) page and install
with your package manager.

### Build from source

See [docs/architecture.md](docs/architecture.md) for a full description of the build system.
Quick start with Docker (produces all platform packages):

```bash
./docker/build.sh --distro all
```

Or build natively:

```bash
cmake -S . -grid_meter_power_w build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

## Configuration

All options can be set in an INI config file and/or overridden with CLI flags.

Default config file location:
`/etc/solakon-one-fritz-powerregulator/solakon-one-fritz-powerregulator.conf`

A fully commented template is installed at that path by the package, and is also available at
`data/solakon-one-fritz-powerregulator.conf` in this repository.

## Quick start

### 0. (Optional) Discover mDNS services on your network

If you don't know the IP address of your Solakon ONE inverter, or prefer to use a stable
mDNS hostname instead, use the `discover-mdns` subcommand to find available services:

```bash
solakon-one-fritz-powerregulator discover-mdns
```

Output example:
```
Name                           Hostname                        Address           Port
----------------------------------------------------------------------------------------------------
SolakonONE-ABC123              solakon-abc123.local            192.168.1.100     502
```

You can then use the discovered hostname with `--solakon-host` (e.g., `--solakon-host solakon-abc123.local`)
in place of a static IP address.

### 1. Find your FRITZ!Box device AIN

```bash
solakon-one-fritz-powerregulator \
  --fritz-host fritz.box \
  --fritz-username admin \
  --fritz-password secret \
  list-fritz-devices
```

Output example:
```
AIN             Name                    Product                 Type            Power   Status
------------------------------------------------------------------------------------------------------------
087610123456    Grid meter              FRITZ!Smart Energy 250  energyMeter     1240 W  online
```

> **Note:** By default only devices matching `FRITZ!Smart Energy 250` are listed (the
> `--fritz-filter-product` filter).  Pass `--fritz-filter-product ''` to see all devices.

### 2. Verify the device is suitable for the control loop

```bash
solakon-one-fritz-powerregulator \
  --fritz-host fritz.box \
  --fritz-username admin \
  --fritz-password secret \
  --fritz-ain 087610123456 \
  check-fritz-device
```

### 3. Test a single dry-run cycle

```bash
solakon-one-fritz-powerregulator \
  --solakon-host 192.168.1.148 \
  --fritz-host fritz.box \
  --fritz-username admin \
  --fritz-password secret \
  --fritz-ain 087610123456 \
  --dry-run \
  -vvv \
  run-loop --interval 0
```

### 4. Run continuously every 30 seconds

```bash
solakon-one-fritz-powerregulator \
  --config /etc/solakon-one-fritz-powerregulator/solakon-one-fritz-powerregulator.conf \
  run-loop --interval 30
```

### 5. Run as a systemd service

```bash
# Edit the config file first
sudoedit /etc/solakon-one-fritz-powerregulator/solakon-one-fritz-powerregulator.conf

systemctl enable --now solakon-one-fritz-powerregulator
journalctl -fu solakon-one-fritz-powerregulator
```

## Subcommands

| Subcommand | Description |
|---|---|
| `run-loop` | Main control cycle: read solakon_grid_power_w and grid_meter_power_w, write solakon_grid_power_w+grid_meter_power_w setpoint, repeat on interval |
| `list-fritz-devices` | List all FRITZ!Box Smart Home devices (AIN, name, power, status) |
| `read-fritz-device` | Print current power of one device identified by `--fritz-ain` |
| `check-fritz-device` | Verify a device is suitable (energy meter present, online) |
| `discover-mdns` | Discover mDNS services on the local network, including Solakon ONE inverters |
| `read-solakon` | Print all Solakon ONE values used by `run-loop`: grid active power, PV power, battery power |
| `write-solakon` | Write an explicit power setpoint directly to the Solakon ONE |

Run `solakon-one-fritz-powerregulator --help` or
`solakon-one-fritz-powerregulator <subcommand> --help` for full option reference.

## Key options

| Flag | Default | Description |
|---|---|---|
| `-H, --solakon-host` | `192.168.1.1` | Solakon ONE hostname, IP, or mDNS name (e.g., `solakon-abc123.local`). Use `discover-mdns` to find available names. |
| `-P, --solakon-port` | `502` | Modbus TCP port |
| `-f, --fritz-host` | `fritz.box` | FRITZ!Box hostname or IP |
| `-u, --fritz-username` | — | FRITZ!Box username |
| `-p, --fritz-password` | — | FRITZ!Box password |
| `-a, --fritz-ain` | — | AIN of the FRITZ!Box device |
| `--fritz-filter-product` | `FRITZ!Smart Energy 250` | Restrict devices to those whose product name contains this substring; repeat to allow multiple products; omit entirely to disable the filter |
| `-i, --interval` | `60` | Poll interval in seconds (0 = run once) |
| `-m, --max-power` | `800` | Maximum export power cap in watts |
| `--smoothing` | `1.0` | EMA factor for anti-oscillation, time-scaled per `--interval` (0.0–1.0; see below) |
| `--max-ramp-w-per-s` | `0` | Max rate of change (W/s) for the WRITTEN setpoint; `0` = disabled (see below) |
| `--min-change` | `1` | Output-side dead band; input-side Δsolakon_grid_power_w fallback guard (used only if `--fritz-min-change` is 0) |
| `--fritz-min-change` | `3` | Input-side Δgrid_meter_power_w dead band; when enabled, grid_meter_power_w alone decides whether to skip a write, regardless of solakon_grid_power_w |
| `--fritz-stuck-cycles` | `3` | Jitter the setpoint ±1/±2 W for N-1 byte-equal grid_meter_power_w readings, then fully skip writes on the Nth |
| `--fritz-baseline-w` | `0` | Target value (W) grid_meter_power_w should be held at instead of 0; setpoint = solakon_grid_power_w + (grid_meter_power_w - baseline) |
| `--release-threshold` | `0` | Optional solakon_grid_power_w + grid_meter_power_w safety-net release condition (W); 0 = disabled, SoC-driven release only |
| `--release-soc-hysteresis` | `2` | Percentage points below MAX_SOC at which release becomes eligible |
| `--release-debounce-cycles` | `2` | Consecutive cycles grid_meter_power_w > 0 must hold after a release before control is regained (recover side only; release itself is immediate) |
| `--battery-dead-band` | `20` | Threshold (W) above which the battery counts as "charging"; release is refused while charging |
| `--release-pv-margin` | `50` | Minimum margin (W) PV must exceed current export (solakon_grid_power_w) by before a release is attempted |
| `--recover-remote-on-start` | on | Refuse release until we engaged remote control ourselves |
| `--settle-delay-ms` | `2000` | Sleep after each write or release so the inverter settles before next read |
| `--min-control-soc` | `10` | Minimum battery SoC (%) required to hold/take control; hard cutoff, 0 = disabled |
| `--min-control-soc-recover` | `15` | Absolute SoC (%) required to resume control after a `--min-control-soc` release |
| `--enable-grid-import` | off | When solakon_grid_power_w + grid_meter_power_w would be negative, actively import the shortfall from the grid instead of clamping the setpoint to 0 |
| `--api-enabled` | off | Enable the optional HTTP REST API (see [REST API](#rest-api) below) |
| `--api-host` | `127.0.0.1` | Address the REST API listener binds to |
| `--api-port` | `8080` | TCP port for the REST API listener |
| `--api-key` | — | Shared-secret API key required on every REST API request; required (non-empty) when `--api-enabled` is set |
| `--api-tls-enabled` | off | Serve the REST API over HTTPS instead of plain HTTP |
| `--api-tls-cert-file` | — | PEM certificate (chain) file; leave unset together with `--api-tls-key-file` to auto-generate a self-signed certificate |
| `--api-tls-key-file` | — | PEM private key file matching `--api-tls-cert-file` |
| `-n, --dry-run` | off | Read values but do not write to inverter |
| `-v` | — | Increase log verbosity (repeat up to four times) |
| `-c, --config` | see above | INI config file path |

## Anti-oscillation

The control loop computes `setpoint = solakon_grid_power_w + grid_meter_power_w` where solakon_grid_power_w is the current export power
(read from the inverter) and grid_meter_power_w is the FRITZ!Box reading. The closed loop has two
inherent failure modes:

1. **Single-cycle delay** — the inverter reacts to the previous setpoint and
   changes solakon_grid_power_w *before* the next reading, so naive solakon_grid_power_w+grid_meter_power_w writes can oscillate with
   growing amplitude.
2. **grid_meter_power_w-noise drift** — the FRITZ!Smart Energy 250 polls its internal energy data
   on a ~2-minute interval and rounds its REST output to whole watts. Successive
   reads at our cycle rate (8–60 s) therefore exhibit ±3..±10 W noise even when
   the household load is stable. Each spike pushes solakon_grid_power_w+grid_meter_power_w, the inverter ramps to
   match, and on the next cycle the elevated solakon_grid_power_w plus a noise-reverted grid_meter_power_w produces
   yet another change — a slow upward drift of the setpoint despite a stable
   load.

Five options dampen these effects (`--min-change` and `--fritz-min-change`
interact on the input side — see below — the other three are independent):

**`--smoothing <0.0–1.0>`** (default `1.0` — no smoothing)

Applies an exponential moving average to the setpoint on each *genuine* update
— i.e. one with a real, non-repeated FRITZ!Box reading (a repeated/stale
reading never blends into the EMA at all; see `--fritz-stuck-cycles` below):

```
new_setpoint = alpha × raw + (1 − alpha) × prev_setpoint
```

`1.0` tracks measurements exactly. Lower values slow the response. A value around
`0.5` is a good starting point: it halves the gain on each genuine update's
correction without making the system noticeably sluggish.

`alpha` is *not* simply `--smoothing` itself — it is scaled by the actual
elapsed wall-clock time (`dt`) since the previous genuine update, relative to
`--interval`:

```
alpha = 1 − (1 − smoothing) ^ (dt / interval)
```

`--smoothing` is thus defined as "the fraction of the gap closed by a genuine
update that arrives exactly `--interval` seconds after the previous one" — the
assumption a plain per-call EMA would otherwise silently make. The
FRITZ!Box's own refresh cadence is not fixed — observed anywhere from ~10 s to
~110 s in the field, and varying over time on the same device — so genuine
updates do not actually arrive once per interval; they arrive whenever grid_meter_power_w
happens to change. Scaling by `dt` makes `--smoothing` mean the same thing in
real time no matter how sparse or dense genuine updates happen to be, with no
need to measure or configure "the current refresh rate" anywhere:

- A rare, hard-won sample after a long refresh gap is trusted close to fully
  (`alpha → 1`) instead of only partially applied and left to wait a further
  full refresh interval for the rest.
- A suspiciously quick update is weighted less (`alpha` shrinks proportionally)
  as a guard against noise/transients.

See `--max-ramp-w-per-s` below for a separate, complementary bound on how fast
the *written* setpoint itself may move — `--smoothing` governs how much a
fresh reading is trusted, not how large a single write's step may be.

**`--max-ramp-w-per-s <W/s>`** (default `0` — disabled)

Bounds the maximum rate of change, in watts per second of real elapsed time,
allowed for the *written* setpoint — applied after smoothing/jitter, as a
separate, independent limit. Protects the inverter/battery from an abrupt
commanded swing regardless of how far the underlying `solakon_grid_power_w + grid_meter_power_w` target has
moved — which `--smoothing` alone does not guard against: after a long
refresh gap, `--smoothing`'s time-based `alpha` correctly trusts the fresh
target close to fully (see above), but that says nothing about how fast the
*written* value is allowed to get there.

The elapsed time used is measured since the most recent write of **any**
kind, including keep-alive refreshes that re-send an unchanged setpoint (see
[Remote-control keep-alive](#remote-control-keep-alive-during-idle-periods)
below) — so a long quiet spell, even one filled with keep-alive pings, never
"banks" an oversized jump allowance; the limit stays a tight, predictable
bound on every single write.

Applied to every write while continuously engaged, including jitter writes.
The very first write of a fresh engagement (cold start or post-release
regain) is exempt: there is no previous commanded value yet to ramp from.

A reasonable starting point is a rate comfortably above your normal
second-to-second load swing but well below what a single large step (e.g. a
big appliance switching on) would require in one write — e.g. `20`–`50`.

**`--min-change <W>`** (default `1`)

Output-side dead band. Skips the write when
`|new_setpoint − last_written| < min-change`. The default of `1` suppresses
redundant writes when neither input has moved by even a single watt since the
last cycle. Also filters sub-watt measurement noise.

On the input side, this Δsolakon_grid_power_w threshold is only consulted as a *fallback* when
`--fritz-min-change` is `0` (disabled) — see below.

**`--fritz-min-change <W>`** (default `3`)

Input-side dead band on grid_meter_power_w. Whenever this guard is enabled, **grid_meter_power_w alone** decides
whether the write is redundant: the write is skipped when `|Δgrid_meter_power_w| <
fritz-min-change`, regardless of what solakon_grid_power_w did. solakon_grid_power_w is deliberately *not* consulted
while this guard is active.

This is intentional, not an oversight: solakon_grid_power_w only moves in response to *our own*
previous write (the inverter tracks whatever we last commanded), so "solakon_grid_power_w
unchanged" is never independent evidence — it is trivially true on every cycle
following a skip, since nothing is driving solakon_grid_power_w to move. Letting solakon_grid_power_w's own quiet
state also veto a write (i.e. skipping whenever *either* input is quiet) would
latch solakon_grid_power_w "unchanged" forever after the very first skipped cycle, permanently
freezing the loop regardless of what grid_meter_power_w does afterwards.

grid_meter_power_w-only skipping also prevents a compounding failure: if grid_meter_power_w is quiet — including
"stuck" on a stale FRITZ!Box cache value not yet caught by
`--fritz-stuck-cycles` — while solakon_grid_power_w has just moved to catch up with a *previous*
write that already contains that same stale/noisy grid_meter_power_w, writing again would
re-add the same grid_meter_power_w on top of a solakon_grid_power_w that already absorbed it, creeping the
setpoint (and battery charge/discharge power) upward every interval despite no
real change in load.

This is the primary defence against grid_meter_power_w-noise drift. The default of `3 W` is above
typical mW-rounding noise but well below a single meaningful household appliance
(a 5 W LED still triggers the loop). Set to `0` to disable this guard;
`--min-change`'s Δsolakon_grid_power_w guard then becomes the sole (fallback) input-side check.
Disabling both means the input-side check never skips a write.

**`--fritz-stuck-cycles <N>`** (default `3`)

Stale-data detection. A real-world household load fluctuates by at least ±1 W
from second to second, so when the FRITZ device returns the *exact* same byte-
equal grid_meter_power_w value for several cycles in a row, its internal cache has almost
certainly not refreshed. Rather than going silent the instant grid_meter_power_w repeats,
cycles *before* `N` is reached still write — but with a small alternating
`±1`/`±2` W jitter added to the setpoint instead of the unchanged `solakon_grid_power_w + grid_meter_power_w`
value, keeping the loop visibly alive without pretending the stale reading
justifies a real setpoint change. Only once the count of consecutive
identical readings *reaches* `N` does the tool log a warning and suppress
writes entirely until grid_meter_power_w changes; the counter resets on the first changed
reading either way. Set to `0` to disable stuck-data detection (and its
jitter) entirely.

**`--fritz-baseline-w <W>`** (default `0`)

The target value grid_meter_power_w (the FRITZ!Box grid-power reading) should be held at, instead of
`0`. The tool computes and holds `setpoint = solakon_grid_power_w + (grid_meter_power_w - baseline)`, steering the
household towards importing/exporting `--fritz-baseline-w` W at the FRITZ!Box meter
rather than towards net zero. A positive value keeps a small standing import (e.g. to
leave headroom for another circuit not visible to the Solakon ONE); a negative value
keeps a small standing export. This baseline shift applies uniformly wherever else grid_meter_power_w
is used as a control decision — the release-eligibility export-growing check and the
post-release recover decision both treat the baseline as the new zero for grid_meter_power_w.
`--fritz-min-change` and `--fritz-stuck-cycles` still operate on the *raw* grid_meter_power_w reading,
independent of the baseline, since they guard against noise/staleness in the
measurement itself rather than deciding where the target sits.

You can use any combination of the five options. `--smoothing`,
`--max-ramp-w-per-s`, and `--fritz-stuck-cycles` are fully independent of the
other options. `--min-change` and `--fritz-min-change` interact on the input
side (grid_meter_power_w decides alone when its guard is enabled; solakon_grid_power_w is only a fallback when
`--fritz-min-change 0` disables it) but each can still be tuned or disabled on
its own — see above.

### Remote-control keep-alive during idle periods

Every `writeRemoteControl` call refreshes the inverter's own revert timeout
(`REMOTE_TIMEOUT_SET`), the countdown after which it reverts to its
configured work mode if no new setpoint arrives. The anti-oscillation guards
above deliberately skip re-writing an unchanged setpoint for many consecutive
cycles when grid_meter_power_w is stuck, stale, or within a dead band — but if a skip streak
outlasts `interval + loop-timeout-extra` seconds without any Modbus write at
all, the inverter would otherwise silently revert to free-run mid-streak
while the tool's own state still believes it is in control. To prevent this,
every skip path re-sends the unchanged, currently-active setpoint purely to
refresh the timeout (no ramp, no settle delay needed, since nothing is
actually changing) — control never lapses silently just because nothing
needed to change.

### Automatic recovery from Modbus connection issues

A failed Modbus exchange (a slow/unresponsive inverter, a dropped connection,
or any other transaction-level failure) can leave the TCP connection's byte
stream misaligned, since a response that arrives just after giving up on it
is still sitting in the socket's receive buffer and corrupts the next read.
Rather than requiring a manual restart to recover, the tool automatically
reconnects with a fresh TCP connection and retries the exact same request
once before giving up on that cycle — a plain read is safe to repeat, and a
repeated write just re-sends the same values, which is harmless.

## Remote-control release

See [How it works — Remote-control release](#remote-control-release) above for the full
decision tree. The individual options are:

**`--release-soc-hysteresis <%>`** (default `2`)

Percentage points below `MAX_SOC` at which release becomes eligible. Example: `MAX_SOC = 95 %`,
hysteresis `2` → release allowed when `SoC ≥ 93 %`. Set to `0` to require SoC to reach the
full `MAX_SOC` limit before release.

**`--release-debounce-cycles <N>`** (default `2`)

**Not** a pre-release debounce — release fires the instant all release-eligibility
conditions hold on a single cycle; that release itself is the behavioural test. This option
instead governs the opposite direction: once released, remote control is regained when the
FRITZ!Box reading `grid_meter_power_w` is positive (importing from the grid) for this many **consecutive**
cycles. Any cycle where `grid_meter_power_w ≤ 0` resets the counter to `0`, so remote control stays released.
Set to `0` or `1` to regain control on the first cycle `grid_meter_power_w` is positive (no debounce).

**`--battery-dead-band <W>`** (default `20`)

Threshold above which the battery is considered actively **charging**; release is refused
while charging, since a charging battery can still absorb whatever PV surplus the setpoint
is currently clipping. Readings at or below this threshold — idling **or discharging** —
count as "not charging" and do not block release; discharging is the expected state right
after the battery reaches `MAX_SOC`. Absorbs LiFePO4 trickle/float reads of `+5..+20 W` that
would otherwise read as "charging" indefinitely. Set to `0` to require the strict
`battery_w ≤ 0` behaviour.

**`--release-pv-margin <W>`** (default `50`)

Minimum margin by which `TOTAL_PV_POWER` must exceed the current `ACTIVE_POWER` (`solakon_grid_power_w`) before
a release is even attempted. While engaged, `solakon_grid_power_w` is approximately whatever the tool last
commanded the inverter to export; releasing can only ever increase the export up to what PV
can supply once the battery stops absorbing more, so unless PV is comfortably above what is
already being exported, there is no PV surplus for a release to unlock. A larger margin
requires more headroom before a release is tested; a negative value relaxes (or,
sufficiently negative, effectively disables) the guard.

This check is bypassed once `SoC` reaches `MAX_SOC` itself — see "Why PV headroom over the
current export" above: some inverters curtail PV harvest to match whatever is currently
being commanded once the battery is genuinely full, which would otherwise permanently veto a
release once that happens regardless of this margin's value.

**`--recover-remote-on-start`** (default on)

On startup, read `REMOTE_CONTROL` and refuse to release until the tool has engaged remote
control itself. Prevents the tool from silently cancelling a FoxESS-app strategy period
that was already active when it started. Disable with `--no-recover-remote-on-start` only
for diagnostic runs.

**`--settle-delay-ms <ms>`** (default `2000`)

Milliseconds to sleep after a successful `writeRemoteControl` **or** `releaseRemoteControl`
before the next cycle's reads. Lets the inverter settle into its new state — either ramping
to a fresh setpoint, or free-running after a release — before solakon_grid_power_w/grid_meter_power_w are measured again. This
matters for release too: the very next `grid_meter_power_w` reading feeds the regain-control decision above,
so it must reflect the settled state rather than a mid-transition transient. Set to `0` to
disable.

**`--release-threshold <W>`** (default `0`)

Optional solakon_grid_power_w + grid_meter_power_w safety-net release condition. When `0` (the default) the solakon_grid_power_w + grid_meter_power_w condition is
disabled and release eligibility is decided purely by SoC, battery-charging state, and
inverter health. Set to a positive value to add `solakon_grid_power_w + grid_meter_power_w ≤ this` as an additional requirement
on top of the SoC trigger.

**`--min-control-soc <%>`** (default `10`)

Minimum battery SoC required to hold or take remote control — see
[Minimum-SoC control cutoff](#minimum-soc-control-cutoff) above for the full rationale. An
independent hard cutoff, not part of the release-when-full bundle above: the instant
`BMS1_SOC` drops below this value, remote control is released, gated only by SoC and session
ownership. Also applies at cold start: if SoC already reads below this value on the very
first cycle, the tool withholds control instead of engaging unconditionally. Enabled by
default as a conservative safety floor; set to `0` to disable the cutoff entirely (SoC can
never be negative, so it then never triggers).

**`--min-control-soc-recover <%>`** (default `15`)

Absolute SoC that `BMS1_SOC` must rise back up to before control resumes after a
`--min-control-soc` release. A second, independently configured absolute value — not an
offset — so the effective hysteresis band is
`--min-control-soc-recover` minus `--min-control-soc`. Recovery is purely SoC-driven and
immediate, with no debounce cycles and no dependency on `grid_meter_power_w`. Must be `>=` `--min-control-soc`;
a value below that — including the `15` % default, if `--min-control-soc` is raised above it —
is clamped up at startup with a warning logged, giving a zero-width band.

**`--enable-grid-import`** (default: disabled)

When the computed `solakon_grid_power_w + grid_meter_power_w` setpoint would be negative — i.e. the inverter would need to import
from the grid to cover the load — the default behaviour clamps the setpoint to `0` and lets
the inverter fall back to whatever it can supply from PV/battery alone, potentially leaving
part of the load uncovered by remote control. Enabling this option instead actively commands
the inverter to **import** the shortfall from the grid, using the same remote-control
mechanism (`REMOTE_CONTROL` direction bit set to consume/absorb) that pushes the export
setpoint up when there is a surplus. The import magnitude is bounded by `--max-power`, the
same limit used for export setpoints, and all of the anti-oscillation guards above
(`--smoothing`, `--max-ramp-w-per-s`, `--min-change`, `--fritz-min-change` /
`--fritz-stuck-cycles`) apply identically to import commands — they operate on the signed
setpoint, which simply extends down to `-max-power` instead of being floored at `0` when this
option is enabled.

## REST API

An optional HTTP REST API can be enabled with `--api-enabled` to read all current control-loop
parameters and to set a manual grid import/export override. It runs in-process alongside
`run-loop` (it is not a separate daemon) and shares the same Solakon ONE connection and
control-loop state.

Disabled by default. When enabled, `--api-key` **must** be set to a non-empty shared secret --
the tool refuses to start the API otherwise rather than exposing it with no authentication.

| Flag | Default | Description |
|---|---|---|
| `--api-enabled` | off | Master switch for the REST API |
| `--api-host` | `127.0.0.1` | Listener bind address; use `0.0.0.0` to expose it on the LAN |
| `--api-port` | `8080` | Listener TCP port |
| `--api-key` | — | Shared-secret API key; required (non-empty) when `--api-enabled` is set |
| `--api-tls-enabled` | off | Serve the API over HTTPS instead of plain HTTP |
| `--api-tls-cert-file` | — | PEM certificate (chain) file (see [TLS](#tls) below) |
| `--api-tls-key-file` | — | PEM private key file matching `--api-tls-cert-file` |

### TLS

Set `--api-tls-enabled` to serve the REST API over HTTPS instead of plain HTTP. Two modes are
supported:

- **External certificate** -- set both `--api-tls-cert-file` and `--api-tls-key-file` to a PEM
  certificate (chain) and matching private key, e.g. one issued by an internal CA, a public CA,
  or obtained via a reverse proxy's ACME client. Recommended for anything beyond local/LAN use.
- **Self-signed (default when TLS is enabled but no files are given)** -- if both
  `--api-tls-cert-file` and `--api-tls-key-file` are left empty, the tool generates an RSA-2048,
  SHA-256 self-signed certificate **in memory** at startup (never written to disk). A new
  certificate is generated on every process restart. Convenient for local/LAN use without
  provisioning real certificates, but clients must either disable certificate verification or
  explicitly trust/pin the generated certificate -- there is no CA behind it, so this mode is
  **not suitable for a public-facing deployment**.

`--api-tls-cert-file` and `--api-tls-key-file` must be set together, or both left empty; setting
only one is a configuration error and the tool refuses to start the API.

```bash
# Self-signed (quick start, local/LAN use)
solakon-one-fritz-powerregulator ... run-loop --api-enabled --api-key <key> --api-tls-enabled

# External certificate
solakon-one-fritz-powerregulator ... run-loop --api-enabled --api-key <key> \
  --api-tls-enabled --api-tls-cert-file /etc/ssl/certs/api.crt \
  --api-tls-key-file /etc/ssl/private/api.key

# Talking to a self-signed instance with curl (skip certificate verification)
curl -k -H "X-API-Key: <key>" https://127.0.0.1:8080/api/v1/status
```

### Endpoints

Every endpoint except `/api/v1/health` requires the configured API key, presented via
**either** header:

```
Authorization: Bearer <api-key>
X-API-Key: <api-key>
```

A missing or incorrect key returns `401 Unauthorized` with a JSON error body.

| Method | Path | Description |
|---|---|---|
| `GET` | `/api/v1/health` | Unauthenticated liveness probe: `{"status":"ok"}` |
| `GET` | `/api/v1/status` | Current Solakon ONE / FRITZ!Box readings and control-loop state |
| `GET` | `/api/v1/override` | Current manual override state |
| `POST` | `/api/v1/override` | Set a manual override (see below) |
| `DELETE` | `/api/v1/override` | Clear the manual override, resuming normal `solakon_grid_power_w + grid_meter_power_w` control |

`GET /api/v1/status` returns a JSON object such as:

```json
{
  "solakon_grid_power_w": 320,
  "pv_power_w": 1500,
  "battery_power_w": -50,
  "battery_soc": 87,
  "max_soc_limit": 95,
  "min_soc_limit": 10,
  "inverter_status": 4,
  "grid_status": 0,
  "grid_meter_power_w": 320,
  "remote_engaged": true,
  "owned_by_us": true,
  "ever_engaged": true,
  "low_soc_hold": false,
  "last_written_w": 320,
  "cycle_count": 42,
  "updated_at": "2026-06-27T14:03:01Z",
  "override": { "active": false, "mode": "setpoint", "watts": 0, "duration_seconds": 0, "set_at": "", "expires_at": null }
}
```

Any field whose underlying Modbus/FRITZ!Box read failed or has not yet been observed is
reported as `null`.

### Manual override

Two mutually exclusive override modes are supported, both set via `POST /api/v1/override` and
both taking **absolute priority** over the entire release/recover state machine while active:
none of the release-eligibility conditions, the low-SoC hard cutoff, or the post-release recover
debounce are consulted -- the operator has taken direct control. Both modes take the same
`duration_seconds` field, which auto-expires the override after that many seconds (`0` = stays
active indefinitely, until explicitly cleared via `DELETE`); for Setpoint mode it is *also* used
as the Modbus revert timeout for each write, so the inverter's own safety-net timeout never lags
behind the override's own auto-expiry.

Both `GET`/`POST /api/v1/override` and the `"override"` object embedded in `GET /api/v1/status`
always include an `expires_at` field derived from `set_at + duration_seconds` -- an absolute
ISO-8601 timestamp when `duration_seconds > 0` (for either mode), or `null` when `0`
(indefinite) or when no override is active. This lets a client show both the absolute expiry
time and the configured timeout duration without having to compute one from the other.

#### Setpoint mode -- command an explicit wattage

```
curl -X POST http://127.0.0.1:8080/api/v1/override \
  -H "X-API-Key: <api-key>" -H "Content-Type: application/json" \
  -d '{"watts": 500, "duration_seconds": 120}'
```

`watts` is signed: a positive value commands the inverter to **export** that many watts to the
grid; a negative value commands it to **import** that many watts from the grid (via
`writeRemoteControlImport`) -- this works regardless of `--enable-grid-import`, since an
explicit override is unambiguous operator intent, not an inferred control decision.
`duration_seconds` is optional; when omitted (or `0`), the override stays active indefinitely
(subject only to `DELETE`), and the loop's own `--interval + --loop-timeout-extra` timeout is
used for the underlying Modbus revert register. When set to a positive value, the override
itself automatically expires after that many seconds -- normal `solakon_grid_power_w +
grid_meter_power_w` control resumes on its own, in the very same cycle the expiry is detected.

Every cycle simply (re)writes the requested wattage (clamped to `--max-power`) until the
override expires (per `duration_seconds`) or is cleared via `DELETE`.

#### Release mode -- force-release remote control

```
curl -X POST http://127.0.0.1:8080/api/v1/override \
  -H "X-API-Key: <api-key>" -H "Content-Type: application/json" \
  -d '{"release": true, "duration_seconds": 600}'
```

Forces the Solakon ONE's remote-control session to be released -- as if the release-eligibility
state machine itself had decided to release, but triggered directly by the operator instead
(e.g. for maintenance, or to let the inverter free-run on its own configured work mode for a
while).  No setpoint is written while this mode is active.

`duration_seconds` is optional:
- Omitted or `0` -- stays released **indefinitely**, until explicitly cleared via `DELETE
  /api/v1/override`.
- A positive value -- automatically expires after that many seconds, at which point the normal
  release/recover state machine resumes on its own, exactly as it would after any other release
  (i.e. it re-engages according to the usual cold-start / `grid_meter_power_w`-driven
  recover-decision logic, not immediately).

`{"release": true}` and `{"watts": ...}` are mutually exclusive in the same request; combining
them returns `400 Bad Request`.

```
curl -X DELETE http://127.0.0.1:8080/api/v1/override -H "X-API-Key: <api-key>"
```

`DELETE` clears either mode immediately and resumes normal control right away.

## Desktop UI

An optional Qt6 desktop application, `solakon-one-fritz-powerregulator-ui` (in `ui/`), connects
to the REST API described above -- it does **not** talk Modbus/FRITZ!Box directly -- and
provides:

- **Status panel** -- live grid power (A), PV power, battery power, household grid-meter reading
  (B), battery SoC, and remote-control/ownership/low-SoC-hold state. All absolute timestamps
  (e.g. "Last update") are shown converted to the desktop's local system timezone, not the raw
  UTC the REST API returns them in.
- **Live charts** -- rolling time-series plots (via Qt Charts) of grid/PV/battery/meter power,
  with a persistent hover tooltip (crosshair + info box, shown for as long as the cursor stays
  over the chart -- not a normal auto-hiding `QToolTip`) reporting the exact values of the
  nearest sample. Up to 24 h of history is retained regardless of the currently displayed
  window, so widening it never comes up empty. Controls (same as
  [`solakon-one-ui`](https://github.com/flunaras/solakon-one-ui)'s chart):
  - **Window** -- a combo box selecting the visible X-axis time span (5 min .. 24 h).
  - **Scroll bar** -- scrolls the visible window back through the retained history; stays
    pinned to live data at the right-hand end unless dragged back.
  - **Lock Y** -- the Y-axis auto-scales to nice round numbers (with the 0 W line always shown)
    to whatever's currently visible; checking this simply freezes the range at its current
    value, so scrolling back to compare an earlier period doesn't also rescale the Y axis.
- **Override panel** -- apply an explicit setpoint or force-release override, or clear the
  active override, via `POST`/`DELETE /api/v1/override`. Shows both the absolute expiry time
  (`expires_at`, converted from the API's UTC timestamp to the desktop's local system timezone --
  same as the Status panel's "Last update" field) and the configured timeout (`duration_seconds`)
  side by side when a duration was set, so neither has to be mentally converted from the other.
  The current state is also shown as a colored status chip: **green** when no override is active,
  **yellow** when one is active
  with a configured duration (`duration_seconds > 0`, either mode), **red** when one is active
  with `duration_seconds == 0` (indefinite, requires `DELETE /api/v1/override` to clear).
- **Dockable panels**, saved and restored across restarts via `QSettings`, in the same style as
  [`solakon-one-ui`](https://github.com/flunaras/solakon-one-ui). Connect.../Disconnect live
  under a **File** menu (also same style as `solakon-one-ui`), and each panel can be shown/hidden
  via a corresponding checkable entry in the **View** menu (also same style as `solakon-one-ui`;
  stays in sync if a panel is closed via its own title-bar instead).
- **Auto-connect at startup** -- the connection dialog has an "Automatically connect to this
  server at startup" option; when enabled, the app connects to that saved host/port immediately
  on launch instead of waiting for `Connect...` to be used manually. An explicit `--host` CLI
  argument always takes priority over this.

**The API key is never stored in a config file or `QSettings`.** It is read from and written to
the desktop's platform secret store (Secret Service/libsecret or KWallet on Linux, Credential
Manager on Windows, Keychain on macOS) via [QtKeychain](https://github.com/frankosterfeld/qtkeychain),
keyed by the connection's host and port. The connection dialog lets you paste in a new key, which
is saved to the secret store on accept; it is loaded back automatically the next time you connect
to the same host/port.

### Building

Manually, as a target alongside the CLI/service binary (off by default so that build never
requires Qt):

```bash
cmake -B build -G Ninja -DBUILD_UI=ON
ninja -C build solakon-one-fritz-powerregulator-ui
```

Requires Qt6 (`Widgets`, `Network`, `Charts`) development packages; QtKeychain is fetched
automatically via `FetchContent`.

#### Docker (recommended for packaging)

`ui/docker/build.sh` builds `ui/` as its own standalone project (independent RPM/DEB from the
CLI/service package), the same way as
[`solakon-one-ui`](https://github.com/flunaras/solakon-one-ui)'s own `docker/build.sh`:

```bash
# openSUSE Tumbleweed x86_64 — Release (produces RPM)
./ui/docker/build.sh --distro opensuse-tumbleweed-x86_64 --build-type Release

# openSUSE Tumbleweed aarch64 — Release (cross-compiled, produces RPM)
./ui/docker/build.sh --distro opensuse-tumbleweed-aarch64 --build-type Release

# Ubuntu 24.04 x86_64 — Release (produces DEB)
./ui/docker/build.sh --distro ubuntu-24.04-x86_64 --build-type Release

# All distros at once (default)
./ui/docker/build.sh

# Debug build (enables qDebug output)
./ui/docker/build.sh --distro opensuse-tumbleweed-x86_64 --build-type Debug
```

The aarch64 target cross-compiles (no QEMU/binfmt_misc required) using the same
`cross-aarch64-gcc14` toolchain as the CLI/service build's own aarch64 target, extended with a
much larger aarch64 sysroot (Qt6, libsecret, glib2, and their transitive dependencies) assembled
by downloading each required `.rpm` directly from the openSUSE ports mirror and extracting only
its file contents (`rpm2cpio | cpio`) -- no RPM database, no scriptlets, nothing aarch64 ever
executes on the x86_64 build host. See `ui/docker/Dockerfile.tumbleweed-aarch64` for details.

Build output is placed under `ui/out/`:

```
ui/out/
├── opensuse/tumbleweed/
│   ├── x86_64/
│   │   ├── solakon-one-fritz-powerregulator-ui
│   │   └── solakon-one-fritz-powerregulator-ui-1.0.0-1.x86_64.rpm
│   └── aarch64/
│       ├── solakon-one-fritz-powerregulator-ui
│       └── solakon-one-fritz-powerregulator-ui-1.0.0-1.aarch64.rpm
└── ubuntu/24.04/amd64/
    ├── solakon-one-fritz-powerregulator-ui
    └── solakon-one-fritz-powerregulator-ui_1.0.0-1_amd64.deb
```

Install with `sudo rpm -i ui/out/opensuse/tumbleweed/x86_64/solakon-one-fritz-powerregulator-ui-*.rpm`
or `sudo dpkg -i ui/out/ubuntu/24.04/amd64/solakon-one-fritz-powerregulator-ui_*.deb`.

### Usage

```
solakon-one-fritz-powerregulator-ui [options]

Options:
  -H, --host <host>          REST API host or IP
  -p, --port <port>          REST API port (default: 8080)
      --scheme <scheme>      http (default) or https
      --ignore-ssl-errors    Ignore TLS certificate errors (for the tool's self-signed cert)
  -i, --interval <seconds>   Poll interval in seconds (default: 10)
```

Without `--host`, the connection dialog is shown on startup. Connection settings (except the API
key, which always lives in the secret store) are saved via `QSettings` and pre-filled on
subsequent launches.

```bash
# Open connection dialog on startup
solakon-one-fritz-powerregulator-ui

# Connect directly, skipping the dialog (the API key is still looked up in the secret store)
solakon-one-fritz-powerregulator-ui --host 192.168.1.50 --port 8080 --scheme https --ignore-ssl-errors
```

## Log levels

| Level | Flag | Meaning |
|---|---|---|
| 0 | `--log-level 0` | Silent |
| 1 | (default) | Errors only |
| 2 | `-v` | Warnings + errors |
| 3 | `-vv` | Normal operation messages |
| 4 | `-vvv` | Per-cycle debug detail |
| 5 | `-vvvv` | Full request/response tracing |

All log output goes to **stderr**.

## License

GPL-3.0-or-later — see [LICENSE](LICENSE).
