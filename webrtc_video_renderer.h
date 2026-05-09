#ifndef WEBRTC_VIDEO_RENDERER_H
#define WEBRTC_VIDEO_RENDERER_H

#include <atomic>

#include <QMetaObject>
#include <QMutex>
#include <QQuickItem>
#include <QString>

#include "video_frame_sink.h"

class QSGNode;
class QQuickWindow;

class WebRTCVideoRenderer : public QQuickItem, public VideoFrameSink
{
    Q_OBJECT
    Q_INTERFACES(VideoFrameSink)
    Q_PROPERTY(bool hasVideo READ hasVideo NOTIFY hasVideoChanged)
    Q_PROPERTY(int highlightFrameId READ highlightFrameId NOTIFY highlightFrameIdChanged)
    Q_PROPERTY(QString sampledPipelineLine READ sampledPipelineLine NOTIFY sampledPipelineStatsChanged)
    Q_PROPERTY(bool hasSampledPipelineUi READ hasSampledPipelineUi NOTIFY sampledPipelineStatsChanged)
    Q_PROPERTY(double pacerWaitMs READ pacerWaitMs NOTIFY sampledPipelineStatsChanged)
    Q_PROPERTY(double syncToRenderMs READ syncToRenderMs NOTIFY sampledPipelineStatsChanged)
    Q_PROPERTY(double uploadDrawMs READ uploadDrawMs NOTIFY sampledPipelineStatsChanged)
    Q_PROPERTY(double wallPresentToRenderMs READ wallPresentToRenderMs NOTIFY sampledPipelineStatsChanged)
    Q_PROPERTY(qulonglong mailboxDropCount READ mailboxDropCount NOTIFY sampledPipelineStatsChanged)

public:
    explicit WebRTCVideoRenderer(QQuickItem *parent = nullptr);
    ~WebRTCVideoRenderer() override;

    /// callable from: ANY thread (SDK callback thread / GUI thread / 任意线程)
    /// 调用方必须传入"已 retain"的帧；本对象接管该 retain 引用计数（最终 release 由本类负责）。
    void presentFrame(librflow_video_frame_t frame) override;

    Q_INVOKABLE void clearVideoTrack() override;

    bool hasVideo() const { return m_hasVideo.load(std::memory_order_acquire); }
    int highlightFrameId() const;
    QString sampledPipelineLine() const;
    bool hasSampledPipelineUi() const { return m_hasSampledPipelineUi; }

    double pacerWaitMs() const { return m_sampledPacerWaitMs; }
    double syncToRenderMs() const { return m_sampledSyncToRenderMs; }
    double uploadDrawMs() const { return m_sampledUploadDrawMs; }
    double wallPresentToRenderMs() const { return m_sampledWallPresentToRenderMs; }
    qulonglong mailboxDropCount() const
    {
        return static_cast<qulonglong>(m_mailboxDropCount.load(std::memory_order_acquire));
    }

    Q_INVOKABLE void applySampledPipelineUi(int frameId,
                                            double pacerWaitMs,
                                            double syncToRenderMs,
                                            double uploadDrawMs,
                                            double decodeToRenderTotalMs,
                                            double wallPresentToRenderMs);

    /// 仅供 QSGRenderNode::sync 调用（渲染线程）：把 m_renderFrame 的当前帧及 trace 元数据拷出，
    /// 并把 slot 置空。返回 true 表示拿到一帧，调用方接管该帧的 retain 引用计数（最终 release）。
    /// `lastConsumedGeneration` 用于跳过同一帧被重复消费的情况。
    struct RenderFrameSnapshot
    {
        librflow_video_frame_t frame = nullptr;
        qint64 queueStartMonoUs = 0;
        qint64 guiUpdateDispatchMonoUs = 0;
        qint64 syncStartMonoUs = 0;
        qint64 mailboxAgeUs = 0;
        qint64 pacerWaitUs = 0;
        int frameId = -1;
        quint64 generation = 0;
    };
    bool consumeRenderFrame(quint64 lastConsumedGeneration, RenderFrameSnapshot &outSnapshot);

Q_SIGNALS:
    void hasVideoChanged();
    void highlightFrameIdChanged();
    void sampledPipelineStatsChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    /// mailbox slot —— 任意线程通过 m_frameMutex 写入；渲染线程在 beforeSynchronizing 内排出。
    struct MailboxFrameSlot {
        librflow_video_frame_t frame = nullptr;
        qint64 queueStartMonoUs = 0;
        qint64 guiUpdateDispatchMonoUs = 0;
        int frameId = -1;
        quint64 generation = 0;
    };

