#pragma once
#include <QObject>
#include <QImage>
#include <QQueue>
#include <QMutex>
#include <QAtomicInt>
#include <QString>
#include <utility>
#include <atomic>
#include <QTimer>
#include <QAudioSink>
#include <QAudioFormat>
#include <QMediaDevices>
#include <QElapsedTimer>
#include <QList>
#include <QByteArray>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

class VideoPlayer : public QObject
{
    Q_OBJECT
public:
    explicit VideoPlayer(QObject *parent = nullptr);
    ~VideoPlayer();

    bool openFile(const QString &filePath);
    void play();
    void pause();
    void stop();
    void seek(double positionSec);

    void forward(double seconds);
    void setPlayRate(double rate);

    void setRenderSize(int w, int h);
    // 优化：设置视频缩放算法（权衡质量和性能）
    // SWS_FAST_BILINEAR - 最快但质量最差
    // SWS_BILINEAR - 平衡
    // SWS_BICUBIC - 较好质量
    // SWS_LANCZOS - 最好质量但最慢（默认不用）
    const QVector<int> scalingAlgorithm = {SWS_FAST_BILINEAR,SWS_BILINEAR,SWS_BICUBIC,SWS_LANCZOS};
    void setScalingAlgorithm(int algo) { m_scalingAlgo.store(algo); m_swsCtxNeedReset.store(true); }

signals:
    void frameReady(const QImage &img);
    void positionChanged(double pos);
    void finished();
    void playingChanged(bool playing);
    void buffering();

private slots:
    void flushAudioBuffer();

private:
    void decodeLoop();
    void clearQueue();
    void freeFFmpegResources();
    bool initAudioFilter(double rate);
    void cleanupAudioFilter();

    double videoPtsToSeconds(AVFrame *vframe);

private:
    // FFmpeg
    AVFormatContext *fmtCtx = nullptr;

    // video
    AVCodecContext *codecCtx = nullptr;
    int videoStreamIndex = -1;

    // audio
    AVCodecContext *audioCodecCtx = nullptr;
    int audioStreamIndex = -1;
    SwrContext *swrCtx = nullptr;

    // audio filter graph for atempo
    AVFilterGraph *audioFilterGraph = nullptr;
    AVFilterContext *audioBufferSrcCtx = nullptr;
    AVFilterContext *audioBufferSinkCtx = nullptr;
    QMutex m_audioFilterMutex;

    // reuse frames/packets
    AVFrame *frame = nullptr;
    AVPacket *packet = nullptr;

    // thread + queue
    QThread *m_decodeThread = nullptr;
    QQueue<std::pair<QImage, double>> m_frameQueue;
    QMutex m_mutex;

    // state
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_finished{false};
    std::atomic<bool> m_seekRequested{false};
    double m_seekTargetSec{0.0};
    QString m_filePath;

    // audio queue & writing (main thread)
    QMutex m_audioQueueMutex;
    QList<QByteArray> m_audioQueue;
    QTimer *m_audioFlushTimer = nullptr;
    QAudioSink *audioSink = nullptr;
    QIODevice *audioIODevice = nullptr;

    // audio tracking
    std::atomic<double> m_audioBasePts{-1.0};
    std::atomic<long long> m_audioPlayedSamples{0};
    int m_audioOutChannels = 2;
    int m_audioSampleRate = 48000;

    // timebases for pts -> seconds
    AVRational videoTimeBase{0,1};
    AVRational audioTimeBase{0,1};

    // wall-clock based video timing
    QElapsedTimer m_playTimer;
    double m_playStartPts{0.0};
    bool m_playStarted{false};

    // video scaler
    SwsContext *swsCtx = nullptr;
    QMutex m_swsMutex;
    QMutex m_swrMutex;
    std::atomic<int> m_renderWidth{0};
    std::atomic<int> m_renderHeight{0};
    std::atomic<bool> m_swsCtxNeedReset{false};
    std::atomic<int> m_scalingAlgo{SWS_BILINEAR};  // 快速缩放算法，减少CPU

    // pause accumulation
    std::atomic<qint64> m_totalPausedMs{0};
    qint64 m_pauseStartMs{0};

    std::atomic<double> m_playRate{1.0};
    std::atomic<bool> m_audioFilterNeedReset{false};
};
