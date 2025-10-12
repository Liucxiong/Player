#include "videoplayer.h"
#include <QThread>
#include <QMutexLocker>
#include <QDebug>

// ---------------- constructor / destructor ----------------
VideoPlayer::VideoPlayer(QObject *parent)
    : QObject(parent)
{
    av_log_set_level(AV_LOG_QUIET);
}

VideoPlayer::~VideoPlayer()
{
    stop();
}

// ---------------- helpers ----------------
double VideoPlayer::videoPtsToSeconds(AVFrame *vframe)
{
    if (!vframe || videoStreamIndex < 0 || !fmtCtx) return 0.0;
    AVRational tb = fmtCtx->streams[videoStreamIndex]->time_base;
    if (vframe->pts != AV_NOPTS_VALUE) return vframe->pts * av_q2d(tb);
    if (vframe->best_effort_timestamp != AV_NOPTS_VALUE) return vframe->best_effort_timestamp * av_q2d(tb);
    return 0.0;
}

// ---------------- openFile ----------------
bool VideoPlayer::openFile(const QString &filePath)
{
    m_filePath = filePath;
    stop();

    if (avformat_open_input(&fmtCtx, filePath.toStdString().c_str(), nullptr, nullptr) < 0) {
        qWarning() << "无法打开视频文件:" << filePath;
        return false;
    }
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        qWarning() << "无法读取流信息";
        avformat_close_input(&fmtCtx);
        fmtCtx = nullptr;
        return false;
    }
    videoStreamIndex = -1;
    audioStreamIndex = -1;
    for (unsigned i = 0; i < fmtCtx->nb_streams; ++i) {
        AVCodecParameters *p = fmtCtx->streams[i]->codecpar;
        if (p->codec_type == AVMEDIA_TYPE_VIDEO && videoStreamIndex < 0) videoStreamIndex = int(i);
        if (p->codec_type == AVMEDIA_TYPE_AUDIO && audioStreamIndex < 0) audioStreamIndex = int(i);
    }
    if (videoStreamIndex < 0) {
        qWarning() << "没有找到视频流";
        avformat_close_input(&fmtCtx);
        fmtCtx = nullptr;
        return false;
    }
    // 视频解码上下文
    {
        AVCodecParameters *vpar = fmtCtx->streams[videoStreamIndex]->codecpar;
        const AVCodec *vcodec = avcodec_find_decoder(vpar->codec_id);
        if (!vcodec) { qWarning() << "未找到视频解码器"; return false; }
        codecCtx = avcodec_alloc_context3(vcodec);
        if (!codecCtx) { qWarning() << "无法分配视频 codecCtx"; return false; }
        if (avcodec_parameters_to_context(codecCtx, vpar) < 0) { qWarning() << "avcodec_parameters_to_context fail"; return false; }
        if (avcodec_open2(codecCtx, vcodec, nullptr) < 0) { qWarning() << "视频解码器打开失败"; return false; }

        videoTimeBase = fmtCtx->streams[videoStreamIndex]->time_base;
    }

    // 音频解码上下文（可选）
    if (audioStreamIndex >= 0) {
        AVCodecParameters *apar = fmtCtx->streams[audioStreamIndex]->codecpar;
        const AVCodec *acodec = avcodec_find_decoder(apar->codec_id);
        if (acodec) {
            audioCodecCtx = avcodec_alloc_context3(acodec);
            if (audioCodecCtx && avcodec_parameters_to_context(audioCodecCtx, apar) >= 0) {
                if (avcodec_open2(audioCodecCtx, acodec, nullptr) < 0) {
                    qWarning() << "音频解码器打开失败，忽略音频";
                    avcodec_free_context(&audioCodecCtx);
                    audioCodecCtx = nullptr;
                    audioStreamIndex = -1;
                } else {
                    audioTimeBase = fmtCtx->streams[audioStreamIndex]->time_base;
                }
            } else {
                if (audioCodecCtx) avcodec_free_context(&audioCodecCtx);
                audioCodecCtx = nullptr;
                audioStreamIndex = -1;
            }
        } else {
            audioStreamIndex = -1;
        }
    }

    frame = av_frame_alloc();
    packet = av_packet_alloc();

    // reset audio tracking and queues
    m_audioBasePts.store(-1.0);
    m_audioPlayedSamples.store(0);
    {
        QMutexLocker locker(&m_audioQueueMutex);
        m_audioQueue.clear();
    }

    m_finished.store(false);
    m_seekRequested.store(false);
    m_playStarted = false;
    m_totalPausedMs.store(0);
    m_pauseStartMs = 0;

    return true;
}

