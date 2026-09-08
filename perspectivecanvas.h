#pragma once

#include "canvasdocument.h"
#include "paintengine.h"
#include "clonetool.h"
#include "planemath.h"
#include "imagetransformtool.h"
#include "planeedittool.h"
#include "scenecontentcache.h"

#include <QColor>
#include <QPointF>
#include <QPainterPath>
#include <QRectF>
#include <QVector>
#include <QWidget>

class QPainter;

// 透视画布：本应用的核心控件。
// 负责视图变换、输入路由和操作事务的协调；工具维护各自的拖动状态，
// 几何、文档、绘画引擎和场景缓存分别由独立模块承担。
class PerspectiveCanvas : public QWidget
{
    Q_OBJECT
public:
    enum Tool { CreatePlane, EditPlane, BrushTool, CloneStampTool, TransformTool, MarqueeTool };
    Q_ENUM(Tool)

    explicit PerspectiveCanvas(QWidget *parent = nullptr);
    bool loadImage(const QString &fileName);   // 从文件加载背景图像并重置文档状态
    bool saveResult(const QString &fileName) const; // 将当前场景（含绘画层）导出为图像文件
    bool hasSelectedPlane() const { return m_doc.selectedPlane() >= 0 && m_doc.selectedPlane() < m_doc.planes().size(); }
    bool hasLoadedImage() const { return m_doc.hasLoadedImage(); }
    bool hasSelectedImage() const { return m_doc.selectedImage() >= 0 && m_doc.selectedImage() < m_doc.images().size(); }
    QColor brushColor() const { return m_paint.color(); }

public slots:
    void setTool(Tool tool);
    void setBrushDiameter(int value) { m_paint.setDiameter(value); m_cloneTool.setDiameter(value); }
    void setBrushHardness(int value) { m_paint.setHardness(value); m_cloneTool.setHardness(value); }
    void setBrushOpacity(int value) { m_paint.setOpacity(value); m_cloneTool.setOpacity(value); }
    void setGridSize(int value) { m_gridSize = qMax(1, value); update(); }
    void setCloneAligned(bool aligned);
    void setBrushColor(const QColor &color) { m_paint.setColor(color); }
    void clearPainting();                  // 清除绘画层上的绘画内容
    void pasteClipboardImage();            // 把剪贴板图像作为浮动图像粘贴到画布
    void undo();                           // 撤销上一步操作
    void redo();                           // 重做被撤销的操作

signals:
    void imageSelectionChanged(bool selected);
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
    void dragEnterEvent(QDragEnterEvent *) override; // 拖放进入：接受本地图像文件
    void dropEvent(QDropEvent *) override;        // 拖放放下：打开图像或生成浮动图像

private:
    // 在控件坐标（经缩放/居中之后）与所有平面几何所使用的原始图像
    // 坐标系之间相互转换。
    QPointF toImage(const QPointF &widgetPoint) const;
    QPointF toWidget(const QPointF &imagePoint) const;
    void updateViewTransform();              // 根据控件尺寸计算缩放与居中偏移
    // 命中测试：判断某个画布坐标是否落在某张浮动图像上（从最上层开始）。
    // 返回该图像索引；可选输出该点在图像内的局部坐标。
    bool floatingImageAt(const QPointF &canvasPoint, int *imageIndex = nullptr,
                         QPointF *imagePoint = nullptr) const;
    // 把指定浮动图像吸附到目标平面所在的曲面分组（拷贝几何快照）。
    void attachImageToPlane(int imageIndex, int planeIndex, const QPointF &canvasPoint);
    // 已吸附图像沿其快照曲面移动；无法映射（越过极点线）时返回 false。
    bool moveAttachedImage(int imageIndex, const QPointF &canvasPoint);
    void updateHoverCursor(const QPointF &imagePoint);  // 根据悬停位置更新鼠标光标形状
    // 清空一切进行中的交互状态（撤销/重做/Esc 取消后调用）
    void cancelInteraction();
    void commitInteraction();
    void updateImageTransform(const QPointF &point, Qt::KeyboardModifiers modifiers);
    // 把拖入的图像设置为新的浮动图像并提示
    void dropFloatingImage(const QImage &image, const QString &statusText);
    // 用当前 4 个创建角点生成平面；有效时追加到文档并进入编辑工具
    void finishPlaneCreation();
    void beginClone(const QPointF &point, bool pickSource);
    void updateCloneMarker(const QPointF &point);
    int imageTransformHandleAt(const QPointF &point) const;
    int imageRotationCornerAt(const QPointF &point) const;
    bool pointToSelectionSurface(const QPointF &point, QPointF *surface) const;
    QPainterPath selectionPath() const;
    void updateSelection(const QPointF &point, Qt::KeyboardModifiers modifiers);
    int copySelectionToFloatingImage(const QPointF &point);
    // 把 Ctrl 拖动（区域克隆）的结果提取为浮动图像，而不是烘焙进绘画层
    int cloneSelectionToFloatingImage();
    void fillSelectionFromPoint(const QPointF &point);
    void clearSelection();

    enum class Gesture { Idle, Plane, Image, Brush, Clone, Selection };
    enum class SelectionAction { None, Create, Move, Fill };
    Gesture m_gesture = Gesture::Idle;
    bool drawing() const { return m_gesture == Gesture::Brush || m_gesture == Gesture::Clone; }
    ImageTransformTool m_imageTool;
    PlaneEditTool m_planeTool;
    CanvasDocument m_doc;                     // 文档模型（平面、绘画层、浮动图像、历史）
    SceneContentCache m_contentCache;
    qreal m_antsPhase = 0;
    PaintEngine m_paint;                      // 笔刷引擎
    CloneTool m_cloneTool;
    QVector<QPointF> m_creationPoints;        // 创建平面过程中已点击的角点
    Tool m_tool = CreatePlane;                // 当前工具
    int m_dragHandle = -1;                    // 正在拖动的控制点索引
    int m_dragEdge = -1;                      // 正在拖动的边缘索引
    int m_draggingImage = -1;                 // 正在拖动的浮动图像索引（-1 无）
    Plane m_extrudePreview;                   // 垂直平面的拖出预览
    bool m_hasExtrudePreview = false;
    Facet m_brushFacet;                       // 画笔锁定的面片快照
    int m_hoverPlane = -1;                    // 当前悬停的平面索引
    qreal m_scale = 1.0;                      // 视图缩放比例
    qreal m_gridSize = 50.0;                  // 平面展开坐标中的网格边长
    QPointF m_offset;                         // 视图居中偏移
    bool m_stateChanged = false;              // 自上次提交以来状态是否已变化
    QVector<Facet> m_selectionFaces;          // 选区所在共享曲面的当前几何快照
    QRectF m_selectionRect;                   // 展开曲面坐标中的矩形选区
    QRectF m_selectionStartRect;
    QPointF m_selectionPressSurface;
    QPointF m_selectionFillOffset;        // 区域克隆当前的取样偏移（展开曲面坐标）
    int m_selectionGroup = -1;
    SelectionAction m_selectionAction = SelectionAction::None;
    QImage m_selectionSampleSource;
    QImage m_selectionPaintBefore;
};
