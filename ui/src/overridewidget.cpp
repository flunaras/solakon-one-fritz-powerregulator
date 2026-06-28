#include "overridewidget.h"

#include <QFormLayout>
#include <QGridLayout>
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

    auto* lowSocGroup = new QGroupBox(tr("Low-SoC mode (--min-control-soc)"), this);
    auto* lowSocLayout = new QVBoxLayout(lowSocGroup);
    lowSocLayout->addWidget(m_lowSocHoldStateLabel);
    lowSocLayout->addWidget(m_lowSocHoldButton);

    // Limits: one row per control profile (normal / low-SoC), each with its
    // own max power (non-negative, see POST /api/v1/limits) and FRITZ!Box
    // baseline (signed).
    m_limitsStateLabel = new QLabel(this);
    m_limitsStateLabel->setWordWrap(true);

    auto* limitsGrid = new QGridLayout;
    limitsGrid->addWidget(new QLabel(tr("Max power"), this), 0, 1);
    limitsGrid->addWidget(new QLabel(tr("FRITZ! baseline"), this), 0, 2);
    const QString profileNames[2] = {tr("Normal"), tr("Low SoC")};
    for (int i = 0; i < 2; ++i) {
        m_profileLabel[i] = new QLabel(profileNames[i], this);
        m_maxPowerSpin[i] = new QSpinBox(this);
        m_maxPowerSpin[i]->setRange(0, 20000);
        m_maxPowerSpin[i]->setSuffix(tr(" W"));
        m_baselineSpin[i] = new QSpinBox(this);
        m_baselineSpin[i]->setRange(-100000, 100000);
        m_baselineSpin[i]->setSuffix(tr(" W"));
        m_limitsButton[i] = new QPushButton(tr("Set"), this);

        limitsGrid->addWidget(m_profileLabel[i], i + 1, 0);
        limitsGrid->addWidget(m_maxPowerSpin[i], i + 1, 1);
        limitsGrid->addWidget(m_baselineSpin[i], i + 1, 2);
        limitsGrid->addWidget(m_limitsButton[i], i + 1, 3);

        connect(m_limitsButton[i], &QPushButton::clicked, this, [this, i]() {
            emit limitsRequested(i, m_maxPowerSpin[i]->value(), m_baselineSpin[i]->value());
        });
    }

    auto* limitsGroup = new QGroupBox(tr("Limits (--max-power / --fritz-baseline-w)"), this);
    auto* limitsLayout = new QVBoxLayout(limitsGroup);
    limitsLayout->addWidget(m_limitsStateLabel);
    limitsLayout->addLayout(limitsGrid);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(lowSocGroup);
    layout->addWidget(limitsGroup);
    layout->addWidget(group);
    layout->addStretch();

    connect(m_setpointRadio, &QRadioButton::toggled, m_wattsSpin, &QWidget::setEnabled);
    connect(m_applyButton, &QPushButton::clicked, this, &OverrideWidget::onApplyClicked);
    connect(m_clearButton, &QPushButton::clicked, this, &OverrideWidget::clearRequested);
    connect(m_lowSocHoldButton, &QPushButton::clicked, this, [this]() {
        emit lowSocHoldRequested(!m_lowSocHoldActive);
    });

    updateLowSocHold(/*active=*/false, QDateTime(), QString(), false);
    updateActiveProfile(false, QString(), QString());
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
    for (int i = 0; i < 2; ++i) {
        m_maxPowerSpin[i]->setEnabled(enabled);
        m_baselineSpin[i]->setEnabled(enabled);
        m_limitsButton[i]->setEnabled(enabled);
    }
}

void OverrideWidget::updateLowSocHold(bool active, const QDateTime& since,
                                      const QString& socTier, bool manual) {
    m_lowSocHoldActive = active;

    // Red when fully released (hold/cutoff), yellow when control continues
    // with the low-SoC limits, green when normal.
    const bool limited = active && socTier == "low_soc_limited";
    const QString background = !active ? "#c8e6c9" : limited ? "#fff59d" : "#ffcdd2";
    const QString textColor  = !active ? "#1b5e20" : limited ? "#7a5b00" : "#b71c1c";
    m_lowSocHoldStateLabel->setStyleSheet(
        QString("QLabel { background-color: %1; color: %2; padding: 4px; border-radius: 3px; }")
            .arg(background, textColor));

    if (active) {
        QString text = limited ? tr("Low SoC: low-SoC limits active")
                               : tr("Low SoC: remote control withheld");
        if (manual)
            text += tr(" (forced manually)");
        if (since.isValid())
            text += tr(" since %1").arg(since.toLocalTime().toString(Qt::ISODate));
        m_lowSocHoldStateLabel->setText(text);
        m_lowSocHoldButton->setText(tr("Clear Low-SoC Mode"));
    } else {
        m_lowSocHoldStateLabel->setText(tr("Not active -- normal operation"));
        m_lowSocHoldButton->setText(tr("Force Low-SoC Mode"));
    }
}

void OverrideWidget::updateLimits(int profile, int maxPowerW, int fritzBaselineW) {
    if (profile < 0 || profile > 1)
        return;
    // Only reset a spin box to the server-reported value while the user
    // isn't actively editing it, so a status poll arriving mid-edit doesn't
    // clobber what they're about to submit.
    if (!m_maxPowerSpin[profile]->hasFocus())
        m_maxPowerSpin[profile]->setValue(maxPowerW);
    if (!m_baselineSpin[profile]->hasFocus())
        m_baselineSpin[profile]->setValue(fritzBaselineW);
}

void OverrideWidget::updateActiveProfile(bool lowSocActive, const QString& socTier,
                                         const QString& action) {
    for (int i = 0; i < 2; ++i) {
        const bool active = (i == 1) == lowSocActive;
        QFont f = m_profileLabel[i]->font();
        f.setBold(active);
        m_profileLabel[i]->setFont(f);
    }

    QString tierText;
    if (socTier == "low_soc_limited")       tierText = tr("Low SoC -- low-SoC limits active");
    else if (socTier == "low_soc_released") tierText = tr("Low SoC -- remote control released");
    else if (socTier == "cutoff")           tierText = tr("SoC cutoff -- remote control released");
    else if (socTier == "normal")           tierText = tr("Normal limits active");
    else                                    tierText = tr("(unknown)");
    if (action == "release")
        tierText += tr("  (low-SoC action: release -- low-SoC limits unused)");
    m_limitsStateLabel->setText(tierText);
}
