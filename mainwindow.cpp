#include "mainwindow.h"
#include "./ui_mainwindow.h"

#include <QScreen>
#include <QShortcut>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
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
        ui->pushButton_5->setChecked(false);

        updateVideoRenderSize();
        safeUpdatePixmap();
    });
    m_currentTarget = m_videoLabel;
    // 全屏按钮点击
    connect(ui->pushButton_5, &QPushButton::clicked, this, &MainWindow::toggleFullScreen);


    //双向绑定播放按钮到Player
    connect(player, &VideoPlayer::playingChanged, ui->pushButton_2, &QPushButton::setChecked);
    //选中播放视频时切换播放
    connect(pathSel,&PathSel::fileSelected,this,[=]{
        qDebug() << "Now Sel Change to : " << manager->selected;
        const VideoFile * __file = manager->findByPos(manager->selected);
        if(!__file) return ;
        __file->printInfo();
        player->stop();         //先暂停在切换
        player->openFile(__file->fullPath());
        ui->label_7->setText(__file->durationStr());
        updateVideoRenderSize();    //更新缩放
        player->play();
    });
    // 绑定 VideoPlayer 信号到 UI
    connect(player, &VideoPlayer::frameReady, this, &MainWindow::onFrameReady);
    // 播放/暂停按钮
    connect(ui->pushButton_2, &QPushButton::clicked, this, &MainWindow::onPlayPauseClicked);

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
        ui->slider->setValue(ui->slider->maximum());
        ui->label_6->setText(VideoFile::FormatStr(manager->findByPos(manager->selected)->getNumDuration()));
        if (m_fullScreen && m_fullScreen->isVisible()) {
            // 直接把 pos/total 传给全屏窗口，它会做映射
            m_fullScreen->setProgress(1.0, 1.0);
        }
    });

    KeysInit(); //快捷键绑定
}


MainWindow::~MainWindow()
{
    delete ui;
}

/**
 * @brief 线程安全地展示视频
 */
void MainWindow::safeUpdatePixmap()
{
    // 拷贝局部指针和帧
    QLabel* target;
    QImage frame;
    {
        QMutexLocker locker(&m_frameMutex);
        target = m_currentTarget;
        frame = m_lastFrame;
    }
    if (!target || frame.isNull()) return; // 在主线程前先检查

    // 确保在主线程执行 UI 更新
    QMetaObject::invokeMethod(this, [target, frame]() {
            QPixmap pix = QPixmap::fromImage(frame);
            pix.setDevicePixelRatio(target->devicePixelRatioF());
            target->setPixmap(pix);
        }, Qt::QueuedConnection);
}

// ----------------------- 视频帧回调 -----------------------
void MainWindow::onFrameReady(const QImage &img)
{
    if (img.isNull()) return;
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
        ui->pushButton_5->setChecked(true);
    } else {
        m_fullScreen->hide();
        m_fullScreen->showProgress(false);
        m_currentTarget = m_videoLabel;
        m_isFullScreen = false;
        ui->pushButton_5->setChecked(false);
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
    QSize targetSize(screenSize.width() / 2, screenSize.height() / 2);
    // 固定视频窗口大小
    ui->label->setFixedSize(targetSize);
    ui->label->setScaledContents(false); // 禁止自动拉伸变形
}

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
    connect(fullShortcut, &QShortcut::activated, ui->pushButton_5, &QPushButton::click);

    // 快进 10 秒：右
    auto *forwardShortcut = new QShortcut(QKeySequence(Qt::Key_Right), this);
    forwardShortcut->setContext(Qt::ApplicationShortcut);
    connect(forwardShortcut, &QShortcut::activated, this ,[this]() {
        player->forward(10.0);
    });
    // 后退 5 秒：左
    auto *backwardShortcut = new QShortcut(QKeySequence(Qt::Key_Left), this);
    backwardShortcut->setContext(Qt::ApplicationShortcut);
    connect(backwardShortcut, &QShortcut::activated, this ,[this]() {
        player->forward(-5.0);
    });
}
