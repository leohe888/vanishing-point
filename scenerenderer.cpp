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
                           const Plane *extrudePreview, bool editHandlesVisible)
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
    // 先绘制每个平面的绘画层。剪贴板图像在被显式移除之前，
    // 始终作为一个可直接移动的顶层图层存在。
    for (const Plane &plane : m_doc.planes()) {
        renderProjectedImage(painter, plane, plane.paint);
    }
    renderPastedImage(painter);

    if (!showGuides)
        return;
    for (int i = 0; i < m_doc.planes().size(); ++i)
        drawPlaneGuides(painter, m_doc.planes()[i], i == m_doc.selectedPlane(),
                        editHandlesVisible);
    if (extrudePreview)
        drawPlaneGuides(painter, *extrudePreview, true, false);

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

// 用单应变换把纹理投影到平面的四边形上
void SceneRenderer::renderProjectedImage(QPainter &painter, const Plane &plane,
                                         const QImage &texture) const
{
    if (texture.isNull())
        return;
    const QPolygonF source{QPointF(0, 0), QPointF(texture.width(), 0),
                           QPointF(texture.width(), texture.height()), QPointF(0, texture.height())};
    // quadToQuad() 生成所需的单应变换，使纹理的每个像素都落在四点
    // 平面上正确的透视位置。
    QTransform projection;
    if (!QTransform::quadToQuad(source, planePolygon(plane.corner), projection))
        return;
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setWorldTransform(projection, true);
    painter.drawImage(QPointF(0, 0), texture);
    painter.restore();
}

// 渲染浮动图像：未吸附时直接绘制；已吸附时按宿主平面及相邻面的
// 单应变换分段投影，使图像可以跨越共享接缝。
void SceneRenderer::renderPastedImage(QPainter &painter) const
{
    const QImage &pastedImage = m_doc.pastedImage();
    if (pastedImage.isNull())
        return;
    if (!m_doc.pastedImageAttached()) {
        painter.drawImage(m_doc.pastedImagePosition(), pastedImage);
        return;
    }

    int host = m_doc.pastedHostPlane();
    if (host < 0 || host >= m_doc.planes().size() ||
        m_doc.planes()[host].surfaceGroup != m_doc.pastedSurfaceGroup()) {
        host = -1;
        for (int i = 0; i < m_doc.planes().size(); ++i) {
            if (m_doc.planes()[i].surfaceGroup == m_doc.pastedSurfaceGroup()) {
                host = i;
                break;
            }
        }
    }
    if (host < 0) {
        painter.drawImage(m_doc.pastedImagePosition(), pastedImage);
        return;
    }

    const QPointF &imagePosition = m_doc.pastedImagePosition();
    const QRectF imageRect(QPointF(0, 0), QSizeF(pastedImage.size()));
    QPainterPath hostClip;
    hostClip.addRect(imageRect);

    auto sourcePolygon = [&imagePosition](const Plane &plane) {
        QPolygonF polygon;
        for (const QPointF &corner : plane.surfaceCorner)
            polygon << corner - imagePosition;
        return polygon;
    };
    auto polygonPath = [](const QPolygonF &polygon) {
        QPainterPath path;
        path.addPolygon(polygon);
        path.closeSubpath();
        return path;
    };

    // 先从宿主投影中减去属于其他面的像素，再用那个面的单应变换重绘。
    // 这样接缝处不会出现重影，同时图像仍可延伸到有限网格之外。
    for (int i = 0; i < m_doc.planes().size(); ++i) {
        if (i == host || m_doc.planes()[i].surfaceGroup != m_doc.pastedSurfaceGroup())
            continue;
        hostClip = hostClip.subtracted(polygonPath(sourcePolygon(m_doc.planes()[i])));
    }

    auto drawFace = [this, &painter, &pastedImage, &sourcePolygon](const Plane &plane,
                                                                   const QPainterPath &clip) {
        const QPolygonF source = sourcePolygon(plane);
        QTransform projection;
        if (!QTransform::quadToQuad(source, planePolygon(plane.corner), projection))
            return;
        painter.save();
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.setWorldTransform(projection, true);
        // 裁剪路径表达在源图像坐标系中，因此要先把图像到画布的变换
        // 安装到 QPainter，再传入该路径。
        painter.setClipPath(clip, Qt::IntersectClip);
        painter.drawImage(QPointF(0, 0), pastedImage);
        painter.restore();
    };

    drawFace(m_doc.planes()[host], hostClip);
    for (int i = 0; i < m_doc.planes().size(); ++i) {
        if (i == host || m_doc.planes()[i].surfaceGroup != m_doc.pastedSurfaceGroup())
            continue;
        drawFace(m_doc.planes()[i], polygonPath(sourcePolygon(m_doc.planes()[i])));
    }
}

// 绘制平面的编辑辅助元素：外框、内部网格，以及选中且处于编辑
// 工具时的控制点方块。
void SceneRenderer::drawPlaneGuides(QPainter &painter, const Plane &plane, bool selected,
                                    bool showHandles) const
{
    painter.save();
    const qreal lineWidth = (selected ? 1.7 : 1.0) / m_viewScale;
    const QColor color = selected ? QColor(52, 195, 255, 230) : QColor(70, 155, 210, 160);
    painter.setPen(QPen(color, lineWidth));
    painter.setBrush(Qt::NoBrush);
    painter.drawPolygon(planePolygon(plane.corner));

    painter.save();
    QPainterPath planeClip;
    planeClip.addPolygon(planePolygon(plane.corner));
    planeClip.closeSubpath();
    painter.setClipPath(planeClip, Qt::IntersectClip);
    painter.setPen(QPen(QColor(65, 182, 235, selected ? 145 : 80), 0.8 / m_viewScale));
    constexpr int divisions = 8;
    for (int i = 1; i < divisions; ++i) {
        const qreal t = qreal(i) / divisions;
        painter.drawLine(uvToPlane(plane, QPointF(t, 0)), uvToPlane(plane, QPointF(t, 1)));
        painter.drawLine(uvToPlane(plane, QPointF(0, t)), uvToPlane(plane, QPointF(1, t)));
    }
    painter.restore();

    if (selected && showHandles) {
        const QVector<QPointF> hs = handles(plane);
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
