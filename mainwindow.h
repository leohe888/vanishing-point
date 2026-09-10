#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

class QAbstractButton;
class QAction;
class QButtonGroup;
class QCheckBox;
class QColor;
class QFrame;
class QLabel;
class QMenu;
class QSlider;
class QVBoxLayout;
class QWidget;
class PerspectiveCanvas;

// 侧栏中的一个"名称 + 数值 + 滑块"参数行。
struct SliderRow
{
    QWidget *group = nullptr;   // 整行容器：负责显示/隐藏、启用、整体变灰
    QSlider *slider = nullptr;  // 真正的滑块：连接数值变化信号

    void setVisible(bool visible);
    void setEnabled(bool enabled);
};

// 应用主窗口：负责构建菜单栏、侧边工具面板并连接信号槽。
// 所有几何计算、渲染与交互状态均由 PerspectiveCanvas 管理，这里只做界面组装。
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private:
    // —— 界面构建（详见 buildUi 中的调用顺序）——
    void buildUi();                             // 总装：菜单栏 + 侧栏 + 画布 + 信号
    void buildMenuBar();                        // 菜单栏：文件 / 编辑 / 视图 / 帮助
    void buildFileActions(QMenu *menu);         // 打开 / 导出
    void buildEditActions(QMenu *menu);         // 撤销 / 重做 / 粘贴
    QFrame *buildSidePanel();                   // 左侧工具与参数面板
    void buildToolButtons(QVBoxLayout *panel);  // 工具按钮组，与工具枚举一一对应
    void buildColorRow(QVBoxLayout *panel);     // 画笔颜色色块 + 选择按钮
    SliderRow addSliderRow(QVBoxLayout *panel, const QString &name,
                           int minimum, int maximum, int value);
    void applyStyleSheet();                     // 应用全局样式表

    // —— 信号连接 ——
    // 约定：控件自身的局部连接（滑块 → 数值标签等）在构建处就地完成；
    // 所有跨组件（画布 ↔ 窗口）的连接统一收在下面这几个方法里。
    void connectSignals();                      // 依次调用下面各项
    void connectFileActions();                  // 打开 / 导出
    void connectEditingActions();               // 撤销 / 重做 / 粘贴剪贴板
    void connectToolSelection();                // 工具按钮与工具快捷键
    void connectBrushParameters();              // 笔刷与图章参数
    void connectPlaneParameters();              // 平面网格与夹角
    void connectDocumentState();                // 文档状态变化 → 界面可用性

    // —— 交互行为 ——
    void openImage();                           // 选择文件并交给画布加载
    void exportResult();                        // 选择文件并导出当前合成结果
    void chooseColor();                         // 打开颜色对话框并更新画笔颜色
    void setSwatchColor(const QColor &color);   // 把色块预览同步成指定颜色
    void updateToolOptions(int toolId);         // 根据当前工具显示/隐藏对应的参数行
    void updatePlaneAngleState();               // 根据"能否调整夹角"切换角度行的可用与锁定样式

    // —— 成员 ——
    PerspectiveCanvas *m_canvas = nullptr;

    QAction *m_openAction = nullptr;
    QAction *m_saveAction = nullptr;
    QAction *m_undoAction = nullptr;
    QAction *m_redoAction = nullptr;
    QAction *m_pasteAction = nullptr;           // 把剪贴板图像粘贴为浮动图像

    QMenu *m_viewMenu = nullptr;                // 视图菜单：功能待实现，目前只有占位项
    QMenu *m_helpMenu = nullptr;                // 帮助菜单：同上

    QButtonGroup *m_tools = nullptr;            // 工具按钮组，button id 即工具枚举值

    QLabel *m_brushTitle = nullptr;             // "笔刷设置"小标题
    QLabel *m_planeTitle = nullptr;             // "平面设置"小标题
    QWidget *m_color = nullptr;                 // 画笔颜色整行
    QLabel *m_colorSwatch = nullptr;            // 颜色预览色块
    QCheckBox *m_cloneAligned = nullptr;        // 图章"对齐"选项

    SliderRow m_diameter;                       // 笔刷直径
    SliderRow m_hardness;                       // 笔刷硬度
    SliderRow m_opacity;                        // 笔刷不透明度
    SliderRow m_gridSize;                       // 平面展开网格边长
    SliderRow m_planeAngle;                     // 子平面与父平面的夹角
};

#endif // MAINWINDOW_H
