#pragma once

#include "planemath.h"

#include <QImage>
#include <QObject>
#include <QPointF>
#include <QVector>

// 文档模型：管理背景图像、全部透视平面、浮动（粘贴/拖入）图像的
// 状态以及撤销/重做历史。它是纯数据与历史的集合体，不涉及视图、
// 渲染或任何交互细节——这些职责属于 PerspectiveCanvas。
class CanvasDocument : public QObject
{
    Q_OBJECT
public:
    explicit CanvasDocument(QObject *parent = nullptr);

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

    // —— 背景（即“文档”） ——
    bool loadImage(const QString &fileName);       // 加载背景并重置文档状态
    const QImage &background() const { return m_background; }
    void setBackground(const QImage &image);       // 设置背景而不重置历史（初始化用）
    bool hasLoadedImage() const { return m_hasLoadedImage; }

    // —— 平面 ——
    const QVector<Plane> &planes() const { return m_planes; }
    QVector<Plane> &planes() { return m_planes; }  // 直接修改后需手动 commitHistory()
    int selectedPlane() const { return m_selectedPlane; }
    void setSelectedPlane(int index) { m_selectedPlane = index; }
    int nextSurfaceGroupId() const;                // 分配一个新的展开曲面分组号
    void clearPainting();                          // 清除所有平面上的绘画内容
    void removePlane(int index);                   // 删除平面并修正浮动图像宿主索引

    // —— 浮动图像 ——
    void setFloatingImage(const QImage &image);    // 放到画布左上角并重置吸附状态
    bool rotateFloatingImage();                    // 顺时针旋转 90°，返回是否成功
    bool flipFloatingImage(bool horizontal, bool vertical); // 翻转，返回是否成功
    const QImage &pastedImage() const { return m_pastedImage; }
    bool hasFloatingImage() const { return !m_pastedImage.isNull(); }
    QPointF pastedImagePosition() const { return m_pastedImagePosition; }
    bool pastedImageAttached() const { return m_pastedImageAttached; }
    int pastedSurfaceGroup() const { return m_pastedSurfaceGroup; }
    int pastedHostPlane() const { return m_pastedHostPlane; }
    // 一次性更新浮动图像的完整放置状态（位置 + 吸附信息）。
    // 在吸附之前 position 处于画布空间；吸附之后处于宿主分组共享的
    // 展开曲面坐标系中。
    void setFloatingImagePlacement(const QPointF &position, bool attached,
                                   int surfaceGroup, int hostPlane);
    void setPastedImagePosition(const QPointF &position); // 已吸附状态下仅移动位置

    // —— 历史 ——
    bool undo();                                   // 回退到上一份快照，返回是否发生了撤销
    bool redo();                                   // 前进到下一份快照，返回是否发生了重做
    bool canUndo() const { return m_historyIndex > 0; }
    bool canRedo() const { return m_historyIndex + 1 < m_history.size(); }
    void resetHistory();                           // 清空历史并以当前状态为初始快照
    void commitHistory();                          // 提交一次状态变更到历史

signals:
    void canUndoChanged(bool available);           // 撤销可用性变化
    void canRedoChanged(bool available);           // 重做可用性变化
    void documentAvailabilityChanged(bool available); // 文档加载状态变化

private:
    CanvasState captureState() const;              // 捕获当前状态为快照
    void restoreState(const CanvasState &state);   // 恢复到指定快照（仅文档数据）

    QImage m_background;                // 背景图像（即“文档”）
    bool m_hasLoadedImage = false;      // 是否已加载背景图像
    QVector<Plane> m_planes;            // 全部透视平面
    int m_selectedPlane = -1;           // 当前选中的平面索引
    QImage m_pastedImage;               // 浮动图像位图
    QPointF m_pastedImagePosition;      // 浮动图像位置（画布或曲面坐标）
    bool m_pastedImageAttached = false; // 浮动图像是否已吸附到某个曲面
    int m_pastedSurfaceGroup = -1;      // 浮动图像吸附到的曲面分组
    int m_pastedHostPlane = -1;         // 浮动图像的宿主平面索引
    // m_historyIndex 指向当前可见的快照。撤销之后的新的编辑会丢弃旧的
    // 重做分支，与主流编辑器的行为一致。
    QVector<CanvasState> m_history;     // 历史快照栈
    int m_historyIndex = -1;            // 当前快照在历史栈中的索引
};
