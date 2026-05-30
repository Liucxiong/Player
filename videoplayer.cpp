#include "videoplayer.h"
#include <QMutexLocker>
#include <QDebug>
#include <cmath>

/* ================================================================
 *  构造 / 析构
 * ================================================================ */

VideoPlayer::VideoPlayer(QObject *parent) : QObject(parent)
{
    av_log_set_level(AV_LOG_QUIET);
}

VideoPlayer::~VideoPlayer() { stop(); }

/* ================================================================
 *  公共查询
 * ================================================================ */

double VideoPlayer::getDuration() const
{
    if (!m_fmtCtx) return 0.0;
    if (m_audioOnly.load() && m_audioIdx >= 0) {
        const AVStream *s = m_fmtCtx->streams[m_audioIdx];
        if (s->duration != AV_NOPTS_VALUE)
            return s->duration * av_q2d(s->time_base);
    }
    if (m_videoIdx >= 0) {
        const AVStream *s = m_fmtCtx->streams[m_videoIdx];
        if (s->duration != AV_NOPTS_VALUE)
            return s->duration * av_q2d(s->time_base);
    }
    if (m_fmtCtx->duration != AV_NOPTS_VALUE)
        return m_fmtCtx->duration / double(AV_TIME_BASE);
    return 0.0;
}

/* ================================================================
 *  音频主时钟
 *
 *  返回媒体时间轴位置 (秒), 考虑:
 *    1. sink 缓冲区延迟 (已写入但未播放的采样)
 *    2. 倍速 (atempo) — 1 秒实际播放 = tempo 秒媒体时间
 * ================================================================ */

double VideoPlayer::audioClock() const
{
    double base = m_audioBasePts.load();
    if (base < 0.0) return 0.0;

    long long written = m_audioWrittenSamples.load();
    int sr  = m_outSampleRate > 0 ? m_outSampleRate : 48000;
    int ch  = m_outChannels   > 0 ? m_outChannels   : 2;
    int bpf = 2 * ch;                       // S16: 2 bytes × channels

    /* 减去仍在 sink 硬件缓冲区里、还没真正播出的采样数 */
    long long buffered = 0;
    if (m_audioSink && bpf > 0) {
        qint64 b = qint64(m_audioSink->bufferSize())
                   - qint64(m_audioSink->bytesFree());
        if (b > 0) buffered = b / bpf;
    }
    long long played = qMax(0LL, written - buffered);

    double realSec = double(played) / double(sr);   // 实际经过的秒数
    double tempo   = m_playRate.load();
    return base + realSec * tempo;                   // 映射到媒体时间轴
}

/* ================================================================
 *  音频 filter 图 (atempo + aformat)
 * ================================================================ */

bool VideoPlayer::initAudioFilter(double tempo, int outSR)
{
    destroyAudioFilter();
    if (!m_aCodecCtx) return false;
    if (tempo <= 0.0) tempo = 1.0;
    if (outSR <= 0)
        outSR = m_aCodecCtx->sample_rate > 0 ? m_aCodecCtx->sample_rate : 48000;

    m_filterGraph = avfilter_graph_alloc();
    if (!m_filterGraph) return false;

    const AVFilter *abuf  = avfilter_get_by_name("abuffer");
    const AVFilter *asink = avfilter_get_by_name("abuffersink");
    if (!abuf || !asink) { destroyAudioFilter(); return false; }

    /* ---- 描述输入格式 ---- */
    char chLayout[128]{};
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 17, 0)
    av_channel_layout_describe(&m_aCodecCtx->ch_layout,
                               chLayout, sizeof(chLayout));
#else
    snprintf(chLayout, sizeof(chLayout), "stereo");