// ---------------- play / pause / stop / seek ----------------
void VideoPlayer::play()
{
    if (!fmtCtx || !codecCtx) return;

    // 如果已经有 decode 线程，只是恢复播放
    if (m_decodeThread) {
        // 处理暂停期间累计时间：如果 play 已经开始并且 pause 有记录，累加 paused 时长
        if (m_playStarted && m_pauseStartMs > 0) {
            qint64 now = m_playTimer.elapsed();
            qint64 pausedMs = now - m_pauseStartMs;
            if (pausedMs > 0) m_totalPausedMs.fetch_add(pausedMs);
            m_pauseStartMs = 0;
        }

        m_paused.store(false);
        if (audioSink) audioSink->resume();
        emit playingChanged(true);
        return;
    }

    m_paused.store(false);
    m_stopRequested.store(false);
    m_playing.store(true);
    emit playingChanged(true);

    // 在主线程创建并启动音频 flush timer 与 QAudioSink（如果有音频）
    if (audioStreamIndex >= 0 && audioCodecCtx) {
        // 创建定时器（只创建一次）
        if (!m_audioFlushTimer) {
            m_audioFlushTimer = new QTimer(this);
            m_audioFlushTimer->setInterval(15); // 15 ms 刷新一次
            connect(m_audioFlushTimer, &QTimer::timeout, this, &VideoPlayer::flushAudioBuffer);
            m_audioFlushTimer->start();
        }
        // 停掉并删除旧的 audioSink（如果存在）
        if (audioSink) {
            audioSink->stop();
            delete audioSink;
            audioSink = nullptr;
            audioIODevice = nullptr;
        }
        // 构造目标输出格式，输出采样率根据播放速率调整
        QAudioFormat fmt;
        int desiredSampleRate = qMax(1, int(audioCodecCtx->sample_rate * m_playRate.load()));
        fmt.setSampleRate(desiredSampleRate);
        fmt.setChannelCount(2);
        fmt.setSampleFormat(QAudioFormat::Int16);

        QAudioDevice device = QMediaDevices::defaultAudioOutput();

        // 如果设备不支持该格式，尝试回退到原始采样率，再尝试再回退到常见采样率
        if (!device.isFormatSupported(fmt)) {
            qWarning() << "Requested audio format not supported (sampleRate =" << fmt.sampleRate() << "), trying fallback.";
            // 先尝试原始采样率
            fmt.setSampleRate(audioCodecCtx->sample_rate);
            if (!device.isFormatSupported(fmt)) {
                // 最后退回 48000 或 44100 常见值
                fmt.setSampleRate(48000);
                if (!device.isFormatSupported(fmt)) {
                    fmt.setSampleRate(44100);
                }
            }
        }
        // 创建新的 audioSink
        audioSink = new QAudioSink(device, fmt, this);
        audioIODevice = audioSink->start();
        if (!audioIODevice) {
            qWarning() << "audioSink start failed";
            delete audioSink;
            audioSink = nullptr;
            audioIODevice = nullptr;
        } else {
            // 更新追踪变量
            m_audioSampleRate = fmt.sampleRate();
            m_audioOutChannels = fmt.channelCount();
        }

        // 重置音频播放追踪
        m_audioBasePts.store(-1.0);
        m_audioPlayedSamples.store(0);
        // 重要：由于我们改变了输出采样率，确保 swrCtx 在解码线程中重新初始化
        {
            QMutexLocker locker(&m_swrMutex);
            if (swrCtx) {
                swr_free(&swrCtx);
                swrCtx = nullptr;
            }
        }
    }

    // 启动解码线程
    m_playStarted = false;
    m_totalPausedMs.store(0);
    m_pauseStartMs = 0;
    m_decodeThread = QThread::create([this]() { decodeLoop(); });
    m_decodeThread->start();
}


