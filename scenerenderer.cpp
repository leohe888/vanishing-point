#include "scenerenderer.h"

#include "canvasdocument.h"
#include "floatingimagemath.h"

#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>

using namespace PlaneMath;

namespace {
struct ImagePatch {
    QTransform projection;
    QPainterPath clip;
};

// 图片内容和蚂蚁线共用同一套裁剪、投影规则。
QVector<ImagePatch> imagePatches(const FloatingImage &image)
{
    if (image.image.isNull())
        return {};
    QPainterPath imagePath;
    imagePath.addRect(QRectF(QPointF(0, 0), QSizeF(image.image.size())));
    if (!image.attached || image.faces.isEmpty()) {
        return {{FloatingImageMath::imageToSpace(image), imagePath}};
    }

    QVector<QPolygonF> sources;
    QVector<QPainterPath> clips;
    QPainterPath hostClip = imagePath;
    const QTransform spaceToImage = FloatingImageMath::imageToSpace(image).inverted();
    for (int i = 0; i < image.faces.size(); ++i) {
        QPolygonF source;
        for (const QPointF &corner : image.faces[i].surfaceCorner)
            source << spaceToImage.map(corner);
        sources.append(source);
        QPainterPath facePath;
        facePath.addPolygon(source);
        facePath.closeSubpath();
        clips.append(imagePath.intersected(facePath));
        if (i != image.hostFace)
            hostClip = hostClip.subtracted(facePath);
    }
    QVector<ImagePatch> patches;
    auto appendFace = [&](int i, const QPainterPath &clip) {
        QTransform projection;
        if (!clip.isEmpty() && QTransform::quadToQuad(sources[i], planePolygon(image.faces[i].corner), projection))
            patches.append({projection, clip});
    };
    if (image.hostFace >= 0 && image.hostFace < image.faces.size())
        appendFace(image.hostFace, hostClip);
    for (int i = 0; i < image.faces.size(); ++i)
        if (i != image.hostFace)
            appendFace(i, clips[i]);
    return patches;
}
}

SceneRenderer::SceneRenderer(const CanvasDocument &doc)
    : m_doc(doc)
{
}

// 渲染完整场景。showGuides 为 true 时额外绘制编辑辅助元素。
void SceneRenderer::render(QPainter &painter, qreal viewScale, bool showGuides,
                           const QVector<QPointF> &creationPoints,
                           const Plane *extrudePreview, bool editHandlesVisible,
                           int hoveredPlane, qreal antsPhase)
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
    const QVector<ImagePatch> patches = imagePatches(image);
    if (patches.size() == 1)
        return patches.first().projection.map(patches.first().clip);
    QPainterPath outline;
    QPainterPathStroker seamTolerance;
    seamTolerance.setWidth(0.0001);
    seamTolerance.setJoinStyle(Qt::MiterJoin);
    for (const ImagePatch &patch : patches) {
        const QPainterPath projected = patch.projection.map(patch.clip);
        // 相邻单应矩阵在共享边上可能相差几个浮点尾数。
        // 极小的亚像素容差将这些边合并，避免并集残留内部细缝。
        outline = outline.united(projected.united(seamTolerance.createStroke(projected)));
    }
    return outline.simplified();
}

// 渲染一张浮动图像：未吸附时直接绘制；已吸附时按几何快照分段投影。
void SceneRenderer::renderFloatingImage(QPainter &painter, const FloatingImage &image) const
{
    for (const ImagePatch &patch : imagePatches(image)) {
        painter.save();
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        // 在共同的画布坐标中裁剪，避免旋转后各面独立栅格化源裁剪路径
        // 时把共享边上的同一个像素同时排除。
        const QPainterPath projectedClip = patch.projection.map(patch.clip);
        QPainterPathStroker seamTolerance;
        seamTolerance.setWidth(.04 / qMax(m_viewScale, 1e-6));
        seamTolerance.setJoinStyle(Qt::MiterJoin);
        painter.setClipPath(projectedClip.united(seamTolerance.createStroke(projectedClip)), Qt::IntersectClip);
        painter.setWorldTransform(patch.projection, true);
        painter.drawImage(QPointF(0, 0), image.image);
        painter.restore();
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
