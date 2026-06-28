#pragma once

#include "apistatus.h"

#include <QWidget>

class QLabel;

// StatusWidget: read-only "instrument panel" showing the latest ApiStatus --
// grid/PV/battery power, SoC, remote-control ownership/engagement state, and
// low-SoC hold -- mirroring the register groups shown by solakon-one-ui's
// dashboard, but sourced from the powerregulator's REST snapshot instead of
// Modbus registers directly.
class StatusWidget : public QWidget {
    Q_OBJECT
public:
    explicit StatusWidget(QWidget* parent = nullptr);

public slots:
    void updateStatus(const ApiStatus& status);
    void setDisconnected();

private:
    QLabel* m_gridPowerValue;
    QLabel* m_pvPowerValue;
    QLabel* m_batteryPowerValue;
    QLabel* m_gridMeterValue;
    QLabel* m_socValue;
    QLabel* m_remoteStateValue;
    QLabel* m_lastWrittenValue;
    QLabel* m_updatedAtValue;

    static QString formatOptionalWatts(int value, bool ok);
};