void VideoPlayer::pause()
{
    // 记录 pause 起始的 wall-clock（只有在播放已经开始后才记录）
    if (m_playStarted) {
        m_pauseStartMs = m_playTimer.elapsed();
    } else {
        m_pauseStartMs = 0;
    }

    m_paused.store(true);
    if (audioSink) audioSink->suspend();
    emit playingChanged(false);
}

void VideoPlayer::stop()
{
    m_stopRequested.store(true);
    m_paused.store(false);
    m_playing.store(false);
    m_finished.store(false);
    m_seekRequested.store(false);

    emit playingChanged(false);

    if (m_decodeThread) {
        m_decodeThread->quit();
        m_decodeThread->wait();
        delete m_decodeThread;
        m_decodeThread = nullptr;
    }

    if (m_audioFlushTimer) {
        m_audioFlushTimer->stop();
        delete m_audioFlushTimer;
        m_audioFlushTimer = nullptr;
    }

    if (audioSink) {
        audioSink->stop();
        delete audioSink;
        audioSink = nullptr;
        audioIODevice = nullptr;
    }

    clearQueue();
    freeFFmpegResources();
}

void VideoPlayer::seek(double positionSec)
{
    if (!fmtCtx) return;

    m_paused.store(true);

    // 清空视频帧队列
    {
        QMutexLocker locker(&m_mutex);
        m_frameQueue.clear();
    }

    // 清空音频队列
    {
        QMutexLocker aLocker(&m_audioQueueMutex);
        m_audioQueue.clear();
    }

    // 重置音频播放起点
    m_audioBasePts.store(positionSec);
    m_audioPlayedSamples.store(0);

    // 停止当前音频输出并刷新缓冲
    if (audioSink) {
        audioSink->stop();
        audioIODevice = audioSink->start();  // 重启输出
    }

    m_seekTargetSec = positionSec;
    m_seekRequested.store(true);
    m_finished.store(false);

    m_playStarted = false;
    m_totalPausedMs.store(0);
    m_pauseStartMs = 0;

    m_paused.store(false);
    emit playingChanged(true);
}


