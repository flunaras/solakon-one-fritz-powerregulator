#include "chartwidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QEvent>
#include <QGraphicsLineItem>
#include <QGraphicsRectItem>
#include <QGraphicsSimpleTextItem>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
// Same QSettings scope as MainWindow (organizationName/applicationName set
// in main.cpp), so QSettings() with no explicit org/app here reads/writes
// the identical settings file -- just under its own "chart/" key prefix.
constexpr char kLockYKey[]  = "chart/lockYScale";
constexpr char kWindowKey[] = "chart/windowIndex";

// X-axis (visible time window) options -- same table as solakon-one-ui's
// PvChartWidget. The last entry (24 h) doubles as the fixed retention
// ceiling (see ChartWidget's class comment / addSample()).
const QVector<int> kWindowSecondsTable = {
    5 * 60,        // 0 -- 5 min
    15 * 60,       // 1 -- 15 min (default)
    30 * 60,       // 2 -- 30 min
    60 * 60,       // 3 -- 1 h
    2 * 60 * 60,   // 4 -- 2 h
    4 * 60 * 60,   // 5 -- 4 h
    8 * 60 * 60,   // 6 -- 8 h
    12 * 60 * 60,  // 7 -- 12 h
    24 * 60 * 60,  // 8 -- 24 h
};

const QStringList kWindowLabels = {
    QStringLiteral("5 min"),
    QStringLiteral("15 min"),
    QStringLiteral("30 min"),
    QStringLiteral("1 h"),
    QStringLiteral("2 h"),
    QStringLiteral("4 h"),
    QStringLiteral("8 h"),
    QStringLiteral("12 h"),
    QStringLiteral("24 h"),
};

int kMaxHistorySeconds() { return kWindowSecondsTable.last(); }

} // namespace

