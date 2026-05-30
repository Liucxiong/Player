#include "mainwindow.h"
#include "./ui_mainwindow.h"

#include <QScreen>
#include <QShortcut>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    /********************************配置防抖写入定时器（500ms 内无新改动才真正写盘）**********/
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(500);
    connect(m_saveTimer, &QTimer::timeout, this, [this](){
        m_config.save();
        qDebug() << "[scheduleSave] 配置已写入磁盘";
    });
    /******************************** UI 初始化(设置界面相关) ******************************/
    ui->setupUi(this);
    m_settings = new SettingsWidget(this);

    m_settings->setWindowTitle("Settings");
    m_settings->setWindowFlag(Qt::Dialog);
    m_settings->setAttribute(Qt::WA_DeleteOnClose, false); // 不自动删除
    //打开设置界面
    connect(ui->pushButton_5, &QPushButton::clicked, this, [this]{
        if (!m_settings) {
            m_settings = new SettingsWidget(this);
            m_settings->setWindowFlag(Qt::Dialog);
        }
        m_settings->show();
        m_settings->raise();
        m_settings->activateWindow();
    });
    // 缩放改变
    connect(m_settings,&SettingsWidget::scalingAlgorithmChanged,this,[=](int index){
        if(index < 0 || index >= 4) return ;
        if(index == manager->m_scalingAlgo) return ;
        qDebug() << "缩放算法改为：" << index;
        manager->m_scalingAlgo = index;
        player->setScalingAlgorithm(player->scalingAlgorithm[index]);
        m_config.setScalingAlgorithm(index);
        scheduleSave();
    });
    //重置功能
    connect(m_settings, &SettingsWidget::resetRequested, this, [this](){

        // 1) 退出全屏
        if (m_isFullScreen && m_fullScreen) {
            m_fullScreen->hide();
            m_fullScreen->showProgress(false);
            m_currentTarget = m_videoLabel;
            m_isFullScreen = false;
            ui->pushButton_4->setChecked(false);
        }

        // 2) 停止播放（必须在 clearAll 之前，否则访问已清空的文件信息）
        player->stop();

        // 3) 清空画面和缓存帧
        {
            QMutexLocker locker(&m_frameMutex);
            m_lastFrame = QImage();
        }
        m_videoLabel->clear();
        if (m_fullScreen)
            m_fullScreen->label()->clear();

        // 4) 重置进度条和时间标签
        ui->slider->setValue(0);
        ui->label_6->setText("00:00:00");   // 当前时间
        ui->label_7->setText("00:00:00");   // 总时长

        // 5) 播放按钮回到未选中
        ui->pushButton_2->setChecked(false);

        // 6) 清空文件列表和路径
        pathSel->clearAll();

        // 7) 重置配置并写盘
        m_config.reset();

        // 8) UI 同步到默认值
        manager->m_scalingAlgo = 1;
        player->setScalingAlgorithm(player->scalingAlgorithm[1]);

        ui->comboBox->setCurrentIndex(3);   // 1.0x

        ui->volumebtn->setVolume(80);
        player->setVolume(80);
        // 重置完成后关闭设置窗口
        m_settings->hide();
        qDebug() << "[MainWindow] 设置已完全重置";
    });
    /********************** 成员对象初始化 *****************************/
    connect(ui->volumebtn, &VolumeButton::volumeChanged, this, [this](int vol){
        player->setVolume(vol);
        m_config.setVolume(vol);
        scheduleSave();
    });

    manager = new VideoManager(this);
    pathSel = new PathSel(ui->tableWidget,ui->label_4, ui->toolButton, manager,
                          ui->label_2, ui->pushButton_3, ui->pushButton);
    connect(manager, &VideoManager::videosUpdated, this, [=]() {
        qDebug() << "视频列表更新，总数：" << manager->videos().size();
    });
    player = new VideoPlayer(this);

    // 初始化全屏窗口
    m_videoLabel = ui->label;
    m_fullScreen = new FullScreenWindow();
    connect(m_fullScreen, &FullScreenWindow::exitRequested, this, [=](){
        m_fullScreen->hide();
        m_fullScreen->showProgress(false);
        m_currentTarget = m_videoLabel;
        m_isFullScreen = false;
        ui->pushButton_4->setChecked(false);

        updateVideoRenderSize();
        safeUpdatePixmap();
    });
    m_currentTarget = m_videoLabel;
    // 全屏按钮点击
    connect(ui->pushButton_4, &QPushButton::clicked, this, &MainWindow::toggleFullScreen);


    //双向绑定播放按钮到Player
    connect(player, &VideoPlayer::playingChanged, ui->pushButton_2, &QPushButton::setChecked);
    //选中播放视频时切换播放
    connect(pathSel,&PathSel::fileSelected,this,[=]{
        qDebug() << "Now Sel Change to : " << manager->selected;
        const VideoFile * __file = manager->findByPos(manager->selected);
        if(!__file) return ;
        __file->printInfo();
        // 保存上一个文件的播放位置
        if (m_firstFileOpened) {
            QString prevFile = m_config.lastPlayedFile();
            if (!prevFile.isEmpty()) {
                double pos = player->getDuration() > 0 ? 0.0 : 0.0;
                // 用 slider 当前值反算位置
                const VideoFile* prevVf = nullptr;
                for (int i = 0; i < manager->getVideoListSize(); ++i) {
                    const VideoFile* vf = manager->findByPos(i);
                    if (vf && vf->fullPath() == prevFile) { prevVf = vf; break; }
                }
                if (prevVf && prevVf->getNumDuration() > 0) {
                    pos = double(ui->slider->value()) / ui->slider->maximum()
                          * prevVf->getNumDuration();
                    m_config.setFilePosition(prevFile, pos);
                }
            }
        }
        m_firstFileOpened = true;  // 标记已打开过第一个文件
        player->stop();         //先暂停在切换
        player->openFile(__file->fullPath());
        ui->label_7->setText(__file->durationStr());

        if (player->isAudioOnlyMode()) {
            QPixmap displayPix;

            if (__file->hasCoverArt()) {
                qDebug() << "封面图片尺寸:" << __file->getCoverArt().size();
                displayPix = QPixmap::fromImage(__file->getCoverArt());
            } else {
                qDebug() << "无嵌入封面, 使用默认图片";
                displayPix.load(":/icons/icons/default_music.svg");
            }

            if (!displayPix.isNull()) {
                qreal dpr = m_currentTarget->devicePixelRatioF();
                QSize fitSize = m_currentTarget->size() * dpr;
                QPixmap scaled = displayPix.scaled(fitSize,
                                                   Qt::KeepAspectRatio,
                                                   Qt::SmoothTransformation);
                scaled.setDevicePixelRatio(dpr);
                m_currentTarget->setPixmap(scaled);
                {
                    QMutexLocker locker(&m_frameMutex);
                    m_lastFrame = displayPix.toImage();  // 存原图, 全屏切换时重新缩放
                }
            }
        } else {
            updateVideoRenderSize();    //更新缩放
        }

        // 恢复上次播放位置
        double savedPos = m_config.filePosition(__file->fullPath());
        qDebug() << "Now File: " << __file->fullPath() << "Come Back To " << savedPos;
        // 如果保存位置接近结尾 (距末尾 < 5秒), 从头播放
        if (savedPos > 0.5 && (__file->getNumDuration() - savedPos) > 5.0) {
            player->play();
            player->seek(savedPos);
            qDebug() << "恢复播放位置:" << savedPos << "秒";
        } else {
            player->play();
        }
        // 记录当前播放文件
        m_config.setLastPlayedFile(__file->fullPath());
        scheduleSave();
    });
    // 绑定 VideoPlayer 信号到 UI
    connect(player, &VideoPlayer::frameReady, this, &MainWindow::onFrameReady);
    // 播放/暂停按钮
    connect(ui->pushButton_2, &QPushButton::clicked, this, &MainWindow::onPlayPauseClicked);
    // 绑定速率切换
    connect(ui->comboBox,&QComboBox::currentIndexChanged,this, &MainWindow::currentIndexSpeedChanged);

    // 初始化滚动条相关
    SlideFuncInit();

    // 播放器播放位置更新时，同步滑块位置
    connect(player, &VideoPlayer::positionChanged, this, [=](double pos){
        const VideoFile* zan = manager->findByPos(manager->selected);
        if(!zan) return ;
        double total = zan->getNumDuration();
        if (!ui->slider->isSliderDown()) {  // 用户未拖动时更 新滑块
            int value = int(pos / total * ui->slider->maximum());
            // qDebug() << value << ' ' << pos << ' ' << total;
            ui->slider->setValue(value);
            ui->label_6->setText(VideoFile::FormatStr(pos));
        }
        // 同步更新全屏进度条（如果全屏窗口存在且可见）
        if (m_fullScreen && m_fullScreen->isVisible()) {
            // 直接把 pos/total 传给全屏窗口，它会做映射
            m_fullScreen->setProgress(pos, total);
        }
    });

    // 连接 sliderMoved（计算 newPos 与 tip 位置）
    connect(ui->slider, &QSlider::sliderMoved, this, [=](int value){
        if (manager->selected < 0) return;
        const VideoFile* zan = manager->findByPos(manager->selected);
        if (!zan) return;
        double total = zan->getNumDuration();
        if (total <= 0) return;
        double newPos = double(value) / ui->slider->maximum() * total;
        QString text = VideoFile::FormatStr(newPos);

        // 计算把手屏幕位置（近似，用 slider 的 handle rect）
        QStyleOptionSlider opt;
        opt.initFrom(ui->slider);
        opt.orientation = ui->slider->orientation();
        opt.minimum = ui->slider->minimum();
        opt.maximum = ui->slider->maximum();
        opt.sliderPosition = value;
        QRect handleRect = ui->slider->style()->subControlRect(
            QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, ui->slider);

        QPoint handleCenter = handleRect.center();
        QPoint globalPos = ui->slider->mapToGlobal(handleCenter);
        // 把 tip 放在把手上方一点
        QPoint tipPos = globalPos + QPoint(-m_sliderTip->width()/2, -30);

        m_sliderTip->setText(text);
        m_sliderTip->adjustSize();
        // 重新计算基于新宽度
        tipPos = globalPos + QPoint(-m_sliderTip->width()/2, -m_sliderTip->height() - 6);

        m_sliderTip->move(tipPos);
        m_sliderTip->show();
    });

    // 隐藏 tip 在释放时 用户拖动结束时跳转视频
    connect(ui->slider, &QSlider::sliderReleased, this, [=](){
        if (m_sliderTip) m_sliderTip->hide();
        const VideoFile* zan = manager->findByPos(manager->selected);
        if(!zan) return ;
        double total = zan->getNumDuration();
        int value = ui->slider->value();
        double newPos = double(value) / ui->slider->maximum() * total;
        player->seek(newPos);
    });

    //播放结束，精度对齐
    connect(player, &VideoPlayer::finished, this, [=]() {
        qDebug() << "emited finished";
        ui->slider->setValue(ui->slider->maximum());
        ui->label_6->setText(VideoFile::FormatStr(manager->findByPos(manager->selected)->getNumDuration()));
        if (m_fullScreen && m_fullScreen->isVisible()) {
            // 直接把 pos/total 传给全屏窗口，它会做映射
            m_fullScreen->setProgress(1.0, 1.0);
        }
        // 播放完成, 清除该文件的位置记忆
        const VideoFile* f = manager->findByPos(manager->selected);
        if (f) {
            m_config.removeFilePosition(f->fullPath());
            m_config.save();
        }
    });

    KeysInit(); //快捷键绑定
    loadConfig(); // 从 config.json 恢复配置
}


