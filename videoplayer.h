#pragma once

#include <QObject>
#include <QImage>
#include <QQueue>
#include <QMutex>
#include <QString>
#include <QTimer>
#include <QAudioSink>
#include <QAudioFormat>
#include <QMediaDevices>
#include <QElapsedTimer>
#include <QThread>
#include <atomic>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

/**
 * 音频驱动的视频播放器
 *
 * 同步策略:
 *   有音频流  → 音频连续播放, 视频帧同步到音频时钟 (Audio Master Clock)
 *   纯视频    → 挂钟 (wall-clock) 同步
 *   纯音频    → 音频播放 + 定时器更新进度
 *
 * 线程模型:
 *   主线程    → UI / QAudioSink / QTimer
 *   解码线程  → demux + decode + filter + A/V 同步判定
 *   解码线程通过 QMetaObject::invokeMethod 投递 QObject 操作到主线程
 */
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
    void setVolume(int vol);   // 0~100

    const QVector<int> scalingAlgorithm = {
        SWS_FAST_BILINEAR, SWS_BILINEAR, SWS_BICUBIC, SWS_LANCZOS
    };
    void setScalingAlgorithm(int algo) {
        m_swsAlgo.store(algo);
        m_swsNeedReset.store(true);
    }

    bool   isAudioOnlyMode() const { return m_audioOnly.load(); }
    double getDuration()     const;
    bool   isPlaying()       const { return m_playing.load() && !m_paused.load(); }

signals:
    void frameReady(const QImage &img);
    void positionChanged(double pos);
    void finished();
    void playingChanged(bool playing);
    void buffering();

private slots:
    void onFlushAudio();
    void onUpdatePosition();

private:
    void decodeLoop();
    bool initAudioFilter(double tempo, int outSampleRate);
    void destroyAudioFilter();
    double audioClock() const;
    void closeFile();

    // ---- FFmpeg ----
    AVFormatContext *m_fmtCtx    = nullptr;
    AVCodecContext  *m_vCodecCtx = nullptr;
    AVCodecContext  *m_aCodecCtx = nullptr;
    int m_videoIdx = -1;
    int m_audioIdx = -1;
    AVRational m_videoTB{0, 1};
    AVRational m_audioTB{0, 1};

    // ---- audio filter ----
    AVFilterGraph   *m_filterGraph = nullptr;
    AVFilterContext *m_filterSrc   = nullptr;
    AVFilterContext *m_filterSink  = nullptr;
    QMutex m_filterMutex;

    // ---- video scaler ----
    SwsContext *m_swsCtx = nullptr;
    QMutex m_swsMutex;
    std::atomic<int>  m_renderW{0};
    std::atomic<int>  m_renderH{0};
    std::atomic<int>  m_swsAlgo{SWS_BILINEAR};
    std::atomic<bool> m_swsNeedReset{false};

    // ---- decode thread ----
    QThread *m_thread = nullptr;

    // ---- shared atomic state ----
    std::atomic<bool>   m_stopFlag{false};
    std::atomic<bool>   m_paused{false};
    std::atomic<bool>   m_playing{false};
    std::atomic<bool>   m_eof{false};
    std::atomic<bool>   m_seekFlag{false};
    std::atomic<bool>   m_filterNeedReset{false};
    std::atomic<bool>   m_audioOnly{false};
    std::atomic<double> m_playRate{1.0};
    std::atomic<int>    m_volume{80};      // 0~100
    double m_seekTarget = 0.0;

    // ---- audio output ----
    QAudioSink *m_audioSink  = nullptr;
    QIODevice  *m_audioIO    = nullptr;
    QTimer     *m_flushTimer = nullptr;
    QMutex      m_audioQMutex;
    QList<QByteArray> m_audioQ;
    int m_outSampleRate = 48000;
    int m_outChannels   = 2;

    // ---- audio clock ----
    std::atomic<double>    m_audioBasePts{-1.0};
    std::atomic<long long> m_audioWrittenSamples{0};

    // ---- position tracking (稳定的位置记录, forward() 使用) ----
    std::atomic<double> m_lastKnownPos{0.0};

    // ---- video queue ----
    QMutex m_vqMutex;
    QQueue<std::pair<QImage, double>> m_vQueue;

    // ---- position timer ----
    QTimer *m_posTimer = nullptr;

    // ---- wall-clock fallback (video without audio) ----
    QElapsedTimer       m_wallClock;
    double              m_wallBasePts = 0.0;
    std::atomic<bool>   m_wallStarted{false};
    std::atomic<qint64> m_totalPausedMs{0};
    std::atomic<qint64> m_pauseStartMs{0};

    QString m_filePath;
};