ChartWidget::ChartWidget(QWidget* parent) : QWidget(parent) {
    m_gridSeries = new QLineSeries();
    m_gridSeries->setName(tr("Grid power (A)"));
    m_pvSeries = new QLineSeries();
    m_pvSeries->setName(tr("PV power"));
    m_batterySeries = new QLineSeries();
    m_batterySeries->setName(tr("Battery power"));
    m_meterSeries = new QLineSeries();
    m_meterSeries->setName(tr("Household meter (B)"));

    m_chart = new QChart();
    m_chart->addSeries(m_gridSeries);
    m_chart->addSeries(m_pvSeries);
    m_chart->addSeries(m_batterySeries);
    m_chart->addSeries(m_meterSeries);
    m_chart->setTitle(tr("Power (W)"));
    m_chart->legend()->setVisible(true);

    m_axisX = new QDateTimeAxis();
    m_axisX->setFormat("hh:mm:ss");
    m_axisX->setTitleText(tr("Time"));
    m_chart->addAxis(m_axisX, Qt::AlignBottom);

    m_axisY = new QValueAxis();
    m_axisY->setTitleText(tr("Watts"));
    m_axisY->setLabelFormat("%.0f");
    m_chart->addAxis(m_axisY, Qt::AlignLeft);

    for (auto* series : {m_gridSeries, m_pvSeries, m_batterySeries, m_meterSeries}) {
        series->attachAxis(m_axisX);
        series->attachAxis(m_axisY);
    }

    m_chartView = new QChartView(m_chart, this);
    m_chartView->setRenderHint(QPainter::Antialiasing);
    // Mouse tracking is required so QEvent::MouseMove fires on every
    // movement rather than only while a button is held down.
    m_chartView->setMouseTracking(true);
    m_chartView->viewport()->setMouseTracking(true);
    m_chartView->viewport()->installEventFilter(this);

    // ── Hover crosshair ────────────────────────────────────────────────────
    // Parented to m_chart so its coordinate system matches plotArea() /
    // mapToValue() / mapToPosition() directly (no manual offset needed) --
    // same approach as solakon-one-ui's PvChartWidget.
    m_hoverLine = new QGraphicsLineItem(m_chart);
    QPen hoverPen(QColor(80, 80, 80));
    hoverPen.setStyle(Qt::DotLine);
    m_hoverLine->setPen(hoverPen);
    m_hoverLine->setZValue(100);
    m_hoverLine->hide();

    // ── Hover info box ─────────────────────────────────────────────────────
    // Added directly to the scene (view coordinates) so it can be freely
    // positioned near the cursor. Persistent: only hidden explicitly on
    // QEvent::Leave, unlike QToolTip which auto-hides on a timer.
    m_hoverInfoBg = new QGraphicsRectItem();
    m_hoverInfoBg->setBrush(QColor(255, 255, 225, 235));
    m_hoverInfoBg->setPen(QPen(QColor(120, 120, 120)));
    m_hoverInfoBg->setZValue(101);
    m_hoverInfoBg->hide();
    m_chartView->scene()->addItem(m_hoverInfoBg);

    m_hoverInfoText = new QGraphicsSimpleTextItem(m_hoverInfoBg);
    m_hoverInfoText->setPos(6, 4);
    m_hoverInfoText->setBrush(QColor(20, 20, 20));

    // ── X-scale controls ───────────────────────────────────────────────────
    // Same "Window:" combo box as solakon-one-ui's PvChartWidget: selects
    // how much of the retained history (up to 24 h) is displayed. Changing
    // it never discards data -- see the class comment / addSample().
    m_windowCombo = new QComboBox(this);
    m_windowCombo->addItems(kWindowLabels);
    connect(m_windowCombo, &QComboBox::currentIndexChanged,
            this, &ChartWidget::onWindowComboChanged);

    // Horizontal scroll bar -- same design as solakon-one-ui's PvChartWidget:
    // value = seconds from the first retained sample to the right edge of
    // the visible window. Maximum = total span of recorded history in
    // seconds. Page step = windowSeconds() (one full window width).
    m_scrollBar = new QScrollBar(Qt::Horizontal, this);
    m_scrollBar->setMinimum(0);
    m_scrollBar->setMaximum(0);
    m_scrollBar->setSingleStep(10);
    connect(m_scrollBar, &QScrollBar::valueChanged, this, &ChartWidget::onScrollBarChanged);

    // ── Y-scale control ────────────────────────────────────────────────────
    // "Lock Y" mirrors solakon-one-ui's PvChartWidget control: freezes the
    // axis at its current range instead of auto-rescaling on every sample --
    // most useful together with the scroll bar above, so scrolling back
    // through history doesn't also keep rescaling the Y axis.
    m_lockYScaleCheckBox = new QCheckBox(tr("Lock Y"), this);
    m_lockYScaleCheckBox->setToolTip(tr("Freeze the Y-axis range instead of auto-scaling it"));
    connect(m_lockYScaleCheckBox, &QCheckBox::toggled, this, &ChartWidget::onLockYScaleToggled);

    auto* controlsRow = new QHBoxLayout;
    // Small vertical margin so the row sits vertically centered in the band
    // between the widget's top edge and the chart, rather than flush
    // against the top edge; right margin balances the scroll bar's own
    // inset below so the combo box lines up with the chart's right edge.
    controlsRow->setContentsMargins(8, 6, 8, 6);
    // Right-aligned: push everything to the right edge with a leading
    // stretch, matching the layout in the reference screenshot.
    controlsRow->addStretch();
    controlsRow->addWidget(m_lockYScaleCheckBox, 0, Qt::AlignVCenter);
    controlsRow->addSpacing(16);
    controlsRow->addWidget(new QLabel(tr("Window:"), this), 0, Qt::AlignVCenter);
    controlsRow->addWidget(m_windowCombo, 0, Qt::AlignVCenter);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    // Zero spacing here so the controls row's own top/bottom margins (set
    // above) are the *only* thing separating it from the widget's top edge
    // and from the chart -- otherwise QVBoxLayout's default inter-item
    // spacing would stack on top of the row's bottom margin only, making
    // the gap below the row bigger than the gap above it instead of equal.
    layout->setSpacing(0);
    layout->addLayout(controlsRow);
    layout->addWidget(m_chartView);
    layout->addWidget(m_scrollBar);

    loadWindowState();
    loadYScaleState();
}

ChartWidget::~ChartWidget() {
    saveYScaleState();
    saveWindowState();
}

namespace {

// niceNum: Paul Heckbert's classic "nice numbers for graph labels" algorithm
// -- rounds `range` to the nearest value of the form {1, 2, 5} x 10^n so that
// axis tick spacing always lands on human-friendly round numbers (never
// something like "137 W" between ticks).
double niceNum(double range, bool round) {
    if (range <= 0)
        return 1.0;
    const double exponent = std::floor(std::log10(range));
    const double fraction = range / std::pow(10.0, exponent);
    double niceFraction;
    if (round) {
        if (fraction < 1.5)      niceFraction = 1.0;
        else if (fraction < 3.0) niceFraction = 2.0;
        else if (fraction < 7.0) niceFraction = 5.0;
        else                     niceFraction = 10.0;
    } else {
        if (fraction <= 1.0)      niceFraction = 1.0;
        else if (fraction <= 2.0) niceFraction = 2.0;
        else if (fraction <= 5.0) niceFraction = 5.0;
        else                      niceFraction = 10.0;
    }
    return niceFraction * std::pow(10.0, exponent);
}

} // namespace

