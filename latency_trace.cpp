#include "latency_trace.h"

#include <QDebug>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QString>
#include <QVector>

#include <chrono>
#include <algorithm>

namespace demo::latency_trace {

namespace {

constexpr int kSummaryWindowSize = 180;
constexpr int kSummaryReportEveryFrames = 120;
constexpr qint64 kSlowFrameThresholdUs = 25000;

struct FrameTrace
{
    qint64 sdkCallbackUs = 0;
    qint64 uiDispatchUs = 0;
    qint64 presentUs = 0;
    qint64 syncUs = 0;
    qint64 renderUs = 0;
    quint64 ptsMs = 0;
    quint64 utcMs = 0;
    quint32 width = 0;
    quint32 height = 0;
    quint32 dataSize = 0;
};

struct CompletedTrace
{
    quint32 frameId = 0;
    qint64 sdkToUiUs = -1;
    qint64 uiToPresentUs = -1;
    qint64 presentToSyncUs = -1;
    qint64 syncToRenderUs = -1;
    qint64 totalUs = -1;
};

QMutex g_mutex;
QHash<quint32, FrameTrace> g_traces;
QVector<CompletedTrace> g_recentTraces;
int g_recordCount = 0;
quint32 g_lastRenderedFrameId = 0;
bool g_hasLastRenderedFrameId = false;
int g_summaryReportCount = 0;
int g_totalRenderedFrames = 0;
int g_totalSlowFrames = 0;
int g_totalRenderSeqGap = 0;

qint64 nowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool shouldLog(quint32 frameId)
{
    return frameId < 5 || (frameId % 30) == 0;
}

qint64 deltaUs(qint64 startUs, qint64 endUs)
{
    if (startUs <= 0 || endUs <= 0 || endUs < startUs) {
        return -1;
    }
    return endUs - startUs;
}

QString formatMetricSummary(const char *label, const QVector<CompletedTrace> &traces, qint64 CompletedTrace::*member)
{
    QVector<qint64> values;
    values.reserve(traces.size());
    for (const CompletedTrace &trace : traces) {
        const qint64 value = trace.*member;
        if (value >= 0) {
            values.push_back(value);
        }
    }

    if (values.isEmpty()) {
        return QStringLiteral("%1=n/a").arg(QString::fromLatin1(label));
    }

    qint64 sumUs = 0;
    qint64 maxUs = 0;
    for (qint64 value : values) {
        sumUs += value;
        maxUs = std::max(maxUs, value);
    }

    std::sort(values.begin(), values.end());
    const int lastIndex = static_cast<int>(values.size()) - 1;
    const int p95Index = std::clamp(static_cast<int>(lastIndex * 0.95), 0, lastIndex);
    const double avgMs = static_cast<double>(sumUs) / static_cast<double>(values.size()) / 1000.0;
    const double p95Ms = static_cast<double>(values[p95Index]) / 1000.0;
    const double maxMs = static_cast<double>(maxUs) / 1000.0;

    return QStringLiteral("%1(avg=%2 p95=%3 max=%4 ms)")
        .arg(QString::fromLatin1(label))
        .arg(QString::number(avgMs, 'f', 3))
        .arg(QString::number(p95Ms, 'f', 3))
        .arg(QString::number(maxMs, 'f', 3));
}

void appendCompletedTraceLocked(const CompletedTrace &trace)
{
    g_recentTraces.push_back(trace);
    if (g_recentTraces.size() > kSummaryWindowSize) {
        g_recentTraces.remove(0, g_recentTraces.size() - kSummaryWindowSize);
    }

    ++g_totalRenderedFrames;
    if (trace.totalUs >= kSlowFrameThresholdUs) {
        ++g_totalSlowFrames;
    }

    if (g_hasLastRenderedFrameId && trace.frameId > g_lastRenderedFrameId + 1) {
        g_totalRenderSeqGap += static_cast<int>(trace.frameId - g_lastRenderedFrameId - 1);
    }
    g_lastRenderedFrameId = trace.frameId;
    g_hasLastRenderedFrameId = true;
}

void logSummaryLocked()
{
    if (g_recentTraces.isEmpty()) {
        return;
    }

    ++g_summaryReportCount;

    int windowSlowFrames = 0;
    for (const CompletedTrace &trace : g_recentTraces) {
        if (trace.totalUs >= kSlowFrameThresholdUs) {
            ++windowSlowFrames;
        }
    }

    qInfo().noquote()
        << QStringLiteral("[LatencySummary] report=%1 window=%2 rendered_total=%3 slow_total=%4 seq_gap_total=%5 "
                          "slow_window=%6 | %7 | %8 | %9 | %10 | %11")
               .arg(g_summaryReportCount)
               .arg(g_recentTraces.size())
               .arg(g_totalRenderedFrames)
               .arg(g_totalSlowFrames)
               .arg(g_totalRenderSeqGap)
               .arg(windowSlowFrames)
               .arg(formatMetricSummary("sdk->ui", g_recentTraces, &CompletedTrace::sdkToUiUs))
               .arg(formatMetricSummary("ui->present", g_recentTraces, &CompletedTrace::uiToPresentUs))
               .arg(formatMetricSummary("present->sync", g_recentTraces, &CompletedTrace::presentToSyncUs))
               .arg(formatMetricSummary("sync->render", g_recentTraces, &CompletedTrace::syncToRenderUs))
               .arg(formatMetricSummary("total", g_recentTraces, &CompletedTrace::totalUs));
}

void pruneExpiredLocked(qint64 now)
{
    if ((++g_recordCount % 120) != 0) {
        return;
    }

    constexpr qint64 kMaxAgeUs = 5 * 1000 * 1000;
    auto it = g_traces.begin();
    while (it != g_traces.end()) {
        const qint64 anchorUs = it->sdkCallbackUs > 0 ? it->sdkCallbackUs : it->presentUs;
        if (anchorUs > 0 && now - anchorUs > kMaxAgeUs) {
            it = g_traces.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace

qint64 monotonicUs()
{
    return nowUs();
}

void reset()
{
    QMutexLocker locker(&g_mutex);
    g_traces.clear();
    g_recentTraces.clear();
    g_recordCount = 0;
    g_lastRenderedFrameId = 0;
    g_hasLastRenderedFrameId = false;
    g_summaryReportCount = 0;
    g_totalRenderedFrames = 0;
    g_totalSlowFrames = 0;
    g_totalRenderSeqGap = 0;
}

void recordSdkCallback(quint32 frameId,
                       quint64 ptsMs,
                       quint64 utcMs,
                       quint32 width,
                       quint32 height,
                       quint32 dataSize)
{
    const qint64 now = nowUs();
    QMutexLocker locker(&g_mutex);
    FrameTrace &trace = g_traces[frameId];
    trace.sdkCallbackUs = now;
    trace.ptsMs = ptsMs;
    trace.utcMs = utcMs;
    trace.width = width;
    trace.height = height;
    trace.dataSize = dataSize;
    pruneExpiredLocked(now);
}

void recordUiDispatch(quint32 frameId)
{
    const qint64 now = nowUs();
    QMutexLocker locker(&g_mutex);
    g_traces[frameId].uiDispatchUs = now;
    pruneExpiredLocked(now);
}

void recordPresent(quint32 frameId, int width, int height)
{
    const qint64 now = nowUs();
    QMutexLocker locker(&g_mutex);
    FrameTrace &trace = g_traces[frameId];
    trace.presentUs = now;
    if (width > 0) {
        trace.width = static_cast<quint32>(width);
    }
    if (height > 0) {
        trace.height = static_cast<quint32>(height);
    }
    pruneExpiredLocked(now);
}

void recordSync(quint32 frameId)
{
    const qint64 now = nowUs();
    QMutexLocker locker(&g_mutex);
    g_traces[frameId].syncUs = now;
    pruneExpiredLocked(now);
}

void recordRender(quint32 frameId)
{
    const qint64 now = nowUs();
    FrameTrace trace;
    CompletedTrace completed;
    {
        QMutexLocker locker(&g_mutex);
        FrameTrace &stored = g_traces[frameId];
        stored.renderUs = now;
        trace = stored;
        g_traces.remove(frameId);

        completed.frameId = frameId;
        completed.sdkToUiUs = deltaUs(trace.sdkCallbackUs, trace.uiDispatchUs);
        completed.uiToPresentUs = deltaUs(trace.uiDispatchUs, trace.presentUs);
        completed.presentToSyncUs = deltaUs(trace.presentUs, trace.syncUs);
        completed.syncToRenderUs = deltaUs(trace.syncUs, trace.renderUs);
        completed.totalUs = deltaUs(trace.sdkCallbackUs, trace.renderUs);
        appendCompletedTraceLocked(completed);

        if ((g_totalRenderedFrames % kSummaryReportEveryFrames) == 0) {
            logSummaryLocked();
        }
    }

    Q_UNUSED(trace);
}

}  // namespace demo::latency_trace