    /// render-owned snapshot —— beforeSynchronizing 把 mailbox 提升到这里。
    struct RenderFrameSlot {
        librflow_video_frame_t frame = nullptr;
        qint64 queueStartMonoUs = 0;
        qint64 guiUpdateDispatchMonoUs = 0;
        qint64 syncStartMonoUs = 0;
        qint64 mailboxAgeUs = 0;
        qint64 pacerWaitUs = 0;
        int frameId = -1;
        quint64 generation = 0;
        bool ready = false;
    };

    /// 必须持有 m_frameMutex 调用。
    void publishMailboxFrameLocked(librflow_video_frame_t frame,
                                   qint64 queueStartMonoUs,
                                   int frameId,
                                   librflow_video_frame_t *outOldFrame);

    /// Render thread only. 把 mailbox 提升到 m_renderFrame，return true 表示拿到新帧。
    /// 若发现过期帧（mailbox_age > kStaleMailboxFrameDropUs），直接丢弃并 ++m_mailboxDropCount。
    bool takeMailboxFrameForRender(qint64 syncStartMonoUs);

    /// 任意线程：通过 QQuickWindow::update() 唤醒渲染线程。
    /// 内部用原子 flag 去重，确保最多一个 in-flight 的请求。
    void requestMailboxRenderUpdate();
    void queueMailboxRenderUpdate();

    /// GUI 线程：建立 beforeSynchronizing 直连、setPersistent*、开启调度。
    void ensureRenderSchedulingActive();
    void onWindowChanged(QQuickWindow *window);

    /// Render 线程（DirectConnection）：sync 入口，拉走 mailbox。
    void onBeforeSynchronizing();

    /// 跨线程释放：假设 librflow_video_frame_release 线程安全。
    void releaseFrame(librflow_video_frame_t frame);

    // ----- shared atomic / state -----
    std::atomic<bool> m_hasVideo{false};
    std::atomic<bool> m_renderSchedulingActive{false};
    std::atomic<bool> m_renderSchedulingStartPending{false};
    std::atomic<bool> m_renderUpdateInvokePending{false};
    std::atomic<bool> m_renderUpdatePending{false};
    std::atomic<quint64> m_mailboxDropCount{0};
    std::atomic<quint64> m_publishedFrames{0};

    // ----- mailbox / render slots -----
    mutable QMutex m_frameMutex;
    MailboxFrameSlot m_mailboxFrame;
    RenderFrameSlot m_renderFrame;
    quint64 m_lastResolvedMailboxGeneration = 0;
    qint64 m_renderUpdateDispatchMonoUs = 0;

    int m_highlightFrameId = -1;

    // ----- HUD sampling (GUI thread) -----
    int m_sampledHighlightFrameId = -1;
    double m_sampledPacerWaitMs = -1.0;
    double m_sampledSyncToRenderMs = -1.0;
    double m_sampledUploadDrawMs = -1.0;
    double m_sampledDecodeToRenderMs = -1.0;
    double m_sampledWallPresentToRenderMs = -1.0;
    bool m_hasSampledPipelineUi = false;

    // ----- window / signal connection -----
    QMetaObject::Connection m_beforeSynchronizingConnection;
    QQuickWindow *m_observedWindow = nullptr;
};

#endif