int ChartWidget::windowSeconds() const {
    if (m_windowIndex >= 0 && m_windowIndex < kWindowSecondsTable.size())
        return kWindowSecondsTable[m_windowIndex];
    return kWindowSecondsTable[1]; // 15 min fallback
}

void ChartWidget::trimSeries(QLineSeries* series, qint64 cutoffMs) {
    // QLineSeries has no bulk "remove older than" API -- points are always
    // appended in increasing-time order by addSample(), so the oldest stale
    // points are simply a prefix; removing from the front until we hit one
    // still within the retention window is O(removed), not O(total history).
    while (series->count() > 0 && series->at(0).x() < cutoffMs)
        series->remove(0);
}

void ChartWidget::addSample(const ApiStatus& s) {
    if (!s.have_data) return;

    const qint64 x = s.updated_at.isValid()
        ? s.updated_at.toMSecsSinceEpoch()
        : QDateTime::currentMSecsSinceEpoch();

    if (s.solakon_grid_power_ok) m_gridSeries->append(x, s.solakon_grid_power_w);
    if (s.pv_power_ok)           m_pvSeries->append(x, s.pv_power_w);
    if (s.battery_power_ok)      m_batterySeries->append(x, s.battery_power_w);
    if (s.grid_meter_power_ok)   m_meterSeries->append(x, s.grid_meter_power_w);

    Sample sample;
    sample.timestampMs = x;
    sample.gridOk = s.solakon_grid_power_ok;
    sample.gridW = s.solakon_grid_power_w;
    sample.pvOk = s.pv_power_ok;
    sample.pvW = s.pv_power_w;
    sample.batteryOk = s.battery_power_ok;
    sample.batteryW = s.battery_power_w;
    sample.meterOk = s.grid_meter_power_ok;
    sample.meterW = s.grid_meter_power_w;
    m_history.append(sample);

    // Retain up to the longest selectable window (24 h) regardless of what
    // window is *currently* selected/displayed, so widening the window
    // later doesn't come up empty for the time already collected.
    const qint64 cutoff = x - static_cast<qint64>(kMaxHistorySeconds()) * 1000;
    for (auto* series : {m_gridSeries, m_pvSeries, m_batterySeries, m_meterSeries})
        trimSeries(series, cutoff);
    int removeCount = 0;
    while (removeCount < m_history.size() && m_history[removeCount].timestampMs < cutoff)
        ++removeCount;
    if (removeCount > 0)
        m_history.remove(0, removeCount);

    updateScrollBar();
    rescaleAxes();
}

qint64 ChartWidget::computeWindowEndMs() const {
    // At the scrolled-to-the-end position (the common case, "follow live
    // data"), or before any scroll range even exists yet, the window end is
    // simply "now" -- this keeps the view live-updating between samples,
    // not just snapping forward once a new one arrives.
    if (m_scrollAtEnd || m_scrollBar->maximum() == 0 || m_history.isEmpty())
        return QDateTime::currentMSecsSinceEpoch();

    const qint64 dataStartMs = m_history.first().timestampMs;
    return dataStartMs + static_cast<qint64>(m_scrollBar->value()) * 1000;
}

