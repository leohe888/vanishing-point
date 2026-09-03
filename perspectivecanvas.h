#pragma once

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QVector>
#include <QWidget>

class QPainter;

// 透视画布：本应用的核心控件。
// 负责背景图像的显示、透视平面的创建与编辑、透视绘画/仿制图章、
// 浮动（粘贴/拖入）图像的管理，以及撤销/重做历史。
class PerspectiveCanvas : public QWidget
{
    Q_OBJECT
public:
    enum Tool { CreatePlane, EditPlane, StampTool, BrushTool }; // 工具枚举：创建平面/编辑平面/图章/画笔
    Q_ENUM(Tool)

    explicit PerspectiveCanvas(QWidget *parent = nullptr);
    bool loadImage(const QString &fileName);   // 从文件加载背景图像并重置文档状态
    bool saveResult(const QString &fileName) const; // 将当前场景（含绘画层）导出为图像文件
    bool hasSelectedPlane() const { return m_selectedPlane >= 0 && m_selectedPlane < m_planes.size(); }
    bool hasLoadedImage() const { return m_hasLoadedImage; }
    QColor brushColor() const { return m_brushColor; }

public slots:
    void setTool(Tool tool);
    void setBrushDiameter(int value) { m_diameter = value; }        // 设置笔刷直径（像素）
    void setBrushHardness(int value) { m_hardness = value / 100.0; } // 设置笔刷硬度（0~1）
    void setBrushOpacity(int value) { m_opacity = value / 100.0; }   // 设置笔刷不透明度（0~1）
    void setBrushColor(const QColor &color) { m_brushColor = color; }
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
    // 一个平面保存其在画布和展开曲面上的几何信息，以及一张供画笔和
    // 仿制图章工具使用的透明纹理。
    struct Plane {
        QPointF corner[4];
        // 相邻平面共享一个展开的 2D 曲面。这些坐标允许同一张浮动图像
        // 跨越接缝，而每个面仍使用各自到画布的单应变换。
        QPointF surfaceCorner[4];
        int surfaceGroup = -1;  // 所属的展开曲面分组（共享曲面的相邻平面同组）
        QImage paint;           // 该平面的绘画纹理（UV 空间）
        QString name;           // 显示用的平面名称
    };

    // 历史快照有意使用 Qt 的隐式共享 QImage，因此未被后续编辑修改的
    // 图层不会真正被复制。
    struct CanvasState {
        QVector<Plane> planes;              // 全部平面
        int selectedPlane = -1;             // 当前选中的平面索引
        QImage pastedImage;                 // 浮动图像位图
        QPointF pastedImagePosition;        // 浮动图像位置（画布或曲面坐标）
        bool pastedImageAttached = false;   // 浮动图像是否已吸附到某个曲面
        int pastedSurfaceGroup = -1;        // 浮动图像吸附到的曲面分组
        int pastedHostPlane = -1;           // 浮动图像的宿主平面索引
    };

    // 在控件坐标（经缩放/居中之后）与所有平面几何所使用的原始图像
    // 坐标系之间相互转换。
    QPointF toImage(const QPointF &widgetPoint) const;
    QPointF toWidget(const QPointF &imagePoint) const;
    void updateViewTransform();              // 根据控件尺寸计算缩放与居中偏移
    int planeAt(const QPointF &imagePoint) const;   // 命中测试：返回点所在的平面索引
    int handleAt(const Plane &plane, const QPointF &imagePoint) const; // 命中测试：控制点索引
    int edgeAt(const Plane &plane, const QPointF &imagePoint) const;   // 命中测试：边缘索引
    QVector<QPointF> handles(const Plane &plane) const; // 平面的 4 个角点 + 4 个边中点
    // 平面操作使用归一化 UV 坐标。一个单应变换在两个方向上把单位正方形
    // 映射到用户的四点四边形。
    QPointF planeToUv(const Plane &plane, const QPointF &point, bool *ok = nullptr) const;
    QPointF uvToPlane(const Plane &plane, const QPointF &uv) const;
    QPointF planeToSurface(const Plane &plane, const QPointF &point,
                           bool *ok = nullptr) const;
    void renderScene(QPainter &painter, bool showGuides) const;          // 渲染完整场景
    void renderProjectedImage(QPainter &painter, const Plane &plane, const QImage &texture) const; // 用单应变换把纹理投影到平面
    void renderPastedImage(QPainter &painter) const;                     // 渲染浮动图像（可跨面）
    bool pastedImageAt(const QPointF &canvasPoint, QPointF *imagePoint = nullptr,
                       int *planeIndex = nullptr) const; // 命中测试：点是否落在浮动图像上
    void setFloatingImage(const QImage &image, const QString &statusText); // 设置新的浮动图像并重置其状态
    void drawPlaneGuides(QPainter &painter, const Plane &plane, bool selected) const; // 绘制平面网格与控制点
    // 在纹理空间落下一个笔触点；透视效果在该纹理被投影回平面时自然产生。
    void applyDab(Plane &plane, const QPointF &uv, bool stamp);
    void drawStrokeTo(const QPointF &imagePoint, bool stamp); // 沿笔迹插值补间并落点
    void updateHoverCursor(const QPointF &imagePoint);        // 根据悬停位置更新鼠标光标形状
    Plane makePerpendicularPlane(const Plane &source, int edge, const QPointF &dragPoint) const; // 沿某条边拖出垂直平面
    Plane resizePlaneAlongEdge(const Plane &source, int edge, const QPointF &dragPoint) const;   // 沿某条边方向缩放平面
    // 由平面法线恢复投影后的第三个消失方向。
    bool perpendicularDirection(const Plane &source, const QPointF &atPoint,
                                QPointF *direction) const;
    static bool isValidPlane(const Plane &plane);  // 校验平面是否为有效的凸四边形
    CanvasState captureState() const;              // 捕获当前状态为快照
    void restoreState(const CanvasState &state);   // 恢复到指定快照
    void resetHistory();                           // 清空历史（用于加载新文档）
    void commitHistory();                          // 提交一次状态变更到历史
    static qreal distanceToSegment(const QPointF &p, const QPointF &a, const QPointF &b,
                                   qreal *t = nullptr); // 点到线段的距离（及投影参数）

