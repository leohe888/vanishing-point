// 程序入口：创建 Qt 应用与主窗口，进入事件循环
#include "mainwindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv); // Qt 应用对象：管理事件循环与全局资源
    MainWindow w;               // 主窗口：包含工具栏、侧边面板与透视画布
    w.show();
    return QApplication::exec(); // 进入事件循环，直至窗口关闭
}