void ChartWidget::rescaleAxes() {
    if (m_history.isEmpty()) return;

    const qint64 endMs = computeWindowEndMs();
    const qint64 startMs = endMs - static_cast<qint64>(windowSeconds()) * 1000;

    m_axisX->setRange(QDateTime::fromMSecsSinceEpoch(startMs), QDateTime::fromMSecsSinceEpoch(endMs));

    // While locked, the Y axis is entirely frozen at whatever range is
    // currently displayed -- skip auto-scaling (and the data scan below,
    // which would otherwise be wasted work) entirely.
    if (m_lockYScaleCheckBox->isChecked())
        return;

    // Seeding min/max with 0 guarantees the "0 W" line is always within
    // range even if every sample so far is e.g. all-positive PV power --
    // the zero baseline is a meaningful reference point (import/export,
    // charge/discharge) that should never scroll out of view.
    qreal minY = 0;
    qreal maxY = 0;

    // Only the samples within the currently visible window [startMs, endMs]
    // should influence the Y range -- m_history itself may hold up to 24 h
    // regardless of what's currently scrolled into view (see addSample()).
    auto rangeBegin = std::lower_bound(m_history.begin(), m_history.end(), startMs,
        [](const Sample& s, qint64 value) { return s.timestampMs < value; });
    auto rangeEnd = std::upper_bound(m_history.begin(), m_history.end(), endMs,
        [](qint64 value, const Sample& s) { return value < s.timestampMs; });
    for (auto it = rangeBegin; it != rangeEnd; ++it) {
        if (it->gridOk)    { minY = std::min(minY, it->gridW);    maxY = std::max(maxY, it->gridW); }
        if (it->pvOk)      { minY = std::min(minY, it->pvW);      maxY = std::max(maxY, it->pvW); }
        if (it->batteryOk) { minY = std::min(minY, it->batteryW); maxY = std::max(maxY, it->batteryW); }
        if (it->meterOk)   { minY = std::min(minY, it->meterW);   maxY = std::max(maxY, it->meterW); }
    }

    // Nice-numbers axis scaling: pick a "round" tick spacing (1/2/5 x 10^n)
    // for the desired approximate tick count, then snap min/max outward to
    // the nearest multiple of that spacing. Because minY <= 0 <= maxY and
    // the snapped bounds are themselves exact multiples of the spacing, 0
    // always falls exactly on a tick rather than merely somewhere inside
    // the range.
    constexpr int kTargetTickCount = 11; // higher tick density than before
    const double rawRange = std::max(maxY - minY, 1.0);
    const double tickSpacing = niceNum(rawRange / (kTargetTickCount - 1), /*round=*/true);

    const qreal niceMin = std::floor(minY / tickSpacing) * tickSpacing;
    qreal niceMax = std::ceil(maxY / tickSpacing) * tickSpacing;
    // Guard against a degenerate zero-width axis (e.g. every sample so far
    // reads exactly 0 W) -- QValueAxis needs min < max to render sensibly.
    if (niceMax <= niceMin)
        niceMax = niceMin + tickSpacing;

    m_axisY->setRange(niceMin, niceMax);
    const int tickCount = static_cast<int>(std::llround((niceMax - niceMin) / tickSpacing)) + 1;
    m_axisY->setTickCount(tickCount);
}

void ChartWidget::updateScrollBar() {
    if (m_history.isEmpty()) {
        const QSignalBlocker block(m_scrollBar);
        m_scrollBar->setRange(0, 0);
        m_scrollBar->setValue(0);
        return;
    }

    const qint64 dataStartMs = m_history.first().timestampMs;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const int spanSec = static_cast<int>((nowMs - dataStartMs) / 1000);
    const int pageSec = windowSeconds();
    const int maxVal = std::max(spanSec, pageSec);

    const QSignalBlocker block(m_scrollBar);
    m_scrollBar->setRange(pageSec, maxVal);
    m_scrollBar->setPageStep(pageSec);
    m_scrollBar->setSingleStep(std::max(1, pageSec / 20));
    if (m_scrollAtEnd)
        m_scrollBar->setValue(maxVal);
}

void ChartWidget::clear() {
    for (auto* series : {m_gridSeries, m_pvSeries, m_batterySeries, m_meterSeries})
        series->clear();
    m_history.clear();
    hideHoverCrosshair();
    updateScrollBar();
}

// ── Hover tooltip (crosshair + persistent info box) ───────────────────────────
//
// Deliberately implemented via an eventFilter on the QChartView's viewport
// plus scene items, rather than QLineSeries::hovered()/QToolTip -- same
// approach as solakon-one-ui's PvChartWidget -- because a QToolTip auto-hides
// on a timer even while the mouse stays put, whereas this box stays visible
// for as long as the cursor remains over the plot area.