#endif

    char srcArgs[512];
    snprintf(srcArgs, sizeof(srcArgs),
             "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
             m_audioTB.num, m_audioTB.den,
             m_aCodecCtx->sample_rate,
             av_get_sample_fmt_name(m_aCodecCtx->sample_fmt),
             chLayout);

    if (avfilter_graph_create_filter(&m_filterSrc,  abuf,  "in",
                                     srcArgs, nullptr, m_filterGraph) < 0 ||
        avfilter_graph_create_filter(&m_filterSink, asink, "out",
                                     nullptr, nullptr, m_filterGraph) < 0) {
        destroyAudioFilter();
        return false;
    }

    /* ---- 构造 filter 链: aresample → atempo(s) → aformat ---- */
    QString atempoChain;
    double r = tempo;
    while (r > 2.0 + 1e-6) {
        if (!atempoChain.isEmpty()) atempoChain += ',';
        atempoChain += "atempo=2.0";
        r /= 2.0;
    }
    while (r < 0.5 - 1e-6) {
        if (!atempoChain.isEmpty()) atempoChain += ',';
        atempoChain += "atempo=0.5";
        r *= 2.0;
    }
    if (std::abs(r - 1.0) > 0.01) {
        if (!atempoChain.isEmpty()) atempoChain += ',';
        atempoChain += QString("atempo=%1").arg(r, 0, 'f', 6);
    }
    if (atempoChain.isEmpty()) atempoChain = "anull";

    QString desc = QString("aresample=%1,%2,"
                           "aformat=sample_fmts=s16:"
                           "channel_layouts=stereo:"
                           "sample_rates=%1")
                       .arg(outSR).arg(atempoChain);

    AVFilterInOut *outs = avfilter_inout_alloc();
    AVFilterInOut *ins  = avfilter_inout_alloc();
    outs->name       = av_strdup("in");
    outs->filter_ctx = m_filterSrc;
    outs->pad_idx    = 0;
    outs->next       = nullptr;
    ins->name        = av_strdup("out");
    ins->filter_ctx  = m_filterSink;
    ins->pad_idx     = 0;
    ins->next        = nullptr;

    int ret = avfilter_graph_parse_ptr(m_filterGraph,
                                       desc.toUtf8().constData(),
                                       &ins, &outs, nullptr);
    avfilter_inout_free(&ins);
    avfilter_inout_free(&outs);
    if (ret < 0) { destroyAudioFilter(); return false; }

    if (avfilter_graph_config(m_filterGraph, nullptr) < 0) {
        destroyAudioFilter();
        return false;
    }

    m_outSampleRate = outSR;
    m_outChannels   = 2;
    return true;
}

void VideoPlayer::destroyAudioFilter()
{
    if (m_filterGraph) {
        avfilter_graph_free(&m_filterGraph);
        m_filterGraph = nullptr;
        m_filterSrc   = nullptr;
        m_filterSink  = nullptr;
    }
}

/* ================================================================
 *  openFile — 打开文件, 初始化解码器, 不启动播放
 * ================================================================ */

bool VideoPlayer::openFile(const QString &filePath)
{
    stop();
    m_filePath = filePath;

    if (avformat_open_input(&m_fmtCtx,
                            filePath.toUtf8().constData(),
                            nullptr, nullptr) < 0)
        return false;

    if (avformat_find_stream_info(m_fmtCtx, nullptr) < 0) {
        avformat_close_input(&m_fmtCtx);
        return false;
    }

    /* 查找音视频流 (跳过封面图等附加图片流) */
    m_videoIdx = m_audioIdx = -1;
    for (unsigned i = 0; i < m_fmtCtx->nb_streams; ++i) {
        AVStream *s = m_fmtCtx->streams[i];
        auto t = s->codecpar->codec_type;
        if (t == AVMEDIA_TYPE_VIDEO && m_videoIdx < 0) {
            /* 跳过专辑封面 —— 它不是真正的视频流 */
            if (s->disposition & AV_DISPOSITION_ATTACHED_PIC)
                continue;
            m_videoIdx = i;
        }
        if (t == AVMEDIA_TYPE_AUDIO && m_audioIdx < 0) m_audioIdx = i;
    }
    if (m_videoIdx < 0 && m_audioIdx < 0) {
        avformat_close_input(&m_fmtCtx);
        return false;
    }
    m_audioOnly.store(m_videoIdx < 0);

    /* 打开视频解码器 */
    if (m_videoIdx >= 0) {
        auto *par = m_fmtCtx->streams[m_videoIdx]->codecpar;
        const AVCodec *c = avcodec_find_decoder(par->codec_id);
        if (!c) return false;
        m_vCodecCtx = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(m_vCodecCtx, par);
        if (avcodec_open2(m_vCodecCtx, c, nullptr) < 0) return false;
        m_videoTB = m_fmtCtx->streams[m_videoIdx]->time_base;
    }

    /* 打开音频解码器 */
    if (m_audioIdx >= 0) {
        auto *par = m_fmtCtx->streams[m_audioIdx]->codecpar;
        const AVCodec *c = avcodec_find_decoder(par->codec_id);
        if (c) {
            m_aCodecCtx = avcodec_alloc_context3(c);
            avcodec_parameters_to_context(m_aCodecCtx, par);
            if (avcodec_open2(m_aCodecCtx, c, nullptr) < 0) {
                avcodec_free_context(&m_aCodecCtx);
                m_audioIdx = -1;
                if (m_audioOnly.load()) {
                    avformat_close_input(&m_fmtCtx);
                    return false;
                }
            } else {
                m_audioTB = m_fmtCtx->streams[m_audioIdx]->time_base;
            }
        } else {
            m_audioIdx = -1;
        }
    }

    /* 重置状态 */
    m_audioBasePts.store(-1.0);
    m_audioWrittenSamples.store(0);
    m_lastKnownPos.store(0.0);
    m_eof.store(false);
    m_seekFlag.store(false);
    m_filterNeedReset.store(false);
    m_wallStarted.store(false);
    m_totalPausedMs.store(0);
    m_pauseStartMs.store(0);

    return true;
}

/* ================================================================
 *  play
 * ================================================================ */

