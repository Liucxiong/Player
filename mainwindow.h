#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTableWidget>
#include <QLabel>
#include <QToolButton>
#include <QDir>

#include "pathsel.h"
#include "videoplayer.h"
#include "fullscreentool.h"
#include "appconfig.h"

#include "ui/settingswidget.h"
// #include "components/volumebutton.h"

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
    SettingsWidget *m_settings = nullptr;
    // VolumeButton   *m_volumeBtn = nullptr;

    PathSel* pathSel;
    VideoPlayer* player;
    VideoManager* manager;

    QLabel *m_sliderTip = nullptr;  // 滑动条时间提示

    QLabel *m_videoLabel;             // 指向主界面的视频 QLabel（例如 ui->videoLabel）
    FullScreenWindow *m_fullScreen;   // 全屏窗口
    QLabel *m_currentTarget;          // 当前显示目标（主UI 或 全屏UI）
    bool m_isFullScreen = false;
    bool m_firstFileOpened = false;  // 新增：是否已经打开过第一个文件

    QImage m_lastFrame;        // 缓存最新视频帧
    QMutex m_frameMutex;       // 保护 m_lastFrame
    AppConfig m_config;        // 本地配置

private:
    void SlideFuncInit();
    void KeysInit();                    //快捷键绑定函数

    QTimer *m_saveTimer = nullptr;   // 配置延迟写入定时器
    void scheduleSave();             // 防抖写入

    void safeUpdatePixmap(); // 用于主线程刷新 pixmap
    void loadConfig();         // 启动时从 config.json 恢复配置
    void saveConfig();         // 退出/切换时写入 config.json
};
#endif // MAINWINDOW_H


