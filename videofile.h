#ifndef VIDEOFILE_H
#define VIDEOFILE_H

#include <QString>
#include <QDateTime>
#include <QImage>
#include <cstdint>

/**
 * @brief 视频/音频文件类
 */
class VideoFile
{
public:
    enum MediaType { Media_Video = 0, Media_Audio = 1, Media_Unknown = 2 };

    VideoFile(const QString &path);

    QString fileName() const;
    QString fullPath() const;
    double sizeMB() const;
    QString lastChangedStr() const;
    QDateTime lastChanged() const;

    MediaType mediaType = Media_Unknown;

    void VideoInit();
    QString durationStr() const;
    double getNumDuration() const;

    void printInfo() const;
    static const QString FormatStr(const double & nums);

    // getters
    int getWidth() const;
    int getHeight() const;
    QString getVideoCodec() const;
    QString getContainer() const;
    QString getFps() const;
    int getChannels() const;
    int getSampleRate() const;
    int64_t getBitrate() const;

    // 嵌入元数据
    bool     hasCoverArt() const;
    QImage   getCoverArt() const;
    bool     hasLyrics() const;
    QString  getLyrics() const;
    QString  getTitle() const;
    QString  getArtist() const;
    QString  getAlbum() const;

private:
    QString m_path;         // 文件路径
    double m_duration{-1};  // 缓存总时长，-1表示未计算

    // 视频属性
    int __width{0}, __height{0};  // 分辨率
    QString __videoCodec;         // 视频编码名称
    QString __container;          // 容器格式名称
    QString __fps;                // 帧率字符串

    // 音频属性
    int __channels{0};            // 声道数
    int __sampleRate{0};          // 采样率

    // 通用
    int64_t __bitrate{0};         // bps

    // 嵌入元数据
    QImage  __coverArt;           // 封面图片
    QString __lyrics;             // 歌词
    QString __title;              // 标题 (ID3 TIT2)
    QString __artist;             // 艺术家 (ID3 TPE1)
    QString __album;              // 专辑名 (ID3 TALB)
};

#endif // VIDEOFILE_H