bool ChartWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_chartView->viewport()) {
        switch (event->type()) {
        case QEvent::MouseMove: {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            const QPointF posInChart =
                m_chart->mapFromScene(m_chartView->mapToScene(mouseEvent->pos()));
            const QRectF plotArea = m_chart->plotArea();

            if (m_history.isEmpty() || !plotArea.contains(posInChart)) {
                hideHoverCrosshair();
                break;
            }

            const double targetXMs = m_chart->mapToValue(posInChart, m_gridSeries).x();
            const Sample* sample = findNearestSample(static_cast<qint64>(targetXMs));
            if (!sample) {
                hideHoverCrosshair();
                break;
            }

            // Snap the crosshair to the sample's exact timestamp (not the
            // raw mouse X) so it lines up with the actual data point.
            const QPointF snappedPos = m_chart->mapToPosition(
                QPointF(static_cast<double>(sample->timestampMs), 0.0), m_gridSeries);
            m_hoverLine->setLine(snappedPos.x(), plotArea.top(), snappedPos.x(), plotArea.bottom());
            m_hoverLine->show();

            m_hoverInfoText->setText(formatTooltip(*sample));
            positionHoverInfoBox(mouseEvent->pos());
            m_hoverInfoBg->show();
            break;
        }
        case QEvent::Leave:
            hideHoverCrosshair();
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

const ChartWidget::Sample* ChartWidget::findNearestSample(qint64 targetMs) const {
    if (m_history.isEmpty())
        return nullptr;

    // m_history is chronologically ordered; binary-search for the first
    // sample whose timestamp is >= targetMs, then compare against its
    // predecessor to find the closest one.
    auto it = std::lower_bound(m_history.begin(), m_history.end(), targetMs,
                                [](const Sample& s, qint64 value) { return s.timestampMs < value; });

    if (it == m_history.begin())
        return &(*it);
    if (it == m_history.end())
        return &m_history.last();

    const Sample& after = *it;
    const Sample& before = *(it - 1);
    return (targetMs - before.timestampMs <= after.timestampMs - targetMs) ? &before : &after;
}

QString ChartWidget::formatTooltip(const Sample& sample) {
    const QString time =
        QDateTime::fromMSecsSinceEpoch(sample.timestampMs).toString(QStringLiteral("hh:mm:ss"));

    auto formatWatts = [](bool ok, double watts) {
        return ok ? QStringLiteral("%1 W").arg(watts, 0, 'f', 0) : QObject::tr("(unknown)");
    };

    return QStringLiteral("%1\n"
                           "Grid power (A): %2\n"
                           "PV power: %3\n"
                           "Battery power: %4\n"
                           "Household meter (B): %5")
        .arg(time)
        .arg(formatWatts(sample.gridOk, sample.gridW))
        .arg(formatWatts(sample.pvOk, sample.pvW))
        .arg(formatWatts(sample.batteryOk, sample.batteryW))
        .arg(formatWatts(sample.meterOk, sample.meterW));
}

void ChartWidget::hideHoverCrosshair() {
    m_hoverLine->hide();
    m_hoverInfoBg->hide();
}

void ChartWidget::positionHoverInfoBox(const QPoint& viewportPos) {
    const QRectF textRect = m_hoverInfoText->boundingRect();
    const QRectF bgRect(0, 0, textRect.width() + 12, textRect.height() + 8);
    m_hoverInfoBg->setRect(bgRect);

    // Anchor near the cursor, offset down-right by default, flipping to the
    // opposite side when it would overflow the viewport so the box always
    // stays fully visible.
    const QSize viewSize = m_chartView->viewport()->size();
    qreal x = viewportPos.x() + 16;
    qreal y = viewportPos.y() + 16;
    if (x + bgRect.width() > viewSize.width())
        x = viewportPos.x() - bgRect.width() - 16;
    if (y + bgRect.height() > viewSize.height())
        y = viewportPos.y() - bgRect.height() - 16;

    const QPointF scenePos = m_chartView->mapToScene(QPoint(qRound(x), qRound(y)));
    m_hoverInfoBg->setPos(scenePos);
}

// ── X-scale (window length + scroll position) ─────────────────────────────────

void ChartWidget::onWindowComboChanged(int index) {
    m_windowIndex = index;
    saveWindowState();
    updateScrollBar();
    rescaleAxes();
}

void ChartWidget::onScrollBarChanged(int value) {
    m_scrollAtEnd = (value >= m_scrollBar->maximum());
    rescaleAxes();
}

void ChartWidget::loadWindowState() {
    QSettings settings;
    m_windowIndex = settings.value(kWindowKey, 1).toInt(); // default: 15 min
    if (m_windowIndex < 0 || m_windowIndex >= kWindowSecondsTable.size())
        m_windowIndex = 1;

    const QSignalBlocker block(m_windowCombo);
    m_windowCombo->setCurrentIndex(m_windowIndex);
}

void ChartWidget::saveWindowState() const {
    QSettings settings;
    settings.setValue(kWindowKey, m_windowIndex);
}

// ── Y-scale lock ───────────────────────────────────────────────────────────────

void ChartWidget::onLockYScaleToggled(bool checked) {
    saveYScaleState();
    if (!checked)
        rescaleAxes(); // resume auto-scaling immediately
    // When checked, nothing further to do -- the Y axis simply keeps
    // whatever range rescaleAxes() last painted (see the early return
    // there), rather than snapping to some other value.
}

void ChartWidget::loadYScaleState() {
    QSettings settings;
    const bool locked = settings.value(kLockYKey, false).toBool();
    m_lockYScaleCheckBox->setChecked(locked);
}

void ChartWidget::saveYScaleState() const {
    QSettings settings;
    settings.setValue(kLockYKey, m_lockYScaleCheckBox->isChecked());
}
