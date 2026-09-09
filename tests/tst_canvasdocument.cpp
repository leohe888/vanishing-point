#include "canvasdocument.h"

#include <QtTest>

// 文档模型（平面增删改、共用边锁定）的纯逻辑测试。
namespace {
// 组装一个面片：surfaceCorner 与 corner 相同，便于直接按画布坐标思考。
Plane makePlane(const QPointF corner[4])
{
    Plane plane;
    for (int i = 0; i < 4; ++i) {
        plane.corner[i] = corner[i];
        plane.surfaceCorner[i] = corner[i];
    }
    return plane;
}

// 父平面 A：100x100 的正方形，边 2 为 (100,100)->(0,100)。
Plane makeParent()
{
    const QPointF corner[4] = {QPointF(0, 0), QPointF(100, 0), QPointF(100, 100), QPointF(0, 100)};
    return makePlane(corner);
}

// 子平面 B：从 A 的边 2 拖出，共用边是 B 的第 0 条边 (100,100)->(0,100)。
Plane makeChild()
{
    const QPointF corner[4] = {QPointF(100, 100), QPointF(0, 100), QPointF(0, 200), QPointF(100, 200)};
    Plane b = makePlane(corner);
    b.parentPlane = 0;
    b.parentEdge = 2;
    b.lockedEdges = 1u; // 子平面的第 0 条边是共用边
    return b;
}
}

class TestCanvasDocument : public QObject
{
    Q_OBJECT
private slots:
    // 回归：子平面被缩放后（延长邻边会连带改变共用边长度，两端点不再重合），
    // 删除它仍必须解开父平面上的锁定边。早先用"端点几何重合"识别共用边，
    // 这种情况下会失配，导致父平面永久锁死。
    void removeChildPlane_unlocksParentEvenAfterChildWasResized()
    {
        CanvasDocument doc;
        QCOMPARE(doc.appendPlane(makeParent()), 0);
        QCOMPARE(doc.appendPlane(makeChild()), 1);
        doc.lockPlaneEdge(0, 2); // 父平面锁定边 2
        QCOMPARE(doc.planes()[0].lockedEdges, quint8(1u << 2));

        // 模拟延长子平面共用边的邻边：共用边的一端沿边方向移动，
        // 于是子平面的边 0 变成 (100,100)->(30,100)，与父平面的边 2 不再重合。
        Plane resized = doc.planes()[1];
        resized.corner[1] = QPointF(30, 100);
        resized.corner[2] = QPointF(30, 200);
        QVERIFY(doc.setPlane(1, resized));

        doc.removePlane(1); // 删除子平面
        QCOMPARE(doc.planes().size(), 1);
        QCOMPARE(doc.planes()[0].lockedEdges, quint8(0)); // 父平面必须完全解锁
    }

    // 删除父平面时，子平面自己那条共用边（第 0 条边）也要解开，并清空父子引用。
    void removeParentPlane_unlocksChildAndClearsParentLink()
    {
        CanvasDocument doc;
        QCOMPARE(doc.appendPlane(makeParent()), 0);
        QCOMPARE(doc.appendPlane(makeChild()), 1);
        doc.lockPlaneEdge(0, 2);

        doc.removePlane(0); // 删除父平面
        QCOMPARE(doc.planes().size(), 1);
        const Plane &child = doc.planes()[0];
        QCOMPARE(child.lockedEdges, quint8(0));
        QCOMPARE(child.parentPlane, -1);
        QCOMPARE(child.parentEdge, -1);
    }

    // 一个平面有多条边被不同子平面共用时，删除其中一个只应解开对应的那一条。
    void removeOneChild_unlocksOnlyItsOwnEdge()
    {
        CanvasDocument doc;
        QCOMPARE(doc.appendPlane(makeParent()), 0);
        QCOMPARE(doc.appendPlane(makeChild()), 1); // 共用父平面的边 2
        doc.lockPlaneEdge(0, 2);
        doc.lockPlaneEdge(0, 0);
        QCOMPARE(doc.planes()[0].lockedEdges, quint8((1u << 2) | (1u << 0)));

        doc.removePlane(1);
        // 边 2 被解开，边 0 仍锁定
        QCOMPARE(doc.planes()[0].lockedEdges, quint8(1u << 0));
    }
};

QTEST_GUILESS_MAIN(TestCanvasDocument)
#include "tst_canvasdocument.moc"
