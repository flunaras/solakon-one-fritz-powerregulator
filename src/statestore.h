#pragma once

#include <string>

// StateStore: minimal on-disk persistence for control-loop state that must
// survive a service restart -- currently just the low-SoC hold flag (see
// Config::min_control_soc / min_control_soc_recover). Deliberately a tiny,
// dedicated JSON file (not the INI config file, which is user-edited and not
// meant to be rewritten by the process itself) so a restart does not forget
// that the battery was too low to hold remote control moments before the
// process was stopped -- without this, a service restart (e.g. a package
// upgrade, a crash-and-restart, or a reboot) would silently resume forcing
// export/discharge on a battery that is still below --min-control-soc.
struct PersistedState {
    // low_soc_hold: whether remote control was being withheld/held-released
    // due to the --min-control-soc cutoff at the time the state was last
    // saved. See Controller state `low_soc_hold` in main.cpp.
    bool low_soc_hold = false;
};

// loadState: reads and parses `path`. Returns false (leaving *out*
// untouched) if the file does not exist, cannot be read, or is not valid
// JSON matching the expected shape -- this is not treated as a hard error by
// callers; a missing/corrupt state file simply means "nothing persisted
// yet", falling back to the tool's normal cold-start behaviour.
[[nodiscard]] bool loadState(const std::string& path, PersistedState& out);

// saveState: serializes `state` to `path`, creating the parent directory
// (recursively, best-effort) if it does not already exist. Writes to a
// temporary file in the same directory and renames it into place, so a
// crash or power loss mid-write cannot leave a truncated/corrupt state file
// behind. Returns false (and leaves any previous file untouched) on any
// failure (permission denied, disk full, etc.) -- callers treat this as a
// best-effort, non-fatal operation and merely log a warning.
[[nodiscard]] bool saveState(const std::string& path, const PersistedState& state);
