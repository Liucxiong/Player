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

signals:
    void videosUpdated(); // 当列表更新时通知 UI

private:
    QList<VideoFile> __videos;
};

#endif // VIDEOMANAGER_H
