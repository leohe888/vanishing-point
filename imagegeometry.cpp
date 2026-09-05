#include "imagegeometry.h"
#include "floatingimagemath.h"

#include <QCache>
#include <QDataStream>
#include <QIODevice>
#include <QPainterPathStroker>

QByteArray ImageGeometry::key(const FloatingImage &image)
{
    QByteArray key;
    QDataStream stream(&key, QIODevice::WriteOnly);
    stream << image.image.size() << image.position << image.scale << image.rotation
           << image.attached << image.hostFace << qint64(image.faces.size());
    for (const Facet &face : image.faces)
        for (int i = 0; i < 4; ++i)
            stream << face.corner[i] << face.surfaceCorner[i];
    return key;
}

QSharedPointer<const ImageGeometry> ImageGeometry::get(const FloatingImage &image)
{
    // 按值构造键，覆盖撤销、旋转、缩放、吸附、尺寸和几何快照变化。
    // 缓存不持有原始位图；按线程隔离，最多保留 128 份几何结果。
    static thread_local QCache<QByteArray, QSharedPointer<const ImageGeometry>> cache(128);
    const QByteArray geometryKey = key(image);
    if (const auto *entry = cache.object(geometryKey))
        return *entry;
    QSharedPointer<const ImageGeometry> geometry(new ImageGeometry(image));
    cache.insert(geometryKey, new QSharedPointer<const ImageGeometry>(geometry));
    return geometry;
}

ImageGeometry::ImageGeometry(const FloatingImage &image)
    : m_imageToSpace(FloatingImageMath::imageToSpace(image)), m_position(image.position)
{
    if (image.image.isNull())
        return;
    QPainterPath imagePath;
    imagePath.addRect(QRectF(QPointF(0, 0), QSizeF(image.image.size())));
    auto append = [&](const QPolygonF &domain, const QPolygonF &canvas, const QPainterPath &clip) {
        const ProjectiveMapping mapping(domain, canvas);
        if (!clip.isEmpty() && mapping.isValid())
            m_patches.append({mapping, clip, mapping.forward().map(clip)});
    };
    if (!image.attached || image.faces.isEmpty()) {
        const QRectF rect(QPointF(0, 0), QSizeF(image.image.size()));
        const QPolygonF domain{rect.topLeft(), rect.topRight(), rect.bottomRight(), rect.bottomLeft()};
        append(domain, m_imageToSpace.map(domain), imagePath);
    } else {
        QVector<QPolygonF> sources;
        QVector<QPainterPath> clips;
        QPainterPath hostClip = imagePath;
        const QTransform inverse = m_imageToSpace.inverted();
        for (int i = 0; i < image.faces.size(); ++i) {
            const QPolygonF polygon = inverse.map(PlaneMath::planePolygon(image.faces[i].surfaceCorner));
            sources.append(polygon);
            QPainterPath facePath;
            facePath.addPolygon(polygon);
            facePath.closeSubpath();
            clips.append(imagePath.intersected(facePath));
            if (i != image.hostFace)
                hostClip = hostClip.subtracted(facePath);
        }
        if (image.hostFace >= 0 && image.hostFace < image.faces.size())
            append(sources[image.hostFace], PlaneMath::planePolygon(image.faces[image.hostFace].corner), hostClip);
        for (int i = 0; i < image.faces.size(); ++i)
            if (i != image.hostFace)
                append(sources[i], PlaneMath::planePolygon(image.faces[i].corner), clips[i]);
    }
    if (m_patches.size() == 1)
        m_outline = m_patches.first().canvasClip;
    else {
        QPainterPathStroker tolerance;
        tolerance.setWidth(.0001);
        tolerance.setJoinStyle(Qt::MiterJoin);
        for (const ImagePatch &patch : m_patches)
            m_outline = m_outline.united(patch.canvasClip.united(tolerance.createStroke(patch.canvasClip)));
        m_outline = m_outline.simplified();
    }
    for (const QPointF &point : FloatingImageMath::controlPoints(image))
        m_controls.append(FloatingImageMath::toCanvas(image, point));
}

bool ImageGeometry::hitTest(const QPointF &point, QPointF *spaceOffset) const
{
    for (auto it = m_patches.crbegin(); it != m_patches.crend(); ++it) {
        QPointF local;
        if (!it->mapping.fromCanvas(point, &local) || !it->clip.contains(local))
            continue;
        if (spaceOffset)
            *spaceOffset = m_imageToSpace.map(local) - m_position;
        return true;
    }
    return false;
}
