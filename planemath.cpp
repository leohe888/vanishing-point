#include "planemath.h"

#include <QImage>
#include <QLineF>
#include <QTransform>
#include <QVector3D>
#include <QtMath>

namespace {

// 判断点 p 在变换 t（面片 -> 某目标空间）下的齐次分母 w 是否与面片质心同号、
// 且不贴近极点（w≈0）。单应变换在面片所在平面外有一个极点线（即“地平线”），
// 越过它之后映射结果没有意义；画笔向外延伸、浮动图像命中测试都需要此保护。
// 用质心的符号而非固定正号，是为了兼容顺时针/逆时针两种绕序。
bool hasFrontHomogeneousW(const QTransform &t, const Facet &facet, const QPointF &p)
{
    const QPointF centroid = (facet.corner[0] + facet.corner[1] +
                              facet.corner[2] + facet.corner[3]) / 4.0;
    const qreal wCenter = t.m13() * centroid.x() + t.m23() * centroid.y() + t.m33();
    const qreal w = t.m13() * p.x() + t.m23() * p.y() + t.m33();
    return wCenter * w > 0.0 && qAbs(w) > 1e-6;
}

} // namespace

namespace PlaneMath {

// 把 4 个角点组装为多边形。
QPolygonF planePolygon(const QPointF corner[4])
{
    // 角点顺序保持不变：调用方必须按顺时针或逆时针提供四个点，
    // 绝不能是交叉的多边形。
    return QPolygonF{corner[0], corner[1], corner[2], corner[3]};
}

// 返回面片的 8 个控制点：4 个角点在前，4 个边中点在后
QVector<QPointF> handles(const Facet &facet)
{
    return {facet.corner[0], facet.corner[1], facet.corner[2], facet.corner[3],
            (facet.corner[0] + facet.corner[1]) / 2.0,
            (facet.corner[1] + facet.corner[2]) / 2.0,
            (facet.corner[2] + facet.corner[3]) / 2.0,
            (facet.corner[3] + facet.corner[0]) / 2.0};
}

// 计算点 p 到线段 ab 的距离；t 返回最近点在线段上的参数化位置（0~1）
qreal distanceToSegment(const QPointF &p, const QPointF &a,
                        const QPointF &b, qreal *t)
{
    const QPointF d = b - a;
    const qreal len2 = QPointF::dotProduct(d, d);
    const qreal amount = len2 < Epsilon ? 0.0
                                        : qBound(0.0, QPointF::dotProduct(p - a, d) / len2, 1.0);
    if (t)
        *t = amount;
    return QLineF(p, a + d * amount).length();
}

// 校验面片是否为可用的单应变换目标：必须是非交叉的凸四边形，
// 且不能过于退化（边过短、面积过小或分母过零）。
bool isValidPlane(const Facet &facet)
{
    // 射影变换把单位正方形映射为简单的凸四边形。
    // 必须在进入 quadToQuad() 之前拒绝凹形、自交叉和近乎退化的
    // 配置，否则变换的极点可能穿过平面。
    qreal windingSign = 0.0;
    qreal twiceArea = 0.0;
    for (int i = 0; i < 4; ++i) {
        const QPointF a = facet.corner[i];
        const QPointF b = facet.corner[(i + 1) % 4];
        const QPointF c = facet.corner[(i + 2) % 4];
        if (QLineF(a, b).length() < 8.0)
            return false;
        const QPointF ab = b - a;
        const QPointF bc = c - b;
        const qreal cross = ab.x() * bc.y() - ab.y() * bc.x();
        if (qAbs(cross) < 4.0)
            return false;
        const qreal sign = cross > 0.0 ? 1.0 : -1.0;
        if (i == 0)
            windingSign = sign;
        else if (sign != windingSign)
            return false;
        twiceArea += a.x() * b.y() - b.x() * a.y();
    }
    if (qAbs(twiceArea) < 100.0)
        return false;

    const QPolygonF unit{QPointF(0, 0), QPointF(1, 0), QPointF(1, 1), QPointF(0, 1)};
    QTransform transform;
    if (!QTransform::quadToQuad(unit, planePolygon(facet.corner), transform))
        return false;

    // 齐次分母必须在完整的单位正方形上保持同一符号。
    // 由于它对 u/v 是线性的，只需检查四个角即可。
    qreal denominatorSign = 0.0;
    for (const QPointF &uv : unit) {
        const qreal w = transform.m13() * uv.x() + transform.m23() * uv.y() + transform.m33();
        if (!qIsFinite(w) || qAbs(w) < 1e-5)
            return false;
        const qreal sign = w > 0.0 ? 1.0 : -1.0;
        if (denominatorSign == 0.0)
            denominatorSign = sign;
        else if (sign != denominatorSign)
            return false;
    }
    return true;
}

// 归一化 UV 坐标 -> 面片上的图像坐标
QPointF uvToPlane(const Facet &facet, const QPointF &uv)
{
    const QPolygonF unit{QPointF(0, 0), QPointF(1, 0), QPointF(1, 1), QPointF(0, 1)};
    QTransform transform;
    if (!QTransform::quadToQuad(unit, planePolygon(facet.corner), transform))
        return {};
    return transform.map(uv);
}

// 面片上的图像坐标 -> 归一化 UV 坐标（ok 返回变换是否有效）
QPointF planeToUv(const Facet &facet, const QPointF &point, bool *ok)
{
    QTransform transform;
    const QPolygonF unit{QPointF(0, 0), QPointF(1, 0), QPointF(1, 1), QPointF(0, 1)};
    const bool valid = QTransform::quadToQuad(planePolygon(facet.corner), unit, transform);
    if (!valid || !hasFrontHomogeneousW(transform, facet, point)) {
        if (ok)
            *ok = false;
        return {};
    }
    if (ok)
        *ok = true;
    return transform.map(point);
}

// 面片上的图像坐标 -> 该面片所属分组的共享展开曲面坐标
QPointF planeToSurface(const Facet &facet, const QPointF &point, bool *ok)
{
    QTransform transform;
    const bool valid = QTransform::quadToQuad(planePolygon(facet.corner),
                                               planePolygon(facet.surfaceCorner), transform);
    if (!valid || !hasFrontHomogeneousW(transform, facet, point)) {
        if (ok)
            *ok = false;
        return {};
    }
    if (ok)
        *ok = true;
    return transform.map(point);
}

// 命中测试：返回点所在的最上层平面索引（后创建的优先），无命中返回 -1
int planeAt(const QVector<Plane> &planes, const QPointF &point)
{
    for (int i = planes.size() - 1; i >= 0; --i) {
        if (planePolygon(planes[i].corner).containsPoint(point, Qt::OddEvenFill))
            return i;
    }
    return -1;
}

// 命中测试：返回距离点最近的控制点索引，无命中返回 -1
int handleAt(const Facet &facet, const QPointF &point, qreal tolerance)
{
    const QVector<QPointF> hs = handles(facet);
    for (int i = 0; i < hs.size(); ++i) {
        if (QLineF(hs[i], point).length() <= tolerance)
            return i;
    }
    return -1;
}

// 命中测试：返回点靠近的边缘索引（0~3），无命中返回 -1
int edgeAt(const Facet &facet, const QPointF &point, qreal tolerance)
{
    for (int i = 0; i < 4; ++i) {
        if (distanceToSegment(point, facet.corner[i], facet.corner[(i + 1) % 4]) <= tolerance)
            return i;
    }
    return -1;
}

// 保持原单应变换，在展开坐标中平移四个角点后重新投影。
bool movePlaneOnSurface(const Plane &source, const QPointF &dragPoint,
                        const QPointF &pressPoint, Plane *result)
{
    if (!result || !qIsFinite(dragPoint.x()) || !qIsFinite(dragPoint.y()))
        return false;
    bool pressOk = false, dragOk = false;
    const QPointF press = planeToSurface(source, pressPoint, &pressOk);
    const QPointF drag = planeToSurface(source, dragPoint, &dragOk);
    if (!pressOk || !dragOk)
        return false;
    QTransform projection;
    if (!QTransform::quadToQuad(planePolygon(source.surfaceCorner), planePolygon(source.corner), projection))
        return false;
    const QPointF center = (source.surfaceCorner[0] + source.surfaceCorner[1]
                            + source.surfaceCorner[2] + source.surfaceCorner[3]) / 4;
    auto denominator = [&projection](const QPointF &point) {
        return projection.m13() * point.x() + projection.m23() * point.y() + projection.m33();
    };
    const qreal referenceW = denominator(center);
    const QPointF delta = drag - press;
    Plane candidate = source;
    for (int i = 0; i < 4; ++i) {
        candidate.surfaceCorner[i] = source.surfaceCorner[i] + delta;
        const qreal w = denominator(candidate.surfaceCorner[i]);
        if (!qIsFinite(w) || w * referenceW <= 0 || qAbs(w) <= qAbs(referenceW) * 1e-6)
            return false;
        candidate.corner[i] = projection.map(candidate.surfaceCorner[i]);
        const QPointF &corner = candidate.corner[i];
        if (!qIsFinite(corner.x()) || !qIsFinite(corner.y())
            || qAbs(corner.x()) > 1e7 || qAbs(corner.y()) > 1e7)
            return false;
    }
    if (!isValidPlane(candidate))
        return false;
    *result = candidate;
    return true;
}

// 沿某条边方向缩放平面：只改变该边到对边的距离，保持透视关系不变
Plane resizePlaneAlongEdge(const Plane &source, int edge,
                           const QPointF &dragPoint, const QPointF &pressPoint)
{
    Plane result = source;
    const int next = (edge + 1) % 4;
    const int oppositeNext = (edge + 2) % 4;
    const int previous = (edge + 3) % 4;
    const QPointF a = source.corner[edge];
    const QPointF b = source.corner[next];
    const QPointF oppositeA = source.corner[oppositeNext];
    const QPointF oppositeB = source.corner[previous];
    const QPointF edgeMidpoint = (a + b) / 2.0;
    const QPointF oppositeMidpoint = (oppositeA + oppositeB) / 2.0;

    // 沿边缩放只有一个自由度。忽略指针的侧向移动，
    // 只保留沿平面既有延伸轴方向的位移量。
    QPointF extensionAxis = edgeMidpoint - oppositeMidpoint;
    qreal axisLength = QLineF(QPointF(), extensionAxis).length();
    if (axisLength < Epsilon) {
        extensionAxis = QPointF(-(b - a).y(), (b - a).x());
        axisLength = QLineF(QPointF(), extensionAxis).length();
    }
    if (axisLength < Epsilon)
        return result;
    extensionAxis /= axisLength;
    const qreal extension = QPointF::dotProduct(dragPoint - pressPoint, extensionAxis);
    const QPointF targetPoint = edgeMidpoint + extensionAxis * extension;

    // 缩放后的边必须保留原边方向的消失点。
    QPointF edgeVanishingPoint;
    const auto vpType = QLineF(a, b).intersects(QLineF(oppositeA, oppositeB),
                                                &edgeVanishingPoint);
    QLineF resizedEdge;
    if (vpType != QLineF::NoIntersection && qIsFinite(edgeVanishingPoint.x()) &&
        qIsFinite(edgeVanishingPoint.y()) &&
        QLineF(edgeVanishingPoint, targetPoint).length() > 1.0 &&
        QLineF(edgeVanishingPoint, edgeMidpoint).length() < 1e7) {
        resizedEdge = QLineF(edgeVanishingPoint, targetPoint);
    } else {
        // 对边平行是消失点位于无穷远处的极限情形。
        resizedEdge = QLineF(targetPoint, targetPoint + (b - a));
    }

    // 每个端点都被约束在它原来所在的侧边线上。
    // 这正是保证“垂直平面在缩放时只改变高度、仍然保持垂直”的原因。
    QPointF movedA;
    QPointF movedB;
    const auto aType = QLineF(a, source.corner[previous]).intersects(resizedEdge, &movedA);
    const auto bType = QLineF(b, source.corner[oppositeNext]).intersects(resizedEdge, &movedB);
    if (aType == QLineF::NoIntersection || bType == QLineF::NoIntersection ||
        !qIsFinite(movedA.x()) || !qIsFinite(movedA.y()) ||
        !qIsFinite(movedB.x()) || !qIsFinite(movedB.y()))
        return result;

    result.corner[edge] = movedA;
    result.corner[next] = movedB;
    return result;
}

// 恢复源平面法线在图像上的投影方向（即第三个消失方向）。
// 该方向被所有垂直于源平面的平面共享。
bool perpendicularDirection(const Plane &source, const QPointF &atPoint,
                            const QSize &backgroundSize, QPointF *direction)
{
    // 在齐次图像坐标下恢复源平面的两个消失点。
    // 齐次形式同时也能覆盖平行线族（消失点在无穷远）的情形。
    auto imagePoint = [](const QPointF &p) {
        return QVector3D(float(p.x()), float(p.y()), 1.0f);
    };
    const QVector3D p0 = imagePoint(source.corner[0]);
    const QVector3D p1 = imagePoint(source.corner[1]);
    const QVector3D p2 = imagePoint(source.corner[2]);
    const QVector3D p3 = imagePoint(source.corner[3]);
    const QVector3D line01 = QVector3D::crossProduct(p0, p1);
    const QVector3D line32 = QVector3D::crossProduct(p3, p2);
    const QVector3D line03 = QVector3D::crossProduct(p0, p3);
    const QVector3D line12 = QVector3D::crossProduct(p1, p2);
    const QVector3D vanishingX = QVector3D::crossProduct(line01, line32);
    const QVector3D vanishingY = QVector3D::crossProduct(line03, line12);
    if (vanishingX.lengthSquared() < 1e-12f || vanishingY.lengthSquared() < 1e-12f)
        return false;

    const qreal cx = backgroundSize.width() / 2.0;
    const qreal cy = backgroundSize.height() / 2.0;
    const qreal imageExtent = qMax(backgroundSize.width(), backgroundSize.height());
    qreal focalLength = imageExtent * 1.2;

    // 当两个消失点均为有限值、且两条网格轴代表相互正交的世界方向时，
    // 可由正交性解出焦距。
    if (qAbs(vanishingX.z()) > 1e-6 && qAbs(vanishingY.z()) > 1e-6) {
        const QPointF vx(vanishingX.x() / vanishingX.z(),
                         vanishingX.y() / vanishingX.z());
        const QPointF vy(vanishingY.x() / vanishingY.z(),
                         vanishingY.y() / vanishingY.z());
        const qreal inferredFocalSquared =
            -QPointF::dotProduct(vx - QPointF(cx, cy), vy - QPointF(cx, cy));
        const qreal minimumFocal = imageExtent * 0.08;
        const qreal maximumFocal = imageExtent * 20.0;
        if (inferredFocalSquared > minimumFocal * minimumFocal &&
            inferredFocalSquared < maximumFocal * maximumFocal)
            focalLength = qSqrt(inferredFocalSquared);
    }

    auto cameraDirection = [cx, cy, focalLength](const QVector3D &v) {
        return QVector3D(float(v.x() - cx * v.z()),
                         float(v.y() - cy * v.z()),
                         float(focalLength * v.z())).normalized();
    };
    const QVector3D directionX = cameraDirection(vanishingX);
    const QVector3D directionY = cameraDirection(vanishingY);
    QVector3D normal = QVector3D::crossProduct(directionX, directionY);
    if (normal.lengthSquared() < 1e-10f)
        return false;
    normal.normalize();

    // 把 3D 法线经内参矩阵 K 投影回图像。这就是与源平面垂直的所有
    // 平面共享的第三个消失点。
    const qreal projectedX = focalLength * normal.x() + cx * normal.z();
    const qreal projectedY = focalLength * normal.y() + cy * normal.z();
    QPointF projectedDirection;
    if (qAbs(normal.z()) > 1e-6) {
        const QPointF perpendicularVanishingPoint(projectedX / normal.z(),
                                                  projectedY / normal.z());
        projectedDirection = perpendicularVanishingPoint - atPoint;
    } else {
        // 齐次分量 w 为零意味着第三个消失点位于无穷远处。
        projectedDirection = QPointF(projectedX, projectedY);
    }

    const qreal length = QLineF(QPointF(), projectedDirection).length();
    if (!qIsFinite(length) || length < Epsilon)
        return false;
    *direction = projectedDirection / length;
    return true;
}

// 从源平面的一条边拖出与之垂直的新平面（Ctrl+拖动边缘）
Plane makePerpendicularPlane(const Plane &source, int edge,
                             const QPointF &dragPoint, const QPointF &pressPoint,
                             const QSize &backgroundSize)
{
    Plane result;
    result.surfaceGroup = source.surfaceGroup;
    result.surfaceCorner[0] = source.surfaceCorner[edge];
    result.surfaceCorner[1] = source.surfaceCorner[(edge + 1) % 4];
    result.surfaceCorner[2] = result.surfaceCorner[1];
    result.surfaceCorner[3] = result.surfaceCorner[0];
    const QPointF a = source.corner[edge];
    const QPointF b = source.corner[(edge + 1) % 4];
    const QPointF midpoint = (a + b) / 2.0;
    QPointF perpendicularAtMidpoint;
    if (!perpendicularDirection(source, midpoint, backgroundSize, &perpendicularAtMidpoint))
        return result;

    // 指针只控制沿投影后 3D 法线方向的有符号距离。
    // 侧向移动无法改变垂直平面的角度。
    const qreal amount = QPointF::dotProduct(dragPoint - pressPoint,
                                             perpendicularAtMidpoint);
    const QPointF targetMidpoint = midpoint + perpendicularAtMidpoint * amount;
    result.corner[0] = a;
    result.corner[1] = b;

    if (qAbs(amount) < 2.0) {
        result.corner[2] = b;
        result.corner[3] = a;
        return result;
    }

    // 共享边与新的外侧边在 3D 中代表同一方向，
    // 因此二者相交于原边线族的消失点。
    const QPointF oppositeA = source.corner[(edge + 2) % 4];
    const QPointF oppositeB = source.corner[(edge + 3) % 4];
    QPointF edgeVanishingPoint;
    const QLineF::IntersectionType vpType =
        QLineF(a, b).intersects(QLineF(oppositeA, oppositeB), &edgeVanishingPoint);

    bool constructedWithVanishingPoint = false;
    if (vpType != QLineF::NoIntersection && qIsFinite(edgeVanishingPoint.x()) &&
        qIsFinite(edgeVanishingPoint.y()) &&
        QLineF(edgeVanishingPoint, (a + b) / 2.0).length() < 1e7) {
        if (QLineF(edgeVanishingPoint, targetMidpoint).length() > 1.0) {
            QPointF outerAtB;
            QPointF outerAtA;
            const QLineF outerLine(edgeVanishingPoint, targetMidpoint);
            QPointF perpendicularAtA;
            QPointF perpendicularAtB;
            if (!perpendicularDirection(source, a, backgroundSize, &perpendicularAtA) ||
                !perpendicularDirection(source, b, backgroundSize, &perpendicularAtB))
                return result;
            const auto bType = QLineF(b, b + perpendicularAtB).intersects(outerLine, &outerAtB);
            const auto aType = QLineF(a, a + perpendicularAtA).intersects(outerLine, &outerAtA);
            if (bType != QLineF::NoIntersection && aType != QLineF::NoIntersection &&
                qIsFinite(outerAtA.x()) && qIsFinite(outerAtA.y()) &&
                qIsFinite(outerAtB.x()) && qIsFinite(outerAtB.y())) {
                result.corner[2] = outerAtB;
                result.corner[3] = outerAtA;
                constructedWithVanishingPoint = true;
            }
        }
    }

    // 源平面的边相互平行时其消失点在无穷远处，因此外侧边保持平行，
    // 但其端点仍沿第三个（垂直）消失方向移动。
    if (!constructedWithVanishingPoint) {
        const QLineF outerLine(targetMidpoint, targetMidpoint + (b - a));
        QPointF perpendicularAtA;
        QPointF perpendicularAtB;
        QPointF outerAtA;
        QPointF outerAtB;
        if (perpendicularDirection(source, a, backgroundSize, &perpendicularAtA) &&
            perpendicularDirection(source, b, backgroundSize, &perpendicularAtB) &&
            QLineF(a, a + perpendicularAtA).intersects(outerLine, &outerAtA) !=
                QLineF::NoIntersection &&
            QLineF(b, b + perpendicularAtB).intersects(outerLine, &outerAtB) !=
                QLineF::NoIntersection) {
            result.corner[2] = outerAtB;
            result.corner[3] = outerAtA;
        } else {
            result.corner[2] = b;
            result.corner[3] = a;
        }
    }

    // 把垂直面绕共享边展开到曲面上。两个面在接缝处保持完全相同的
    // 曲面坐标，而新的外侧边被放置在源面内部的另一侧。
    const QPointF surfaceA = result.surfaceCorner[0];
    const QPointF surfaceB = result.surfaceCorner[1];
    const QPointF surfaceEdge = surfaceB - surfaceA;
    const qreal surfaceEdgeLength = QLineF(surfaceA, surfaceB).length();
    const qreal canvasEdgeLength = qMax(Epsilon, QLineF(a, b).length());
    QPointF outward(-surfaceEdge.y(), surfaceEdge.x());
    const qreal outwardLength = QLineF(QPointF(), outward).length();
    if (outwardLength > Epsilon)
        outward /= outwardLength;
    QPointF sourceCenter;
    for (const QPointF &corner : source.surfaceCorner)
        sourceCenter += corner;
    sourceCenter /= 4.0;
    const QPointF seamCenter = (surfaceA + surfaceB) / 2.0;
    if (QPointF::dotProduct(outward, sourceCenter - seamCenter) > 0.0)
        outward = -outward;
    const qreal canvasDepth = (QLineF(result.corner[0], result.corner[3]).length() +
                               QLineF(result.corner[1], result.corner[2]).length()) / 2.0;
    const qreal surfaceDepth = qMax(1.0, canvasDepth * surfaceEdgeLength / canvasEdgeLength);
    result.surfaceCorner[2] = surfaceB + outward * surfaceDepth;
    result.surfaceCorner[3] = surfaceA + outward * surfaceDepth;
    return result;
}

} // namespace PlaneMath