// ---------------- decodeLoop ----------------
void VideoPlayer::decodeLoop()
{
    // swsCtx is member; swrCtx for audio
    swrCtx = nullptr;

    while (!m_stopRequested.load()) {
        if (m_paused.load()) { QThread::msleep(10); continue; }

        // ------------------ 处理跳转 ------------------
        if (m_seekRequested.load()) {
            int64_t ts = static_cast<int64_t>(m_seekTargetSec * AV_TIME_BASE);
            av_seek_frame(fmtCtx, -1, ts, AVSEEK_FLAG_BACKWARD);
            if (codecCtx) avcodec_flush_buffers(codecCtx);
            if (audioCodecCtx) avcodec_flush_buffers(audioCodecCtx);

            {
                QMutexLocker locker(&m_mutex);
                m_frameQueue.clear();
            }
            {
                QMutexLocker aLocker(&m_audioQueueMutex);
                m_audioQueue.clear();
            }

            m_audioBasePts.store(m_seekTargetSec);
            m_audioPlayedSamples.store(0);
            m_seekRequested.store(false);
            m_playStarted = false;
            m_totalPausedMs.store(0);
            m_pauseStartMs = 0;
        }

        int ret = av_read_frame(fmtCtx, packet);
        if (ret < 0) {
            if (!m_finished.load()) {
                m_finished.store(true);
                emit playingChanged(false);
                emit finished();
            }
            QThread::msleep(20);
            continue;
        }

        // ------------------ 处理音频 ------------------
        if (audioStreamIndex >= 0 && packet->stream_index == audioStreamIndex && audioCodecCtx) {
            if (avcodec_send_packet(audioCodecCtx, packet) == 0) {
                AVFrame *aframe = av_frame_alloc();
                while (avcodec_receive_frame(audioCodecCtx, aframe) == 0) {
                    double apts = 0.0;
                    if (aframe->pts != AV_NOPTS_VALUE) apts = aframe->pts * av_q2d(audioTimeBase);
                    else if (aframe->best_effort_timestamp != AV_NOPTS_VALUE) apts = aframe->best_effort_timestamp * av_q2d(audioTimeBase);

                    // 初始化或重建 swrCtx
                    if (!swrCtx) {
                        swrCtx = swr_alloc();
                        if (!swrCtx) { qWarning() << "swr_alloc failed"; }
                        else {
                            AVChannelLayout inLayout;
                            av_channel_layout_copy(&inLayout, &audioCodecCtx->ch_layout);
                            AVChannelLayout outLayout;
                            av_channel_layout_default(&outLayout, 2);

                            int out_sample_rate = int(audioCodecCtx->sample_rate * m_playRate.load());
                            int r = swr_alloc_set_opts2(&swrCtx,
                                                        &outLayout,
                                                        AV_SAMPLE_FMT_S16,
                                                        out_sample_rate,
                                                        &inLayout,
                                                        audioCodecCtx->sample_fmt,
                                                        audioCodecCtx->sample_rate,
                                                        0, nullptr);
                            av_channel_layout_uninit(&inLayout);
                            av_channel_layout_uninit(&outLayout);

                            if (r < 0 || swr_init(swrCtx) < 0) {
                                qWarning() << "swr init failed";
                                swr_free(&swrCtx);
                                swrCtx = nullptr;
                            } else {
                                m_audioSampleRate = out_sample_rate;
                                m_audioOutChannels = 2;
                                m_audioBasePts.store(-1.0);
                                m_audioPlayedSamples.store(0);
                            }
                        }
                    }

                    if (swrCtx) {
                        int out_sample_rate = m_audioSampleRate;
                        int64_t out_nb_samples = av_rescale_rnd(
                            swr_get_delay(swrCtx, audioCodecCtx->sample_rate) + aframe->nb_samples,
                            out_sample_rate,
                            audioCodecCtx->sample_rate,
                            AV_ROUND_UP);

                        uint8_t **out = nullptr;
                        av_samples_alloc_array_and_samples(&out, nullptr, m_audioOutChannels,
                                                           (int)out_nb_samples, AV_SAMPLE_FMT_S16, 0);

                        int converted = swr_convert(swrCtx, out, (int)out_nb_samples,
                                                    (const uint8_t**)aframe->data, aframe->nb_samples);
                        if (converted > 0) {
                            int bytes = av_samples_get_buffer_size(nullptr, m_audioOutChannels, converted, AV_SAMPLE_FMT_S16, 1);
                            if (bytes > 0) {
                                QByteArray chunk(reinterpret_cast<const char*>(out[0]), bytes);
                                {
                                    QMutexLocker aLocker(&m_audioQueueMutex);
                                    m_audioQueue.push_back(chunk);
                                }
                                double base = m_audioBasePts.load();
                                if (base < 0.0) {
                                    m_audioBasePts.store(apts);
                                    m_audioPlayedSamples.store(0);
                                }
                            }
                        }

                        if (out) {
                            av_freep(&out[0]);
                            av_freep(&out);
                        }
                    }
                }
                av_frame_free(&aframe);
            }
            av_packet_unref(packet);
            continue;
        }

        // ------------------ 处理视频 ------------------
        if (packet->stream_index == videoStreamIndex) {
            if (avcodec_send_packet(codecCtx, packet) == 0) {
                while (avcodec_receive_frame(codecCtx, frame) == 0) {
                    double vpts = 0.0;
                    if (frame->pts != AV_NOPTS_VALUE) vpts = frame->pts * av_q2d(videoTimeBase);
                    else if (frame->best_effort_timestamp != AV_NOPTS_VALUE) vpts = frame->best_effort_timestamp * av_q2d(videoTimeBase);

                    int dstW = m_renderWidth.load();
                    int dstH = m_renderHeight.load();
                    if (dstW <= 0 || dstH <= 0) {
                        dstW = codecCtx->width;
                        dstH = codecCtx->height;
                    }

                    // ------------------ 安全重建 swsCtx ------------------
                    {
                        QMutexLocker locker(&m_swsMutex);
                        if (!swsCtx || m_swsCtxNeedReset.load()) {
                            if (swsCtx) {
                                sws_freeContext(swsCtx);
                                swsCtx = nullptr;
                            }

                            swsCtx = sws_getContext(codecCtx->width, codecCtx->height, codecCtx->pix_fmt,
                                                    dstW, dstH, AV_PIX_FMT_RGB24,
                                                    SWS_LANCZOS, nullptr, nullptr, nullptr);
                            if (!swsCtx) {
                                swsCtx = sws_getContext(codecCtx->width, codecCtx->height, codecCtx->pix_fmt,
                                                        dstW, dstH, AV_PIX_FMT_RGB24,
                                                        SWS_BICUBIC, nullptr, nullptr, nullptr);
                            }
                            m_swsCtxNeedReset.store(false);
                        }
                    }

                    QImage img(dstW, dstH, QImage::Format_RGB888);
                    uint8_t *dst[4] = { img.bits(), nullptr, nullptr, nullptr };
                    int dst_linesize[4] = { static_cast<int>(img.bytesPerLine()), 0, 0, 0 };

                    sws_scale(swsCtx, frame->data, frame->linesize, 0, codecCtx->height, dst, dst_linesize);

                    // ------------------ 时间控制 ------------------
                    if (!m_playStarted) {
                        m_playStartPts = vpts;
                        m_playTimer.start();
                        m_totalPausedMs.store(0);
                        m_pauseStartMs = 0;
                        m_playStarted = true;
                    }

                    qint64 elapsedMsRaw = m_playTimer.elapsed();
                    qint64 totalPaused = m_totalPausedMs.load();
                    if (m_pauseStartMs > 0) {
                        qint64 now = m_playTimer.elapsed();
                        elapsedMsRaw -= now - m_pauseStartMs;
                    } else {
                        elapsedMsRaw -= totalPaused;
                    }

                    double rate = m_playRate.load();
                    qint64 targetMs = qint64((vpts - m_playStartPts) * 1000.0 / rate);
                    qint64 waitMs = targetMs - elapsedMsRaw;
                    if (waitMs > 0) {
                        if (waitMs > 200) waitMs = 200;
                        QThread::msleep(waitMs);
                    }

                    {
                        QMutexLocker locker(&m_mutex);
                        if (m_frameQueue.size() >= 10) m_frameQueue.dequeue();
                        m_frameQueue.enqueue(std::make_pair(img, vpts));
                    }

                    emit frameReady(img);
                    emit positionChanged(vpts);
                }
            }
        }

        av_packet_unref(packet);
    }

    // ------------------ 线程结束时释放 swsCtx ------------------
    {
        QMutexLocker locker(&m_swsMutex);
        if (swsCtx) {
            sws_freeContext(swsCtx);
            swsCtx = nullptr;
        }
    }
}



