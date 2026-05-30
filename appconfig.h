#ifndef APPCONFIG_H
#define APPCONFIG_H

#include <QString>
#include <QJsonObject>
#include <QMap>

/**
 * @brief 本地 JSON 配置管理器
 *
 * 配置文件 config.json 保存在可执行文件所在目录,
 * 删除该文件即可重置所有配置.
 *
 * 使用方式:
 *   AppConfig cfg;                         // 构造时自动加载
 *   cfg.setLastDirectory("/path/to/dir");  // 修改
 *   cfg.save();                            // 写盘
 */
class AppConfig
{
public:
    AppConfig();

    bool load();
    bool save() const;
    void reset();   // 恢复所有默认值并写盘

    /* 上次打开的文件夹 */
    QString lastDirectory() const;
    void    setLastDirectory(const QString &path);

    /* 缩放算法下标 (0~3) */
    int  scalingAlgorithm() const;
    void setScalingAlgorithm(int index);

    /* 播放速度 comboBox 下标 */
    int  speedIndex() const;
    void setSpeedIndex(int index);

    /* 窗口大小 */
    int  windowWidth() const;
    int  windowHeight() const;
    void setWindowSize(int w, int h);

    /* 文件播放位置记忆 (完整路径 → 秒) */
    double filePosition(const QString &filePath) const;
    void   setFilePosition(const QString &filePath, double posSec);
    void   removeFilePosition(const QString &filePath);

    /* 音量 (0~100) */
    int  volume() const;
    void setVolume(int vol);

    /* 上次播放的文件名 */
    QString lastPlayedFile() const;
    void    setLastPlayedFile(const QString &fileName);

private:
    QString configFilePath() const;

    QString m_lastDirectory;
    int     m_scalingAlgo   = 1;
    int     m_speedIndex    = 3;
    int     m_windowWidth   = 0;
    int     m_windowHeight  = 0;
    int     m_volume        = 80;   // 默认音量 80%
    QString m_lastPlayedFile;
    QMap<QString, double> m_filePositions;
};

#endif // APPCONFIG_H