void VideoPlayer::play()
{
    if (!m_fmtCtx) return;
    if (!m_vCodecCtx && !m_aCodecCtx) return;

    /* ---------- 从暂停恢复 ---------- */
    if (m_thread) {

        /* EOF 状态: 解码线程卡在等待循环, 只能通过 seek 唤醒
         * 标准行为: 从头播放 */
        if (m_eof.load()) {
            seek(0.0);
            m_paused.store(false);
            if (m_audioSink) {
                auto st = m_audioSink->state();
                if (st == QAudio::SuspendedState)
                    m_audioSink->resume();
                else if (st == QAudio::StoppedState)
                    m_audioIO = m_audioSink->start();
            }
            if (m_posTimer && m_audioOnly.load() && !m_posTimer->isActive())
                m_posTimer->start();
            if (m_flushTimer && !m_flushTimer->isActive())
                m_flushTimer->start();
            emit playingChanged(true);
            return;
        }

        /* 普通暂停恢复 */
        qint64 ps = m_pauseStartMs.load();
        if (m_wallStarted.load() && ps > 0) {
            m_totalPausedMs.fetch_add(m_wallClock.elapsed() - ps);
            m_pauseStartMs.store(0);
        }
        m_paused.store(false);

        /* 恢复音频设备 */
        if (m_audioSink) {
            auto st = m_audioSink->state();
            if (st == QAudio::SuspendedState)
                m_audioSink->resume();
            else if (st == QAudio::StoppedState)
                m_audioIO = m_audioSink->start();
        }

        if (m_posTimer && m_audioOnly.load() && !m_posTimer->isActive())
            m_posTimer->start();

        emit playingChanged(true);
        return;
    }

    /* ---------- 全新启动 ---------- */
    m_paused.store(false);
    m_stopFlag.store(false);
    m_playing.store(true);
    m_eof.store(false);
    emit playingChanged(true);

    /* ---- 创建音频输出 ---- */
    if (m_audioIdx >= 0 && m_aCodecCtx) {
        /* flush 定时器 */
        if (!m_flushTimer) {
            m_flushTimer = new QTimer(this);
            m_flushTimer->setInterval(10);
            connect(m_flushTimer, &QTimer::timeout,
                    this, &VideoPlayer::onFlushAudio);
        }
        m_flushTimer->start();

        /* QAudioSink */
        /* QAudioSink
         *
         * 始终使用固定安全采样率 (48000 → 44100 → 32000 回退), 不使用原始采样率.
         * 原因: Qt 内部重采样器在某些音频设备上会失败并报
         *   "qt.multimedia.audioresampler: Resampling failed"
         * 由 FFmpeg 的 aresample filter 统一完成重采样, Qt 层不再介入.
         */
        QAudioFormat fmt;
        fmt.setChannelCount(2);
        fmt.setSampleFormat(QAudioFormat::Int16);

        QAudioDevice dev = QMediaDevices::defaultAudioOutput();
        const QList<int> preferredRates = {48000, 44100, 32000};
        int chosenRate = 48000;
        for (int rate : preferredRates) {
            fmt.setSampleRate(rate);
            if (dev.isFormatSupported(fmt)) {
                chosenRate = rate;
                break;
            }
        }
        fmt.setSampleRate(chosenRate);
        qDebug() << "[play] 音频输出格式:" << chosenRate << "Hz / stereo / S16";

        if (m_audioSink) {
            delete m_audioSink;
            m_audioSink = nullptr;
            m_audioIO   = nullptr;
        }
        m_audioSink = new QAudioSink(dev, fmt, this);
        m_audioSink->setBufferSize(32768);          // ~170 ms
        m_audioSink->setVolume(m_volume.load() / 100.0);

        /* 设备空闲时额外触发一次 flush */
        connect(m_audioSink, &QAudioSink::stateChanged, this,
                [this](QAudio::State st) {
                    if (st == QAudio::IdleState)
                        QMetaObject::invokeMethod(
                            this, &VideoPlayer::onFlushAudio,
                            Qt::QueuedConnection);
                });

        m_audioIO = m_audioSink->start();
        if (m_audioIO) {
            m_outSampleRate = chosenRate;   // 以实际协商值为准
            m_outChannels   = fmt.channelCount();
            QMutexLocker lk(&m_filterMutex);
            initAudioFilter(m_playRate.load(), m_outSampleRate);
        } else {
            qWarning() << "[play] QAudioSink 启动失败, 禁用音频输出";
            delete m_audioSink;
            m_audioSink = nullptr;
        }

        m_audioBasePts.store(-1.0);
        m_audioWrittenSamples.store(0);
    }

    /* ---- 位置定时器 (纯音频) ---- */
    if (m_audioOnly.load()) {
        if (!m_posTimer) {
            m_posTimer = new QTimer(this);
            m_posTimer->setInterval(100);
            connect(m_posTimer, &QTimer::timeout,
                    this, &VideoPlayer::onUpdatePosition);
        }
        m_posTimer->start();
    }

    /* ---- 挂钟初始化 ---- */
    m_wallStarted.store(false);
    m_totalPausedMs.store(0);
    m_pauseStartMs.store(0);

    /* ---- 启动解码线程 ---- */
    m_thread = QThread::create([this]() { decodeLoop(); });
    m_thread->start();
}

