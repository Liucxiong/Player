#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTableWidget>
#include <QLabel>
#include <QToolButton>

#include "pathsel.h"
#include "videoplayer.h"
#include "fullscreentool.h"

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE


class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    void updateVideoRenderSize();

private slots:
    void onFrameReady(const QImage &img);
    void onPlayPauseClicked();
    void toggleFullScreen();

    void currentIndexSpeedChanged(int index);

private:
    Ui::MainWindow *ui;
    PathSel* pathSel;
    VideoPlayer* player;
    VideoManager* manager;

    QLabel *m_sliderTip = nullptr;  // 滑动条时间提示

    QLabel *m_videoLabel;             // 指向主界面的视频 QLabel（例如 ui->videoLabel）
    FullScreenWindow *m_fullScreen;   // 全屏窗口
    QLabel *m_currentTarget;          // 当前显示目标（主UI 或 全屏UI）
    bool m_isFullScreen = false;

    QImage m_lastFrame;        // 缓存最新视频帧
    QMutex m_frameMutex;       // 保护 m_lastFrame

private:
    void SlideFuncInit();
    void KeysInit();                    //快捷键绑定函数

    void safeUpdatePixmap(); // 用于主线程刷新 pixmap

};
#endif // MAINWINDOW_H


