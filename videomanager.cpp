#include "videomanager.h"

VideoManager::VideoManager(QObject *parent)
    : QObject{parent}
{
    selected = -1;
}
/**
 * @brief 根据传入的视频文件路径构建文件信息对象，再添加到列表
 * @param path
 */
void VideoManager::addByFilePath(const QString & path){
    __videos.append(VideoFile(path));
    emit videosUpdated();
}
/**
 * @brief 根据传入的视频文件路径列表构建文件信息对象后，添加到列表
 * @param list
 */
void VideoManager::addByFilePathList(const QStringList & list){
    QList<VideoFile> video;
    for(const auto & i : list){
        video.append(VideoFile(i));
    }
    __videos.append(video);
    emit videosUpdated();
}

void VideoManager::addSingleVideo(const VideoFile &video) {
    __videos.append(video);
    emit videosUpdated();
}

void VideoManager::addMulVideo(const QList<VideoFile> &videos) {
    __videos.append(videos);
    emit videosUpdated();
}

const QList<VideoFile>& VideoManager::videos() const {
    return __videos;
}
/**
 * @brief 返回指定下标的视频信息VideoFile
 * @param position 下标位置
 * @return
 */
const VideoFile * VideoManager::findByPos(int position) const {
    if (position < 0 || position >= __videos.size()) return nullptr;
    return &__videos[position];
}

int VideoManager::getVideoListSize() const{
    return __videos.size();
}

/**
 * @brief 删除视频文件列表 同时会重置选中行下标
 */
void VideoManager::clear() {
    selected =  -1;         //重置选中
    if(__videos.size() == 0) return ;
    __videos.clear();
    emit videosUpdated();
}
