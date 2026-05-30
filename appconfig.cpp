#include "appconfig.h"
#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QDebug>

AppConfig::AppConfig()
{
    if (!load()) {
        // 配置文件不存在或解析失败, 用默认值立即创建一份
        save();
    }
}

QString AppConfig::configFilePath() const
{
    // 保存在可执行文件所在目录, 便于用户管理和清除
    return QCoreApplication::applicationDirPath() + "/config.json";
}

bool AppConfig::load()
{
    QFile file(configFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        qDebug() << "[AppConfig] 配置文件不存在, 使用默认值";
        return false;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "[AppConfig] 配置文件解析失败:" << err.errorString();
        return false;
    }

    QJsonObject root = doc.object();

    m_lastDirectory  = root.value("lastDirectory").toString();
    m_scalingAlgo    = root.value("scalingAlgorithm").toInt(1);
    m_speedIndex     = root.value("speedIndex").toInt(3);
    m_windowWidth    = root.value("windowWidth").toInt(0);
    m_windowHeight   = root.value("windowHeight").toInt(0);
    m_volume         = qBound(0, root.value("volume").toInt(80), 100);
    m_lastPlayedFile = root.value("lastPlayedFile").toString();

    // 文件播放位置: { "filePath": seconds, ... }
    m_filePositions.clear();
    QJsonObject posObj = root.value("filePositions").toObject();
    for (auto it = posObj.begin(); it != posObj.end(); ++it) {
        m_filePositions.insert(it.key(), it.value().toDouble());
    }

    qDebug() << "[AppConfig] 配置加载成功:"
             << "目录=" << m_lastDirectory
             << "缩放=" << m_scalingAlgo
             << "速度下标=" << m_speedIndex
             << "记忆位置数=" << m_filePositions.size();
    return true;
}

bool AppConfig::save() const
{
    QJsonObject root;

    root["lastDirectory"]    = m_lastDirectory;
    root["scalingAlgorithm"] = m_scalingAlgo;
    root["speedIndex"]       = m_speedIndex;
    root["windowWidth"]      = m_windowWidth;
    root["windowHeight"]     = m_windowHeight;
    root["volume"]           = m_volume;
    root["lastPlayedFile"]   = m_lastPlayedFile;

    // 文件播放位置
    QJsonObject posObj;
    for (auto it = m_filePositions.constBegin();
         it != m_filePositions.constEnd(); ++it) {
        posObj.insert(it.key(), it.value());
    }
    root["filePositions"] = posObj;

    QJsonDocument doc(root);
    QFile file(configFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "[AppConfig] 无法写入配置文件:" << configFilePath();
        return false;
    }

    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();

    qDebug() << "[AppConfig] 配置已保存到" << configFilePath();
    return true;
}

void AppConfig::reset()
{
    m_lastDirectory.clear();
    m_scalingAlgo    = 1;      // 默认 Bilinear
    m_speedIndex     = 3;      // 默认 1.0x
    m_windowWidth    = 0;
    m_windowHeight   = 0;
    m_volume         = 80;
    m_lastPlayedFile.clear();
    m_filePositions.clear();

    save();
    qDebug() << "[AppConfig] 配置已重置为默认值";
}

/* ---- getters / setters ---- */

QString AppConfig::lastDirectory() const { return m_lastDirectory; }
void AppConfig::setLastDirectory(const QString &path) { m_lastDirectory = path; }

int AppConfig::scalingAlgorithm() const { return m_scalingAlgo; }
void AppConfig::setScalingAlgorithm(int index) { m_scalingAlgo = qBound(0, index, 3); }

int AppConfig::speedIndex() const { return m_speedIndex; }
void AppConfig::setSpeedIndex(int index) { m_speedIndex = index; }

int AppConfig::windowWidth() const { return m_windowWidth; }
int AppConfig::windowHeight() const { return m_windowHeight; }
void AppConfig::setWindowSize(int w, int h) { m_windowWidth = w; m_windowHeight = h; }

double AppConfig::filePosition(const QString &filePath) const
{
    return m_filePositions.value(filePath, 0.0);
}

void AppConfig::setFilePosition(const QString &filePath, double posSec)
{
    if (posSec < 0.5) {
        // 太小的位置不保存 (已看完或刚开始)
        m_filePositions.remove(filePath);
    } else {
        m_filePositions.insert(filePath, posSec);
    }
}

void AppConfig::removeFilePosition(const QString &filePath)
{
    m_filePositions.remove(filePath);
}

int AppConfig::volume() const { return m_volume; }
void AppConfig::setVolume(int vol) { m_volume = qBound(0, vol, 100); }

QString AppConfig::lastPlayedFile() const { return m_lastPlayedFile; }
void AppConfig::setLastPlayedFile(const QString &fileName) { m_lastPlayedFile = fileName; }
