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

    void forward(double seconds);       //快进 n 秒 , 可正可负
    void setPlayRate(double rate);

    void setRenderSize(int w, int h); // 新接口：设置渲染目标像素尺寸（乘 DPR）

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
    std::atomic<double> m_audioBasePts{ -1.0 };    // pts of first enqueued audio block
    std::atomic<long long> m_audioPlayedSamples{0}; // number of samples (per channel) written to device
    int m_audioOutChannels = 2;
    int m_audioSampleRate = 48000;

    // timebases for pts -> seconds
    AVRational videoTimeBase{0,1};
    AVRational audioTimeBase{0,1};

    // wall-clock based video timing
    QElapsedTimer m_playTimer;
    double m_playStartPts{0.0};
    bool m_playStarted{false};

    // video scaler（用于在 decodeLoop 中按目标尺寸缩放）
    SwsContext *swsCtx = nullptr;
    QMutex m_swsMutex;
    QMutex m_swrMutex;   // <--- 新增：保护 swrCtx 的互斥量
    std::atomic<int> m_renderWidth{0};
    std::atomic<int> m_renderHeight{0};
    std::atomic<bool> m_swsCtxNeedReset{false};  // 新增

    // pause accumulation: total paused milliseconds to subtract from elapsed
    std::atomic<qint64> m_totalPausedMs{0}; // 在 decodeLoop 中读取
    qint64 m_pauseStartMs{0};               // 记录 pause 开始时刻（仅在主线程修改）

    std::atomic<double> m_playRate{1.0};  // 播放速度，1.0 = 正常，2.0 = 2倍速

};
