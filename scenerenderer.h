#pragma once

#include "planemath.h"

#include <QVector>
#include <QPainterPath>

class QPainter;
class CanvasDocument;
struct FloatingImage;

// 场景渲染器：把文档内容绘制到 QPainter。
// 既用于屏幕显示（带编辑辅助层：网格、控制点、创建角点预览），
// 也用于导出（仅文档内容本身）。viewScale 用于把辅助层线宽换算为
// 恒定的屏幕像素宽度。
//
// 渲染顺序：背景 -> 绘画层 -> 浮动图像 -> 编辑辅助层。
// 绘画层与浮动图像都不再附着于平面，因此渲染只依赖它们自身的数据。
class SceneRenderer
{
public:
    explicit SceneRenderer(const CanvasDocument &doc);

    // 渲染完整场景。showGuides 为 true 时额外绘制编辑辅助元素；
    // creationPoints 为创建平面过程中已点击的角点；
    // extrudePreview 为垂直平面的拖出预览（可为空）；
    // editHandlesVisible 控制选中平面的控制点是否显示；
    // hoveredPlane 为当前悬停平面的索引（用于高亮）。
    void render(QPainter &painter, qreal viewScale, bool showGuides,
                const QVector<QPointF> &creationPoints = {},
                const Plane *extrudePreview = nullptr,
                bool editHandlesVisible = false,
                int hoveredPlane = -1, qreal antsPhase = 0);

    // 各投影片段的并集外轮廓，不包含平面之间的内部接缝。
    static QPainterPath floatingImageOutline(const FloatingImage &image);

private:
    // 渲染一张浮动图像：未吸附时直接绘制；已吸附时按几何快照分段投影，
    // 使图像可以跨越共享接缝。
    void renderFloatingImage(QPainter &painter, const FloatingImage &image) const;
    // 绘制面片的编辑辅助元素：外框、内部网格，以及选中且处于编辑
    // 工具时的控制点方块。
    void drawPlaneGuides(QPainter &painter, const Facet &facet, bool selected,
                         bool hovered, bool showHandles) const;

    const CanvasDocument &m_doc;   // 被渲染的文档（只读）
    qreal m_viewScale = 1.0;       // 当前视图缩放（用于辅助层线宽换算）
};