    QImage m_background;               // 背景图像（即“文档”）
    // 在吸附之前，这是画布空间的位置。当图像被拖到某个平面上之后，
    // 它变为该平面所在分组共享的展开曲面坐标系中的位置。
    QImage m_pastedImage;
    QPointF m_pastedImagePosition;
    QPointF m_pastedDragStartPosition;  // 拖动开始时的位置（供 Esc 取消恢复）
    QPointF m_pastedDragOffset;         // 拖动时鼠标相对图像左上角的偏移
    bool m_pastedImageAttached = false;
    int m_pastedSurfaceGroup = -1;
    int m_pastedHostPlane = -1;
    bool m_pastedDragStartAttached = false;      // 拖动开始时的吸附状态
    int m_pastedDragStartSurfaceGroup = -1;      // 拖动开始时的曲面分组
    int m_pastedDragStartHostPlane = -1;         // 拖动开始时的宿主平面
    bool m_hasLoadedImage = false;               // 是否已加载背景图像
    QVector<Plane> m_planes;                     // 全部透视平面
    QVector<QPointF> m_creationPoints;           // 创建平面过程中已点击的角点
    Tool m_tool = CreatePlane;                   // 当前工具
    int m_selectedPlane = -1;                    // 当前选中的平面索引
    int m_dragHandle = -1;                       // 正在拖动的控制点索引
    int m_dragEdge = -1;                         // 正在拖动的边缘索引
    bool m_dragging = false;                     // 正在拖动平面/控制点
    bool m_drawing = false;                      // 正在绘制笔迹
    bool m_extruding = false;                    // 正在从边缘拖出垂直平面
    bool m_draggingPastedImage = false;          // 正在拖动浮动图像
    QPointF m_pressImagePoint;                   // 鼠标按下时的图像坐标
    QPointF m_lastImagePoint;                    // 最近一次的图像坐标
    QPointF m_lastUv;                            // 最近一次笔迹的 UV 坐标
    Plane m_dragStartPlane;                      // 拖动开始时的平面快照
    Plane m_extrudePreview;                      // 垂直平面的拖出预览
    bool m_hasExtrudePreview = false;
    QPointF m_cloneSourceUv;                     // 仿制源在源平面上的 UV
    QPointF m_cloneAnchorUv;                     // 本次仿制笔迹起点的 UV（偏移锚）
    bool m_hasCloneSource = false;               // 是否已设置仿制源
    bool m_cloneStrokeStarted = false;           // 当前笔迹是否已开始
    int m_cloneSourcePlane = -1;                 // 仿制源所在平面索引
    qreal m_scale = 1.0;                         // 视图缩放比例
    QPointF m_offset;                            // 视图居中偏移
    int m_diameter = 42;                         // 笔刷直径（图像像素）
    qreal m_hardness = .75;                      // 笔刷硬度（0~1）
    qreal m_opacity = 1.0;                       // 笔刷不透明度（0~1）
    QColor m_brushColor = QColor("#e85d4a");     // 画笔颜色
    // m_historyIndex 指向当前可见的快照。撤销之后的新的编辑会丢弃旧的
    // 重做分支，与主流编辑器的行为一致。
    QVector<CanvasState> m_history;              // 历史快照栈
    int m_historyIndex = -1;                     // 当前快照在历史栈中的索引
    bool m_stateChanged = false;                 // 自上次提交以来状态是否已变化
};
