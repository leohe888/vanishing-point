#include "projectivemapping.h"

#include <QtTest>

// 平面单应变换（ProjectiveMapping）的纯逻辑测试。
// 不依赖任何 GUI，使用 QTEST_APPLESS_MAIN（连 QCoreApplication 都不创建）。
namespace {
// 用相对误差比较两点是否接近（单应除法会引入极小浮点误差）。
bool closeTo(const QPointF &a, const QPointF &b, qreal eps = 1e-4)
{
    return QLineF(a, b).length() <= eps;
}
}

class TestProjectiveMapping : public QObject
{
    Q_OBJECT
private slots:
    // 纯缩放+平移（单位正方形 -> 100x100 矩形）：应精确映射。
    void rectangularMapping_isValidAndExact()
    {
        const QPolygonF domain{QPointF(0, 0), QPointF(1, 0), QPointF(1, 1), QPointF(0, 1)};
        const QPolygonF canvas{QPointF(0, 0), QPointF(100, 0), QPointF(100, 100), QPointF(0, 100)};
        const ProjectiveMapping m(domain, canvas);
        QVERIFY(m.isValid());

        QPointF r;
        QVERIFY(m.toCanvas(QPointF(0, 0), &r));
        QVERIFY(closeTo(r, QPointF(0, 0)));
        QVERIFY(m.toCanvas(QPointF(1, 1), &r));
        QVERIFY(closeTo(r, QPointF(100, 100)));
        QVERIFY(m.toCanvas(QPointF(0.5, 0.5), &r));
        QVERIFY(closeTo(r, QPointF(50, 50)));
    }

    // 真实透视四边形：四个角必须精确落位。
    void perspectiveMapping_mapsCornersExactly()
    {
        const QPolygonF domain{QPointF(0, 0), QPointF(1, 0), QPointF(1, 1), QPointF(0, 1)};
        const QPolygonF canvas{QPointF(10, 10), QPointF(200, 30), QPointF(180, 220), QPointF(20, 190)};
        const ProjectiveMapping m(domain, canvas);
        QVERIFY(m.isValid());

        for (int i = 0; i < 4; ++i) {
            QPointF r;
            QVERIFY2(m.toCanvas(domain[i], &r), qPrintable(QString("corner %1 映射失败").arg(i)));
            QVERIFY2(closeTo(r, canvas[i]), qPrintable(QString("corner %1 落位不精确").arg(i)));
        }
    }

    // toCanvas 与 fromCanvas 应互为逆映射。
    void toCanvas_fromCanvas_areInverse()
    {
        const QPolygonF domain{QPointF(0, 0), QPointF(1, 0), QPointF(1, 1), QPointF(0, 1)};
        const QPolygonF canvas{QPointF(5, 20), QPointF(300, 10), QPointF(260, 210), QPointF(40, 240)};
        const ProjectiveMapping m(domain, canvas);
        QVERIFY(m.isValid());

        const QVector<QPointF> samples{
            QPointF(0.1, 0.2), QPointF(0.5, 0.5), QPointF(0.9, 0.8), QPointF(0.3, 0.7)};
        for (const QPointF &p : samples) {
            QPointF c, back;
            QVERIFY(m.toCanvas(p, &c));
            QVERIFY(m.fromCanvas(c, &back));
            QVERIFY2(closeTo(back, p, 1e-3), qPrintable(QString("点(%1,%2) 往返不一致").arg(p.x()).arg(p.y())));
        }
    }

    // 点数不是 4 时应判为无效。
    void degenerateInput_isInvalid()
    {
        const QPolygonF domain{QPointF(0, 0), QPointF(1, 0), QPointF(1, 1)}; // 只有 3 个点
        const QPolygonF canvas{QPointF(0, 0), QPointF(1, 0), QPointF(1, 1), QPointF(0, 1)};
        QVERIFY(!ProjectiveMapping(domain, canvas).isValid());

        // 四点共线（退化）也应无效。
        const QPolygonF degenerate{QPointF(0, 0), QPointF(1, 0), QPointF(2, 0), QPointF(3, 0)};
        QVERIFY(!ProjectiveMapping(domain, degenerate).isValid());
    }
};

QTEST_APPLESS_MAIN(TestProjectiveMapping)
#include "tst_projectivemapping.moc"