MainWindow::~MainWindow()
{
    saveConfig(); // 退出时保存配置
    delete ui;
}

void MainWindow::scheduleSave()
{
    m_saveTimer->start();   // 每次调用都重置倒计时
}

/**
 * @brief 线程安全地展示视频
 */
void MainWindow::safeUpdatePixmap()
{
    // 拷贝局部指针和帧
    QLabel* target;
    QImage frame;
    bool audioOnly;
    {
        QMutexLocker locker(&m_frameMutex);
        target = m_currentTarget;
        frame = m_lastFrame;
    }
    if (!target || frame.isNull()) return;
    audioOnly = player && player->isAudioOnlyMode();

    // 确保在主线程执行 UI 更新
    QMetaObject::invokeMethod(this, [target, frame, audioOnly]() {
            QPixmap pix = QPixmap::fromImage(frame);
            if (audioOnly) {
                // 封面图: 缩放到适应标签大小
                qreal dpr = target->devicePixelRatioF();
                QSize fitSize = target->size() * dpr;
                pix = pix.scaled(fitSize,
                                 Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation);
            }
            pix.setDevicePixelRatio(target->devicePixelRatioF());
            target->setPixmap(pix);
        }, Qt::QueuedConnection);
}

// ----------------------- 视频帧回调 -----------------------
void MainWindow::onFrameReady(const QImage &img)
{
    if (img.isNull()) return;
    // 音频模式下忽略视频帧 (可能是 stop() 后事件队列中残留的旧信号)
    if (player->isAudioOnlyMode()) return;
    {
        QMutexLocker locker(&m_frameMutex);
        m_lastFrame = img;  // 缓存最新帧
    }
    safeUpdatePixmap();     // 主线程安全更新 QLabel
}

