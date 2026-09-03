#pragma once

#include "planemath.h"

#include <QVector>

class QPainter;
class CanvasDocument;

// 场景渲染器：把文档内容绘制到 QPainter。
// 既用于屏幕显示（带编辑辅助层：网格、控制点、创建角点预览），
// 也用于导出（仅文档内容本身）。viewScale 用于把辅助层线宽换算为
// 恒定的屏幕像素宽度。
class SceneRenderer
{
public:
    explicit SceneRenderer(const CanvasDocument &doc);

    // 渲染完整场景。showGuides 为 true 时额外绘制编辑辅助元素；
    // creationPoints 为创建平面过程中已点击的角点；
    // extrudePreview 为垂直平面的拖出预览（可为空）；
    // editHandlesVisible 控制选中平面的控制点是否显示。
    void render(QPainter &painter, qreal viewScale, bool showGuides,
                const QVector<QPointF> &creationPoints = {},
                const Plane *extrudePreview = nullptr,
                bool editHandlesVisible = false);

private:
    // 用单应变换把纹理投影到平面的四边形上
    void renderProjectedImage(QPainter &painter, const Plane &plane,
                              const QImage &texture) const;
    // 渲染浮动图像：未吸附时直接绘制；已吸附时按宿主平面及相邻面的
    // 单应变换分段投影，使图像可以跨越共享接缝。
    void renderPastedImage(QPainter &painter) const;
    // 绘制平面的编辑辅助元素：外框、内部网格，以及选中且处于编辑
    // 工具时的控制点方块。
    void drawPlaneGuides(QPainter &painter, const Plane &plane, bool selected,
                         bool showHandles) const;

    const CanvasDocument &m_doc;   // 被渲染的文档（只读）
    qreal m_viewScale = 1.0;       // 当前视图缩放（用于辅助层线宽换算）
};
