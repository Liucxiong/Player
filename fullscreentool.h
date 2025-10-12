// fullscreentool.h
#pragma once
#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
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
        if (show) {
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

private:
    QLabel *m_label = nullptr;
    QProgressBar *m_progress = nullptr;
};
