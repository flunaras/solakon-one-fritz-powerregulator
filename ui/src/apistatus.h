#pragma once

#include <QDateTime>
#include <QMetaType>
#include <QString>

// ApiStatus: Qt-side mirror of the JSON produced by snapshotToJson()/
// overrideToJson() in src/restapi.cpp (GET /api/v1/status). Optional/nullable
// fields on the wire (JSON null when the corresponding Modbus/FRITZ read
// failed or was never taken) are represented here as a value plus a separate
// "_ok"/"_valid" bool, mirroring ApiSnapshot's own has-flags -- never silently
// defaulted to 0, so the UI can distinguish "0 W" from "unknown".
struct ApiStatus {
    bool have_data = false;
    QDateTime updated_at;
    qint64 cycle_count = 0;

    int  solakon_grid_power_w = 0;
    bool solakon_grid_power_ok = false;

    int  pv_power_w = 0;
    bool pv_power_ok = false;

    int  battery_power_w = 0;
    bool battery_power_ok = false;

    int  battery_soc = -1;      // -1 == unknown
    int  max_soc_limit = -1;
    int  min_soc_limit = -1;

    quint16 inverter_status = 0;
    bool    inverter_status_ok = false;
    quint16 grid_status = 0;
    bool    grid_status_ok = false;

    int  grid_meter_power_w = 0;
    bool grid_meter_power_ok = false;

    bool remote_engaged = false;
    bool owned_by_us = false;
    bool ever_engaged = false;
    bool low_soc_hold = false;
    QDateTime low_soc_hold_since;  // invalid == never toggled this run

    int  last_written_w = 0;
    bool has_last_written = false;

    int  max_power_w = 0;  // normal-profile --max-power ceiling, watts

    // Control profiles (index: 0 = normal, 1 = low-SoC limit tier) -- see
    // GET /api/v1/limits / --min-control-soc-action.
    struct Limits { int max_power_w = 0; int fritz_baseline_w = 0; };
    Limits limits[2];
    bool    low_soc_limited = false;           // low-SoC profile currently in use
    bool    low_soc_active = false;            // any low-SoC state (hold or limit tier)
    bool    low_soc_manual = false;            // forced via the API (sticky)
    QString soc_tier;                          // normal | low_soc_limited | low_soc_released | cutoff
    QString min_control_soc_action;            // release | limit
    int     effective_max_power_w = 0;
    int     effective_fritz_baseline_w = 0;
};

enum class ApiOverrideMode { None, Setpoint, Release };

// ApiOverride: mirror of overrideToJson()'s output, returned both from
// GET/POST/DELETE /api/v1/override and embedded under "override" in
// GET /api/v1/status.
struct ApiOverride {
    bool            active = false;
    ApiOverrideMode mode   = ApiOverrideMode::None;
    QDateTime       set_at;
    int             duration_seconds = 0;
    int             watts = 0;               // Setpoint mode only
    QDateTime       expires_at;               // valid whenever duration_seconds > 0; invalid == indefinite
};

Q_DECLARE_METATYPE(ApiStatus)
Q_DECLARE_METATYPE(ApiOverride)