/* ================================================================
 *  pause  — 可从任意线程安全调用
 * ================================================================ */

void VideoPlayer::pause()
{
    m_paused.store(true);

    if (m_wallStarted.load())
        m_pauseStartMs.store(m_wallClock.elapsed());

    /* QObject 操作必须在主线程 */
    auto fn = [this]() {
        if (m_audioSink && !m_eof.load())
            m_audioSink->suspend();
        if (m_posTimer && m_posTimer->isActive())
            m_posTimer->stop();
        emit playingChanged(false);
    };

    if (QThread::currentThread() == thread())
        fn();
    else
        QMetaObject::invokeMethod(this, fn, Qt::QueuedConnection);
}

/* ================================================================
 *  stop
 * ================================================================ */

void VideoPlayer::stop()
{
    m_stopFlag.store(true);
    m_paused.store(false);
    m_playing.store(false);
    m_eof.store(false);

    emit playingChanged(false);

    /* 先停定时器, 再等线程 */
    if (m_flushTimer) m_flushTimer->stop();
    if (m_posTimer)   m_posTimer->stop();

    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
        delete m_thread;
        m_thread = nullptr;
    }

    delete m_flushTimer; m_flushTimer = nullptr;
    delete m_posTimer;   m_posTimer   = nullptr;

    if (m_audioSink) {
        disconnect(m_audioSink, nullptr, this, nullptr);
        m_audioSink->stop();
        delete m_audioSink;
        m_audioSink = nullptr;
        m_audioIO   = nullptr;
    }

    { QMutexLocker lk(&m_audioQMutex); m_audioQ.clear(); }
    { QMutexLocker lk(&m_vqMutex);     m_vQueue.clear(); }

    m_audioWrittenSamples.store(0);
    m_audioBasePts.store(-1.0);

    closeFile();
}

/* ================================================================
 *  seek
 * ================================================================ */

void VideoPlayer::seek(double pos)
{
    if (!m_fmtCtx) return;

    { QMutexLocker lk(&m_vqMutex);    m_vQueue.clear(); }
    { QMutexLocker lk(&m_audioQMutex); m_audioQ.clear(); }

    m_audioBasePts.store(-1.0);
    m_audioWrittenSamples.store(0);

    /* 重启 sink 以清空硬件缓冲 */
    if (m_audioSink) {
        m_audioSink->stop();
        m_audioIO = m_audioSink->start();
    }

    m_seekTarget = pos;
    m_seekFlag.store(true);
    m_eof.store(false);
    m_lastKnownPos.store(pos);

    m_wallStarted.store(false);
    m_totalPausedMs.store(0);
    m_pauseStartMs.store(0);
}

/* ================================================================
 *  forward / setPlayRate / setRenderSize
 * ================================================================ */

void VideoPlayer::forward(double sec)
{
    if (!m_fmtCtx) return;
    double dur = getDuration();
    if (dur <= 0.0) return;

    /*
     * 使用 m_lastKnownPos 而非 audioClock():
     *   - audioClock() 在 seek 后返回 0 (base=-1), 导致位置计算错误
     *   - m_lastKnownPos 在 seek() 中被设为 seekTarget,
     *     在 positionChanged 中持续更新, 始终是可靠的当前位置
     */
    double cur = m_lastKnownPos.load();
    seek(qBound(0.0, cur + sec, dur));
}

void VideoPlayer::setPlayRate(double rate)
{
    if (rate <= 0.0) return;
    double oldRate = m_playRate.load();
    if (std::abs(oldRate - rate) < 1e-6) return;

    /* EOF: 仅存储速率, 下次 seek/play 时自动生效 */
    if (m_eof.load()) {
        m_playRate.store(rate);
        return;
    }

    /*
     * 用 m_lastKnownPos 获取当前位置:
     *   - 比 audioClock() 更可靠: 即使在 seek 未完成时也有正确值
     *   - m_lastKnownPos 由 positionChanged 和 seek() 持续维护
     */
    double cur = m_lastKnownPos.load();

    m_playRate.store(rate);

    /*
     * 直接 seek 到当前位置:
     *   - seek 会 flush 解码器、重建 filter (用新速率)、重启 sink、重置时钟
     *   - seek 设置 m_audioBasePts=-1 → hasAudioClock=false → 视频用挂钟同步
     *   - 不会出现 "音频时钟冻结导致视频阻塞解码线程" 的死锁
     *   - 代价: 约 50-100ms 的极短暂静音, 可接受
     */
    seek(cur);

    qDebug() << "[setPlayRate]" << oldRate << "->" << rate << "pos" << cur;
}

void VideoPlayer::setRenderSize(int w, int h)
{
    if (w <= 0 || h <= 0) return;
    m_renderW.store(w);
    m_renderH.store(h);
    m_swsNeedReset.store(true);
}