// ----------------------- 全屏切换 -----------------------
void MainWindow::toggleFullScreen()
{
    if (!m_fullScreen) return;

    if (!m_isFullScreen) {
        m_fullScreen->showFullScreen();
        m_fullScreen->showProgress(true);
        m_currentTarget = m_fullScreen->label();
        m_isFullScreen = true;
        ui->pushButton_4->setChecked(true);
    } else {
        m_fullScreen->hide();
        m_fullScreen->showProgress(false);
        m_currentTarget = m_videoLabel;
        m_isFullScreen = false;
        ui->pushButton_4->setChecked(false);
    }
    // 更新 VideoPlayer 输出尺寸
    updateVideoRenderSize();
    // 切换目标后立即刷新缓存帧
    safeUpdatePixmap();
}

// ----------------------- 通知 VideoPlayer 输出尺寸 -----------------------
void MainWindow::updateVideoRenderSize()
{
    if (!m_currentTarget || !player) return;
    const VideoFile* __file = manager->findByPos(manager->selected);
    if(!__file) return ;
    // 音频文件无视频尺寸, 不需要设置渲染大小
    if (__file->getWidth() <= 0 || __file->getHeight() <= 0) return;
    QSize targetSize = m_currentTarget->size();
    double rate = std::min(targetSize.width() * 1.0 / __file->getWidth(),targetSize.height() * 1.0 / __file->getHeight());

      // 逻辑尺寸 2239 x 1319 || 1119 x 699
    qreal dpr = m_currentTarget->devicePixelRatioF();   // DPR

    int pixelW = qRound(__file->getWidth() * rate * dpr);
    int pixelH = qRound(__file->getHeight() * rate * dpr);

    qDebug() << "[updateVideoRenderSize] target pixels:" << pixelW << "x" << pixelH
             << "DPR:" << dpr;

    player->setRenderSize(pixelW, pixelH); // decodeLoop 会重新创建 swsCtx
}

