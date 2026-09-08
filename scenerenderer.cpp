#include "scenerenderer.h"

#include "canvasdocument.h"
#include "imagegeometry.h"

#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QLineF>

using namespace PlaneMath;

// 控制点（创建平面的角点标记、平面编辑的角点与边缘中心点）统一的半边长，
// 单位为画布像素，绘制时再按视图缩放换算。
constexpr qreal HandleHalfSize = 4.0;

SceneRenderer::SceneRenderer(const CanvasDocument &doc)
    : m_doc(doc)
{
}

// 渲染完整场景。showGuides 为 true 时额外绘制编辑辅助元素。
void SceneRenderer::render(QPainter &painter, qreal viewScale, bool showGuides,
                           const QVector<QPointF> &creationPoints,
                           const Plane *extrudePreview, bool editHandlesVisible,
                           int hoveredPlane, qreal antsPhase, bool drawContent, qreal gridSize,
                           const QPointF &cursorPoint)
{
    m_viewScale = qMax(viewScale, 1e-6);

    // 画布背景属于文档输出的一部分，总是被渲染。showGuides 标志
    // 只控制下方这些编辑器叠加层的绘制。
    if (drawContent)
        painter.drawImage(QPointF(0, 0), m_doc.background());
    if (showGuides && !m_doc.hasLoadedImage() && m_doc.planes().isEmpty() &&
        creationPoints.isEmpty()) {
        painter.save();
        painter.setPen(QColor("#89919b"));
        QFont placeholderFont = painter.font();
        placeholderFont.setPointSize(20);
        painter.setFont(placeholderFont);
        painter.drawText(m_doc.background().rect(), Qt::AlignCenter,
                         QObject::tr("请打开一张图片开始操作"));
        painter.restore();
    }

    // 绘画层：画笔笔触烘焙在画布同尺寸的透明层上，与平面几何完全无关。
    if (drawContent && !m_doc.paintLayer().isNull())
        painter.drawImage(QPointF(0, 0), m_doc.paintLayer());

    // 浮动图像：每张各自按吸附瞬间的几何快照渲染。
    if (drawContent)
        for (const FloatingImage &image : m_doc.images())
            renderFloatingImage(painter, image);

    if (!showGuides)
        return;
    for (int i = 0; i < m_doc.planes().size(); ++i) {
        drawPlaneGuides(painter, m_doc.planes()[i], i == m_doc.selectedPlane(),
                        i == hoveredPlane, editHandlesVisible, gridSize, i);
    }
    if (extrudePreview)
        drawPlaneGuides(painter, *extrudePreview, true, false, false, gridSize);

    // 先画连线：已确定角点之间的边，以及连到光标的预览边。
    painter.save();
    painter.setPen(QPen(QColor("#4bc3ff"), 2.0 / m_viewScale));
    painter.setBrush(Qt::NoBrush);
    for (int i = 1; i < creationPoints.size(); ++i)
        painter.drawLine(creationPoints[i - 1], creationPoints[i]);
    // 橡皮筋预览：把光标与「下一个角点将要连到的那些角点」连起来。
    // 已点 1 个：连第 1 个；已点 2 个：连第 2 个；已点 3 个：连第 3 个，
    // 同时闭合回第 1 个——第四点落下后正好是四边形的两条收口边。
    const int last = creationPoints.size() - 1;
    const bool showCursor = last >= 0 && last <= 2 && !cursorPoint.isNull();
    if (showCursor) {
        painter.drawLine(creationPoints[last], cursorPoint);
        if (last == 2)
            painter.drawLine(cursorPoint, creationPoints[0]);
    }
    // 角点标记与平面控制点保持一致：白色方块 + 深色描边。
    // 光标处的待放置角点也用同一种方块，预览边两端看起来完全对称。
    painter.setPen(QPen(QColor("#0e526e"), 1.0 / m_viewScale));
    painter.setBrush(QColor("#f3f8fa"));
    const qreal handle = HandleHalfSize / m_viewScale;
    for (const QPointF &point : creationPoints)
        painter.drawRect(QRectF(point.x() - handle, point.y() - handle, handle * 2, handle * 2));
    if (showCursor)
        painter.drawRect(QRectF(cursorPoint.x() - handle, cursorPoint.y() - handle,
                                handle * 2, handle * 2));
    painter.restore();

    // 在屏幕坐标中描边，透视和缩放只改变轮廓，不改变线宽、虚线长度和速度。
    painter.save();
    const QTransform view = painter.worldTransform();
    painter.resetTransform();
    painter.setBrush(Qt::NoBrush);
    const int selectedImage = m_doc.selectedImage();
    if (selectedImage >= 0 && selectedImage < m_doc.images().size()) {
        const QPainterPath outline = view.map(floatingImageOutline(m_doc.image(selectedImage)));
        QPen pen(Qt::white, 1);
        pen.setJoinStyle(Qt::MiterJoin);
        painter.setPen(pen);
        painter.drawPath(outline);
        pen.setColor(Qt::black);
        pen.setDashPattern({4, 4});
        pen.setDashOffset(antsPhase);
        painter.setPen(pen);
        painter.drawPath(outline);
    }
    painter.restore();
}

QPainterPath SceneRenderer::floatingImageOutline(const FloatingImage &image)
{
    return ImageGeometry::get(image)->outline();
}

