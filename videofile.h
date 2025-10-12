#ifndef VIDEOFILE_H
#define VIDEOFILE_H

#include <QString>
#include <QDateTime>

/**
 * @brief 视频文件类
 */
class VideoFile
{   
public:
    VideoFile(const QString &path);

    QString fileName() const;           //文件名
    QString fullPath() const;           //文件绝对路径
    double sizeMB() const;              //文件大小（MB）
    QString lastChangedStr() const;     //格式化的最后修改时间
    QDateTime lastChanged() const;      //原始修改时间

    // 🔹 获取视频时长
    void Init();          //初始化调用获取总时长
    QString durationStr() const; // 返回格式化时长
    double getNumDuration() const;  //获取数字类型时长

    void printInfo() const;  // 序列化输出
    static const QString FormatStr(const double & nums);    //静态函数转为格式化的串

    // getters
    int getWidth() const;
    int getHeight() const;
    QString getFormat() const;
    QString getCode() const;
    QString getFps() const;
    int getChannels() const;
    int64_t getBitrate() const;

private:
    QString m_path;         //视频文件路径
    double m_duration{-1}; // 缓存视频总时长，-1表示未计算

    int __width, __height;  //分辨率
    QString __format;   //视频编码
    QString __code;     //容器格式
    QString __fps;      //帧率
    int __channels;     //声道数
    int64_t __bitrate;  // bps

};

#endif // VIDEOFILE_H