/**
 * @brief 绑定按钮与播放状态
 */
void MainWindow::onPlayPauseClicked()
{
    if(manager->selected == -1) {ui->pushButton_2->setChecked(false); return ;}
    if (ui->pushButton_2->isChecked()) {
        player->play();  // 如果暂停中，继续播放
    } else {
        player->pause();
    }
}
/**
 * @brief 播放速率选择
 */
void MainWindow::currentIndexSpeedChanged(int index)
{
    qDebug() << "Change Index to " << index;
    if(index < 0 || index >= manager->speedList.size()) return;
    if(std::abs(manager->playSpeed - manager->speedList[index]) < 0.01) return;

    manager->playSpeed = manager->speedList[index];

    // 不暂停，直接切换（推荐）
    player->setPlayRate(manager->playSpeed);
    m_config.setSpeedIndex(index);
    scheduleSave();
    qDebug() << "Change Rate to " << manager->playSpeed;
}

/**
 * @brief 滑动条和tip和视频窗口初始化函数
 */
void MainWindow::SlideFuncInit(){
    //滑动条初始化
    ui->slider->setMinimum(0);
    ui->slider->setMaximum(1000);  // 分成1000个刻度
    ui->slider->setOrientation(Qt::Horizontal);
    ui->slider->setSingleStep(1);
    ui->slider->setPageStep(10);
    // 初始化提示tip
    m_sliderTip = new QLabel(this, Qt::ToolTip);
    m_sliderTip->setAttribute(Qt::WA_ShowWithoutActivating);
    m_sliderTip->setAlignment(Qt::AlignCenter);
    m_sliderTip->setMargin(6);
    m_sliderTip->hide();

    QSize screenSize = qApp->primaryScreen()->size();
    QSize targetSize(screenSize.width() * 0.54, screenSize.height() * 0.54);
    // 固定视频窗口大小
    ui->label->setFixedSize(targetSize);
    // ui->label->setScaledContents(false); // 禁止自动拉伸变形
}

/**
 * @brief 从 config.json 恢复配置
 */
