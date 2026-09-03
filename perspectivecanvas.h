#pragma once

#include "canvasdocument.h"
#include "paintengine.h"
#include "planemath.h"

#include <QColor>
#include <QPointF>
#include <QVector>
#include <QWidget>

class QPainter;

// 透视画布：本应用的核心控件。
// 拆分之后它只负责两件事：视图变换（缩放/居中）与全部鼠标、键盘、
// 拖放交互。文档状态与撤销历史在 CanvasDocument 中，笔迹生成在
// PaintEngine 中，场景绘制在 SceneRenderer 中。
class PerspectiveCanvas : public QWidget
{
    Q_OBJECT
public:
    enum Tool { CreatePlane, EditPlane, StampTool, BrushTool }; // 工具枚举：创建平面/编辑平面/图章/画笔
    Q_ENUM(Tool)

    explicit PerspectiveCanvas(QWidget *parent = nullptr);
    bool loadImage(const QString &fileName);   // 从文件加载背景图像并重置文档状态
    bool saveResult(const QString &fileName) const; // 将当前场景（含绘画层）导出为图像文件
    bool hasSelectedPlane() const { return m_doc.selectedPlane() >= 0 && m_doc.selectedPlane() < m_doc.planes().size(); }
    bool hasLoadedImage() const { return m_doc.hasLoadedImage(); }
    QColor brushColor() const { return m_paint.color(); }

public slots:
    void setTool(Tool tool);
    void setBrushDiameter(int value) { m_paint.setDiameter(value); }        // 设置笔刷直径（像素）
    void setBrushHardness(int value) { m_paint.setHardness(value); }        // 设置笔刷硬度（0~1）
    void setBrushOpacity(int value) { m_paint.setOpacity(value); }          // 设置笔刷不透明度（0~1）
    void setBrushColor(const QColor &color) { m_paint.setColor(color); }
    void clearPainting();                  // 清除所有平面上的绘画内容
    void pasteClipboardImage();            // 把剪贴板图像作为浮动图像粘贴到画布
    void rotateFloatingImage();            // 将浮动图像顺时针旋转 90°
    void flipFloatingImageHorizontal();    // 将浮动图像水平翻转
    void flipFloatingImageVertical();      // 将浮动图像垂直翻转
    void undo();                           // 撤销上一步操作
    void redo();                           // 重做被撤销的操作

signals:
    void statusMessage(const QString &text, int timeout = 0);   // 在状态栏显示提示消息
    void toolChangeRequested(Tool tool);                        // 画布请求切换工具（如创建完平面后）
    void canUndoChanged(bool available);                        // 撤销可用性变化
    void canRedoChanged(bool available);                        // 重做可用性变化
    void documentAvailabilityChanged(bool available);           // 文档（背景图像）加载状态变化

protected:
    void paintEvent(QPaintEvent *) override;      // 绘制整个场景
    void resizeEvent(QResizeEvent *) override;    // 窗口尺寸变化时更新视图变换
    void mousePressEvent(QMouseEvent *) override; // 鼠标按下：按工具分派操作
    void mouseMoveEvent(QMouseEvent *) override;  // 鼠标移动：拖动/绘制/悬停光标
    void mouseReleaseEvent(QMouseEvent *) override; // 鼠标释放：提交历史记录
    void keyPressEvent(QKeyEvent *) override;     // 键盘事件：粘贴/Esc 取消/Delete 删除平面
    bool focusNextPrevChild(bool next) override;  // Tab 键在平面之间切换选中
    void dragEnterEvent(QDragEnterEvent *) override; // 拖放进入：接受本地图像文件
    void dropEvent(QDropEvent *) override;        // 拖放放下：打开图像或生成浮动图像

private:
    // 在控件坐标（经缩放/居中之后）与所有平面几何所使用的原始图像
    // 坐标系之间相互转换。
    QPointF toImage(const QPointF &widgetPoint) const;
    QPointF toWidget(const QPointF &imagePoint) const;
    void updateViewTransform();              // 根据控件尺寸计算缩放与居中偏移
    // 命中测试：判断某个画布坐标是否落在浮动图像上。
    // 已吸附时可返回该点在浮动图像内的局部坐标及其所在平面索引。
    bool pastedImageAt(const QPointF &canvasPoint, QPointF *imagePoint = nullptr,
                       int *planeIndex = nullptr) const;
    void updateHoverCursor(const QPointF &imagePoint);  // 根据悬停位置更新鼠标光标形状
    // 清空一切进行中的交互状态（撤销/重做/Esc 取消后调用）
    void cancelInteraction();
    // 把拖入的图像设置为新的浮动图像并提示
    void dropFloatingImage(const QImage &image, const QString &statusText);
    // 用当前 4 个创建角点生成平面；有效时追加到文档并进入编辑工具
    void finishPlaneCreation();

    CanvasDocument m_doc;                     // 文档模型（平面、浮动图像、历史）
    PaintEngine m_paint;                      // 笔刷/仿制图章引擎
    QVector<QPointF> m_creationPoints;        // 创建平面过程中已点击的角点
    Tool m_tool = CreatePlane;                // 当前工具
    int m_dragHandle = -1;                    // 正在拖动的控制点索引
    int m_dragEdge = -1;                      // 正在拖动的边缘索引
    bool m_dragging = false;                  // 正在拖动平面/控制点
    bool m_drawing = false;                   // 正在绘制笔迹
    bool m_extruding = false;                 // 正在从边缘拖出垂直平面
    bool m_draggingPastedImage = false;       // 正在拖动浮动图像
    QPointF m_pressImagePoint;                // 鼠标按下时的图像坐标
    QPointF m_lastImagePoint;                 // 最近一次的图像坐标
    Plane m_dragStartPlane;                   // 拖动开始时的平面快照
    Plane m_extrudePreview;                   // 垂直平面的拖出预览
    bool m_hasExtrudePreview = false;
    QPointF m_pastedDragStartPosition;        // 拖动开始时的浮动图像位置（供 Esc 取消恢复）
    QPointF m_pastedDragOffset;               // 拖动时鼠标相对图像左上角的偏移
    bool m_pastedDragStartAttached = false;   // 拖动开始时的吸附状态
    int m_pastedDragStartSurfaceGroup = -1;   // 拖动开始时的曲面分组
    int m_pastedDragStartHostPlane = -1;      // 拖动开始时的宿主平面
    qreal m_scale = 1.0;                      // 视图缩放比例
    QPointF m_offset;                         // 视图居中偏移
    bool m_stateChanged = false;              // 自上次提交以来状态是否已变化
};
