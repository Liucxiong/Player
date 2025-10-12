#include "mainwindow.h"

#include <QApplication>
#include <QLocale>
#include <QTranslator>

extern "C" {
#include <libavformat/avformat.h>
}

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    QTranslator translator;
    const QStringList uiLanguages = QLocale::system().uiLanguages();
    for (const QString &locale : uiLanguages) {
        const QString baseName = "Player_" + QLocale(locale).name();
        if (translator.load(":/i18n/" + baseName)) {
            a.installTranslator(&translator);
            break;
        }
    }
    //FFmpeg初始化
    avformat_network_init();

    MainWindow w;
    w.show();
    return a.exec();
}
