#ifndef PATHSEL_H
#define PATHSEL_H

#include <QTableWidget>
#include <QLabel>
#include <QToolButton>
#include <QObject>
#include <QPushButton>

#include "videomanager.h"

class PathSel: public QObject
{
    Q_OBJECT
private:
    QTableWidget* tableWidget;

    QLabel* pathLabel;
    QLabel* infoLabel;
    QToolButton* button;
    QString path;       //选择的文件夹路径
    QPushButton* Next;  //下一集
    QPushButton* Last;  //上一级
    QString m_lastSelectedPath;  // 保存上次选择的路径

    VideoManager* manager;

    void setLabelContent();
    QStringList getVideoList();

signals:
    void fileSelected(const QString& name, double sizeMB, const QString& duration);

private slots:
    void chooseDirectory();
    void updateTable();
    void updateInfoLabel();

    void onCellEntered(const QModelIndex& index);     // 鼠标 hover
    void onRowDoubleClicked(int row, int column);

public:
    PathSel(QTableWidget* tableWidget, QLabel* pathLabel, QToolButton* button, VideoManager* manager,
            QLabel* infoLabel, QPushButton* Next, QPushButton* Last);
    QString getPath();

    void initTable();
};

#endif // PATHSEL_H
