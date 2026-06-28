#pragma once

#include "apistatus.h"

#include <QDateTime>
#include <QWidget>

class QSpinBox;
class QPushButton;
class QLabel;
class QRadioButton;

// OverrideWidget: operator control panel for POST/DELETE /api/v1/override --
// mirrors the "Remote control" panel of solakon-one-ui, but issues the
// override via this project's REST API instead of writing Modbus registers
// directly (the powerregulator process itself performs the actual Modbus
// write on its next control-loop cycle).
class OverrideWidget : public QWidget {
    Q_OBJECT
public:
    explicit OverrideWidget(QWidget* parent = nullptr);

public slots:
    void updateOverride(const ApiOverride& override);
    void setControlsEnabled(bool enabled); // hides/shows controls based on connection state

    // Reflects the current low_soc_hold/low_soc_hold_since from the latest
    // GET /api/v1/status (or a lowSocHoldApplied response) -- see
    // StatusWidget for the read-only summary shown elsewhere.
    void updateLowSocHold(bool active, const QDateTime& since);

signals:
    // Emitted when the user presses one of the action buttons; MainWindow
    // wires these to ApiClient calls and re-enables the buttons once the
    // request completes (success or failure).
    void applySetpointRequested(int watts, int durationSeconds);
    void applyReleaseRequested(int durationSeconds);
    void clearRequested();

    // Emitted when the user presses the low-SoC hold set/clear button.
    // MainWindow wires this to ApiClient::setLowSocHold().
    void lowSocHoldRequested(bool active);

private:
    QRadioButton* m_setpointRadio;
    QRadioButton* m_releaseRadio;
    QSpinBox*     m_wattsSpin;
    QSpinBox*     m_durationSpin;
    QPushButton*  m_applyButton;
    QPushButton*  m_clearButton;
    QLabel*       m_currentStateLabel;

    QPushButton*  m_lowSocHoldButton;
    QLabel*       m_lowSocHoldStateLabel;
    bool          m_lowSocHoldActive = false;

    void onApplyClicked();

    // Colors m_currentStateLabel as a status "chip": green when no override
    // is active, yellow when one is active with a configured duration
    // (duration_seconds > 0, either mode), red when one is active with
    // duration_seconds == 0 (indefinite -- requires DELETE
    // /api/v1/override to clear either way, but 0 means no automatic
    // timeout/expiry was requested).
    void updateStateLabelColor(bool active, bool autoExpires);
};
