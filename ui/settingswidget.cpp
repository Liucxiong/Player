// settingswidget.cpp
#include "settingswidget.h"
#include "ui/ui_settingswidget.h"

SettingsWidget::SettingsWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::SettingsWidget)
{
    ui->setupUi(this);

    // Designer 中的 listWidget 和 stackedWidget 会变成 ui->listWidget / ui->stackedWidget
    connect(ui->listWidget, &QListWidget::currentRowChanged,
            ui->stackedWidget, &QStackedWidget::setCurrentIndex);

    playQualityInit();  // 第一项设置初始化

    // 如果你在 Designer 已经为 list 添加了 items，它们会存在
    if (ui->listWidget->count() > 0)
        ui->listWidget->setCurrentRow(0);
}

SettingsWidget::~SettingsWidget()
{
    delete ui;
}

/**
 * @brief 设置 1 缩放质量选择
 */
void SettingsWidget::playQualityInit(){
    m_buttonGroup = new QButtonGroup(this);
    m_buttonGroup->addButton(ui->radioButton, 0);
    m_buttonGroup->addButton(ui->radioButton_2, 1);
    m_buttonGroup->addButton(ui->radioButton_3, 2);
    m_buttonGroup->addButton(ui->radioButton_4, 3);

    m_buttonGroup->button(1)->setChecked(true);

    connect(m_buttonGroup, &QButtonGroup::idClicked, this,[=](int id){
        emit scalingAlgorithmChanged(id); // 只发信号
    });
}
