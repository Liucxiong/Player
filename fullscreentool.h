// fullscreentool.h
#pragma once
#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTimer>
#include <QCursor>
#include <QProgressBar>

/**
 * @brief 全屏展示功能
 */
class FullScreenWindow : public QWidget {
    Q_OBJECT
public:
    explicit FullScreenWindow(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setWindowFlag(Qt::FramelessWindowHint);
        setWindowFlag(Qt::Window);
        setAttribute(Qt::WA_DeleteOnClose, false);
        setStyleSheet("background-color:black;"); // 全屏背景

        QVBoxLayout *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0,0,0,0);
        lay->setSpacing(0);

        // 视频显示区
        m_label = new QLabel(this);
        m_label->setAlignment(Qt::AlignCenter);
        m_label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        m_label->setStyleSheet("background: transparent;");
        lay->addWidget(m_label, /*stretch=*/1);

        // 底部容器，给进度条留出小的内边距
        QWidget *bottom = new QWidget(this);
        bottom->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        QVBoxLayout *bLay = new QVBoxLayout(bottom);
        bLay->setContentsMargins(8, 6, 8, 8); // left, top, right, bottom
        bLay->setSpacing(0);

        // 始终为进度条保留固定高度，避免进度条显隐导致视频区域大小变化
        bottom->setFixedHeight(20); // 为进度条保留 ~20px 空间

        // VERY THIN subtle progress bar (一直显示时尽量低调)
        m_progress = new QProgressBar(bottom);
        m_progress->setTextVisible(false);
        m_progress->setRange(0, 1000); // 精度到千分之一
        m_progress->setFixedHeight(3);  // 更细：3px
        m_progress->setAlignment(Qt::AlignCenter);
        m_progress->setFormat("");
        m_progress->setFocusPolicy(Qt::NoFocus);
        m_progress->setStyleSheet(R"(
            QProgressBar {
                background: rgba(255,255,255,0.04); /* 非常淡的轨道 */
                border-radius: 2px;
                border: 0px;
            }
            QProgressBar::chunk {
                border-radius: 2px;
                background: rgba(80,200,255,180); /* 更亮但仍柔和的蓝色 */
            }
        )");

        bLay->addWidget(m_progress);
        lay->addWidget(bottom, /*stretch=*/0);

        // 初始隐藏（进入全屏时可 showProgress(true)）
        m_progress->hide();

        // 控制项可见性状态
        m_controlsVisible = true;
        m_progressRequested = false;

        // 3 秒无鼠标活动后隐藏鼠标与进度条
        m_hideTimer = new QTimer(this);
        m_hideTimer->setSingleShot(true);
        connect(m_hideTimer, &QTimer::timeout, this, &FullScreenWindow::hideControls);
        m_hideTimer->start(3000);

        // 允许接收鼠标移动（如果你想在鼠标移动时做别的交互）
        setMouseTracking(true);
        m_label->setMouseTracking(true);
        bottom->setMouseTracking(true);
    }

    QLabel* label() const { return m_label; }

    // 外部用这个接口更新进度：pos/total（单位秒）
    void setProgress(double pos, double total) {
        if (total <= 0.0) {
            m_progress->setValue(0);
            return;
        }
        double ratio = pos / total;
        if (ratio < 0.0) ratio = 0.0;
        if (ratio > 1.0) ratio = 1.0;
        int v = int(ratio * m_progress->maximum());
        m_progress->setValue(v);
        // qDebug() << v;
    }

    // 控制进度条显隐（现在显示后不会自动隐藏）
    void showProgress(bool show) {
        m_progressRequested = show;
        // 仅当控件处于可见状态时才真正显示进度条；否则记录意图
        if (m_controlsVisible && show) {
            m_progress->show();
        } else {
            m_progress->hide();
        }
    }

signals:
    void exitRequested(); // 由用户触发退出（ESC 或 双击）

protected:
    void keyPressEvent(QKeyEvent *e) override {
        if (e->key() == Qt::Key_Escape)
            emit exitRequested();
        else
            QWidget::keyPressEvent(e);
    }
    void mouseDoubleClickEvent(QMouseEvent *) override {
        emit exitRequested();
    }

    void mouseMoveEvent(QMouseEvent *e) override {
        // 鼠标移动时显示控件并重置隐藏计时器
        showControls();
        if (m_hideTimer) m_hideTimer->start(3000);
        QWidget::mouseMoveEvent(e);
    }

private:
    QLabel *m_label = nullptr;
    QProgressBar *m_progress = nullptr;
    QTimer *m_hideTimer = nullptr;
    bool m_controlsVisible = true;
    bool m_progressRequested = false;

    void hideControls() {
        if (!m_controlsVisible) return;
        m_controlsVisible = false;
        // 隐藏鼠标
        setCursor(Qt::BlankCursor);
        // 隐藏进度条（但底部空间仍保留）
        m_progress->hide();
    }

    void showControls() {
        if (m_controlsVisible) return;
        m_controlsVisible = true;
        // 恢复默认鼠标
        unsetCursor();
        // 如果用户之前请求显示进度条，则恢复显示
        if (m_progressRequested) m_progress->show();
    }
};
