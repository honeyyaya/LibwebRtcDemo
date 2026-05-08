#include "latency_trace.h"

#include <QHash>
#include <QMutex>
#include <QMutexLocker>

#include <chrono>

namespace demo::latency_trace {

namespace {

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

QMutex g_mutex;
QHash<quint32, FrameTrace> g_traces;
int g_recordCount = 0;

qint64 nowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
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
    g_recordCount = 0;
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

bool peekSdkCallbackUs(quint32 frameId, qint64 *outUs)
{
    if (!outUs) {
        return false;
    }
    QMutexLocker locker(&g_mutex);
    auto it = g_traces.constFind(frameId);
    if (it == g_traces.constEnd() || it.value().sdkCallbackUs <= 0) {
        return false;
    }
    *outUs = it.value().sdkCallbackUs;
    return true;
}

void recordRender(quint32 frameId)
{
    QMutexLocker locker(&g_mutex);
    auto it = g_traces.find(frameId);
    if (it == g_traces.end()) {
        return;
    }
    it.value().renderUs = nowUs();
    g_traces.erase(it);
}

}  // namespace demo::latency_trace
