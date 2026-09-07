#pragma once

#include "planemath.h"

#include <QImage>
#include <QObject>
#include <QPointF>
#include <QRect>
#include <QVector>

// 浮动图像：一张可独立移动、可吸附到透视曲面的位图。
// 吸附瞬间会把所在曲面分组的几何拷贝成快照（faces），之后平面的增删改
// 都不会影响它——这正是内容与平面解耦的关键。
struct FloatingImage {
    QImage image;              // 位图
    QPointF position;          // 位置：未吸附=画布坐标；已吸附=展开曲面坐标
    QPointF scale = QPointF(1, 1); // 非破坏性缩放，保留原始位图
    qreal rotation = 0; // 在画布/展开曲面中绕图片中心旋转，单位为度
    QSizeF displayedSize() const { return QSizeF(image.width() * scale.x(), image.height() * scale.y()); }
    bool attached = false;     // 是否已吸附到某个展开曲面
    QVector<Facet> faces;      // 吸附瞬间曲面分组的几何快照（严格快照）
    int hostFace = -1;         // 宿主面在 faces 中的索引（-1 表示无）
};

// 文档模型：管理背景图像、全部透视平面、绘画层、浮动图像数组以及
// 撤销/重做历史。它是纯数据与历史的集合体，不涉及视图、渲染或交互细节。
//
// 内容与平面解耦的核心约定：
//  - 平面只提供透视规则（几何），不持有绘画或图像内容；
//  - 画笔笔触烘焙在画布同尺寸的绘画层上（画布坐标）；
//  - 浮动图像各自携带吸附瞬间的几何快照，严格独立于平面。
class CanvasDocument : public QObject
{
    Q_OBJECT
public:
    explicit CanvasDocument(QObject *parent = nullptr);

    // —— 背景（即“文档”） ——
    bool loadImage(const QString &fileName);       // 加载背景并重置文档状态
    const QImage &background() const { return m_background; }
    void setBackground(const QImage &image);       // 设置背景而不重置历史（初始化用）
    bool hasLoadedImage() const { return m_hasLoadedImage; }

    // —— 平面 ——
    const QVector<Plane> &planes() const { return m_planes; }
    int appendPlane(const Plane &plane);           // 几何预览修改由 beginEdit/commitEdit 包围
    bool setPlane(int index, const Plane &plane);
    void lockPlaneEdge(int index, int edge);
    int selectedPlane() const { return m_selectedPlane; }
    void setSelectedPlane(int index) { m_selectedPlane = index; }
    int nextSurfaceGroupId() const;                // 分配一个新的展开曲面分组号
    void removePlane(int index);                   // 删除平面（不影响任何已存在内容）

    // —— 绘画层（画布同尺寸，画笔笔触烘焙于此） ——
    QImage &paintLayer() { return m_paintLayer; }
    const QImage &paintLayer() const { return m_paintLayer; }
    bool hasPaintContent() const;                  // 绘画层是否含有不透明像素
    void clearPainting();                          // 清空绘画层
    void beginPaintTransaction();                  // 开始一次绘画事务（记录 before）
    void addPaintDirty(const QRect &rect);         // 累积绘画脏矩形

    // —— 浮动图像 ——
    const QVector<FloatingImage> &images() const { return m_images; }
    const FloatingImage &image(int index) const { return m_images[index]; }
    bool setImage(int index, const FloatingImage &image);
    int selectedImage() const { return m_selectedImage; }
    void setSelectedImage(int index);
    int addFloatingImage(const QImage &image);     // 追加到左上角，返回索引
    void removeFloatingImage(int index);          // 删除指定浮动图像
    void setImagePosition(int index, const QPointF &position); // 仅移动位置
    // 把图像吸附到一组几何快照上（surfacePosition 为展开曲面坐标）
    void attachImage(int index, const QVector<Facet> &faces, int hostFace,
                     const QPointF &surfacePosition);
    void detachImage(int index, const QPointF &canvasPosition); // 脱离曲面回到画布坐标
    bool rotateImage(int index);                   // 顺时针旋转 90°，返回是否成功
    bool flipImage(int index, bool horizontal, bool vertical); // 翻转，返回是否成功

    // —— 历史 ——
    bool undo();                                   // 回退到上一状态，返回是否发生了撤销
    bool redo();                                   // 前进到下一状态，返回是否发生了重做
    bool canUndo() const { return m_historyIndex > 0; }
    bool canRedo() const { return m_historyIndex + 1 < m_history.size(); }
    void resetHistory();                           // 清空历史并以当前状态为初始状态
    void commitHistory();                          // 提交一次状态变更到历史
    // 交互事务只保存本次操作前的 COW 快照，历史仍使用绘画脏矩形增量。
    void beginEdit();
    void commitEdit(bool changed);
    void cancelEdit();
    bool editActive() const { return m_editActive; }

signals:
    void imageSelectionChanged(bool selected);
    void canUndoChanged(bool available);           // 撤销可用性变化
    void canRedoChanged(bool available);           // 重做可用性变化
    void documentAvailabilityChanged(bool available); // 文档加载状态变化

private:
    // 历史条目：结构（平面 + 浮动图像）整份快照 + 绘画层的脏矩形增量。
    // 结构部分体积小、位图靠隐式共享，可直接整份拷贝；而绘画层整份快照
    // 在 40 步历史下会因整层逐笔复制而失控，故只记录本次变动的脏矩形前后像素。
    struct HistoryEntry {
        QVector<Plane> planes;
        int selectedPlane = -1;
        QVector<FloatingImage> images;
        int selectedImage = -1;
        QRect paintRect;              // 绘画层脏区域（空=本次无绘画变更）
        QImage paintBefore;           // 脏区域旧像素
        QImage paintAfter;            // 脏区域新像素
    };

    void restoreStructure(const HistoryEntry &entry); // 仅恢复结构部分
    void applyPaint(const QRect &rect, const QImage &pixels); // 把像素写回绘画层

    QImage m_background;                // 背景图像（即“文档”）
    bool m_hasLoadedImage = false;      // 是否已加载背景图像
    QVector<Plane> m_planes;            // 全部透视平面
    int m_selectedPlane = -1;           // 当前选中的平面索引
    QImage m_paintLayer;                // 绘画层（画布同尺寸）
    QVector<FloatingImage> m_images;    // 全部浮动图像
    int m_selectedImage = -1;           // 当前操作的浮动图像索引
    QImage m_paintBefore;               // 绘画事务开始时的绘画层浅拷贝
    QRect m_paintDirtyRect;             // 当前绘画事务累积的脏矩形
    bool m_paintTransactionActive = false; // 是否存在进行中的绘画事务
    // m_historyIndex 指向当前可见的状态。撤销之后的新的编辑会丢弃旧的
    // 重做分支，与主流编辑器的行为一致。
    QVector<HistoryEntry> m_history;    // 历史条目栈
    int m_historyIndex = -1;            // 当前状态在历史栈中的索引
    bool m_editActive = false;
    HistoryEntry m_editBefore;
    QImage m_editPaintBefore;
};