void VideoPlayer::setVolume(int vol)
{
    m_volume.store(qBound(0, vol, 100));
    // QAudioSink 操作必须在主线程
    auto fn = [this, vol]() {
        if (m_audioSink)
            m_audioSink->setVolume(qBound(0, vol, 100) / 100.0);
    };
    if (QThread::currentThread() == thread())
        fn();
    else
        QMetaObject::invokeMethod(this, fn, Qt::QueuedConnection);
}

/* ================================================================
 *  主线程定时器槽
 * ================================================================ */

void VideoPlayer::onFlushAudio()
{
    if (!m_audioIO) return;
    if (m_paused.load() && !m_eof.load()) return;

    int bpf = 2 * (m_outChannels > 0 ? m_outChannels : 2);

    /* 逐块写入, 不合并整个队列 — 避免大量内存拷贝 */
    while (true) {
        qint64 free = m_audioSink ? m_audioSink->bytesFree() : 0;
        if (free <= 0) break;

        QByteArray chunk;
        {
            QMutexLocker lk(&m_audioQMutex);
            if (m_audioQ.isEmpty()) break;
            chunk = m_audioQ.takeFirst();
        }

        qint64 toWrite = qMin(qint64(chunk.size()), free);
        qint64 w = m_audioIO->write(chunk.constData(), toWrite);
        if (w > 0) {
            m_audioWrittenSamples.fetch_add(w / bpf);
        } else {
            w = 0;
        }

        /* 未写完的部分放回队首 */
        if (w < chunk.size()) {
            QMutexLocker lk(&m_audioQMutex);
            m_audioQ.push_front(chunk.mid(w));
            break;
        }
    }
}

void VideoPlayer::onUpdatePosition()
{
    if (!m_audioOnly.load() || m_paused.load()) return;
    double pos = audioClock();
    double dur = getDuration();
    if (dur > 0.0 && pos > dur) pos = dur;
    m_lastKnownPos.store(pos);
    emit positionChanged(pos);
}

/* ================================================================
 *  资源释放
 * ================================================================ */

void VideoPlayer::closeFile()
{
    { QMutexLocker lk(&m_swsMutex);
        if (m_swsCtx) { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; } }
    { QMutexLocker lk(&m_filterMutex);
        destroyAudioFilter(); }

    if (m_vCodecCtx) { avcodec_free_context(&m_vCodecCtx); }
    if (m_aCodecCtx) { avcodec_free_context(&m_aCodecCtx); }
    if (m_fmtCtx)    { avformat_close_input(&m_fmtCtx); }
    m_videoIdx = m_audioIdx = -1;
}

/* ================================================================
 *  解码线程主循环
 * ================================================================ */

