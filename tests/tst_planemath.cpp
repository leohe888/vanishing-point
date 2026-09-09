#include "planemath.h"

#include <QtTest>

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
};

QTEST_APPLESS_MAIN(TestPlanemath)
#include "tst_planemath.moc"
