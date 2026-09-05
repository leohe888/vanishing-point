#include "floatingimagemath.h"

#include <QTransform>
#include <QLineF>
#include <cmath>
#include <QtMath>

namespace FloatingImageMath {
QTransform imageToSpace(const FloatingImage &image)
{
    const QSizeF size = image.displayedSize();
    const QPointF center = image.position + QPointF(size.width() / 2, size.height() / 2);
    QTransform transform;
    transform.translate(center.x(), center.y());
    transform.rotate(image.rotation);
    transform.translate(-size.width() / 2, -size.height() / 2);
    transform.scale(image.scale.x(), image.scale.y());
    return transform;
}

QPointF toCanvas(const FloatingImage &image, const QPointF &point, int *faceIndex)
{
    int face = image.hostFace;
    if (image.attached) {
        for (int i = image.faces.size() - 1; i >= 0; --i)
            if (PlaneMath::planePolygon(image.faces[i].surfaceCorner).containsPoint(point, Qt::OddEvenFill)) {
                face = i;
                break;
            }
    }
    if (faceIndex)
        *faceIndex = face;
    if (!image.attached || face < 0 || face >= image.faces.size())
        return point;
    QPointF result;
    if (!PlaneMath::surfaceMapping(image.faces[face]).toCanvas(point, &result))
        return QPointF(qQNaN(), qQNaN());
    return result;
}

bool fromCanvas(const FloatingImage &image, const QPointF &point, QPointF *result, int fallbackFace)
{
    if (!image.attached || image.faces.isEmpty()) {
        *result = point;
        return true;
    }
    int face = fallbackFace >= 0 ? fallbackFace : image.hostFace;
    for (int i = image.faces.size() - 1; i >= 0; --i)
        if (PlaneMath::planePolygon(image.faces[i].corner).containsPoint(point, Qt::OddEvenFill)) {
            face = i;
            break;
        }
    if (face < 0 || face >= image.faces.size())
        return false;
    bool ok = false;
    *result = PlaneMath::planeToSurface(image.faces[face], point, &ok);
    return ok && std::isfinite(result->x()) && std::isfinite(result->y());
}

QVector<QPointF> controlPoints(const FloatingImage &image)
{
    const QRectF rect(QPointF(0, 0), QSizeF(image.image.size()));
    QVector<QPointF> points{rect.topLeft(), rect.topRight(), rect.bottomRight(), rect.bottomLeft(),
            (rect.topLeft() + rect.topRight()) / 2, (rect.topRight() + rect.bottomRight()) / 2,
            (rect.bottomLeft() + rect.bottomRight()) / 2, (rect.topLeft() + rect.bottomLeft()) / 2};
    const QTransform transform = imageToSpace(image);
    for (QPointF &point : points)
        point = transform.map(point);
    return points;
}

FloatingImage resized(const FloatingImage &start, int handle, const QPointF &spacePoint,
                      bool keepAspect, bool fromCenter)
{
    FloatingImage result = start;
    if (handle < 0 || handle >= 8 || !std::isfinite(spacePoint.x()) || !std::isfinite(spacePoint.y()))
        return result;
    const QRectF rect(start.position, start.displayedSize());
    QTransform rotation;
    rotation.translate(rect.center().x(), rect.center().y());
    rotation.rotate(start.rotation);
    rotation.translate(-rect.center().x(), -rect.center().y());
    const QPointF point = rotation.inverted().map(spacePoint);
    const bool left = handle == 0 || handle == 3 || handle == 7;
    const bool right = handle == 1 || handle == 2 || handle == 5;
    const bool top = handle == 0 || handle == 1 || handle == 4;
    const bool bottom = handle == 2 || handle == 3 || handle == 6;
    qreal width = rect.width(), height = rect.height();
    const QPointF anchor = fromCenter ? rect.center() : QPointF(left ? rect.right() : rect.left(),
                                                               top ? rect.bottom() : rect.top());
    const qreal multiplier = fromCenter ? 2 : 1;
    if (left || right)
        width = qBound(1.0, (left ? anchor.x() - point.x() : point.x() - anchor.x()) * multiplier, 100000.0);
    if (top || bottom)
        height = qBound(1.0, (top ? anchor.y() - point.y() : point.y() - anchor.y()) * multiplier, 100000.0);
    if (keepAspect) {
        qreal factor = (left || right) ? width / rect.width() : height / rect.height();
        if (handle < 4)
            factor = qMax(factor, height / rect.height());
        factor = qBound(qMax(1 / rect.width(), 1 / rect.height()), factor,
                        qMin(100000 / rect.width(), 100000 / rect.height()));
        width = rect.width() * factor;
        height = rect.height() * factor;
    }
    result.position = QPointF(left ? rect.right() - width : (right ? rect.left() : rect.center().x() - width / 2),
                              top ? rect.bottom() - height : (bottom ? rect.top() : rect.center().y() - height / 2));
    if (fromCenter)
        result.position = rect.center() - QPointF(width / 2, height / 2);
    const QPointF halfSize(width / 2, height / 2);
    result.position = rotation.map(result.position + halfSize) - halfSize;
    result.scale = QPointF(width / start.image.width(), height / start.image.height());
    return result;
}

FloatingImage rotated(const FloatingImage &start, const QPointF &press, const QPointF &point, bool snap)
{
    FloatingImage result = start;
    const QPointF center = QRectF(start.position, start.displayedSize()).center();
    const QPointF a = press - center, b = point - center;
    if (QLineF(center, press).length() < 1e-6 || QLineF(center, point).length() < 1e-6)
        return result;
    qreal angle = start.rotation + qRadiansToDegrees(std::atan2(b.y(), b.x()) - std::atan2(a.y(), a.x()));
    if (!std::isfinite(angle))
        return result;
    if (snap)
        angle = qRound(angle / 15) * 15;
    result.rotation = std::remainder(angle, 360.0);
    return result;
}
}
