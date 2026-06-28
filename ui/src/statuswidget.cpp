#include "statuswidget.h"

#include <QFontMetrics>
#include <QFormLayout>
#include <QLabel>

namespace {
// kRemoteStateMaxWidth: matches m_remoteStateValue's fixed maximum width
// below -- kept as a named constant since both the label's own size cap and
// the elision width used when truncating its text must stay in sync.
constexpr int kRemoteStateMaxWidth = 260;
}

QString StatusWidget::formatOptionalWatts(int value, bool ok) {
    return ok ? QString("%1 W").arg(value) : QObject::tr("(unknown)");
}

StatusWidget::StatusWidget(QWidget* parent) : QWidget(parent) {
    m_gridPowerValue   = new QLabel(this);
    m_pvPowerValue     = new QLabel(this);
    m_batteryPowerValue = new QLabel(this);
    m_gridMeterValue   = new QLabel(this);
    m_socValue         = new QLabel(this);
    m_remoteStateValue = new QLabel(this);
    m_lastWrittenValue = new QLabel(this);
    m_updatedAtValue   = new QLabel(this);

    // m_remoteStateValue's text length varies quite a bit depending on which
    // state bits are currently set (e.g. the low-SoC hold's "since ..."
    // timestamp is appended/removed as it toggles). A plain QLabel's
    // sizeHint grows with its text, and QFormLayout would widen the whole
    // widget/window to fit it every time the text changes.
    //
    // Word-wrapping was tried here first, but QFormLayout has a long-
    // standing limitation where it does not recompute a row's HEIGHT when a
    // wrapped label's available WIDTH changes (e.g. as the dock is resized),
    // so the row stayed a fixed single-line height while the label wrapped
    // to two lines underneath it -- overlapping the next row instead of
    // resizing anything.
    //
    // Capping the label's width and eliding overflow to "..." (single line,
    // no wrapping) sidesteps that limitation entirely: sizeHint() never
    // exceeds kRemoteStateMaxWidth, so the widget never grows, and the row
    // height stays exactly one line, matching every other row. The full,
    // untruncated text is always available as a tooltip.
    m_remoteStateValue->setMaximumWidth(kRemoteStateMaxWidth);

    auto* form = new QFormLayout(this);
    form->addRow(tr("Grid power (Solakon, A)"), m_gridPowerValue);
    form->addRow(tr("PV power"), m_pvPowerValue);
    form->addRow(tr("Battery power"), m_batteryPowerValue);
    form->addRow(tr("Household grid meter (FRITZ!Box, B)"), m_gridMeterValue);
    form->addRow(tr("Battery SoC"), m_socValue);
    form->addRow(tr("Remote control"), m_remoteStateValue);
    form->addRow(tr("Last written setpoint"), m_lastWrittenValue);
    form->addRow(tr("Last update"), m_updatedAtValue);

    setDisconnected();
}

void StatusWidget::updateStatus(const ApiStatus& s) {
    m_gridPowerValue->setText(formatOptionalWatts(s.solakon_grid_power_w, s.solakon_grid_power_ok));
    m_pvPowerValue->setText(formatOptionalWatts(s.pv_power_w, s.pv_power_ok));
    m_batteryPowerValue->setText(formatOptionalWatts(s.battery_power_w, s.battery_power_ok));
    m_gridMeterValue->setText(formatOptionalWatts(s.grid_meter_power_w, s.grid_meter_power_ok));

    m_socValue->setText(s.battery_soc >= 0
        ? tr("%1% (max %2%, min %3%)")
              .arg(s.battery_soc)
              .arg(s.max_soc_limit >= 0 ? QString::number(s.max_soc_limit) : "?")
              .arg(s.min_soc_limit >= 0 ? QString::number(s.min_soc_limit) : "?")
        : tr("(unknown)"));

    QStringList remoteBits;
    remoteBits << (s.remote_engaged ? tr("engaged") : tr("released"));
    if (s.remote_engaged)
        remoteBits << (s.owned_by_us ? tr("owned by us") : tr("owned externally"));
    if (s.low_soc_hold)
        remoteBits << tr("low-SoC hold");
    if (!s.ever_engaged)
        remoteBits << tr("never engaged yet");
    const QString remoteText = remoteBits.join(", ");

    // The low-SoC hold's "since <timestamp>" detail is shown ONLY in the
    // tooltip, never in the normal status text -- it is diagnostic detail,
    // not something that needs to be visible at a glance, and keeping it out
    // of remoteText keeps that text short and unlikely to ever need eliding.
    QString remoteTooltip = remoteText;
    if (s.low_soc_hold && s.low_soc_hold_since.isValid()) {
        remoteTooltip += tr("\nLow-SoC hold since: %1")
                             .arg(s.low_soc_hold_since.toLocalTime().toString(Qt::ISODate));
    }
    m_remoteStateValue->setToolTip(remoteTooltip);
    m_remoteStateValue->setText(
        QFontMetrics(m_remoteStateValue->font())
            .elidedText(remoteText, Qt::ElideRight, kRemoteStateMaxWidth));

    m_lastWrittenValue->setText(s.has_last_written
        ? tr("%1 W").arg(s.last_written_w)
        : tr("(none yet)"));

    m_updatedAtValue->setText(s.have_data
        ? s.updated_at.toLocalTime().toString(Qt::ISODate)
        : tr("(no data yet)"));
}

void StatusWidget::setDisconnected() {
    const QString na = tr("--");
    m_gridPowerValue->setText(na);
    m_pvPowerValue->setText(na);
    m_batteryPowerValue->setText(na);
    m_gridMeterValue->setText(na);
    m_socValue->setText(na);
    m_remoteStateValue->setToolTip(QString());
    m_remoteStateValue->setText(tr("not connected"));
    m_lastWrittenValue->setText(na);
    m_updatedAtValue->setText(na);
}
