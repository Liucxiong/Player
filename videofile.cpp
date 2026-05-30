// videofile.cpp
#include "videofile.h"
#include <QFileInfo>
#include <QDebug>

// FFmpeg 头文件（C 接口）
extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/version.h>
}

VideoFile::VideoFile(const QString &path)
    : m_path(path)
{
    VideoInit();
}

QString VideoFile::fileName() const { return QFileInfo(m_path).fileName(); }
QString VideoFile::fullPath() const { return m_path; }
double VideoFile::sizeMB() const { qint64 size = QFileInfo(m_path).size(); return size / (1024.0 * 1024.0); }
QString VideoFile::lastChangedStr() const { return QFileInfo(m_path).lastModified().toString("yyyy-MM-dd HH:mm:ss"); }
QDateTime VideoFile::lastChanged() const { return QFileInfo(m_path).lastModified(); }

void VideoFile::VideoInit()
{
    static bool s_ffmpeg_inited = false;
    if (!s_ffmpeg_inited) {
        avformat_network_init();
        s_ffmpeg_inited = true;
    }

    m_duration = -1;
    __width = __height = 0;
    __fps.clear();
    __videoCodec.clear();
    __container.clear();
    __channels = 0;
    __sampleRate = 0;
    __bitrate = 0;
    mediaType = Media_Unknown;

    AVFormatContext *fmtCtx = nullptr;
    const char *cpath = m_path.toUtf8().constData();

    if (avformat_open_input(&fmtCtx, cpath, nullptr, nullptr) != 0) {
        qWarning() << "无法打开媒体文件:" << m_path;
        return;
    }

    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        qWarning() << "无法读取媒体流信息:" << m_path;
        avformat_close_input(&fmtCtx);
        return;
    }

    if (fmtCtx->duration != AV_NOPTS_VALUE) {
        m_duration = fmtCtx->duration / (double)AV_TIME_BASE;
    } else {
        m_duration = -1;
    }

    if (fmtCtx->iformat && fmtCtx->iformat->name)
        __container = QString::fromUtf8(fmtCtx->iformat->name);

    bool foundVideo = false;
    bool foundAudio = false;

    if (fmtCtx->bit_rate > 0)
        __bitrate = fmtCtx->bit_rate;

    for (unsigned i = 0; i < fmtCtx->nb_streams; ++i) {
        AVStream *stream = fmtCtx->streams[i];
        if (!stream) continue;
        AVCodecParameters *par = stream->codecpar;
        if (!par) continue;

        if (par->codec_type == AVMEDIA_TYPE_VIDEO && !foundVideo
            && !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            foundVideo = true;
            __width  = par->width;
            __height = par->height;
            __videoCodec = QString::fromUtf8(avcodec_get_name(par->codec_id));

            AVRational fr = (stream->avg_frame_rate.num && stream->avg_frame_rate.den)
                                ? stream->avg_frame_rate
                                : stream->r_frame_rate;
            if (fr.num != 0 && fr.den != 0) {
                double fps = av_q2d(fr);
                __fps = QString::number(fps, 'f', 2);
            }

            if (__bitrate == 0 && par->bit_rate > 0)
                __bitrate = par->bit_rate;

            if (__bitrate == 0 && fmtCtx->pb && fmtCtx->duration > 0) {
                int64_t size = avio_size(fmtCtx->pb);
                if (size > 0)
                    __bitrate = static_cast<int64_t>((size * 8) / (fmtCtx->duration / (double)AV_TIME_BASE));
            }
        }

        if (par->codec_type == AVMEDIA_TYPE_AUDIO && !foundAudio) {
            foundAudio = true;

// ==== 兼容写法（修复了之前用 defined(par->channels) 的错误） ====
#if LIBAVUTIL_VERSION_MAJOR >= 57
            // 新的 channel API：AVChannelLayout ch_layout
            if (par->ch_layout.nb_channels > 0) {
                __channels = par->ch_layout.nb_channels;
            } else {
                __channels = 0;
            }
#else \
            // 旧 API：使用 par->channels 或 par->channel_layout 回退
            int ch = 0;
            if (par->channels > 0) {
                ch = par->channels;
            } else if (par->channel_layout) {
                ch = av_get_channel_layout_nb_channels(par->channel_layout);
            }
            __channels = ch;
#endif \
            // ============================================================

            if (par->sample_rate > 0) __sampleRate = par->sample_rate;

            if (__videoCodec.isEmpty())
                __videoCodec = QString::fromUtf8(avcodec_get_name(par->codec_id));

            if (__bitrate == 0 && par->bit_rate > 0)
                __bitrate = par->bit_rate;
        }

        if (foundVideo && foundAudio)
            break;
    }

    if (foundVideo) mediaType = Media_Video;
    else if (foundAudio) mediaType = Media_Audio;
    else mediaType = Media_Unknown;

    /* ============ 嵌入元数据提取 ============ */

    // 1) 封面图片: 查找 AV_DISPOSITION_ATTACHED_PIC 流
    __coverArt = QImage();
    for (unsigned i = 0; i < fmtCtx->nb_streams; ++i) {
        AVStream *s = fmtCtx->streams[i];
        if (s->disposition & AV_DISPOSITION_ATTACHED_PIC) {
            AVPacket &pic = s->attached_pic;
            if (pic.size > 0 && pic.data) {
                __coverArt.loadFromData(pic.data, pic.size);
            }
            break;  // 只取第一张
        }
    }

    // 2) 元数据标签 (ID3 / Vorbis Comment / APE 等, FFmpeg 统一到 metadata 字典)
    __title.clear();
    __artist.clear();
    __album.clear();
    __lyrics.clear();

    if (fmtCtx->metadata) {
        AVDictionaryEntry *tag = nullptr;

        tag = av_dict_get(fmtCtx->metadata, "title", nullptr, 0);
        if (tag) __title = QString::fromUtf8(tag->value);

        tag = av_dict_get(fmtCtx->metadata, "artist", nullptr, 0);
        if (tag) __artist = QString::fromUtf8(tag->value);

        tag = av_dict_get(fmtCtx->metadata, "album", nullptr, 0);
        if (tag) __album = QString::fromUtf8(tag->value);

        // 歌词: 不同格式用不同 key
        tag = av_dict_get(fmtCtx->metadata, "lyrics", nullptr, 0);
        if (!tag) tag = av_dict_get(fmtCtx->metadata, "LYRICS", nullptr, 0);
        if (!tag) tag = av_dict_get(fmtCtx->metadata, "USLT", nullptr, 0);
        if (!tag) tag = av_dict_get(fmtCtx->metadata, "lyrics-eng", nullptr, 0);
        if (!tag) tag = av_dict_get(fmtCtx->metadata, "UNSYNCEDLYRICS", nullptr, 0);
        if (tag) __lyrics = QString::fromUtf8(tag->value);
    }

    // 有些格式把歌词放在单独流的 metadata 里
    if (__lyrics.isEmpty()) {
        for (unsigned i = 0; i < fmtCtx->nb_streams; ++i) {
            AVStream *s = fmtCtx->streams[i];
            if (!s->metadata) continue;
            AVDictionaryEntry *tag = av_dict_get(s->metadata, "lyrics", nullptr, 0);
            if (!tag) tag = av_dict_get(s->metadata, "LYRICS", nullptr, 0);
            if (tag) {
                __lyrics = QString::fromUtf8(tag->value);
                break;
            }
        }
    }

    avformat_close_input(&fmtCtx);
}

