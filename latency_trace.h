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

/// 渲染线程在 render 末尾用，查询本帧 SDK 回调入口时刻（不消费、不删除条目）。
/// 返回 false 时 outUs 未修改，通常表示对应 frameId 的 trace 已被裁剪/未记录。
bool peekSdkCallbackUs(quint32 frameId, qint64 *outUs);

}  // namespace demo::latency_trace

#endif