void VideoPlayer::decodeLoop()
{
    AVPacket *pkt   = av_packet_alloc();
    AVFrame  *frame = av_frame_alloc();
    AVFrame  *filt  = av_frame_alloc();     // filter 输出帧复用

    /* 音频解码路径的位置更新节流 */
    QElapsedTimer audioPosTimer;
    audioPosTimer.start();

    /* 虚假 EOF 恢复计数 (防止无限重试) */
    int eofRetryCount = 0;
    const int MAX_EOF_RETRIES = 3;

    /* ---------- 工具 lambda: 从 filter sink 读出所有可用帧并入队 ---------- */
    auto drainFilter = [&](double inputPts) {
        /* 先读出所有 filter 输出帧 (无需 mutex) */
        QList<QByteArray> batch;
        for (;;) {
            int r = av_buffersink_get_frame(m_filterSink, filt);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF || r < 0) break;

            int ch = 0;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 17, 0)
            ch = filt->ch_layout.nb_channels;
#else
            ch = filt->nb_channels;
#endif
            if (ch <= 0) ch = m_outChannels > 0 ? m_outChannels : 2;

            int bytes = av_samples_get_buffer_size(
                nullptr, ch, filt->nb_samples, AV_SAMPLE_FMT_S16, 1);
            if (bytes > 0 && filt->data[0]) {
                batch.append(QByteArray(
                    reinterpret_cast<const char *>(filt->data[0]), bytes));
            }
            av_frame_unref(filt);
        }

        /* 一次性入队 (单次 mutex) */
        if (!batch.isEmpty()) {
            QMutexLocker lk(&m_audioQMutex);
            for (const auto &chunk : batch) {
                if (m_audioQ.size() >= 50) break;
                m_audioQ.push_back(chunk);
                if (m_audioBasePts.load() < 0.0 && m_audioQ.size() == 1) {
                    m_audioBasePts.store(inputPts);
                    m_audioWrittenSamples.store(0);
                }
            }
        }
    };

    /* ========================= 主循环 ========================= */
    while (!m_stopFlag.load()) {

        /* ---- 暂停 ---- */
        if (m_paused.load()) {
            QThread::msleep(10);
            continue;
        }

        /* ---- Seek ---- */
        if (m_seekFlag.load()) {
            m_seekFlag.store(false);

            int64_t ts = int64_t(m_seekTarget * AV_TIME_BASE);
            av_seek_frame(m_fmtCtx, -1, ts, AVSEEK_FLAG_BACKWARD);

            if (m_vCodecCtx) avcodec_flush_buffers(m_vCodecCtx);
            if (m_aCodecCtx) avcodec_flush_buffers(m_aCodecCtx);

            { QMutexLocker lk(&m_vqMutex);    m_vQueue.clear(); }
            { QMutexLocker lk(&m_audioQMutex); m_audioQ.clear(); }

            /* 重建 filter (flush 后旧 graph 状态不可靠) */
            if (m_aCodecCtx) {
                QMutexLocker lk(&m_filterMutex);
                initAudioFilter(m_playRate.load(), m_outSampleRate);
            }

            m_audioBasePts.store(-1.0);
            m_audioWrittenSamples.store(0);
            m_wallStarted.store(false);
            m_totalPausedMs.store(0);
            m_pauseStartMs.store(0);
            /* 注意: 不在此处重置 eofRetryCount
             * EOF 恢复也走 seek 路径, 重置会导致计数器永远到不了上限.
             * 计数器由成功解码帧时重置 (在音频/视频处理段) */
            continue;
        }

        /* ---- 变速时重建 filter ---- */
        if (m_filterNeedReset.load()) {
            m_filterNeedReset.store(false);
            if (m_aCodecCtx) {
                QMutexLocker lk(&m_filterMutex);
                initAudioFilter(m_playRate.load(), m_outSampleRate);
            }
            /*
             * 丢弃在 setPlayRate 清空队列之后、filter 重建之前
             * 被旧 filter 产出并入队的残留数据
             */
            { QMutexLocker lk(&m_audioQMutex); m_audioQ.clear(); }
        }

        /* ---- 读包 ---- */
        int ret = av_read_frame(m_fmtCtx, pkt);

        if (ret < 0) {

            /* 只有 AVERROR_EOF 是真正的文件结束.
             * 其它错误 (AVERROR_INVALIDDATA 等) 是坏帧/坏包,
             * 跳过继续读下一个包即可. */
            if (ret != AVERROR_EOF) {
                av_packet_unref(pkt);
                continue;
            }

            /* 虚假 EOF 检测:
             * 某些音频文件 (MP3 Xing头错误、嵌入封面、拼接文件等)
             * 会在播放中途返回 AVERROR_EOF.
             * 如果当前位置离预期结尾还很远, 通过正常 seek 路径恢复.
             * 正常 seek 会: 清队列、重建 filter、重置时钟 — 不能跳过这些步骤,
             * 否则旧数据重放 + 时钟累积 → 位置超出时长. */
            {
                double dur = getDuration();
                double pos = m_lastKnownPos.load();
                if (dur > 1.0 && (dur - pos) > 2.0
                    && eofRetryCount < MAX_EOF_RETRIES) {
                    eofRetryCount++;
                    qDebug() << "[EOF recovery] pos=" << pos
                             << "dur=" << dur
                             << "retry=" << eofRetryCount;

                    /* 走完整的 seek 路径 (与正常 seek handler 一致) */
                    m_seekTarget = pos + 0.5;
                    m_seekFlag.store(true);

                    /* 重启 sink 清硬件缓冲 (主线程操作) */
                    QMetaObject::invokeMethod(this, [this]() {
                            if (m_audioSink) {
                                m_audioSink->stop();
                                m_audioIO = m_audioSink->start();
                            }
                        }, Qt::QueuedConnection);

                    av_packet_unref(pkt);
                    continue;   // → 循环顶部的 seek handler 处理剩余步骤
                }
            }
            eofRetryCount = 0;  // 真正的 EOF, 重置计数

            /* ====================== 真正的 EOF ====================== */

            /* 把 filter 中残余的音频全部冲出来 */
            if (m_aCodecCtx) {
                QMutexLocker lk(&m_filterMutex);
                if (m_filterSrc) {
                    int flushRet = av_buffersrc_add_frame(m_filterSrc, nullptr);
                    if (flushRet < 0)
                        qWarning() << "[EOF flush] av_buffersrc_add_frame 失败:" << flushRet;
                    /* 使用当前已知位置, 而非 0.0
                     * 避免 base PTS 还是 -1 时被错误设为 0 */
                    drainFilter(m_lastKnownPos.load());
                }
            }

            /* 等待音频队列 + sink 缓冲区播完 */
            if (m_audioIdx >= 0) {
                /* 确保 sink 处于播放状态 */
                QMetaObject::invokeMethod(this, [this]() {
                        if (!m_audioSink) return;
                        auto st = m_audioSink->state();
                        if (st == QAudio::SuspendedState) m_audioSink->resume();
                        else if (st == QAudio::StoppedState)
                            m_audioIO = m_audioSink->start();
                    }, Qt::QueuedConnection);
                QThread::msleep(30);    // 给主线程一点处理时间

                QElapsedTimer drainTimer;
                drainTimer.start();
                while (!m_stopFlag.load() && !m_seekFlag.load()
                       && drainTimer.elapsed() < 8000) {
                    int qs;
                    { QMutexLocker lk(&m_audioQMutex); qs = m_audioQ.size(); }
                    qint64 buf = m_audioSink
                                     ? (qint64(m_audioSink->bufferSize())
                                        - qint64(m_audioSink->bytesFree()))
                                     : 0;
                    if (qs == 0 && buf <= 0) break;
                    QThread::msleep(20);
                }
            }

            /* 通知主线程 (不直接调 pause / 不直接操作 QObject) */
            if (!m_eof.load()) {
                m_eof.store(true);
                m_paused.store(true);

                QMetaObject::invokeMethod(this, [this]() {
                        if (m_posTimer && m_posTimer->isActive())
                            m_posTimer->stop();
                        emit playingChanged(false);
                        emit finished();
                    }, Qt::QueuedConnection);
            }

            /* 停在这里等 seek 或 stop */
            while (!m_stopFlag.load() && !m_seekFlag.load())
                QThread::msleep(50);

            if (m_stopFlag.load()) break;
            continue;       // 处理 seek
        }

        /* ====================== 音频包 ====================== */
        if (pkt->stream_index == m_audioIdx && m_aCodecCtx) {

            if (avcodec_send_packet(m_aCodecCtx, pkt) == 0) {
                while (avcodec_receive_frame(m_aCodecCtx, frame) == 0) {

                    /* 成功解码出帧 → 说明上一次 EOF 恢复有效, 重置计数 */
                    eofRetryCount = 0;

                    /* 取 PTS */
                    double apts = 0.0;
                    if (frame->pts != AV_NOPTS_VALUE)
                        apts = frame->pts * av_q2d(m_audioTB);
                    else if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                        apts = frame->best_effort_timestamp * av_q2d(m_audioTB);

                    /* 送入 filter */
                    {
                        QMutexLocker lk(&m_filterMutex);
                        if (m_filterSrc && m_filterSink) {
                            if (av_buffersrc_add_frame_flags(
                                    m_filterSrc, frame,
                                    AV_BUFFERSRC_FLAG_KEEP_REF) >= 0) {
                                drainFilter(apts);
                            }
                        }
                    }
                    av_frame_unref(frame);

                    /* 音频模式下定期发射位置更新 (每 ~100ms)
                     * 即使 m_audioOnly 为 false (如视频帧极稀疏),
                     * 也保证进度条跟踪音频时钟 */
                    if (audioPosTimer.elapsed() >= 100) {
                        audioPosTimer.restart();
                        double pos = audioClock();
                        double dur = getDuration();
                        if (dur > 0.0 && pos > dur) pos = dur;
                        if (pos > 0.0) {
                            m_lastKnownPos.store(pos);
                            emit positionChanged(pos);
                        }
                    }

                    /* 背压: 队列过大时降速 */
                    // 替换原有的背压逻辑
                    {
                        int bpf = 2 * (m_outChannels > 0 ? m_outChannels : 2);
                        int sr  = m_outSampleRate > 0 ? m_outSampleRate : 48000;
                        int totalBytes = 0;
                        {
                            QMutexLocker lk(&m_audioQMutex);
                            for (const auto &c : m_audioQ) totalBytes += c.size();
                        }
                        double bufferedSec = (bpf > 0 && sr > 0)
                                                 ? double(totalBytes) / double(bpf * sr)
                                                 : 0.0;
                        // 超过 2 秒才限速，超过 1 秒轻微限速
                        if      (bufferedSec > 2.0) QThread::msleep(50);
                        else if (bufferedSec > 1.0) QThread::msleep(10);
                    }
                }
            }

            av_packet_unref(pkt);
            continue;
        }

        /* ====================== 视频包 ====================== */
        if (!m_audioOnly.load()
            && pkt->stream_index == m_videoIdx
            && m_vCodecCtx) {

            if (avcodec_send_packet(m_vCodecCtx, pkt) == 0) {
                while (avcodec_receive_frame(m_vCodecCtx, frame) == 0) {

                    /* 成功解码出帧 → 重置虚假 EOF 计数 */
                    eofRetryCount = 0;

                    /* 取 PTS */
                    double vpts = 0.0;
                    if (frame->pts != AV_NOPTS_VALUE)
                        vpts = frame->pts * av_q2d(m_videoTB);
                    else if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                        vpts = frame->best_effort_timestamp
                               * av_q2d(m_videoTB);

                    /* ========== A/V 同步 ========== */
                    bool display = true;
                    bool hasAudioClock =
                        (m_audioIdx >= 0 && m_aCodecCtx
                         && m_audioSink
                         && m_audioBasePts.load() >= 0.0);

                    if (hasAudioClock) {
                        /*
                         * 音频主时钟同步:
                         *   diff > 0: 视频超前 → 等
                         *   diff < 0: 视频落后 → 丢
                         */
                        constexpr double SYNC_OK   = 0.04;   // 40 ms
                        constexpr double DROP_LATE = 0.10;    // 100 ms

                        double apos = audioClock();
                        double diff = vpts - apos;

                        if (diff > 0.5) {
                            // 直接显示，不等待
                        }else if (diff < -DROP_LATE) {
                            display = false;        // 丢帧追赶
                        } else if (diff > SYNC_OK) {
                            int waitMs = qMin(int(diff * 1000), 200);
                            int waited = 0;
                            double lastApos = apos;
                            while (waited < waitMs
                                   && !m_stopFlag.load()
                                   && !m_seekFlag.load()
                                   && !m_paused.load()) {
                                QThread::msleep(5);
                                waited += 5;
                                apos = audioClock();
                                if (vpts - apos <= SYNC_OK) break;
                                /*
                                 * 安全阀: 音频时钟 40ms 内没有前进 →
                                 * 音频可能卡住 (filter 初始缓冲 / sink 异常),
                                 * 立即退出等待, 避免阻塞解码线程导致死锁
                                 */
                                if (waited >= 40
                                    && std::abs(apos - lastApos) < 1e-4)
                                    break;
                            }
                        }
                    } else {
                        /*
                         * 挂钟回退 (纯视频 / 音频尚未就绪):
                         *   elapsed = 挂钟 - 暂停累积
                         *   target  = (vpts - base) / rate
                         *   等待 target - elapsed
                         */
                        if (!m_wallStarted.load()) {
                            m_wallBasePts = vpts;
                            m_wallClock.start();
                            m_totalPausedMs.store(0);
                            m_pauseStartMs.store(0);
                            m_wallStarted.store(true);
                        }

                        qint64 elapsed =
                            m_wallClock.elapsed() - m_totalPausedMs.load();
                        double rate = m_playRate.load();
                        double target =
                            (vpts - m_wallBasePts) / rate * 1000.0;
                        double wait = target - double(elapsed);
                        if (wait > 0)
                            QThread::msleep(
                                static_cast<unsigned long>(
                                    qMin(wait, 200.0)));
                    }

                    /* ---------- 缩放 + 显示 ---------- */
                    if (display) {
                        int dstW = m_renderW.load();
                        int dstH = m_renderH.load();
                        if (dstW <= 0 || dstH <= 0) {
                            dstW = m_vCodecCtx->width;
                            dstH = m_vCodecCtx->height;
                        }

                        {
                            QMutexLocker lk(&m_swsMutex);
                            if (!m_swsCtx || m_swsNeedReset.load()) {
                                if (m_swsCtx) sws_freeContext(m_swsCtx);
                                m_swsCtx = sws_getContext(
                                    m_vCodecCtx->width,
                                    m_vCodecCtx->height,
                                    m_vCodecCtx->pix_fmt,
                                    dstW, dstH, AV_PIX_FMT_RGB24,
                                    m_swsAlgo.load(),
                                    nullptr, nullptr, nullptr);
                                if (!m_swsCtx)
                                    m_swsCtx = sws_getContext(
                                        m_vCodecCtx->width,
                                        m_vCodecCtx->height,
                                        m_vCodecCtx->pix_fmt,
                                        dstW, dstH, AV_PIX_FMT_RGB24,
                                        SWS_FAST_BILINEAR,
                                        nullptr, nullptr, nullptr);
                                m_swsNeedReset.store(false);
                            }
                        }

                        QImage img(dstW, dstH, QImage::Format_RGB888);
                        uint8_t *dst[4] = {
                                           img.bits(), nullptr, nullptr, nullptr };
                        int linesize[4] = {
                                           int(img.bytesPerLine()), 0, 0, 0 };
                        sws_scale(m_swsCtx,
                                  frame->data, frame->linesize,
                                  0, m_vCodecCtx->height,
                                  dst, linesize);

                        {
                            QMutexLocker lk(&m_vqMutex);
                            while (m_vQueue.size() >= 20) m_vQueue.dequeue();
                            m_vQueue.enqueue({img, vpts});
                        }
                        emit frameReady(img);
                    }

                    /* 进度条: 有音频时用音频时钟, 否则用 vpts */
                    {
                        double pos = hasAudioClock ? audioClock() : vpts;
                        double dur = getDuration();
                        if (dur > 0.0 && pos > dur) pos = dur;
                        m_lastKnownPos.store(pos);
                        emit positionChanged(pos);
                    }

                    av_frame_unref(frame);
                }
            }
        }

        av_packet_unref(pkt);
    }
    /* ========================= 退出清理 ========================= */

    av_frame_free(&filt);
    av_frame_free(&frame);
    av_packet_free(&pkt);

    { QMutexLocker lk(&m_swsMutex);
        if (m_swsCtx) { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; } }
    { QMutexLocker lk(&m_filterMutex);
        destroyAudioFilter(); }
}
