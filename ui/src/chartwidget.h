#pragma once

#include "apistatus.h"

#include <QWidget>

#include <QChart>
#include <QChartView>
#include <QDateTimeAxis>
#include <QLineSeries>
#include <QValueAxis>
#include <QVector>

class QCheckBox;
class QComboBox;
class QScrollBar;
class QGraphicsLineItem;
class QGraphicsRectItem;
class QGraphicsSimpleTextItem;

// ChartWidget: rolling time-series chart of grid/PV/battery/household power,
// analogous to solakon-one-ui's "Live charts" panel. Samples are retained
// for up to kMaxHistorySeconds (24 h, the longest selectable window) so
// switching the visible window wider doesn't lose history that was already
// collected while a narrower window was selected; addSample() only trims
// data older than that fixed retention ceiling, while the currently
// selected window merely controls what's *displayed* (see rescaleAxes()).
//
// Hovering the chart shows a persistent tooltip (crosshair + info box) with
// the exact values of the sample nearest the cursor -- same pattern as
// solakon-one-ui's PvChartWidget: a QGraphicsLineItem crosshair and a
// QGraphicsRectItem/QGraphicsSimpleTextItem info box added directly to the
// QChartView's scene, both driven by an eventFilter on the viewport rather
// than QChart's own built-in QToolTip-based hover signals -- this is what
// makes the box persistent (only hidden on QEvent::Leave) instead of
// auto-hiding on a timer like a normal QToolTip.
//
// X-axis scaling: the visible time window's *length* is configurable via a
// combo box (5 min .. 24 h, same table as solakon-one-ui's PvChartWidget),
// and its *position* within the retained history is configurable via a
// horizontal scroll bar (also matching solakon-one-ui's PvChartWidget: the
// scroll bar's value is seconds from the first retained sample to the right
// edge of the visible window; at its maximum, the view tracks live data as
// new samples arrive -- see m_scrollAtEnd/computeWindowEndMs()). Both are
// persisted via QSettings.
//
// Y-axis scaling: by default the Y axis auto-scales to the samples within
// the currently visible window (nice round numbers, 0 always included --
// see rescaleAxes()). Checking "Lock Y" (same control/behavior as
// solakon-one-ui's PvChartWidget) simply freezes the axis at whatever range
// is currently displayed -- most useful together with the scroll bar, so
// scrolling back through history to compare an earlier period doesn't also
// keep rescaling the Y axis out from under you. The lock state is
// persisted via QSettings.
class ChartWidget : public QWidget {
    Q_OBJECT
public:
    explicit ChartWidget(QWidget* parent = nullptr);
    ~ChartWidget() override;

public slots:
    void addSample(const ApiStatus& status);
    void clear();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onLockYScaleToggled(bool checked);
    void onWindowComboChanged(int index);
    void onScrollBarChanged(int value);

private:
    // Sample: one time-stamped snapshot of everything the tooltip needs to
    // display, kept independently of the QLineSeries point data so the
    // tooltip can still report "(unknown)" for any reading that failed on a
    // given cycle instead of silently reusing a stale/neighboring value.
    struct Sample {
        qint64 timestampMs = 0;
        bool   gridOk = false;
        double gridW = 0;
        bool   pvOk = false;
        double pvW = 0;
        bool   batteryOk = false;
        double batteryW = 0;
        bool   meterOk = false;
        double meterW = 0;
    };

    QChart*        m_chart;
    QChartView*    m_chartView;
    QLineSeries*   m_gridSeries;
    QLineSeries*   m_pvSeries;
    QLineSeries*   m_batterySeries;
    QLineSeries*   m_meterSeries;
    QDateTimeAxis* m_axisX;
    QValueAxis*    m_axisY;

    QComboBox*  m_windowCombo;
    QCheckBox*  m_lockYScaleCheckBox;
    QScrollBar* m_scrollBar;

    // Retained for up to kMaxHistorySeconds -- see class comment above.
    // Chronologically ordered, same retention window as the QLineSeries.
    QVector<Sample> m_history;

    // Hover crosshair + persistent info box (see class comment above).
    QGraphicsLineItem*       m_hoverLine;
    QGraphicsRectItem*       m_hoverInfoBg;
    QGraphicsSimpleTextItem* m_hoverInfoText;

    int  m_windowIndex = 1;      // index into kWindowSecondsTable/kWindowLabels; default 15 min
    bool m_scrollAtEnd = true;   // true == tracking live data; false == scrolled back in history

    void trimSeries(QLineSeries* series, qint64 cutoffMs);
    void rescaleAxes();
    void updateScrollBar();
    qint64 computeWindowEndMs() const;
    int windowSeconds() const;

    const Sample* findNearestSample(qint64 targetMs) const;
    static QString formatTooltip(const Sample& sample);
    void hideHoverCrosshair();
    void positionHoverInfoBox(const QPoint& viewportPos);

    void loadYScaleState();
    void saveYScaleState() const;
    void loadWindowState();
    void saveWindowState() const;
};
