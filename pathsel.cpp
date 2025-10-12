#include "pathsel.h"

#include <QFileDialog>
#include <QDir>
#include <QDebug>
#include <QStringList>
#include <QHeaderView>

/**
 * @brief PathSel::PathSel
 * @param tableWidget 表格组件
 * @param pathLabel 路径标签
 * @param button 选择按钮
 */
PathSel::PathSel(QTableWidget* tableWidget, QLabel* pathLabel, QToolButton* button, VideoManager* manager,
                 QLabel* infoLabel, QPushButton* Next, QPushButton* Last){
    this->pathLabel = pathLabel;
    this->infoLabel = infoLabel;
    this->tableWidget = tableWidget;
    this->button = button;
    this->manager = manager;
    this->Last = Last; this->Next = Next;

    infoLabel->setTextFormat(Qt::RichText);
    infoLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    infoLabel->setWordWrap(true);

    initTable();

    //监听 ToolButton 点击信号
    if (button)
        connect(button, &QToolButton::clicked, this, &PathSel::chooseDirectory);
    // 监听 VideoManager 列表更新信号
    if (manager)
        connect(manager, &VideoManager::videosUpdated, this, &PathSel::updateTable);

    if(infoLabel)
        connect(this, &PathSel::fileSelected, this,[=]{
            this->updateInfoLabel();
        });

    connect(Last, &QPushButton::clicked, this,[=]{
        int num = manager->getVideoListSize();
        if(!num) return ;
        int row = (manager->selected - 1 + num) % num;
        onRowDoubleClicked(row,0);
    });
    connect(Next, &QPushButton::clicked, this,[=]{
        int num = manager->getVideoListSize();
        if(!num) return ;
        int row = (manager->selected + 1 + num) % num;
        onRowDoubleClicked(row,0);
    });

    path = "D:/python_code/pa_chong/OutVideo";
    this->manager->clear();
    qDebug() << "Now Path: "<< path;
    setLabelContent();
    QStringList list =  getVideoList();
    this->manager->addByFilePathList(list);
}
/**
 * @brief 槽函数，文件选择关键函数
 */
void PathSel::chooseDirectory(){
    path = QFileDialog::getExistingDirectory(nullptr,"选择文件夹",QDir::homePath());
    if(!path.isEmpty()){
        manager->clear();   //触发清空
        qDebug() << "Now Path: "<< path;
        setLabelContent();
        QStringList list =  getVideoList();
        manager->addByFilePathList(list);
    }
}
/**
 * @brief 根据当前视频信息列表创建选中行
 */
void PathSel::updateTable()
{
    if (!tableWidget || !manager) return;

    const QList<VideoFile> &videos = manager->videos();

    tableWidget->clearContents();
    tableWidget->setRowCount(videos.size());

    for (int i = 0; i < videos.size(); ++i) {
        const VideoFile &v = videos.at(i);

        // 文件名 → 左对齐
        QTableWidgetItem *nameItem = new QTableWidgetItem(v.fileName());
        nameItem->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        nameItem->setToolTip(v.fullPath()); // 鼠标悬停显示完整路径
        tableWidget->setItem(i, 0, nameItem);

        // 大小 → 居中
        QString sizeStr = QString::number(v.sizeMB(), 'f', 2);
        QTableWidgetItem *sizeItem = new QTableWidgetItem(sizeStr);
        sizeItem->setTextAlignment(Qt::AlignCenter);
        sizeItem->setToolTip(sizeStr); // 悬停也显示完整数字
        tableWidget->setItem(i, 1, sizeItem);

        // 时长 → 居中
        QString durStr = v.durationStr();
        QTableWidgetItem *durationItem = new QTableWidgetItem(durStr);
        durationItem->setTextAlignment(Qt::AlignCenter);
        durationItem->setToolTip(durStr); // 悬停显示完整时长
        tableWidget->setItem(i, 2, durationItem);
    }
}

void PathSel::initTable()
{
    // 设置列头
    tableWidget->setColumnCount(3);
    QStringList headers = { "文件名", "大小 (MB)", "时长" };
    tableWidget->setHorizontalHeaderLabels(headers);

    // 禁止 Qt 默认的单击选中效果
    tableWidget->setSelectionMode(QAbstractItemView::NoSelection);
    tableWidget->setSelectionBehavior(QAbstractItemView::SelectRows);

    // 鼠标进入 cell 信号，用来做“整行 hover”
    tableWidget->setMouseTracking(true); // 必须启用
    connect(tableWidget, &QTableWidget::entered,
            this, &PathSel::onCellEntered);

    // 双击时真正选中
    connect(tableWidget, &QTableWidget::cellDoubleClicked,
            this, &PathSel::onRowDoubleClicked);


}
/**
 * @brief 鼠标悬浮某一行改变其样式
 * @param index
 */
