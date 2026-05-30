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

    // path = "D:/python_code/pa_chong/OutVideo";
    // this->manager->clear();
    // qDebug() << "Now Path: "<< path;
    // setLabelContent();
    // QStringList list =  getVideoList();
    // this->manager->addByFilePathList(list);
}
/**
 * @brief 槽函数，文件选择关键函数
 */
void PathSel::chooseDirectory()
{
    // 1. 使用上次选择的路径作为初始目录（记住上次目录）
    QString initialDir = m_lastSelectedPath.isEmpty()
                             ? QDir::homePath()
                             : m_lastSelectedPath;

    // 2. 创建文件对话框（仅选文件，不选文件夹）
    QFileDialog dialog(nullptr, "选择视频文件", initialDir);
    // 3. 关键配置（仅允许选音视频文件，不允许选文件夹）
    dialog.setFileMode(QFileDialog::ExistingFile);  // 仅允许选择单个现有文件（不允许文件夹）
    dialog.setNameFilter("所有媒体文件 (*.mp4 *.avi *.mov *.mkv *.flv *.wmv *.mp3 *.aac *.wav *.flac *.ogg *.opus *.m4a);;"
                         "视频文件 (*.mp4 *.avi *.mov *.mkv *.flv *.wmv);;"
                         "音频文件 (*.mp3 *.aac *.wav *.flac *.ogg *.opus *.m4a)");
    dialog.setViewMode(QFileDialog::Detail);  // 详细视图（方便查看文件信息）
    dialog.setOption(QFileDialog::DontUseNativeDialog, true);  // 使用Qt统一对话框（避免原生对话框限制）
    // 4. 显示对话框（用户只能选择文件，无法选文件夹）
    if (dialog.exec() != QDialog::Accepted) {
        return;  // 用户取消选择
    }
    // 5. 获取选中的文件路径（安全方式，避免临时对象问题）
    QStringList selectedFiles = dialog.selectedFiles();
    if (selectedFiles.isEmpty()) {
        return;  // 安全检查（理论上不会为空，因为已接受）
    }
    QString videoFilePath = selectedFiles.first();  // 选中的视频文件路径
    QFileInfo fileInfo(videoFilePath);
    // 6. 核心逻辑：选中视频文件 → 返回其所在文件夹路径
    QString folderPath = fileInfo.absolutePath();  // 提取文件所在目录
    // 7. 更新上次选择的路径（下次打开从此目录开始）
    m_lastSelectedPath = folderPath;
    // 8. 原有业务逻辑（使用文件夹路径）
    if (!folderPath.isEmpty()) {
        path = folderPath;  // 更新当前路径
        manager->clear();   // 清空原有数据
        qDebug() << "选中音视频文件所在文件夹：" << path;
        setLabelContent();  // 更新UI标签
        QStringList videoList = getVideoList();  // 获取该文件夹下视频文件列表
        manager->addByFilePathList(videoList);  // 加载视频列表
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

    int rowsToShow = 10;
    int headerH = tableWidget->horizontalHeader()->height();
    int rowH = tableWidget->verticalHeader()->defaultSectionSize();
    int frame = tableWidget->frameWidth() * 2;
    tableWidget->setMaximumHeight(headerH + rowH * rowsToShow + frame);

}

void PathSel::clearAll()
{
    path.clear();
    m_lastSelectedPath.clear();
    manager->clear();          // 清列表 + selected = -1 + 触发 updateTable
    pathLabel->setText("");
    pathLabel->setToolTip("");
    infoLabel->setText("");
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
 * @brief 加载指定目录的文件列表 (用于配置恢复)
 * @param dirPath 目录路径
 */
void PathSel::loadDirectory(const QString &dirPath)
{
    if (dirPath.isEmpty() || !QDir(dirPath).exists()) return;

    path = dirPath;
    m_lastSelectedPath = dirPath;
    manager->clear();
    setLabelContent();
    QStringList list = getVideoList();
    manager->addByFilePathList(list);
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
    QStringList videofilters,audiofilters;
    videofilters << "*.mp4" << "*.avi" << "*.mkv" << "*.mov"
            << "*.flv" << "*.wmv" << "*.mpeg" << "*.mpg";
    audiofilters << "*.mp3"<< "*.aac"<<"*.wav"<< "*.flac"<< "*.ogg"<< "*.opus"<< "*.m4a";
    // 获取文件
    QStringList videoFiles = dir.entryList(videofilters, QDir::Files | QDir::NoSymLinks);
    QStringList audioFiles = dir.entryList(audiofilters, QDir::Files | QDir::NoSymLinks);
    // 按名称排序
    videoFiles.sort(Qt::CaseInsensitive);  // 不区分大小写排序（推荐）
    audioFiles.sort(Qt::CaseInsensitive);
    // 带路径返回（可选）
    for (int i = 0; i < videoFiles.size(); ++i) {
        videoFiles[i] = dir.absoluteFilePath(videoFiles[i]);
        qDebug() << videoFiles[i];
    }
    for(int i = 0; i < audioFiles.size(); ++i) {
        audioFiles[i] = dir.absoluteFilePath(audioFiles[i]);
        qDebug() << audioFiles[i];
    }
    // 合并在一起
    QStringList result;
    result << audioFiles << videoFiles;
    return result;
}
/**
 * @brief 按类别输出相关信息
 */
void PathSel::updateInfoLabel()
{
    const VideoFile* file = manager->findByPos(manager->selected);
    if (!file) return;

    // 公共样式
    const QString style = QStringLiteral(R"(
        <style>
        table {
            border-collapse: separate;
            border-spacing: 10px 6px;
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
            color: #0078D7;
        }
        </style>
    )");

    // 辅助 lambda：如果字符串为空则显示 fallback（默认 "未知"）
    auto showOr = [](const QString &val, const QString &fallback = QStringLiteral("未知")) {
        return val.isEmpty() ? fallback : val;
    };
    // 数字显示（int/long）
    auto showNumOr = [](qint64 num, const QString &fallback = QStringLiteral("未知")) {
        return (num > 0) ? QString::number(num) : fallback;
    };

    // 公共字段
    QString fileName = file->fileName();
    QString sizeMb = QString::number(file->sizeMB(), 'f', 2);
    QString duration = file->durationStr();
    QString container = showOr(file->getContainer());
    QString bitrateKb = (file->getBitrate() > 0) ? QString::number(file->getBitrate() / 1000) : QStringLiteral("未知");
    QString channels = (file->getChannels() > 0) ? QString::number(file->getChannels()) : QStringLiteral("未知");
    QString sampleRate = (file->getSampleRate() > 0) ? QString::number(file->getSampleRate()) : QStringLiteral("未知");

    // 构造表格行
    QString rows;
    auto addRow = [&](const QString &k, const QString &v) {
        rows += QStringLiteral("<tr><td>%1</td><td>%2</td></tr>\n").arg(k, v);
    };

    // 始终显示的项
    addRow(QStringLiteral("文件名"), fileName);
    addRow(QStringLiteral("大小 (MB)"), sizeMb);
    addRow(QStringLiteral("时长"), duration);

    // 按类型显示特定项
    if (file->mediaType == VideoFile::Media_Video) {
        QString width = (file->getWidth() > 0) ? QString::number(file->getWidth()) : QStringLiteral("-");
        QString height = (file->getHeight() > 0) ? QString::number(file->getHeight()) : QStringLiteral("-");
        QString fps = showOr(file->getFps(), QStringLiteral("未知"));
        QString vcodec = showOr(file->getVideoCodec(), QStringLiteral("未知"));

        addRow(QStringLiteral("分辨率"), QString("%1 × %2").arg(width, height));
        addRow(QStringLiteral("帧率"), fps);
        addRow(QStringLiteral("视频编码"), vcodec);
        addRow(QStringLiteral("视频码率 (kbps)"), bitrateKb);
        addRow(QStringLiteral("声道数"), channels);
        addRow(QStringLiteral("采样率 (Hz)"), sampleRate);
        addRow(QStringLiteral("容器格式"), container);
    }
    else if (file->mediaType == VideoFile::Media_Audio) {
        QString acodec = showOr(file->getVideoCodec(), QStringLiteral("未知")); // 在你的实现里 audio codec 可能也在 getVideoCodec()
        addRow(QStringLiteral("音频编码"), acodec);
        addRow(QStringLiteral("采样率 (Hz)"), sampleRate);
        addRow(QStringLiteral("声道数"), channels);
        addRow(QStringLiteral("音频码率 (kbps)"), bitrateKb);
        addRow(QStringLiteral("容器格式"), container);
    }
    else {
        // 未知类型：尽量显示可用字段
        QString width = (file->getWidth() > 0) ? QString::number(file->getWidth()) : QStringLiteral("-");
        QString height = (file->getHeight() > 0) ? QString::number(file->getHeight()) : QStringLiteral("-");
        QString fps = showOr(file->getFps(), QStringLiteral("未知"));
        QString codec = showOr(file->getVideoCodec(), QStringLiteral("未知"));

        addRow(QStringLiteral("分辨率"), QString("%1 × %2").arg(width, height));
        addRow(QStringLiteral("帧率"), fps);
        addRow(QStringLiteral("Codec"), codec);
        addRow(QStringLiteral("码率 (kbps)"), bitrateKb);
        addRow(QStringLiteral("声道数"), channels);
        addRow(QStringLiteral("采样率 (Hz)"), sampleRate);
        addRow(QStringLiteral("容器格式"), container);
    }

    const QString html = style + QStringLiteral("<table>\n") + rows + QStringLiteral("</table>\n");
    infoLabel->setText(html);
    infoLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
}


