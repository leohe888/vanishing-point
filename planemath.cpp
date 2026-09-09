#include "planemath.h"

#include <QtMath>

#include <QImage>
#include <QLineF>
#include <QSize>
#include <QTransform>
#include <QtMath>

#include <cmath>

// —— 内部工具：双精度三维向量与相机模型 ——
// 图像坐标可达数千，两条直线叉乘后的中间量会超过 float 的有效位数，
// 因此这里不用 QVector3D，全部按双精度计算。
namespace {

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

Vec3 operator+(const Vec3 &a, const Vec3 &b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(const Vec3 &a, const Vec3 &b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(const Vec3 &a, double s) { return {a.x * s, a.y * s, a.z * s}; }

double dot(const Vec3 &a, const Vec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vec3 cross(const Vec3 &a, const Vec3 &b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

bool normalize(Vec3 *v)
{
    const double length = std::sqrt(dot(*v, *v));
    if (!qIsFinite(length) || length < 1e-12)
        return false;
    *v = *v * (1.0 / length);
    return true;
}

// 图像点 / 图像直线的齐次表示
Vec3 imagePoint(const QPointF &p) { return {p.x(), p.y(), 1.0}; }
Vec3 joinLines(const Vec3 &a, const Vec3 &b) { return cross(a, b); } // 过两点的直线
Vec3 meetLines(const Vec3 &l, const Vec3 &m) { return cross(l, m); } // 两直线的交点

// 齐次点 -> 图像点；位于无穷远（w≈0）时失败
bool toImagePoint(const Vec3 &v, QPointF *out)
{
    if (!out || qAbs(v.z) < 1e-12)
        return false;
    const QPointF p(v.x / v.z, v.y / v.z);
    if (!qIsFinite(p.x()) || !qIsFinite(p.y()))
        return false;
    *out = p;
    return true;
}

// 相机内参的估计值：主点固定在图像中心，焦距由一对正交消失点解出。
struct CameraFrame {
    double focal = 0.0;
    double cx = 0.0;
    double cy = 0.0;
};

// 由两个相互正交的世界方向的消失点解出焦距。
// 退化（消失点在无穷远或解不合理）时返回与画幅相关的经验值。
double focalFromOrthogonalVanishingPoints(const QPointF &vx, const QPointF &vy,
                                          const QSize &backgroundSize)
{
    const double cx = backgroundSize.width() / 2.0;
    const double cy = backgroundSize.height() / 2.0;
    const double extent = qMax(backgroundSize.width(), backgroundSize.height());
    const double fallback = extent * 1.2;
    if (!qIsFinite(vx.x()) || !qIsFinite(vx.y()) || !qIsFinite(vy.x()) || !qIsFinite(vy.y()))
        return fallback;
    const double squared = -((vx.x() - cx) * (vy.x() - cx) + (vx.y() - cy) * (vy.y() - cy));
    const double minimum = extent * 0.08;
    const double maximum = extent * 20.0;
    if (squared > minimum * minimum && squared < maximum * maximum)
        return std::sqrt(squared);
    return fallback;
}

// 消失点 -> 相机坐标系下的世界方向
Vec3 vanishingDirection(const Vec3 &v, const CameraFrame &frame)
{
    return {v.x - frame.cx * v.z, v.y - frame.cy * v.z, frame.focal * v.z};
}

// 世界方向 -> 图像上的消失点（齐次；w≈0 表示位于无穷远）
Vec3 projectDirection(const Vec3 &d, const CameraFrame &frame)
{
    return {frame.focal * d.x + frame.cx * d.z,
            frame.focal * d.y + frame.cy * d.z,
            d.z};
}

// 图像点 -> 相机坐标系下的射线
Vec3 imageRay(const QPointF &p, const CameraFrame &frame)
{
    return {p.x() - frame.cx, p.y() - frame.cy, frame.focal};
}

// 相机坐标点 -> 图像点（经内参矩阵 K 投影）。
// 落到相机后方或飞到极远处都视为失败。
bool projectPoint(const Vec3 &p, const CameraFrame &frame, QPointF *out)
{
    if (!out || !(p.z > 1e-9))
        return false;
    const QPointF q(frame.cx + frame.focal * p.x / p.z,
                    frame.cy + frame.focal * p.y / p.z);
    if (!qIsFinite(q.x()) || !qIsFinite(q.y()) ||
        qAbs(q.x()) > 1e7 || qAbs(q.y()) > 1e7)
        return false;
    *out = q;
    return true;
}

} // namespace

namespace PlaneMath {
ProjectiveMapping surfaceMapping(const Facet &facet)
{
    return {planePolygon(facet.surfaceCorner), planePolygon(facet.corner)};
}

ProjectiveMapping uvMapping(const Facet &facet)
{
    return {QPolygonF{QPointF(0, 0), QPointF(1, 0), QPointF(1, 1), QPointF(0, 1)}, planePolygon(facet.corner)};
}


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
    QPointF result;
    uvMapping(facet).toCanvas(uv, &result);
    return result;
}

// 面片上的图像坐标 -> 归一化 UV 坐标（ok 返回变换是否有效）
QPointF planeToUv(const Facet &facet, const QPointF &point, bool *ok)
{
    QPointF result;
    const bool valid = uvMapping(facet).fromCanvas(point, &result);
    if (ok)
        *ok = valid;
    return result;
}

// 面片上的图像坐标 -> 该面片所属分组的共享展开曲面坐标
QPointF planeToSurface(const Facet &facet, const QPointF &point, bool *ok)
{
    QPointF result;
    const bool valid = surfaceMapping(facet).fromCanvas(point, &result);
    if (ok)
        *ok = valid;
    return result;
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
    const ProjectiveMapping projection = surfaceMapping(source);
    const QPointF delta = drag - press;
    Plane candidate = source;
    for (int i = 0; i < 4; ++i) {
        candidate.surfaceCorner[i] = source.surfaceCorner[i] + delta;
        if (!projection.toCanvas(candidate.surfaceCorner[i], &candidate.corner[i]))
            return false;
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

    // 四边形范围变了，但它在展开曲面上的参数化必须原封不动：用改动前的
    // 映射反推两个新角点的曲面坐标。只改 corner 而留下旧的 surfaceCorner，
    // 等于把整张展开图重新拉伸标定——相邻平面在接缝处会把同一个曲面坐标
    // 送到不同的画布点，跨缝的浮动图像和笔迹就会错位。
    const ProjectiveMapping projection = surfaceMapping(source);
    QPointF surface[2];
    for (int i = 0; i < 2; ++i) {
        const int corner = i == 0 ? edge : next;
        // 新角点落到极点线之外时无法保持展开参数化，此时宁可放弃这次改动。
        if (!projection.fromCanvas(result.corner[corner], &surface[i]))
            return source;
    }
    result.surfaceCorner[edge] = surface[0];
    result.surfaceCorner[next] = surface[1];
    return result;
}

// 恢复源平面法线在图像上的投影方向（即第三个消失方向）。
// 该方向被所有垂直于源平面的平面共享。
bool perpendicularDirection(const Plane &source, const QPointF &atPoint,
                            const QSize &backgroundSize, QPointF *direction)
{
    // 在齐次图像坐标下恢复源平面的两个消失点。
    // 齐次形式同时也能覆盖平行线族（消失点在无穷远）的情形。
    const Vec3 p0 = imagePoint(source.corner[0]);
    const Vec3 p1 = imagePoint(source.corner[1]);
    const Vec3 p2 = imagePoint(source.corner[2]);
    const Vec3 p3 = imagePoint(source.corner[3]);
    const Vec3 line01 = joinLines(p0, p1);
    const Vec3 line32 = joinLines(p3, p2);
    const Vec3 line03 = joinLines(p0, p3);
    const Vec3 line12 = joinLines(p1, p2);
    const Vec3 vanishingX = meetLines(line01, line32);
    const Vec3 vanishingY = meetLines(line03, line12);
    if (dot(vanishingX, vanishingX) < 1e-12 || dot(vanishingY, vanishingY) < 1e-12)
        return false;

    const qreal cx = backgroundSize.width() / 2.0;
    const qreal cy = backgroundSize.height() / 2.0;
    const qreal imageExtent = qMax(backgroundSize.width(), backgroundSize.height());
    qreal focalLength = imageExtent * 1.2;

    // 当两个消失点均为有限值、且两条网格轴代表相互正交的世界方向时，
    // 可由正交性解出焦距。
    if (qAbs(vanishingX.z) > 1e-6 && qAbs(vanishingY.z) > 1e-6) {
        QPointF vx;
        QPointF vy;
        if (toImagePoint(vanishingX, &vx) && toImagePoint(vanishingY, &vy))
            focalLength = focalFromOrthogonalVanishingPoints(vx, vy, backgroundSize);
    }

    const CameraFrame frame{focalLength, cx, cy};
    Vec3 directionX = vanishingDirection(vanishingX, frame);
    Vec3 directionY = vanishingDirection(vanishingY, frame);
    if (!normalize(&directionX) || !normalize(&directionY))
        return false;
    Vec3 normal = cross(directionX, directionY);
    if (!normalize(&normal))
        return false;

    // 把 3D 法线经内参矩阵 K 投影回图像。这就是与源平面垂直的所有
    // 平面共享的第三个消失点。
    const Vec3 projected = projectDirection(normal, frame);
    QPointF projectedDirection;
    if (qAbs(projected.z) > 1e-6) {
        // 齐次分量 w 非零：第三个消失点是有限点。
        QPointF perpendicularVanishingPoint;
        if (!toImagePoint(projected, &perpendicularVanishingPoint))
            return false;
        projectedDirection = perpendicularVanishingPoint - atPoint;
    } else {
        // 齐次分量 w 为零意味着第三个消失点位于无穷远处。
        projectedDirection = QPointF(projected.x, projected.y);
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
    result.lockedEdges = 1u; // 新平面的第 0 条边就是与源平面共用的边
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

// 把子平面绕共享边做真正的三维旋转，再重新投影回图像。
//
// 以前这里做的是图像平面上的二维旋转：让两个外侧角点绕共享边的端点画圆弧。
// 那是错的——绕三维直线旋转的投影并不是圆周运动，于是夹角数值与几何互相脱节，
// 把夹角调到 0° 时两个平面看上去依然有角度。
//
// 现在的步骤：
//  1. 由子平面自身的两个消失点解出相机内参，并把共享边方向 u、深度方向 v
//     恢复成三维方向（u ⊥ v，因为子平面在世界里是矩形）；
//  2. 由 u、v 重建子平面所在的三维平面，把四个角点反投影到该平面上；
//  3. 用 Rodrigues 公式把外侧角点绕共享边（方向 u）旋转 Δ = 目标角 − 当前角；
//  4. 重新投影回图像。
// 旋转是刚体的，因此 0° 与 180° 时子平面必然与父平面共面。
Plane rotateChildPlane(const Plane &source, int edge, qreal targetAngle,
                       const QSize &backgroundSize)
{
    Plane result = source;
    if (edge < 0 || edge >= 4 || !qIsFinite(targetAngle) || backgroundSize.isEmpty())
        return result;

    const int next = (edge + 1) % 4;
    const int farB = (edge + 2) % 4; // 外侧角点，与共享边的 next 端点相连
    const int farA = (edge + 3) % 4; // 外侧角点，与共享边的 edge 端点相连
    const QPointF seamA = source.corner[edge];
    const QPointF seamB = source.corner[next];

    // 1. 子平面的两个消失点：共享边方向 u 与深度方向 v。
    const Vec3 seamLine = joinLines(imagePoint(seamA), imagePoint(seamB));
    const Vec3 outerLine = joinLines(imagePoint(source.corner[farA]),
                                     imagePoint(source.corner[farB]));
    const Vec3 sideA = joinLines(imagePoint(seamA), imagePoint(source.corner[farA]));
    const Vec3 sideB = joinLines(imagePoint(seamB), imagePoint(source.corner[farB]));
    const Vec3 vanishingU = meetLines(seamLine, outerLine);
    const Vec3 vanishingV = meetLines(sideA, sideB);
    if (dot(vanishingU, vanishingU) < 1e-12 || dot(vanishingV, vanishingV) < 1e-12)
        return result;

    const qreal cx = backgroundSize.width() / 2.0;
    const qreal cy = backgroundSize.height() / 2.0;
    const qreal imageExtent = qMax(backgroundSize.width(), backgroundSize.height());
    qreal focalLength = imageExtent * 1.2;
    QPointF vuImage;
    QPointF vvImage;
    if (toImagePoint(vanishingU, &vuImage) && toImagePoint(vanishingV, &vvImage))
        focalLength = focalFromOrthogonalVanishingPoints(vuImage, vvImage, backgroundSize);
    const CameraFrame frame{focalLength, cx, cy};

    Vec3 axis = vanishingDirection(vanishingU, frame);
    Vec3 depthDirection = vanishingDirection(vanishingV, frame);
    if (!normalize(&axis) || !normalize(&depthDirection))
        return result;
    Vec3 normal = cross(axis, depthDirection);
    if (!normalize(&normal))
        return result; // 两个消失方向重合，无法定义子平面

    // 2. 把角点反投影到平面 normal·X = h 上。h 只决定整体尺度，而透视投影
    //    对整体尺度不敏感，所以直接由共享边端点所在的射线确定它。
    const Vec3 raySeamA = imageRay(seamA, frame);
    const double h = dot(normal, raySeamA);
    if (qAbs(h) < 1e-9)
        return result; // 子平面几乎穿过相机中心
    auto onPlane = [&](const QPointF &p, Vec3 *out) {
        const Vec3 ray = imageRay(p, frame);
        const double denominator = dot(normal, ray);
        if (qAbs(denominator) < 1e-12)
            return false;
        *out = ray * (h / denominator);
        return true;
    };
    Vec3 a;
    Vec3 b;
    Vec3 outerA;
    Vec3 outerB;
    if (!onPlane(seamA, &a) || !onPlane(seamB, &b) ||
        !onPlane(source.corner[farA], &outerA) || !onPlane(source.corner[farB], &outerB))
        return result;
    // 消失点的齐次符号是任意的。把旋转轴统一成“由 seamA 指向 seamB”，
    // 夹角增大的方向才不会随四边形绕向而变。翻转 axis 不影响上面的平面，
    // 因为 h 与 normal 会同时变号，交点位置是它们的比值。
    if (dot(b - a, axis) < 0.0)
        axis = axis * -1.0;

    // 3. 绕共享边旋转 Δ。用完整的 Rodrigues 公式，即使子平面被自由拖动成
    //    一般四边形（深度方向不严格垂直于共享边）也能保持刚体旋转。
    qreal delta = targetAngle - source.relativeAngle;
    while (delta > 180.0)
        delta -= 360.0;
    while (delta <= -180.0)
        delta += 360.0;
    const qreal radians = qDegreesToRadians(delta);
    const double cosine = qCos(radians);
    const double sine = qSin(radians);
    auto rotateAroundSeam = [&](const Vec3 &v) {
        // 取 v × axis 为正方向，使 relativeAngle 减小时子平面朝远离父平面的
        // 一侧倒下：于是 0° 恰好是“完全展开、与父平面共面并向外延展”的状态。
        return v * cosine + cross(v, axis) * sine + axis * (dot(axis, v) * (1.0 - cosine));
    };
    const Vec3 movedA = a + rotateAroundSeam(outerA - a);
    const Vec3 movedB = b + rotateAroundSeam(outerB - b);

    // 4. 重新投影回图像。
    QPointF projectedA;
    QPointF projectedB;
    if (!projectPoint(movedA, frame, &projectedA) || !projectPoint(movedB, frame, &projectedB))
        return result;
    result.corner[farA] = projectedA;
    result.corner[farB] = projectedB;

    // 曲面坐标保持不变：旋转不改变子平面的固有尺寸，纹理应当继续贴合角点。
    result.relativeAngle = std::fmod(targetAngle, 360.0);
    if (qFuzzyIsNull(result.relativeAngle) && targetAngle > 0.0)
        result.relativeAngle = 360.0;
    else if (result.relativeAngle < 0.0)
        result.relativeAngle += 360.0;
    result.angleAdjusted = true;
    if (!isValidPlane(result))
        return source;
    return result;
}

} // namespace PlaneMath
