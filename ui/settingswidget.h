// settingswidget.h
#pragma once
#include <QWidget>
#include <QButtonGroup>

namespace Ui { class SettingsWidget; }

class SettingsWidget : public QWidget
{
    Q_OBJECT
public:
    explicit SettingsWidget(QWidget *parent = nullptr);
    ~SettingsWidget();

private:
    void playQualityInit();

signals:
    void scalingAlgorithmChanged(int algo);

private:
    Ui::SettingsWidget *ui;   // ← 必须有

    QButtonGroup *m_buttonGroup;    // 缩放质量按钮组
};
