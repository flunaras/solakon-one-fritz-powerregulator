#include "overridewidget.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>

OverrideWidget::OverrideWidget(QWidget* parent) : QWidget(parent) {
    m_setpointRadio = new QRadioButton(tr("Explicit setpoint (watts)"), this);
    m_setpointRadio->setChecked(true);
    m_releaseRadio = new QRadioButton(tr("Force-release remote control"), this);

    m_wattsSpin = new QSpinBox(this);
    // Signed: positive = command export, negative = command import -- see
    // ManualOverride::watts in restapi.h. Range is generous; the tool itself
    // still clamps to --max-power server-side.
    m_wattsSpin->setRange(-20000, 20000);
    m_wattsSpin->setSuffix(tr(" W"));

    m_durationSpin = new QSpinBox(this);
    m_durationSpin->setRange(0, 86400);
    m_durationSpin->setSuffix(tr(" s"));
    m_durationSpin->setSpecialValueText(tr("0 = default/indefinite"));

    m_applyButton = new QPushButton(tr("Apply Override"), this);
    m_clearButton = new QPushButton(tr("Clear Override"), this);
    m_currentStateLabel = new QLabel(tr("(no override active)"), this);
    m_currentStateLabel->setWordWrap(true);
    updateStateLabelColor(/*active=*/false, /*autoExpires=*/false);

    auto* modeLayout = new QVBoxLayout;
    modeLayout->addWidget(m_setpointRadio);
    modeLayout->addWidget(m_releaseRadio);

    auto* form = new QFormLayout;
    form->addRow(tr("Setpoint watts"), m_wattsSpin);
    form->addRow(tr("Duration"), m_durationSpin);

    auto* buttons = new QHBoxLayout;
    buttons->addWidget(m_applyButton);
    buttons->addWidget(m_clearButton);

    auto* group = new QGroupBox(tr("Manual override"), this);
    auto* groupLayout = new QVBoxLayout(group);
    groupLayout->addLayout(modeLayout);
    groupLayout->addLayout(form);
    groupLayout->addLayout(buttons);
    groupLayout->addWidget(m_currentStateLabel);

    m_lowSocHoldButton = new QPushButton(this);
    m_lowSocHoldStateLabel = new QLabel(this);
    m_lowSocHoldStateLabel->setWordWrap(true);

    auto* lowSocGroup = new QGroupBox(tr("Low-SoC hold (--min-control-soc)"), this);
    auto* lowSocLayout = new QVBoxLayout(lowSocGroup);
    lowSocLayout->addWidget(m_lowSocHoldStateLabel);
    lowSocLayout->addWidget(m_lowSocHoldButton);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(lowSocGroup);
    layout->addWidget(group);
    layout->addStretch();

    connect(m_setpointRadio, &QRadioButton::toggled, m_wattsSpin, &QWidget::setEnabled);
    connect(m_applyButton, &QPushButton::clicked, this, &OverrideWidget::onApplyClicked);
    connect(m_clearButton, &QPushButton::clicked, this, &OverrideWidget::clearRequested);
    connect(m_lowSocHoldButton, &QPushButton::clicked, this, [this]() {
        emit lowSocHoldRequested(!m_lowSocHoldActive);
    });

    updateLowSocHold(/*active=*/false, QDateTime());
    setControlsEnabled(false);
}

void OverrideWidget::onApplyClicked() {
    const int duration = m_durationSpin->value();
    if (m_setpointRadio->isChecked())
        emit applySetpointRequested(m_wattsSpin->value(), duration);
    else
        emit applyReleaseRequested(duration);
}

void OverrideWidget::updateOverride(const ApiOverride& ov) {
    if (!ov.active) {
        m_currentStateLabel->setText(tr("(no override active)"));
        updateStateLabelColor(/*active=*/false, /*autoExpires=*/false);
        return;
    }

    // "Timed" simply means a duration was configured (duration_seconds > 0),
    // regardless of mode -- shown as yellow, with both the countdown
    // (duration_seconds) and the absolute expiry time (expires_at) shown so
    // neither has to be mentally converted from the other. duration_seconds
    // == 0 means indefinite (stays active until DELETE /api/v1/override) --
    // shown as red.
    const bool autoExpires = ov.duration_seconds > 0 && ov.expires_at.isValid();

    const QString expirySuffix = autoExpires
        ? tr(" -- expires at %1 (timeout: %2 s)")
              .arg(ov.expires_at.toLocalTime().toString(Qt::ISODate))
              .arg(ov.duration_seconds)
        : tr(" (indefinite, until cleared)");

    if (ov.mode == ApiOverrideMode::Setpoint) {
        m_currentStateLabel->setText(
            tr("Active: commanding %1 W%2").arg(ov.watts).arg(expirySuffix));
    } else {
        m_currentStateLabel->setText(tr("Active: released%1").arg(expirySuffix));
    }
    updateStateLabelColor(/*active=*/true, autoExpires);
}

void OverrideWidget::updateStateLabelColor(bool active, bool autoExpires) {
    // Light "chip" background with dark, always-readable text -- green: no
    // override; yellow: active but will expire on its own; red: active and
    // will NOT expire on its own (requires DELETE /api/v1/override).
    QString background;
    QString textColor;
    if (!active) {
        background = "#c8e6c9"; // light green
        textColor  = "#1b5e20";
    } else if (autoExpires) {
        background = "#fff59d"; // light yellow
        textColor  = "#7a5b00";
    } else {
        background = "#ffcdd2"; // light red
        textColor  = "#b71c1c";
    }
    m_currentStateLabel->setStyleSheet(
        QString("QLabel { background-color: %1; color: %2; padding: 4px; border-radius: 3px; }")
            .arg(background, textColor));
}

void OverrideWidget::setControlsEnabled(bool enabled) {
    m_setpointRadio->setEnabled(enabled);
    m_releaseRadio->setEnabled(enabled);
    m_wattsSpin->setEnabled(enabled && m_setpointRadio->isChecked());
    m_durationSpin->setEnabled(enabled);
    m_applyButton->setEnabled(enabled);
    m_clearButton->setEnabled(enabled);
    m_lowSocHoldButton->setEnabled(enabled);
}

void OverrideWidget::updateLowSocHold(bool active, const QDateTime& since) {
    m_lowSocHoldActive = active;

    // Same green/red chip convention as m_currentStateLabel above: green
    // when not held (normal operation), red when held (a hard safety
    // cutoff -- there is no "auto-expiring" middle state here, unlike the
    // setpoint/release override, since recovery is SoC-driven and open-
    // ended, not on a timer).
    const QString background = active ? "#ffcdd2" : "#c8e6c9";
    const QString textColor  = active ? "#b71c1c" : "#1b5e20";
    m_lowSocHoldStateLabel->setStyleSheet(
        QString("QLabel { background-color: %1; color: %2; padding: 4px; border-radius: 3px; }")
            .arg(background, textColor));

    if (active) {
        m_lowSocHoldStateLabel->setText(since.isValid()
            ? tr("Held: remote control withheld since %1").arg(since.toLocalTime().toString(Qt::ISODate))
            : tr("Held: remote control withheld"));
        m_lowSocHoldButton->setText(tr("Clear Low-SoC Hold"));
    } else {
        m_lowSocHoldStateLabel->setText(tr("Not held -- remote control governed normally"));
        m_lowSocHoldButton->setText(tr("Force Low-SoC Hold"));
    }
}
