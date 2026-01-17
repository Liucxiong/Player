#ifndef VIDEOMANAGER_H
#define VIDEOMANAGER_H

#include <QObject>
#include "videofile.h"

class VideoManager : public QObject
{
    Q_OBJECT
public:
    explicit VideoManager(QObject *parent = nullptr);

    void addByFilePath(const QString & path);
    void addByFilePathList(const QStringList & list);
    void addSingleVideo(const VideoFile & video);
    void addMulVideo(const QList<VideoFile> & list);
    const QList<VideoFile>& videos() const;
    void clear();

    const VideoFile * findByPos(int position) const;
    int getVideoListSize() const;

    int selected = -1;  //表示当前选中播放的行下标
    const QList<double> speedList = {0.25,0.5,0.75,1.0,1.25,1.5,2.0,3.0};   //播放速度表
    double playSpeed = 1.0; //当前播放速度

signals:
    void videosUpdated(); // 当列表更新时通知 UI

private:
    QList<VideoFile> __videos;
};

#endif // VIDEOMANAGER_H