void MainWindow::loadConfig()
{
    // m_config 构造时已自动加载

    // 1) 恢复缩放算法
    int algo = m_config.scalingAlgorithm();
    if (algo >= 0 && algo < 4) {
        manager->m_scalingAlgo = algo;
        player->setScalingAlgorithm(player->scalingAlgorithm[algo]);
        qDebug() << "[loadConfig] 缩放算法:" << algo;
    }

    // 2) 恢复播放速度
    int spIdx = m_config.speedIndex();
    if (spIdx >= 0 && spIdx < manager->speedList.size()) {
        ui->comboBox->setCurrentIndex(spIdx);
        // setCurrentIndex 会触发 currentIndexSpeedChanged 信号
        qDebug() << "[loadConfig] 速度下标:" << spIdx;
    }

    // 3) 恢复上次打开的文件夹
    QString dir = m_config.lastDirectory();
    if (!dir.isEmpty() && QDir(dir).exists()) {
        pathSel->loadDirectory(dir);
        qDebug() << "[loadConfig] 目录:" << dir;
    }

    // 4) 恢复窗口大小
    int ww = m_config.windowWidth();
    int wh = m_config.windowHeight();
    if (ww > 200 && wh > 200) {
        resize(ww, wh);
        qDebug() << "[loadConfig] 窗口大小:" << ww << "x" << wh;
    }

    // 5) 恢复音量
    int vol = m_config.volume();
    ui->volumebtn->setVolume(vol);        // 更新 UI (会触发 volumeChanged)
    player->setVolume(vol);             // 同步到播放器
    qDebug() << "[loadConfig] 音量:" << vol;
}

/**
 * @brief 保存当前状态到 config.json
 */
void MainWindow::saveConfig()
{
    // 停掉防抖定时器，直接写盘
    if (m_saveTimer && m_saveTimer->isActive())
        m_saveTimer->stop();
    // 保存当前播放文件的位置
    const VideoFile* f = manager->findByPos(manager->selected);
    if (f && f->getNumDuration() > 0) {
        double pos = double(ui->slider->value()) / ui->slider->maximum()
                     * f->getNumDuration();
        m_config.setFilePosition(f->fullPath(), pos);
    }

    // 保存窗口大小
    m_config.setWindowSize(width(), height());

    // 保存当前目录
    m_config.setLastDirectory(pathSel->getPath());

    // 保存音量
    m_config.setVolume(ui->volumebtn->volume());

    m_config.save();
}

/**
 * @brief 快捷键设置
 */
void MainWindow::KeysInit(){
    // 播放 / 暂停：空格
    auto *playShortcut = new QShortcut(QKeySequence(Qt::Key_Space), this);
    playShortcut->setContext(Qt::ApplicationShortcut);  // 全局有效
    connect(playShortcut, &QShortcut::activated, ui->pushButton_2, &QPushButton::click);

    // 上一集：'['
    auto *prevShortcut = new QShortcut(QKeySequence(Qt::Key_BracketLeft), this);
    prevShortcut->setContext(Qt::ApplicationShortcut);
    connect(prevShortcut, &QShortcut::activated, ui->pushButton, &QPushButton::click);

    // 下一集：']'
    auto *nextShortcut = new QShortcut(QKeySequence(Qt::Key_BracketRight), this);
    nextShortcut->setContext(Qt::ApplicationShortcut);
    connect(nextShortcut, &QShortcut::activated, ui->pushButton_3, &QPushButton::click);

    // 全屏：回车
    auto *fullShortcut = new QShortcut(QKeySequence(Qt::Key_Return), this);
    fullShortcut->setContext(Qt::ApplicationShortcut);
    connect(fullShortcut, &QShortcut::activated, ui->pushButton_4, &QPushButton::click);

    // 快进 10 秒：右
    auto *forwardShortcut = new QShortcut(QKeySequence(Qt::Key_Right), this);
    forwardShortcut->setContext(Qt::ApplicationShortcut);
    forwardShortcut->setAutoRepeat(false); // 禁用自动重复
    connect(forwardShortcut, &QShortcut::activated, this, [this]() {
        player->forward(10.0);
    });
    // 后退 5 秒：左
    auto *backwardShortcut = new QShortcut(QKeySequence(Qt::Key_Left), this);
    backwardShortcut->setContext(Qt::ApplicationShortcut);
    backwardShortcut->setAutoRepeat(false); // 禁用自动重复
    connect(backwardShortcut, &QShortcut::activated, this, [this]() {
        player->forward(-5.0);
    });
}


