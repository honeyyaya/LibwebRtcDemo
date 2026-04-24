#ifndef LATENCY_TRACE_H
#define LATENCY_TRACE_H

#include <QtGlobal>

namespace demo::latency_trace {

qint64 monotonicUs();
void reset();

void recordSdkCallback(quint32 frameId,
                       quint64 ptsMs,
                       quint64 utcMs,
                       quint32 width,
                       quint32 height,
                       quint32 dataSize);
void recordUiDispatch(quint32 frameId);
void recordPresent(quint32 frameId, int width, int height);
void recordSync(quint32 frameId);
void recordRender(quint32 frameId);

}  // namespace demo::latency_trace

#endif