void PathSel::onCellEntered(const QModelIndex& index)
{
    // 清除之前的 hover 样式
    for (int r = 0; r < tableWidget->rowCount(); ++r) {
        for (int c = 0; c < tableWidget->columnCount(); ++c) {
            QTableWidgetItem* item = tableWidget->item(r, c);
            if (item && r != manager->selected) { // 避免覆盖选中行
                item->setBackground(Qt::white);
            }
        }
    }
    // 给当前 hover 的整行上色
    int row = index.row();
    if (row != manager->selected) { // 不覆盖选中行的颜色
        for (int c = 0; c < tableWidget->columnCount(); ++c) {
            QTableWidgetItem* item = tableWidget->item(row, c);
            if (item) {
                item->setBackground(QColor(230, 242, 255));
            }
        }
    }
}
/**
 * @brief 双击选中行
 * @param row
 * @param column
 */
void PathSel::onRowDoubleClicked(int row, int column)
{
    Q_UNUSED(column);

    // 清除之前选中行的加粗效果
    if (manager->selected >= 0) {
        for (int c = 0; c < tableWidget->columnCount(); ++c) {
            QTableWidgetItem* item = tableWidget->item(manager->selected, c);
            if (item) {
                QFont font = item->font();
                font.setBold(false);
                item->setFont(font);
            }
        }
    }
    // 设置当前行加粗
    for (int c = 0; c < tableWidget->columnCount(); ++c) {
        QTableWidgetItem* item = tableWidget->item(row, c);
        if (item) {
            QFont font = item->font();
            font.setBold(true);
            item->setFont(font);
        }
    }
    // 更新记录的选中行
    manager->selected = row;
    // 发射信号
    QString fileName    = tableWidget->item(row, 0)->text();
    double sizeMB       = tableWidget->item(row, 1)->text().toDouble();
    QString durationStr = tableWidget->item(row, 2)->text();

    emit fileSelected(fileName, sizeMB, durationStr);
}

/**
 * @brief PathSel::getPath
 * @return 返回当前文件夹路径
 */
QString PathSel::getPath(){
    return path;
}
/**
 * @brief 为路径区域设置路径名
 */
void PathSel::setLabelContent(){
    QFontMetrics fm(pathLabel->font());
    QString elided = fm.elidedText(path, Qt::ElideMiddle, pathLabel->width());
    pathLabel->setText(elided);
    pathLabel->setToolTip(path); // 悬停显示完整路径
}
/**
 * @brief 查询选择的文件夹下有哪些可播放的视频
 * @return 视频路径列表
 */
QStringList PathSel::getVideoList(){
    QDir dir(path);
    // 设定要匹配的视频扩展名
    QStringList filters;
    filters << "*.mp4" << "*.avi" << "*.mkv" << "*.mov"
            << "*.flv" << "*.wmv" << "*.mpeg" << "*.mpg";
    // 获取文件
    QStringList videoFiles = dir.entryList(filters, QDir::Files | QDir::NoSymLinks);
    // 带路径返回（可选）
    for (int i = 0; i < videoFiles.size(); ++i) {
        videoFiles[i] = dir.absoluteFilePath(videoFiles[i]);
        qDebug() << videoFiles[i];
    }
    return videoFiles;
}

void PathSel::updateInfoLabel()
{
    const VideoFile* __file = manager->findByPos(manager->selected);
    if (!__file) return;

    QString info = R"(
        <style>
        table {
            border-collapse: separate;
            border-spacing: 10px 6px; /* 横向列距10px，纵向行距6px */
            font-family: "Microsoft YaHei", "微软雅黑", sans-serif;
            font-size: 13px;
            color: #222;
        }
        td:first-child {
            font-weight: bold;
            color: #444;
            text-align: right;
            min-width: 90px;
            white-space: nowrap;
        }
        td:last-child {
            color: #0078D7; /* Windows 蓝色风格 */
        }
        </style>

        <table>
        <tr><td>文件名</td><td>%1</td></tr>
        <tr><td>大小 (MB)</td><td>%2</td></tr>
        <tr><td>时长 (秒)</td><td>%3</td></tr>
        <tr><td>分辨率</td><td>%4 × %5</td></tr>
        <tr><td>帧率</td><td>%6</td></tr>
        <tr><td>视频编码</td><td>%7</td></tr>
        <tr><td>视频码率</td><td>%8 kbps</td></tr>
        <tr><td>声道数</td><td>%9</td></tr>
        <tr><td>容器格式</td><td>%10</td></tr>
        </table>
    )";

    infoLabel->setText(info.arg(
            __file->fileName(),
            QString::number(__file->sizeMB(), 'f', 2),
            __file->durationStr(),
            QString::number(__file->getWidth()),
            QString::number(__file->getHeight()),
            __file->getFps(),
            __file->getFormat(),
            QString::number(__file->getBitrate() / 1000),
            QString::number(__file->getChannels()),
            __file->getCode()
        ));

    infoLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
}

