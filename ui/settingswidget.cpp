// settingswidget.cpp
#include "settingswidget.h"
#include "ui/ui_settingswidget.h"
#include <QMessageBox>
#include <QKeyEvent>

SettingsWidget::SettingsWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::SettingsWidget)
{
    ui->setupUi(this);

    // Designer 中的 listWidget 和 stackedWidget 会变成 ui->listWidget / ui->stackedWidget
    connect(ui->listWidget, &QListWidget::currentRowChanged,
            ui->stackedWidget, &QStackedWidget::setCurrentIndex);

    playQualityInit();  // 第一项设置初始化
    resetRequestedInit();   // 第二项设置初始化

    // 如果你在 Designer 已经为 list 添加了 items，它们会存在
    if (ui->listWidget->count() > 0)
        ui->listWidget->setCurrentRow(0);
}
void SettingsWidget::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape)
        hide();
    else
        QWidget::keyPressEvent(e);
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

void SettingsWidget::resetRequestedInit(){
    // 方式 A：.ui 中已有按钮
    connect(ui->resetButton, &QPushButton::clicked, this, [this](){
        // 弹确认框，避免误触
        auto ret = QMessageBox::question(
            this, "重置设置",
            "确定要将所有设置恢复为默认值吗？",
            QMessageBox::Yes | QMessageBox::No);
        if (ret == QMessageBox::Yes)
            emit resetRequested();
    });
}
