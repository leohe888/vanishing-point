#include "scenerenderer.h"

#include "canvasdocument.h"

#include <QFont>
#include <QPainter>
#include <QPainterPath>

using namespace PlaneMath;

SceneRenderer::SceneRenderer(const CanvasDocument &doc)
    : m_doc(doc)
{
}

// 渲染完整场景。showGuides 为 true 时额外绘制编辑辅助元素。
void SceneRenderer::render(QPainter &painter, qreal viewScale, bool showGuides,
                           const QVector<QPointF> &creationPoints,
                           const Plane *extrudePreview, bool editHandlesVisible,
                           int hoveredPlane)
{
    m_viewScale = qMax(viewScale, 1e-6);

    // 画布背景属于文档输出的一部分，总是被渲染。showGuides 标志
    // 只控制下方这些编辑器叠加层的绘制。
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
    if (!m_doc.paintLayer().isNull())
        painter.drawImage(QPointF(0, 0), m_doc.paintLayer());

    // 浮动图像：每张各自按吸附瞬间的几何快照渲染。
    for (const FloatingImage &image : m_doc.images())
        renderFloatingImage(painter, image);

    if (!showGuides)
        return;
    for (int i = 0; i < m_doc.planes().size(); ++i) {
        drawPlaneGuides(painter, m_doc.planes()[i], i == m_doc.selectedPlane(),
                        i == hoveredPlane, editHandlesVisible);
    }
    if (extrudePreview)
        drawPlaneGuides(painter, *extrudePreview, true, false, false);

    painter.save();
    painter.setPen(QPen(QColor("#4bc3ff"), 2.0 / m_viewScale));
    painter.setBrush(QColor("#4bc3ff"));
    for (int i = 0; i < creationPoints.size(); ++i) {
        const QPointF &point = creationPoints[i];
        painter.drawEllipse(point, 4.5 / m_viewScale, 4.5 / m_viewScale);
        if (i > 0)
            painter.drawLine(creationPoints[i - 1], point);
    }
    painter.restore();
}

// 渲染一张浮动图像：未吸附时直接绘制；已吸附时按几何快照分段投影。
void SceneRenderer::renderFloatingImage(QPainter &painter, const FloatingImage &image) const
{
    if (image.image.isNull())
        return;
    if (!image.attached || image.faces.isEmpty()) {
        painter.drawImage(image.position, image.image);
        return;
    }

    const QRectF imageRect(QPointF(0, 0), QSizeF(image.image.size()));
    QPainterPath hostClip;
    hostClip.addRect(imageRect);

    // 图像位置处于展开曲面坐标系，减掉它即得到图像局部坐标。
    auto sourcePolygon = [&image](const Facet &face) {
        QPolygonF polygon;
        for (const QPointF &corner : face.surfaceCorner)
            polygon << (corner - image.position);
        return polygon;
    };
    auto polygonPath = [](const QPolygonF &polygon) {
        QPainterPath path;
        path.addPolygon(polygon);
        path.closeSubpath();
        return path;
    };

    // 先从宿主投影中减去属于其他面的像素，再用那个面的单应变换重绘，
    // 这样接缝处不会出现重影，同时图像仍可延伸到有限网格之外。
    for (int i = 0; i < image.faces.size(); ++i) {
        if (i == image.hostFace)
            continue;
        hostClip = hostClip.subtracted(polygonPath(sourcePolygon(image.faces[i])));
    }

    auto drawFace = [&painter, &image, &sourcePolygon](const Facet &face,
                                                       const QPainterPath &clip) {
        const QPolygonF source = sourcePolygon(face);
        QTransform projection;
        if (!QTransform::quadToQuad(source, planePolygon(face.corner), projection))
            return;
        painter.save();
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.setWorldTransform(projection, true);
        // 裁剪路径表达在源图像坐标系中，因此要先把图像到画布的变换
        // 安装到 QPainter，再传入该路径。
        painter.setClipPath(clip, Qt::IntersectClip);
        painter.drawImage(QPointF(0, 0), image.image);
        painter.restore();
    };

    if (image.hostFace >= 0 && image.hostFace < image.faces.size())
        drawFace(image.faces[image.hostFace], hostClip);
    for (int i = 0; i < image.faces.size(); ++i) {
        if (i == image.hostFace)
            continue;
        drawFace(image.faces[i], polygonPath(sourcePolygon(image.faces[i])));
    }
}

// 绘制面片的编辑辅助元素：外框、内部网格，以及选中且处于编辑
// 工具时的控制点方块。
void SceneRenderer::drawPlaneGuides(QPainter &painter, const Facet &facet, bool selected,
                                    bool hovered, bool showHandles) const
{
    painter.save();
    const qreal lineWidth = (selected ? 1.7 : 1.0) / m_viewScale;
    const QColor color = selected ? QColor(52, 195, 255, 230)
                                  : (hovered ? QColor(120, 210, 255, 210)
                                             : QColor(70, 155, 210, 160));
    painter.setPen(QPen(color, lineWidth));
    painter.setBrush(Qt::NoBrush);
    painter.drawPolygon(planePolygon(facet.corner));

    painter.save();
    QPainterPath planeClip;
    planeClip.addPolygon(planePolygon(facet.corner));
    planeClip.closeSubpath();
    painter.setClipPath(planeClip, Qt::IntersectClip);
    painter.setPen(QPen(QColor(65, 182, 235, selected ? 145 : 80), 0.8 / m_viewScale));
    constexpr int divisions = 8;
    for (int i = 1; i < divisions; ++i) {
        const qreal t = qreal(i) / divisions;
        painter.drawLine(uvToPlane(facet, QPointF(t, 0)), uvToPlane(facet, QPointF(t, 1)));
        painter.drawLine(uvToPlane(facet, QPointF(0, t)), uvToPlane(facet, QPointF(1, t)));
    }
    painter.restore();

    if (selected && showHandles) {
        const QVector<QPointF> hs = handles(facet);
        for (int i = 0; i < hs.size(); ++i) {
            const qreal radius = (i < 4 ? 5.5 : 4.5) / m_viewScale;
            painter.setPen(QPen(QColor("#0e526e"), 1.0 / m_viewScale));
            painter.setBrush(i < 4 ? QColor("#f3f8fa") : QColor("#4bc3ff"));
            painter.drawRect(QRectF(hs[i].x() - radius, hs[i].y() - radius,
                                    radius * 2, radius * 2));
        }
    }
    painter.restore();
}
