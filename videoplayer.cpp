#include "videoplayer.h"
#include <QThread>
#include <QMutexLocker>
#include <QDebug>
#include <cmath>

// ---------------- constructor / destructor ----------------
VideoPlayer::VideoPlayer(QObject *parent)
    : QObject(parent)
{
    av_log_set_level(AV_LOG_QUIET);
    // 在新版 FFmpeg 中一般不再需要显式注册，但调用无害
    // avfilter_register_all();
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

// ---------------- audio filter init / cleanup ----------------
bool VideoPlayer::initAudioFilter(double rate)
{
    // 必须在调用之前持有 m_audioFilterMutex
    if (audioFilterGraph) {
        avfilter_graph_free(&audioFilterGraph);
        audioFilterGraph = nullptr;
        audioBufferSrcCtx = nullptr;
        audioBufferSinkCtx = nullptr;
    }

    if (!audioCodecCtx) {
        qWarning() << "No audio codec context";
        return false;
    }

    if (rate <= 0.0) rate = 1.0;

    audioFilterGraph = avfilter_graph_alloc();
    if (!audioFilterGraph) {
        qWarning() << "Failed to allocate audio filter graph";
        return false;
    }

    const AVFilter *abuffer = avfilter_get_by_name("abuffer");
    const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
    if (!abuffer || !abuffersink) {
        qWarning() << "Audio filters (abuffer/abuffersink) not found";
        cleanupAudioFilter();
        return false;
    }

    // prepare abuffer args: sample_fmt name, sample_rate, channel_layout, time_base
    char channel_layout_str[128];
    av_channel_layout_describe(&audioCodecCtx->ch_layout, channel_layout_str, sizeof(channel_layout_str));

    char args[512];
    // Use audioTimeBase (from stream) and codec sample info
    snprintf(args, sizeof(args),
             "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
             audioTimeBase.num, audioTimeBase.den,
             audioCodecCtx->sample_rate,
             av_get_sample_fmt_name(audioCodecCtx->sample_fmt),
             channel_layout_str);

    int ret = avfilter_graph_create_filter(&audioBufferSrcCtx, abuffer, "in", args, nullptr, audioFilterGraph);
    if (ret < 0) {
        char errbuf[128]; av_strerror(ret, errbuf, sizeof(errbuf));
        qWarning() << "avfilter_graph_create_filter abuffer failed:" << errbuf;
        cleanupAudioFilter();
        return false;
    }

    ret = avfilter_graph_create_filter(&audioBufferSinkCtx, abuffersink, "out", nullptr, nullptr, audioFilterGraph);
    if (ret < 0) {
        char errbuf[128]; av_strerror(ret, errbuf, sizeof(errbuf));
        qWarning() << "avfilter_graph_create_filter abuffersink failed:" << errbuf;
        cleanupAudioFilter();
        return false;
    }

    // build atempo chain with aformat to force s16 stereo at original sample rate
    QString filterDesc;
    double remaining = rate;

    // atempo supports 0.5..2.0; chain multiple filters as needed
    while (remaining > 2.0 + 1e-6) {
        if (!filterDesc.isEmpty()) filterDesc += ",";
        filterDesc += "atempo=2.0";
        remaining /= 2.0;
    }
    while (remaining < 0.5 - 1e-6) {
        if (!filterDesc.isEmpty()) filterDesc += ",";
        filterDesc += "atempo=0.5";
        remaining /= 0.5;
    }
    if (std::abs(remaining - 1.0) > 0.01) {
        if (!filterDesc.isEmpty()) filterDesc += ",";
        filterDesc += QString("atempo=%1").arg(remaining, 0, 'f', 6);
    }
    if (filterDesc.isEmpty()) filterDesc = "anull";

    // force output to s16, stereo, and original sample rate
    filterDesc += QString(",aformat=sample_fmts=s16:channel_layouts=stereo:sample_rates=%1")
                      .arg(audioCodecCtx->sample_rate);

    qDebug() << "initAudioFilter desc:" << filterDesc;

    AVFilterInOut *inputs = avfilter_inout_alloc();
    AVFilterInOut *outputs = avfilter_inout_alloc();
    if (!inputs || !outputs) {
        qWarning() << "failed to alloc filter inputs/outputs";
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        cleanupAudioFilter();
        return false;
    }

    outputs->name = av_strdup("in");
    outputs->filter_ctx = audioBufferSrcCtx;
    outputs->pad_idx = 0;
    outputs->next = nullptr;

    inputs->name = av_strdup("out");
    inputs->filter_ctx = audioBufferSinkCtx;
    inputs->pad_idx = 0;
    inputs->next = nullptr;

    ret = avfilter_graph_parse_ptr(audioFilterGraph, filterDesc.toUtf8().constData(), &inputs, &outputs, nullptr);

    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    if (ret < 0) {
        char errbuf[128]; av_strerror(ret, errbuf, sizeof(errbuf));
        qWarning() << "avfilter_graph_parse_ptr failed:" << errbuf;
        cleanupAudioFilter();
        return false;
    }

    ret = avfilter_graph_config(audioFilterGraph, nullptr);
    if (ret < 0) {
        char errbuf[128]; av_strerror(ret, errbuf, sizeof(errbuf));
        qWarning() << "avfilter_graph_config failed:" << errbuf;
        cleanupAudioFilter();
        return false;
    }

    // after graph configured, output will be s16/stereo at codec sample rate
    m_audioSampleRate = audioCodecCtx->sample_rate;
    m_audioOutChannels = 2;

    return true;
}

void VideoPlayer::cleanupAudioFilter()
{
    // 必须持有 m_audioFilterMutex
    if (audioFilterGraph) {
        avfilter_graph_free(&audioFilterGraph);
        audioFilterGraph = nullptr;
        audioBufferSrcCtx = nullptr;
        audioBufferSinkCtx = nullptr;
    }
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
        if (avcodec_parameters_to_context(codecCtx, vpar) < 0) {
            qWarning() << "avcodec_parameters_to_context fail";
            return false;
        }
        if (avcodec_open2(codecCtx, vcodec, nullptr) < 0) {
            qWarning() << "视频解码器打开失败";
            return false;
        }
        videoTimeBase = fmtCtx->streams[videoStreamIndex]->time_base;
    }

    // 音频解码上下文
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

    if (m_decodeThread) {
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

    // 创建音频输出
    if (audioStreamIndex >= 0 && audioCodecCtx) {
        if (!m_audioFlushTimer) {
            m_audioFlushTimer = new QTimer(this);
            m_audioFlushTimer->setInterval(20);     // 刷新间隔
            connect(m_audioFlushTimer, &QTimer::timeout, this, &VideoPlayer::flushAudioBuffer);
            m_audioFlushTimer->start();
        }

        if (audioSink) {
            audioSink->stop();
            delete audioSink;
            audioSink = nullptr;
            audioIODevice = nullptr;
        }

        // 初始化 audio filter（首次）
        {
            QMutexLocker filterLocker(&m_audioFilterMutex);
            cleanupAudioFilter();
            if (!initAudioFilter(m_playRate.load())) {
                qWarning() << "Failed to initialize audio filter";
            }
        }

        QAudioFormat fmt;
        fmt.setSampleRate(audioCodecCtx->sample_rate);
        fmt.setChannelCount(2);
        fmt.setSampleFormat(QAudioFormat::Int16);

        QAudioDevice device = QMediaDevices::defaultAudioOutput();

        if (!device.isFormatSupported(fmt)) {
            qWarning() << "Requested audio format not supported, trying fallback.";
            fmt.setSampleRate(48000);
            if (!device.isFormatSupported(fmt)) {
                fmt.setSampleRate(44100);
            }
        }

        audioSink = new QAudioSink(device, fmt, this);
        audioIODevice = audioSink->start();
        if (!audioIODevice) {
            qWarning() << "audioSink start failed";
            delete audioSink;
            audioSink = nullptr;
            audioIODevice = nullptr;
        } else {
            m_audioSampleRate = fmt.sampleRate();
            m_audioOutChannels = fmt.channelCount();
        }

        m_audioBasePts.store(-1.0);
        m_audioPlayedSamples.store(0);
    }

    m_playStarted = false;
    m_totalPausedMs.store(0);
    m_pauseStartMs = 0;
    m_decodeThread = QThread::create([this]() { decodeLoop(); });
    m_decodeThread->start();
}

void VideoPlayer::pause()
{
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

    {
        QMutexLocker locker(&m_mutex);
        m_frameQueue.clear();
    }

    {
        QMutexLocker aLocker(&m_audioQueueMutex);
        m_audioQueue.clear();
    }

    // 重置音频播放起点（在主线程中安全操作）
    m_audioBasePts.store(-1.0);
    m_audioPlayedSamples.store(0);

    // 停止并重启音频输出（保留 audioSink）
    if (audioSink) {
        audioSink->stop();
        audioIODevice = audioSink->start();
    }

    m_seekTargetSec = positionSec;
    m_seekRequested.store(true);
    m_finished.store(false);

    m_playStarted = false;
    m_totalPausedMs.store(0);
    m_pauseStartMs = 0;
}

// ---------------- decodeLoop ----------------
void VideoPlayer::decodeLoop()
{
    swrCtx = nullptr;
    // 用于缓冲音频帧，减少频繁的filter操作
    std::vector<AVFrame*> audioFrameBatch;
    const int AUDIO_BATCH_SIZE = 8;  // 批量处理音频帧

    while (!m_stopRequested.load()) {
        if (m_paused.load()) {
            QThread::msleep(10);
            continue;
        }

        // 处理跳转
        if (m_seekRequested.load()) {
            m_seekRequested.store(false);

            int64_t ts = static_cast<int64_t>(m_seekTargetSec * AV_TIME_BASE);
            int seekRet = av_seek_frame(fmtCtx, -1, ts, AVSEEK_FLAG_BACKWARD);
            if (seekRet < 0) {
                qWarning() << "Seek failed, trying AVSEEK_FLAG_ANY";
                seekRet = av_seek_frame(fmtCtx, -1, ts, AVSEEK_FLAG_ANY);
            }

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

            // 清空音频帧缓冲
            for (auto f : audioFrameBatch) av_frame_free(&f);
            audioFrameBatch.clear();

            // 重建 audio filter（解码线程内）
            if (audioCodecCtx) {
                QMutexLocker filterLocker(&m_audioFilterMutex);
                cleanupAudioFilter();
                if (!initAudioFilter(m_playRate.load())) {
                    qWarning() << "Failed to reinit audio filter after seek";
                }
            }

            m_audioBasePts.store(-1.0);
            m_audioPlayedSamples.store(0);
            m_playStarted = false;
            m_totalPausedMs.store(0);
            m_pauseStartMs = 0;

            continue;
        }

        // 检查是否需要重置 audio filter（来自 setPlayRate）
        if (m_audioFilterNeedReset.load()) {
            m_audioFilterNeedReset.store(false);
            QMutexLocker filterLocker(&m_audioFilterMutex);
            cleanupAudioFilter();
            if (audioCodecCtx) {
                if (!initAudioFilter(m_playRate.load())) {
                    qWarning() << "Failed to reinit audio filter on rate change";
                }
            }
            // we continue; loop will read next packets
        }

        int ret = av_read_frame(fmtCtx, packet);
        if (ret < 0) {
            // 处理剩余的音频帧
            if (!audioFrameBatch.empty() && audioCodecCtx) {
                QMutexLocker filterLocker(&m_audioFilterMutex);
                if (audioBufferSrcCtx && audioBufferSinkCtx) {
                    for (AVFrame *aframe : audioFrameBatch) {
                        int addRet = av_buffersrc_add_frame_flags(audioBufferSrcCtx, aframe, AV_BUFFERSRC_FLAG_KEEP_REF);
                        if (addRet < 0) {
                            char errbuf[128]; av_strerror(addRet, errbuf, sizeof(errbuf));
                            qWarning() << "Error feeding audio filter on EOF:" << errbuf;
                        }
                        av_frame_unref(aframe);
                    }
                    // 最后冲洗filter
                    int flushRet = av_buffersrc_add_frame_flags(audioBufferSrcCtx, nullptr, 0);
                    if (flushRet < 0) {
                        char errbuf[128]; av_strerror(flushRet, errbuf, sizeof(errbuf));
                        qWarning() << "Error flushing audio filter:" << errbuf;
                    }
                    // 读取所有剩余帧
                    while (true) {
                        AVFrame *filteredFrame = av_frame_alloc();
                        if (av_buffersink_get_frame(audioBufferSinkCtx, filteredFrame) < 0) {
                            av_frame_free(&filteredFrame);
                            break;
                        }
                        // ... 处理 filteredFrame ...
                        av_frame_free(&filteredFrame);
                    }
                }
                for (auto f : audioFrameBatch) av_frame_free(&f);
                audioFrameBatch.clear();
            }

            if (!m_finished.load()) {
                m_finished.store(true);
                pause();
                emit finished();
            }
            QThread::msleep(20);
            continue;
        }

        // 处理音频帧（批量处理模式）
        if (audioStreamIndex >= 0 && packet->stream_index == audioStreamIndex && audioCodecCtx) {
            if (avcodec_send_packet(audioCodecCtx, packet) == 0) {
                AVFrame *aframe = av_frame_alloc();
                while (avcodec_receive_frame(audioCodecCtx, aframe) == 0) {
                    // 缓冲音频帧而不是立即处理
                    AVFrame *frameClone = av_frame_clone(aframe);
                    if (frameClone) {
                        audioFrameBatch.push_back(frameClone);
                    }

                    // 当缓冲满或需要处理时，批量通过filter
                    if (audioFrameBatch.size() >= AUDIO_BATCH_SIZE) {
                        QMutexLocker filterLocker(&m_audioFilterMutex);
                        if (audioBufferSrcCtx && audioBufferSinkCtx) {
                            for (AVFrame *batchFrame : audioFrameBatch) {
                                int addRet = av_buffersrc_add_frame_flags(audioBufferSrcCtx, batchFrame, AV_BUFFERSRC_FLAG_KEEP_REF);
                                if (addRet < 0) {
                                    char errbuf[128]; av_strerror(addRet, errbuf, sizeof(errbuf));
                                    qWarning() << "Error feeding audio filter (batch):" << errbuf;
                                    av_frame_unref(batchFrame);
                                    continue;
                                }

                                double apts = 0.0;
                                if (batchFrame->pts != AV_NOPTS_VALUE)
                                    apts = batchFrame->pts * av_q2d(audioTimeBase);
                                else if (batchFrame->best_effort_timestamp != AV_NOPTS_VALUE)
                                    apts = batchFrame->best_effort_timestamp * av_q2d(audioTimeBase);

                                // 从 filter 读取处理后的帧
                                while (true) {
                                    AVFrame *filteredFrame = av_frame_alloc();
                                    ret = av_buffersink_get_frame(audioBufferSinkCtx, filteredFrame);
                                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                                        av_frame_free(&filteredFrame);
                                        break;
                                    }
                                    if (ret < 0) {
                                        av_frame_free(&filteredFrame);
                                        break;
                                    }

                                    int outChannels = 0;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 17, 0)
                                    outChannels = filteredFrame->ch_layout.nb_channels;
#else
                                    outChannels = filteredFrame->nb_channels;
#endif
                                    if (outChannels <= 0) {
                                        outChannels = m_audioOutChannels > 0 ? m_audioOutChannels : 2;
                                    }

                                    int bytesPerSample = av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
                                    if (bytesPerSample <= 0) bytesPerSample = 2;

                                    int bytes = av_samples_get_buffer_size(nullptr,
                                                                           outChannels,
                                                                           filteredFrame->nb_samples,
                                                                           AV_SAMPLE_FMT_S16, 1);
                                    if (bytes > 0 && filteredFrame->data[0]) {
                                        QByteArray chunk(reinterpret_cast<const char*>(filteredFrame->data[0]), bytes);
                                        {
                                            QMutexLocker aLocker(&m_audioQueueMutex);
                                            m_audioQueue.push_back(chunk);
                                        }

                                        double base = m_audioBasePts.load();
                                        if (base < 0.0) {
                                            m_audioBasePts.store(apts);
                                            m_audioPlayedSamples.store(0);
                                            qDebug() << "Audio base PTS set to:" << apts;
                                        }
                                    }

                                    av_frame_free(&filteredFrame);
                                }
                                av_frame_unref(batchFrame);
                            }
                        }
                        for (auto f : audioFrameBatch) av_frame_free(&f);
                        audioFrameBatch.clear();
                    }
                }
                av_frame_free(&aframe);
            }
            av_packet_unref(packet);
            continue;
        }

        // 处理视频帧
        if (packet->stream_index == videoStreamIndex) {
            if (avcodec_send_packet(codecCtx, packet) == 0) {
                while (avcodec_receive_frame(codecCtx, frame) == 0) {
                    double vpts = 0.0;
                    if (frame->pts != AV_NOPTS_VALUE)
                        vpts = frame->pts * av_q2d(videoTimeBase);
                    else if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                        vpts = frame->best_effort_timestamp * av_q2d(videoTimeBase);

                    int dstW = m_renderWidth.load();
                    int dstH = m_renderHeight.load();
                    if (dstW <= 0 || dstH <= 0) {
                        dstW = codecCtx->width;
                        dstH = codecCtx->height;
                    }

                    // 重建 swsCtx（使用更快的缩放算法减少CPU占用）
                    {
                        QMutexLocker locker(&m_swsMutex);
                        if (!swsCtx || m_swsCtxNeedReset.load()) {
                            if (swsCtx) {
                                sws_freeContext(swsCtx);
                                swsCtx = nullptr;
                            }

                            int algo = m_scalingAlgo.load();
                            swsCtx = sws_getContext(codecCtx->width, codecCtx->height, codecCtx->pix_fmt,
                                                    dstW, dstH, AV_PIX_FMT_RGB24,
                                                    algo, nullptr, nullptr, nullptr);
                            if (!swsCtx) {
                                // 降级到最快的算法
                                swsCtx = sws_getContext(codecCtx->width, codecCtx->height, codecCtx->pix_fmt,
                                                        dstW, dstH, AV_PIX_FMT_RGB24,
                                                        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                            }
                            m_swsCtxNeedReset.store(false);
                        }
                    }

                    QImage img(dstW, dstH, QImage::Format_RGB888);
                    uint8_t *dst[4] = { img.bits(), nullptr, nullptr, nullptr };
                    int dst_linesize[4] = { static_cast<int>(img.bytesPerLine()), 0, 0, 0 };

                    sws_scale(swsCtx, frame->data, frame->linesize, 0, codecCtx->height, dst, dst_linesize);

                    // 时间控制
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
                        // 优化：只在队列过大时丢弃，而不是每帧都检查
                        while (m_frameQueue.size() >= 20) m_frameQueue.dequeue();
                        m_frameQueue.enqueue(std::make_pair(img, vpts));
                    }

                    emit frameReady(img);
                    emit positionChanged(vpts);
                }
            }
        }

        av_packet_unref(packet);
    }

    // 清理音频帧缓冲
    for (auto f : audioFrameBatch) av_frame_free(&f);
    audioFrameBatch.clear();

    {
        QMutexLocker locker(&m_swsMutex);
        if (swsCtx) {
            sws_freeContext(swsCtx);
            swsCtx = nullptr;
        }
    }
    // cleanup audio filter on exit
    QMutexLocker filterLocker(&m_audioFilterMutex);
    cleanupAudioFilter();
}