// 渲染一张浮动图像：未吸附时直接绘制；已吸附时按几何快照分段投影。
void SceneRenderer::renderFloatingImage(QPainter &painter, const FloatingImage &image) const
{
    const auto geometry = ImageGeometry::get(image);
    for (const ImagePatch &patch : geometry->patches()) {
        painter.save();
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        // 在共同的画布坐标中裁剪，避免旋转后各面独立栅格化源裁剪路径
        // 时把共享边上的同一个像素同时排除。
        const QPainterPath projectedClip = patch.canvasClip;
        QPainterPathStroker seamTolerance;
        seamTolerance.setWidth(.04 / qMax(m_viewScale, 1e-6));
        seamTolerance.setJoinStyle(Qt::MiterJoin);
        painter.setClipPath(projectedClip.united(seamTolerance.createStroke(projectedClip)), Qt::IntersectClip);
        painter.setWorldTransform(patch.mapping.forward(), true);
        painter.drawImage(QPointF(0, 0), image.image);
        painter.restore();
    }
}

// 绘制面片的编辑辅助元素：外框、内部网格，以及选中且处于编辑
// 工具时的控制点方块。
void SceneRenderer::drawPlaneGuides(QPainter &painter, const Facet &facet, bool selected,
                                    bool hovered, bool showHandles, qreal gridSize,
                                    int planeIndex) const
{
    painter.save();
    const qreal lineWidth = (selected ? 1.7 : 1.0) / m_viewScale;
    const QColor color = selected ? QColor(52, 195, 255, 230)
                                  : (hovered ? QColor(120, 210, 255, 210)
                                             : QColor(70, 155, 210, 160));
    painter.setPen(QPen(color, lineWidth));
    painter.setBrush(Qt::NoBrush);
    painter.drawPolygon(planePolygon(facet.corner));

    if (selected) {
        painter.save();
        QPainterPath planeClip;
        planeClip.addPolygon(planePolygon(facet.corner));
        planeClip.closeSubpath();
        painter.setClipPath(planeClip, Qt::IntersectClip);
        painter.setPen(QPen(QColor(65, 182, 235, 145), 0.8 / m_viewScale));
        const qreal safeGridSize = qMax(gridSize, 1.0);
        // Derive cell counts from current projected edge lengths so resizing
        // a plane changes the number of cells instead of stretching them.
        const qreal horizontalLength =
            (QLineF(facet.corner[0], facet.corner[1]).length()
             + QLineF(facet.corner[3], facet.corner[2]).length()) * 0.5;
        const qreal verticalLength =
            (QLineF(facet.corner[0], facet.corner[3]).length()
             + QLineF(facet.corner[1], facet.corner[2]).length()) * 0.5;
        const int horizontalDivisions = qMax(1, qRound(horizontalLength / safeGridSize));
        const int verticalDivisions = qMax(1, qRound(verticalLength / safeGridSize));
        for (int i = 1; i < horizontalDivisions; ++i) {
            const qreal t = qreal(i) / horizontalDivisions;
            painter.drawLine(uvToPlane(facet, QPointF(t, 0)), uvToPlane(facet, QPointF(t, 1)));
        }
        for (int i = 1; i < verticalDivisions; ++i) {
            const qreal t = qreal(i) / verticalDivisions;
            painter.drawLine(uvToPlane(facet, QPointF(0, t)), uvToPlane(facet, QPointF(1, t)));
        }
        painter.restore();
    }

    if (selected && showHandles) {
        const QVector<QPointF> hs = handles(facet);
        quint8 sharedEdges = 0;
        if (planeIndex >= 0) {
            const Plane &plane = m_doc.planes()[planeIndex];
            for (int edge = 0; edge < 4; ++edge)
                if (plane.lockedEdges & quint8(1u << edge)) sharedEdges |= quint8(1u << edge);
            for (int edge = 0; edge < 4; ++edge) {
                const QPointF a = facet.corner[edge];
                const QPointF b = facet.corner[(edge + 1) % 4];
                for (int other = 0; other < m_doc.planes().size(); ++other) {
                    if (other == planeIndex)
                        continue;
                    for (int oe = 0; oe < 4; ++oe) {
                        const QPointF oa = m_doc.planes()[other].corner[oe];
                        const QPointF ob = m_doc.planes()[other].corner[(oe + 1) % 4];
                        if ((QLineF(a, oa).length() < 0.01 && QLineF(b, ob).length() < 0.01) ||
                            (QLineF(a, ob).length() < 0.01 && QLineF(b, oa).length() < 0.01)) {
                            sharedEdges |= quint8(1u << edge);
                            break;
                        }
                    }
                }
            }
        }
        for (int i = 0; i < hs.size(); ++i) {
            if ((i < 4 && ((sharedEdges & quint8(1u << i)) ||
                           (sharedEdges & quint8(1u << ((i + 3) % 4))))) ||
                (i >= 4 && (sharedEdges & quint8(1u << (i - 4)))))
                continue;
            const qreal radius = HandleHalfSize / m_viewScale;
            painter.setPen(QPen(QColor("#0e526e"), 1.0 / m_viewScale));
            painter.setBrush(QColor("#f3f8fa"));
            painter.drawRect(QRectF(hs[i].x() - radius, hs[i].y() - radius,
                                    radius * 2, radius * 2));
        }
    }
    painter.restore();
}