QString VideoFile::durationStr() const
{
    double dur = m_duration;
    if (dur < 0)
        return "00:00:00";
    int totalSeconds = static_cast<int>(dur + 0.5);
    int hours   = totalSeconds / 3600;
    int minutes = (totalSeconds % 3600) / 60;
    int seconds = totalSeconds % 60;

    return QString("%1:%2:%3")
        .arg(hours, 2, 10, QChar('0'))
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'));
}

double VideoFile::getNumDuration() const { return m_duration; }

const QString VideoFile::FormatStr(const double & nums) {
    if (nums < 0) return QString("00:00:00");
    int totalSeconds = static_cast<int>(nums + 0.5);
    int hours   = totalSeconds / 3600;
    int minutes = (totalSeconds % 3600) / 60;
    int seconds = totalSeconds % 60;

    return QString("%1:%2:%3")
        .arg(hours, 2, 10, QChar('0'))
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'));
}

void VideoFile::printInfo() const
{
    qDebug() << "================ VideoFile ================";
    qDebug() << "文件名        :" << fileName();
    qDebug() << "完整路径      :" << fullPath();
    qDebug() << "大小(MB)      :" << QString::number(sizeMB(), 'f', 2);
    qDebug() << "最后修改时间  :" << lastChangedStr();
    qDebug() << "时长[h:m:s]   :" << durationStr();
    qDebug() << "时长(秒)      :" << (m_duration < 0 ? QString("未知") : QString::number(m_duration, 'f', 2));

    if (mediaType == Media_Video) {
        qDebug() << "-- 媒体类型: 视频 --";
        qDebug() << "分辨率        :" << __width << "x" << __height;
        qDebug() << "帧率          :" << (__fps.isEmpty() ? QString("未知") : __fps);
        qDebug() << "视频/音频 Codec:" << (__videoCodec.isEmpty() ? QString("未知") : __videoCodec);
        qDebug() << "容器格式      :" << (__container.isEmpty() ? QString("未知") : __container);
        qDebug() << "视频码率(kbps) :" << (__bitrate > 0 ? QString::number(__bitrate / 1000) : QString("未知"));
        if (__channels > 0) qDebug() << "声道数        :" << __channels;
        if (__sampleRate > 0) qDebug() << "采样率(Hz)    :" << __sampleRate;
    } else if (mediaType == Media_Audio) {
        qDebug() << "-- 媒体类型: 音频 --";
        qDebug() << "音频Codec     :" << (__videoCodec.isEmpty() ? QString("未知") : __videoCodec);
        qDebug() << "容器格式      :" << (__container.isEmpty() ? QString("未知") : __container);
        qDebug() << "声道数        :" << (__channels > 0 ? QString::number(__channels) : QString("未知"));
        qDebug() << "采样率(Hz)    :" << (__sampleRate > 0 ? QString::number(__sampleRate) : QString("未知"));
        qDebug() << "码率(kbps)    :" << (__bitrate > 0 ? QString::number(__bitrate / 1000) : QString("未知"));
    } else {
        qDebug() << "-- 媒体类型: 未知 --";
        qDebug() << "容器格式      :" << (__container.isEmpty() ? QString("未知") : __container);
        qDebug() << "Codec         :" << (__videoCodec.isEmpty() ? QString("未知") : __videoCodec);
        qDebug() << "码率(kbps)    :" << (__bitrate > 0 ? QString::number(__bitrate / 1000) : QString("未知"));
        if (__width > 0 || __height > 0) qDebug() << "分辨率        :" << __width << "x" << __height;
        if (!__fps.isEmpty()) qDebug() << "帧率          :" << __fps;
        if (__channels > 0) qDebug() << "声道数        :" << __channels;
    }

    qDebug() << "==========================================";
}

int VideoFile::getWidth() const { return __width; }
int VideoFile::getHeight() const { return __height; }
QString VideoFile::getVideoCodec() const { return __videoCodec; }
QString VideoFile::getContainer() const { return __container; }
QString VideoFile::getFps() const { return __fps; }
int VideoFile::getChannels() const { return __channels; }
int VideoFile::getSampleRate() const { return __sampleRate; }
int64_t VideoFile::getBitrate() const { return __bitrate; }

bool    VideoFile::hasCoverArt() const { return !__coverArt.isNull(); }
QImage  VideoFile::getCoverArt() const { return __coverArt; }
bool    VideoFile::hasLyrics()   const { return !__lyrics.isEmpty(); }
QString VideoFile::getLyrics()   const { return __lyrics; }
QString VideoFile::getTitle()    const { return __title; }
QString VideoFile::getArtist()   const { return __artist; }
QString VideoFile::getAlbum()    const { return __album; }
