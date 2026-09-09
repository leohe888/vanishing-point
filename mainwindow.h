#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

class PerspectiveCanvas;
class QButtonGroup;
class QLabel;
class QSlider;

// 应用主窗口：负责构建工具栏、侧边工具面板并连接信号槽。
// 所有几何计算、渲染与交互状态均由 PerspectiveCanvas 管理。
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private:
    void buildUi();                     // 构建全部界面控件、样式表与信号连接
    void chooseColor();                 // 打开颜色对话框并更新画笔颜色
    void updateToolOptions(int toolId); // 根据当前工具显示/隐藏对应的参数行

    PerspectiveCanvas *m_canvas = nullptr; // 透视画布（核心绘制区域）
    QButtonGroup *m_tools = nullptr;       // 互斥的工具选择按钮组
    QSlider *m_diameter = nullptr;         // 笔刷直径滑块
    QSlider *m_hardness = nullptr;         // 笔刷硬度滑块
    QSlider *m_opacity = nullptr;          // 笔刷不透明度滑块
    QSlider *m_gridSize = nullptr;         // 平面网格大小滑块
    QSlider *m_planeAngle = nullptr;       // 子平面角度滑块（0–360°）
    QLabel *m_colorSwatch = nullptr;       // 当前画笔颜色的色块预览
    QWidget *m_brushTitle = nullptr;       // “笔刷设置”标题（按工具显隐）
    QWidget *m_planeTitle = nullptr;       // “平面设置”标题（按工具和文档显隐）
    QWidget *m_diameterRow = nullptr;      // 直径“标签+数值”行（按工具显隐）
    QWidget *m_hardnessRow = nullptr;      // 硬度“标签+数值”行（按工具显隐）
    QWidget *m_opacityRow = nullptr;       // 不透明度“标签+数值”行（按工具显隐）
    QWidget *m_gridSizeRow = nullptr;      // 网格大小“标签+数值”行（创建/编辑工具显示）
    QWidget *m_planeAngleRow = nullptr;    // 角度“标签+数值”行（创建/编辑工具显示）
    QWidget *m_colorRow = nullptr;         // 颜色选择行（仅画笔工具显示）
    QWidget *m_cloneAligned = nullptr;
};
#endif // MAINWINDOW_H
