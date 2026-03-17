#include "webrtc_video_renderer.h"
#include <QPainter>
#include <QDebug>

using namespace libwebrtc;

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
    // OnFrame 在 WebRTC 工作线程调用，需转到主线程更新 UI
    QMetaObject::invokeMethod(this, [this, frame]() {
        updateFrameImage(frame);
        update();
    }, Qt::QueuedConnection);
}

// I420 (YUV) 转 BGRA 的 BT.601 公式
static void i420ToBgra(int w, int h,
                       const uint8_t *y, int strideY,
                       const uint8_t *u, int strideU,
                       const uint8_t *v, int strideV,
                       uint8_t *bgra, int strideBgra)
{
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            int yVal = y[row * strideY + col];
            int uVal = u[(row / 2) * strideU + (col / 2)];
            int vVal = v[(row / 2) * strideV + (col / 2)];

            int c = yVal - 16;
            int d = uVal - 128;
            int e = vVal - 128;
            int r = (298 * c + 409 * e + 128) >> 8;
            int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            int b = (298 * c + 516 * d + 128) >> 8;

            r = qBound(0, r, 255);
            g = qBound(0, g, 255);
            b = qBound(0, b, 255);

            uint8_t *px = bgra + row * strideBgra + col * 4;
            px[0] = static_cast<uint8_t>(b);
            px[1] = static_cast<uint8_t>(g);
            px[2] = static_cast<uint8_t>(r);
            px[3] = 255;
        }
    }
}

void WebRTCVideoRenderer::updateFrameImage(scoped_refptr<RTCVideoFrame> frame)
{
    if (!frame)
        return;
    const int w = frame->width();
    const int h = frame->height();
    if (w <= 0 || h <= 0)
        return;

    const uint8_t *y = frame->DataY();
    const uint8_t *u = frame->DataU();
    const uint8_t *v = frame->DataV();
    if (!y || !u || !v) {
        qWarning() << "[VideoRenderer] I420 data planes null, skip frame";
        return;
    }

    QImage img(w, h, QImage::Format_ARGB32);
    if (img.isNull())
        return;

    // 使用 I420 YUV 手动转换（部分 libwebrtc 的 ConvertToARGB 返回值异常，如 1228800）
    i420ToBgra(w, h,
               y, frame->StrideY(),
               u, frame->StrideU(),
               v, frame->StrideV(),
               img.bits(), img.bytesPerLine());

    QMutexLocker lock(&m_imageMutex);
    m_image = img.copy();
    if (!m_hasVideo) {
        m_hasVideo = true;
        Q_EMIT hasVideoChanged();
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
    QMutexLocker lock(&m_imageMutex);
    if (m_image.isNull()) {
        painter->fillRect(boundingRect(), QColor(15, 22, 41));
        return;
    }
    QRectF src(0, 0, m_image.width(), m_image.height());
    painter->drawImage(boundingRect(), m_image, src, Qt::AutoColor);
}