// ---------------- flushAudioBuffer (main thread) ----------------
void VideoPlayer::flushAudioBuffer()
{
    if (!audioIODevice) return;
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
        int bytesPerSample = 2; // s16
        long long samplesWritten = written / (bytesPerSample * m_audioOutChannels);
        m_audioPlayedSamples.fetch_add(samplesWritten);
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
    QMutexLocker filterLocker(&m_audioFilterMutex);
    cleanupAudioFilter();
}

void VideoPlayer::forward(double seconds)
{
    if (!fmtCtx || videoStreamIndex < 0) return;

    const AVStream* vs = fmtCtx->streams[videoStreamIndex];
    double durationSec = vs->duration * av_q2d(vs->time_base);

    double currentPos = 0.0;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_frameQueue.isEmpty()) {
            currentPos = m_frameQueue.back().second;
        }
    }
    double newPos = currentPos + seconds;
    if (newPos < 0.0) newPos = 0.0;
    if (newPos > durationSec) newPos = durationSec;

    seek(newPos);
}

void VideoPlayer::setPlayRate(double rate)
{
    if (rate <= 0.0) return;

    double oldRate = m_playRate.load();
    if (std::abs(oldRate - rate) < 1e-6) return;

    // 1) 更新原子值（让 decodeLoop 看到新速率）
    m_playRate.store(rate);

    // 2) 计算当前播放位置（优先使用最近视频帧 pts，其次使用音频播放进度）
    double currentPos = 0.0;
    bool havePos = false;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_frameQueue.isEmpty()) {
            currentPos = m_frameQueue.back().second;
            havePos = true;
        }
    }
    if (!havePos) {
        double base = m_audioBasePts.load();
        long long playedSamples = m_audioPlayedSamples.load();
        int sr = m_audioSampleRate > 0 ? m_audioSampleRate : 48000;
        if (base >= 0.0) {
            currentPos = base + double(playedSamples) / double(sr);
            havePos = true;
        }
    }
    if (!havePos) {
        // fallback 使用已有的 playStartPts
        currentPos = m_playStartPts;
    }

    // 3) 重置播放时间基（保证视频等待逻辑在速率切换时平滑）
    m_playStartPts = currentPos;
    m_playTimer.restart();
    m_totalPausedMs.store(0);
    m_pauseStartMs = 0;
    m_playStarted = true;

    // 4) 清空音频队列并重置音频基点（避免旧缓冲在新速率下播放出错）
    {
        QMutexLocker aLocker(&m_audioQueueMutex);
        m_audioQueue.clear();
    }
    m_audioBasePts.store(currentPos);
    m_audioPlayedSamples.store(0);

    // 5) 请求在解码线程重建 audio filter（安全）
    m_audioFilterNeedReset.store(true);

    qDebug() << "setPlayRate: from" << oldRate << "to" << rate << "currentPos" << currentPos;
}

void VideoPlayer::setRenderSize(int w, int h)
{
    if (w <= 0 || h <= 0) return;

    m_renderWidth.store(w);
    m_renderHeight.store(h);
    m_swsCtxNeedReset.store(true);
}
