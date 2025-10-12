#include "videofile.h"
#include <QFileInfo>

// FFmpeg 头文件
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

VideoFile::VideoFile(const QString &path)
    : m_path(path)
{
    Init(); //构造时触发时长计算
}
/**
 * @brief 获取文件名
 * @return
 */
QString VideoFile::fileName() const {
    return QFileInfo(m_path).fileName();
}
/**
 * @brief 获取完整路径
 * @return
 */
QString VideoFile::fullPath() const {
    return m_path;
}
/**
 * @brief 获取文件大小（MB）
 * @return
 */
double VideoFile::sizeMB() const {
    return QFileInfo(m_path).size() / (1024.0 * 1024.0);
}
/**
 * @brief 获取格式化文件最后修改时间
 * @return
 */
QString VideoFile::lastChangedStr() const {
    return QFileInfo(m_path).lastModified().toString("yyyy-MM-dd HH:mm:ss");
}
/**
 * @brief 文件最后修改时间
 * @return
 */
QDateTime VideoFile::lastChanged() const {
    return QFileInfo(m_path).lastModified();
}

/**
 * @brief 获取视频时长（秒），缓存
 * @return 时长：s
 */
void VideoFile::Init()
{
    AVFormatContext *fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, m_path.toStdString().c_str(), nullptr, nullptr) != 0) {
        qWarning() << "无法打开视频文件:" << m_path;
        return;
    }
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        qWarning() << "无法读取视频流信息";
        avformat_close_input(&fmtCtx);
        return;
    }
    // 文件总时长（秒）
    if (fmtCtx->duration != AV_NOPTS_VALUE)
        m_duration = fmtCtx->duration / (double)AV_TIME_BASE;
    else
        m_duration = -1;

    __width = 0;
    __height = 0;
    __fps.clear();
    __format.clear();
    __code.clear();
    __channels = 0;
    __bitrate = 0;

    for (unsigned i = 0; i < fmtCtx->nb_streams; i++) {
        AVStream *stream = fmtCtx->streams[i];
        AVCodecParameters *par = stream->codecpar;

        if (par->codec_type == AVMEDIA_TYPE_VIDEO && __width == 0) {
            __width  = par->width;
            __height = par->height;

            const AVCodec *codec = avcodec_find_decoder(par->codec_id);
            if (codec) __format = codec->name;

            if (stream->avg_frame_rate.num != 0 && stream->avg_frame_rate.den != 0) {
                double fps = av_q2d(stream->avg_frame_rate);
                __fps = QString::number(fps, 'f', 2);
            }

            if (fmtCtx->iformat)
                __code = fmtCtx->iformat->name;
            if (par->bit_rate > 0)
                __bitrate = par->bit_rate;
            else if (fmtCtx->duration > 0 && fmtCtx->pb)
                __bitrate = (avio_size(fmtCtx->pb) * 8) / (fmtCtx->duration / AV_TIME_BASE);
        }

        if (par->codec_type == AVMEDIA_TYPE_AUDIO && __channels == 0) {
            __channels = par->ch_layout.nb_channels;
        }
    }
    avformat_close_input(&fmtCtx);
}

/**
 * @brief 将秒格式化为时、分、秒格式
 * @return 格式化串
 */
QString VideoFile::durationStr() const
{
    // 如果还没计算时长，可以直接返回 "00:00:00"
    double dur = m_duration;
    if (dur < 0)
        return "00:00:00";
    int totalSeconds = static_cast<int>(dur);
    int hours   = totalSeconds / 3600;
    int minutes = (totalSeconds % 3600) / 60;
    int seconds = totalSeconds % 60;

    return QString("%1:%2:%3")
        .arg(hours, 2, 10, QChar('0'))
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'));
}

double VideoFile::getNumDuration() const{
    return m_duration;
}
/**
 * @brief 将double类型数字转为格式化时间
 * @param nums 时长
 * @return QString
 */
const QString VideoFile::FormatStr(const double & nums){
    int totalSeconds = static_cast<int>(nums);
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
    qDebug() << "大小(MB)      :" << sizeMB();
    qDebug() << "最后修改时间  :" << lastChangedStr();

    qDebug() << "时长[h:m:s]   :" << durationStr();
    qDebug() << "时长(秒)      :" << m_duration;
    qDebug() << "分辨率        :" << __width << "x" << __height;
    qDebug() << "帧率          :" << __fps;
    qDebug() << "视频编码      :" << __format;
    qDebug() << "视频码率      :" << __bitrate / 1000 << "kbps";

    qDebug() << "声道数        :" << __channels;

    qDebug() << "容器格式      :" << __code;
    qDebug() << "==========================================";
}

int VideoFile::getWidth() const { return __width; }
int VideoFile::getHeight() const { return __height; }
QString VideoFile::getFormat() const { return __format; }
QString VideoFile::getCode() const { return __code; }
QString VideoFile::getFps() const { return __fps; }
int VideoFile::getChannels() const { return __channels; }
int64_t VideoFile::getBitrate() const { return __bitrate; }
