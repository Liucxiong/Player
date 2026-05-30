#ifndef VOLUMEBUTTON_H
#define VOLUMEBUTTON_H

#include <QToolButton>
#include <QSlider>
#include <QLabel>

class VolumeButton : public QToolButton
{
    Q_OBJECT
    // 暴露给设计器的属性（可选）
    Q_PROPERTY(int volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(int maxVolume READ maxVolume WRITE setMaxVolume)   // 去掉 NOTIFY
    Q_PROPERTY(int minVolume READ minVolume WRITE setMinVolume)   // 去掉 NOTIFY

public:
    explicit VolumeButton(QWidget *parent = nullptr);

    int volume() const;
    int maxVolume() const;
    int minVolume() const;

public slots:
    void setVolume(int vol);
    void setMaxVolume(int max);
    void setMinVolume(int min);

signals:
    void volumeChanged(int volume);

private:
    void setupPopup();
    QSlider *m_slider;
    QLabel *m_label;
    int m_maxVol = 100;
    int m_minVol = 0;
};

#endif