// ---------------- flushAudioBuffer (main thread) ----------------
void VideoPlayer::flushAudioBuffer()
{
    if (!audioIODevice) return;

    // 如果当前处于暂停，不写入音频设备，避免误计已播放样本
    if (m_paused.load()) return;

    QByteArray all;
    {
        QMutexLocker locker(&m_audioQueueMutex);
        if (m_audioQueue.empty()) return;
        for (const QByteArray &b : m_audioQueue) all.append(b);
        m_audioQueue.clear();
    }

    if (all.isEmpty()) return;

    qint64 written = audioIODevice->write(all);
    if (written > 0) {
        int bytesPerSample = 2; // S16 => 2 bytes per sample per channel
        long long samplesWritten = written / (bytesPerSample * m_audioOutChannels);
        m_audioPlayedSamples.fetch_add(samplesWritten);
    } else {
        // 写入失败或 0，忽略，下次重试
    }
}

// ---------------- clear / free ----------------
void VideoPlayer::clearQueue()
{
    QMutexLocker locker(&m_mutex);
    m_frameQueue.clear();
    QMutexLocker aLocker(&m_audioQueueMutex);
    m_audioQueue.clear();
    m_audioPlayedSamples.store(0);
    m_audioBasePts.store(-1.0);
    m_totalPausedMs.store(0);
    m_pauseStartMs = 0;
}

