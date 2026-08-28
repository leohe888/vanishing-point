#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

class PerspectiveCanvas;
class QButtonGroup;
class QLabel;
class QSlider;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private:
    void buildUi();
    void chooseColor();

    PerspectiveCanvas *m_canvas = nullptr;
    QButtonGroup *m_tools = nullptr;
    QSlider *m_diameter = nullptr;
    QSlider *m_hardness = nullptr;
    QSlider *m_opacity = nullptr;
    QLabel *m_colorSwatch = nullptr;
};
#endif // MAINWINDOW_H
