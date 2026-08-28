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
    void updateToolOptions(int toolId);

    PerspectiveCanvas *m_canvas = nullptr;
    QButtonGroup *m_tools = nullptr;
    QSlider *m_diameter = nullptr;
    QSlider *m_hardness = nullptr;
    QSlider *m_opacity = nullptr;
    QLabel *m_colorSwatch = nullptr;
    QWidget *m_brushTitle = nullptr;
    QWidget *m_diameterRow = nullptr;
    QWidget *m_hardnessRow = nullptr;
    QWidget *m_opacityRow = nullptr;
    QWidget *m_colorRow = nullptr;
};
#endif // MAINWINDOW_H
