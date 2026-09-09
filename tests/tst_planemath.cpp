#include "planemath.h"

#include <QtMath>
#include <QtTest>

#include <cmath>

// 平面几何纯函数（PlaneMath）的测试。这些函数不持有状态，最适合独立测试。

using namespace PlaneMath;

namespace {
// 构造一个 100x100 的正方形面片（surfaceCorner 与 corner 相同）。
Facet makeSquareFacet()
{
    Facet f;
    f.corner[0] = QPointF(0, 0);
    f.corner[1] = QPointF(100, 0);
    f.corner[2] = QPointF(100, 100);
    f.corner[3] = QPointF(0, 100);
    f.surfaceCorner[0] = QPointF(0, 0);
    f.surfaceCorner[1] = QPointF(100, 0);
    f.surfaceCorner[2] = QPointF(100, 100);
    f.surfaceCorner[3] = QPointF(0, 100);
    return f;
}

bool closeTo(const QPointF &a, const QPointF &b, qreal eps = 1e-4)
{
    return QLineF(a, b).length() <= eps;
}

// —— 合成相机 ——
// rotateChildPlane 做的是“绕共享边的三维旋转再投影”，而投影依赖相机内参。
// 与其去猜内参，不如反过来：自己摆一台内参已知的针孔相机，把世界里的矩形
// 投影成图像四边形，于是任意夹角下的正确答案都是可以直接算出来的。
struct V3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};
V3 vadd(const V3 &a, const V3 &b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 vsub(const V3 &a, const V3 &b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 vmul(const V3 &a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double vdot(const V3 &a, const V3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 vcross(const V3 &a, const V3 &b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
V3 vnorm(const V3 &v) { return vmul(v, 1.0 / std::sqrt(vdot(v, v))); }
// 去掉 a 在单位向量 u 方向上的分量后归一化（Gram-Schmidt 正交化）
V3 vorthogonal(const V3 &a, const V3 &u) { return vnorm(vsub(a, vmul(u, vdot(a, u)))); }

// 位于原点、朝 +Z 看的针孔相机
struct TestCamera {
    double focal = 1200.0;
    double cx = 800.0;
    double cy = 600.0;
    QPointF project(const V3 &p) const
    {
        return QPointF(cx + focal * p.x / p.z, cy + focal * p.y / p.z);
    }
};

// 世界中的一堵“墙”：以 origin 为角，沿 U 方向宽、沿 W 方向高，法向为 normal。
// 子平面从 v = 0 那条边（即 U 方向）翻出；angle = 90° 表示垂直，
// angle = 0° 表示与墙共面并朝墙外延展，180° 表示折回到墙面上。
struct TestWall {
    V3 origin{0.0, -1.2, 7.0};
    V3 U = vnorm(V3{1.0, 0.0, 0.3});
    V3 W = vorthogonal(V3{0.2, 1.0, -0.1}, U); // 平面内、垂直于 U
    V3 normal = vnorm(vcross(U, W));           // 墙面的法向
    double width = 3.0;
    double height = 2.0;
    double depth = 1.5;                        // 子平面的进深

    V3 at(double u, double v) const { return vadd(origin, vadd(vmul(U, u), vmul(W, v))); }
    V3 childDirection(double angle) const
    {
        const double radians = qDegreesToRadians(angle);
        return vadd(vmul(W, -qCos(radians)), vmul(normal, qSin(radians)));
    }
    V3 childAt(double u, double t, double angle) const
    {
        return vadd(at(u, 0.0), vmul(childDirection(angle), t));
    }
};
}

class TestPlanemath : public QObject
{
    Q_OBJECT
private slots:
    void planePolygon_returnsFourPointsInOrder()
    {
        const Facet f = makeSquareFacet();
        const QPolygonF p = planePolygon(f.corner);
        QCOMPARE(p.size(), 4);
        QCOMPARE(p[0], f.corner[0]);
        QCOMPARE(p[1], f.corner[1]);
        QCOMPARE(p[2], f.corner[2]);
        QCOMPARE(p[3], f.corner[3]);
    }

    void handles_returnsCornersThenEdgeMidpoints()
    {
        const Facet f = makeSquareFacet();
        const QVector<QPointF> hs = handles(f);
        QCOMPARE(hs.size(), 8);
        // 前 4 个是角点
        for (int i = 0; i < 4; ++i)
            QCOMPARE(hs[i], f.corner[i]);
        // 后 4 个是边中点
        QVERIFY(closeTo(hs[4], QPointF(50, 0)));
        QVERIFY(closeTo(hs[5], QPointF(100, 50)));
        QVERIFY(closeTo(hs[6], QPointF(50, 100)));
        QVERIFY(closeTo(hs[7], QPointF(0, 50)));
    }

    void distanceToSegment_pointOnLineIsZero()
    {
        QCOMPARE(distanceToSegment(QPointF(50, 0), QPointF(0, 0), QPointF(100, 0)), 0.0);
        // 垂足距离
        QCOMPARE(distanceToSegment(QPointF(50, 30), QPointF(0, 0), QPointF(100, 0)), 30.0);
        // 垂足在线段外时取最近端点
        QCOMPARE(distanceToSegment(QPointF(150, 0), QPointF(0, 0), QPointF(100, 0)), 50.0);
    }

    void isValidPlane_acceptsConvexRejectsConcave()
    {
        QVERIFY(isValidPlane(makeSquareFacet()));

        // 凹四边形：第 3 个点内凹，绕向符号不一致
        Facet concave;
        concave.corner[0] = QPointF(0, 0);
        concave.corner[1] = QPointF(100, 0);
        concave.corner[2] = QPointF(30, 30);
        concave.corner[3] = QPointF(0, 100);
        QVERIFY(!isValidPlane(concave));

        // 边长过短（< 8 像素）应拒绝
        Facet tiny = makeSquareFacet();
        tiny.corner[1] = QPointF(4, 0);
        QVERIFY(!isValidPlane(tiny));
    }

    void uvMapping_roundTripsThroughUvSpace()
    {
        const Facet f = makeSquareFacet();
        QVERIFY(closeTo(uvToPlane(f, QPointF(0, 0)), f.corner[0]));
        QVERIFY(closeTo(uvToPlane(f, QPointF(1, 1)), f.corner[2]));

        bool ok = false;
        const QPointF uv = planeToUv(f, QPointF(50, 50), &ok);
        QVERIFY(ok);
        QVERIFY(closeTo(uv, QPointF(0.5, 0.5)));
    }

    void surfaceMapping_roundTripsThroughSurfaceSpace()
    {
        const Facet f = makeSquareFacet();
        bool ok = false;
        const QPointF s = planeToSurface(f, QPointF(100, 100), &ok);
        QVERIFY(ok);
        QVERIFY(closeTo(s, QPointF(100, 100)));
    }

    void planeAt_prefersTopmostPlane()
    {
        QVector<Plane> planes;
        Plane a, b;
        a.corner[0] = QPointF(0, 0);
        a.corner[1] = QPointF(100, 0);
        a.corner[2] = QPointF(100, 100);
        a.corner[3] = QPointF(0, 100);
        b = a; // 完全重叠的两个平面
        planes.append(a);
        planes.append(b);
        // 后创建的 b 优先命中
        QCOMPARE(planeAt(planes, QPointF(50, 50)), 1);
        // 平面外无命中
        QCOMPARE(planeAt(planes, QPointF(200, 200)), -1);
    }

    void handleAt_and_edgeAt_findNearest()
    {
        const Facet f = makeSquareFacet();
        QCOMPARE(handleAt(f, QPointF(0, 0), 5.0), 0);
        QCOMPARE(handleAt(f, QPointF(50, 0), 5.0), 4);   // 边中点
        QCOMPARE(handleAt(f, QPointF(50, 50), 5.0), -1); // 中心无控制点
        QCOMPARE(edgeAt(f, QPointF(50, 0), 5.0), 0);     // 靠近边 0-1
        QCOMPARE(edgeAt(f, QPointF(50, 50), 5.0), -1);
    }

    // —— 夹角调整：必须是真正的三维旋转，而不是图像上的二维圆周运动 ——

    void rotateChildPlane_matchesTruePerspectiveRotation()
    {
        const TestCamera camera;
        const TestWall wall;
        const QSize background(1600, 1200);

        // 直接用世界坐标构造任意夹角下的子平面，作为标准答案
        auto buildChild = [&](double angle) {
            Plane p;
            p.corner[0] = camera.project(wall.at(0.0, 0.0));
            p.corner[1] = camera.project(wall.at(wall.width, 0.0));
            p.corner[2] = camera.project(wall.childAt(wall.width, wall.depth, angle));
            p.corner[3] = camera.project(wall.childAt(0.0, wall.depth, angle));
            p.parentPlane = 0;
            p.parentEdge = 0;
            p.lockedEdges = 1u;
            p.relativeAngle = angle;
            return p;
        };

        const Plane start = buildChild(90.0);
        QVERIFY(isValidPlane(start));
        for (const double target : {0.0, 30.0, 60.0, 120.0, 150.0, 180.0}) {
            const Plane rotated = rotateChildPlane(start, 0, target, background);
            const Plane expected = buildChild(target);
            QVERIFY2(closeTo(rotated.corner[0], expected.corner[0], 0.05),
                     "共享边端点必须保持不动");
            QVERIFY2(closeTo(rotated.corner[1], expected.corner[1], 0.05),
                     "共享边端点必须保持不动");
            QVERIFY2(closeTo(rotated.corner[2], expected.corner[2], 0.05),
                     qPrintable(QString("夹角 %1° 的外侧角点 2 应为 %2,%3，实际 %4,%5")
                                    .arg(target).arg(expected.corner[2].x())
                                    .arg(expected.corner[2].y())
                                    .arg(rotated.corner[2].x())
                                    .arg(rotated.corner[2].y())));
            QVERIFY2(closeTo(rotated.corner[3], expected.corner[3], 0.05),
                     qPrintable(QString("夹角 %1° 的外侧角点 3 应为 %2,%3，实际 %4,%5")
                                    .arg(target).arg(expected.corner[3].x())
                                    .arg(expected.corner[3].y())
                                    .arg(rotated.corner[3].x())
                                    .arg(rotated.corner[3].y())));
            QCOMPARE(rotated.relativeAngle, target);
        }
    }

    void rotateChildPlane_zeroDegreeUnfoldsFlatOutsideParent()
    {
        const TestCamera camera;
        const TestWall wall;
        const QSize background(1600, 1200);

        // 父平面四角：与子平面共用 corner[0]→corner[1] 那条边
        QPolygonF parentPolygon;
        parentPolygon << camera.project(wall.at(0.0, 0.0))
                      << camera.project(wall.at(wall.width, wall.height))
                      << camera.project(wall.at(0.0, wall.height));

        auto outerMidpoint = [&](const Plane &p) {
            return (p.corner[2] + p.corner[3]) / 2.0;
        };

        Plane child;
        child.corner[0] = camera.project(wall.at(0.0, 0.0));
        child.corner[1] = camera.project(wall.at(wall.width, 0.0));
        child.corner[2] = camera.project(wall.childAt(wall.width, wall.depth, 90.0));
        child.corner[3] = camera.project(wall.childAt(0.0, wall.depth, 90.0));
        child.parentPlane = 0;
        child.parentEdge = 0;
        child.lockedEdges = 1u;
        child.relativeAngle = 90.0;

        // 0°：完全展开，与父平面共面，落在父平面之外
        const Plane flat = rotateChildPlane(child, 0, 0.0, background);
        QVERIFY(isValidPlane(flat));
        QVERIFY2(!parentPolygon.containsPoint(outerMidpoint(flat), Qt::OddEvenFill),
                 "0° 时子平面应当展开到父平面外侧，而不是盖在父平面上");

        // 180°：折回父平面之内
        const Plane folded = rotateChildPlane(child, 0, 180.0, background);
        QVERIFY(isValidPlane(folded));
        QVERIFY2(parentPolygon.containsPoint(outerMidpoint(folded), Qt::OddEvenFill),
                 "180° 时子平面应当折回到父平面之上");
    }

    // 真实入口：Ctrl+拖边造出来的垂直子平面，调到 0° 后必须真的与父平面共面。
    // 判据是两个平面的“进深消失点”重合（同一个消失点 = 同一个世界方向族）。
    void rotateChildPlane_flattensPlaneBuiltByMakePerpendicular()
    {
        const TestCamera camera;
        const TestWall wall;
        const QSize background(1600, 1200);

        Plane parent;
        parent.corner[0] = camera.project(wall.at(0.0, 0.0));
        parent.corner[1] = camera.project(wall.at(wall.width, 0.0));
        parent.corner[2] = camera.project(wall.at(wall.width, wall.height));
        parent.corner[3] = camera.project(wall.at(0.0, wall.height));
        for (int i = 0; i < 4; ++i)
            parent.surfaceCorner[i] = parent.corner[i];
        parent.surfaceGroup = 0;

        // 两侧边交点 = 进深方向的消失点（齐次，已归一化，符号无关）
        auto depthVanishing = [](const Plane &p) {
            const V3 left = vcross(V3{p.corner[0].x(), p.corner[0].y(), 1.0},
                                   V3{p.corner[3].x(), p.corner[3].y(), 1.0});
            const V3 right = vcross(V3{p.corner[1].x(), p.corner[1].y(), 1.0},
                                    V3{p.corner[2].x(), p.corner[2].y(), 1.0});
            return vnorm(vcross(left, right));
        };
        auto angleBetween = [](const V3 &a, const V3 &b) {
            return std::acos(std::min(1.0, std::fabs(vdot(a, b))));
        };

        const QPointF seamMid = (parent.corner[0] + parent.corner[1]) / 2.0;
        QPointF outward;
        QVERIFY(perpendicularDirection(parent, seamMid, background, &outward));
        const Plane child = makePerpendicularPlane(parent, 0, seamMid + outward * 220.0,
                                                   seamMid, background);
        QVERIFY(isValidPlane(child));

        const V3 parentDepth = depthVanishing(parent);
        // 垂直时两者的进深方向必须不同，否则下面的共面断言就成了空断言
        QVERIFY2(angleBetween(parentDepth, depthVanishing(child)) > 0.01,
                 "垂直子平面的进深消失点不应与父平面重合");

        const Plane flat = rotateChildPlane(child, 0, 0.0, background);
        QVERIFY(isValidPlane(flat));
        QVERIFY2(angleBetween(parentDepth, depthVanishing(flat)) < 1e-6,
                 "0° 时子平面与父平面必须共面（进深消失点应当重合）");
    }
};

QTEST_APPLESS_MAIN(TestPlanemath)
#include "tst_planemath.moc"
