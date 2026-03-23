#include "webrtc_video_renderer.h"
#include <QPainter>
#include <QDebug>
#include <QElapsedTimer>

using namespace libwebrtc;

// 耗时统计：每 STATS_INTERVAL 帧打印一次
#define STATS_INTERVAL 60

WebRTCVideoRenderer::WebRTCVideoRenderer(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    setSmooth(true);
}

WebRTCVideoRenderer::~WebRTCVideoRenderer()
{
    clearVideoTrack();
}

void WebRTCVideoRenderer::OnFrame(scoped_refptr<RTCVideoFrame> frame)
{
    if (!frame)
        return;
    // 帧间隔（距上一帧交付时间，可反映解码/网络节奏）
    qint64 tDecodeIntervalUs = m_decodeIntervalTimer.isValid() ? m_decodeIntervalTimer.nsecsElapsed() / 1000 : 0;
    m_decodeIntervalTimer.start();
    // 记录本帧接收时刻，用于计算「线程队列」延迟
    m_receiveTimer.start();
    // OnFrame 在 WebRTC 工作线程调用，需转到主线程更新 UI
    QMetaObject::invokeMethod(this, [this, frame, tDecodeIntervalUs]() {
        updateFrameImage(frame, tDecodeIntervalUs);
        update();
    }, Qt::QueuedConnection);
}

void WebRTCVideoRenderer::updateFrameImage(scoped_refptr<RTCVideoFrame> frame, qint64 tDecodeIntervalUs)
{
    if (!frame)
        return;
    qint64 tThreadQueueUs = m_receiveTimer.nsecsElapsed() / 1000;
    QElapsedTimer localTimer;
    localTimer.start();

    const int w = frame->width();
    const int h = frame->height();
    if (w <= 0 || h <= 0)
        return;

    QImage img(w, h, QImage::Format_ARGB32);
    if (img.isNull())
        return;

    // 使用 libwebrtc 内置的 ConvertToARGB（底层 libyuv，带 NEON/SSE SIMD 加速）
    int ret = frame->ConvertToARGB(RTCVideoFrame::Type::kARGB,
                                   img.bits(), img.bytesPerLine(),
                                   w, h);
    if (ret < 0) {
        qWarning() << "[VideoRenderer] ConvertToARGB failed, ret=" << ret;
        return;
    }
    qint64 tConvertUs = localTimer.nsecsElapsed() / 1000;

    QMutexLocker lock(&m_imageMutex);
    m_image = std::move(img);
    qint64 tCopyUs = localTimer.nsecsElapsed() / 1000 - tConvertUs;
    if (!m_hasVideo) {
        m_hasVideo = true;
        Q_EMIT hasVideoChanged();
    }

    m_frameCount++;
    m_lastFrameTotalUs = tThreadQueueUs + tConvertUs + tCopyUs;
    if (m_frameCount % STATS_INTERVAL == 0) {
        qDebug() << "[VideoPerf] frame#" << m_frameCount
                 << "| 线程队列:" << (tThreadQueueUs / 1000.0) << "ms"
                 << "| YUV转换:" << (tConvertUs / 1000.0) << "ms"
                 << "| 缓冲拷贝:" << (tCopyUs / 1000.0) << "ms"
                 << "| 帧间隔:" << (tDecodeIntervalUs / 1000.0) << "ms"
                 << "| 本帧总:" << (m_lastFrameTotalUs / 1000.0) << "ms";
    }
}

void WebRTCVideoRenderer::setVideoTrack(scoped_refptr<RTCVideoTrack> track)
{
    if (m_track == track)
        return;
    clearVideoTrack();
    m_track = track;
    if (m_track) {
        m_track->AddRenderer(this);
        if (!m_hasVideo) {
            m_hasVideo = true;
            Q_EMIT hasVideoChanged();
        }
        qDebug() << "[VideoRenderer] 已绑定视频轨道";
    }
}

void WebRTCVideoRenderer::clearVideoTrack()
{
    if (m_track) {
        m_track->RemoveRenderer(this);
        m_track = nullptr;
    }
    QMutexLocker lock(&m_imageMutex);
    m_image = QImage();
    if (m_hasVideo) {
        m_hasVideo = false;
        Q_EMIT hasVideoChanged();
    }
    update();
}

void WebRTCVideoRenderer::paint(QPainter *painter)
{
    QElapsedTimer paintTimer;
    paintTimer.start();
    QMutexLocker lock(&m_imageMutex);
    if (m_image.isNull()) {
        painter->fillRect(boundingRect(), QColor(15, 22, 41));
        return;
    }
    QRectF src(0, 0, m_image.width(), m_image.height());
    painter->drawImage(boundingRect(), m_image, src, Qt::AutoColor);
    qint64 tPaintUs = paintTimer.nsecsElapsed() / 1000;
    if (m_frameCount > 0 && m_frameCount % STATS_INTERVAL == 0) {
        qDebug() << "[VideoPerf] paint:" << (tPaintUs / 1000.0) << "ms"
                 << "| 全链路(含渲染):" << ((m_lastFrameTotalUs + tPaintUs) / 1000.0) << "ms";
    }
}
