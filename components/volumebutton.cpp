#include "VolumeButton.h"
#include <QMenu>
#include <QWidgetAction>
#include <QVBoxLayout>
#include <QIcon>

VolumeButton::VolumeButton(QWidget *parent)
    : QToolButton(parent)
{
    // 基本外观设置
    setAutoRaise(true);
    setPopupMode(QToolButton::InstantPopup);
    setToolButtonStyle(Qt::ToolButtonIconOnly);
    setIconSize(QSize(24, 24));

    // 默认使用一个音量图标（可在 Designer 中覆盖）
    setIcon(QIcon(":/icons/icons/volume-high.svg"));

    setArrowType(Qt::NoArrow);

    setupPopup();
}

void VolumeButton::setupPopup()
{
    QMenu *menu = new QMenu(this);

    QWidget *container = new QWidget();
    QVBoxLayout *lay = new QVBoxLayout(container);
    lay->setContentsMargins(5, 5, 5, 5);
    lay->setSpacing(4);

    m_label = new QLabel("80%", container);
    m_label->setAlignment(Qt::AlignCenter);

    m_slider = new QSlider(Qt::Vertical, container);
    m_slider->setRange(m_minVol, m_maxVol);
    m_slider->setValue(80);
    m_slider->setFixedHeight(120);
    m_slider->setTickPosition(QSlider::TicksRight);

    lay->addWidget(m_label);
    lay->addWidget(m_slider, 0, Qt::AlignCenter);

    QWidgetAction *action = new QWidgetAction(menu);
    action->setDefaultWidget(container);
    menu->addAction(action);

    setMenu(menu);

    // 信号转发
    connect(m_slider, &QSlider::valueChanged, this, [this](int val) {
        m_label->setText(QString("%1%").arg(val));

        // 图标随音量变化
        if (val == 0)
            setIcon(QIcon(":/icons/icons/volume-mute.svg"));
        else if (val < 50)
            setIcon(QIcon(":/icons/icons/volume-low.svg"));
        else
            setIcon(QIcon(":/icons/icons/volume-high.svg"));

        emit volumeChanged(val);
    });
}

int VolumeButton::volume() const  { return m_slider->value(); }
int VolumeButton::maxVolume() const { return m_maxVol; }
int VolumeButton::minVolume() const { return m_minVol; }

void VolumeButton::setVolume(int vol) {
    m_slider->setValue(vol);
}
void VolumeButton::setMaxVolume(int max) {
    m_maxVol = max;
    m_slider->setMaximum(max);
}
void VolumeButton::setMinVolume(int min) {
    m_minVol = min;
    m_slider->setMinimum(min);
}