void VideoPlayer::freeFFmpegResources()
{
    if (packet) { av_packet_free(&packet); packet = nullptr; }
    if (frame) { av_frame_free(&frame); frame = nullptr; }
    if (swrCtx) { swr_free(&swrCtx); swrCtx = nullptr; }
    if (codecCtx) { avcodec_free_context(&codecCtx); codecCtx = nullptr; }
    if (audioCodecCtx) { avcodec_free_context(&audioCodecCtx); audioCodecCtx = nullptr; }
    if (fmtCtx) { avformat_close_input(&fmtCtx); fmtCtx = nullptr; }
    if (swsCtx) { sws_freeContext(swsCtx); swsCtx = nullptr; }

}
/**
 * @brief 跳过或回退n秒
 * @param seconds
 */
void VideoPlayer::forward(double seconds)
{
    if (!fmtCtx || videoStreamIndex < 0) return;

    const AVStream* vs = fmtCtx->streams[videoStreamIndex];
    double durationSec = vs->duration * av_q2d(vs->time_base);

    double currentPos = 0.0;
    if (!m_frameQueue.isEmpty()) {
        currentPos = m_frameQueue.back().second;  // 最新帧的 pts
    }

    double newPos = currentPos + seconds;
    if (newPos > durationSec) newPos = durationSec;

    seek(newPos);
}

void VideoPlayer::setPlayRate(double rate)
{
    if (rate <= 0.0) return;
    m_playRate.store(rate);

    // 变速时需要重建音频输出及重置 swrCtx，避免残留缓冲/采样率不一致
    // 重建 QAudioSink（如果已经有 audioCodecCtx）
    if (audioCodecCtx) {
        // stop existing sink and delete
        if (audioSink) {
            audioSink->stop();
            delete audioSink;
            audioSink = nullptr;
            audioIODevice = nullptr;
        }

        // create new QAudioSink with changed sample rate
        QAudioFormat fmt;
        fmt.setSampleRate(int(audioCodecCtx->sample_rate * m_playRate.load()));
        fmt.setChannelCount(2);
        fmt.setSampleFormat(QAudioFormat::Int16);

        QAudioDevice device = QMediaDevices::defaultAudioOutput();
        audioSink = new QAudioSink(device, fmt, this);
        audioIODevice = audioSink->start();
        if (!audioIODevice) {
            qWarning() << "audioSink start failed after setPlayRate";
            if (audioSink) { delete audioSink; audioSink = nullptr; audioIODevice = nullptr; }
        } else {
            m_audioSampleRate = fmt.sampleRate();
            m_audioOutChannels = 2;
        }
    }

    // reset and free swrCtx so that decodeLoop will re-init with new out sample rate
    if (swrCtx) {
        swr_free(&swrCtx);
        swrCtx = nullptr;
    }
}


void VideoPlayer::setRenderSize(int w, int h)
{
    if (w <= 0 || h <= 0) return;

    m_renderWidth.store(w);
    m_renderHeight.store(h);

    // 通知 decodeLoop 需要重建 swsCtx，而不是主线程直接释放
    m_swsCtxNeedReset.store(true);
}


