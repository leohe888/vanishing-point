#include "planemath.h"
#include "canvasdocument.h"
#include "floatingimagemath.h"
#include "imagegeometry.h"
#include "scenerenderer.h"

#include <QPainter>
#include <QPainterPathStroker>
#include <QTest>

using namespace PlaneMath;

// 垂直平面与共享接缝的几何回归测试。
// 核心不变量：相邻平面必须把同一个展开曲面坐标送到同一个画布点，
// 否则跨接缝的浮动图像会在接缝处错位。
class PerpendicularPlaneTests : public QObject
{
    Q_OBJECT
private:
    static constexpr qreal SurfaceW = 500;
    static constexpr qreal SurfaceH = 200;
    // 跨缝图像在展开曲面中的位置：横跨接缝 y=SurfaceH，宽度只占中间一段
    static constexpr qreal ImageX = 150;
    static constexpr qreal ImageY = 150;
    static constexpr qreal ImageW = 200;
    static constexpr qreal ImageH = 100;

    // 地面：角点顺序与 finishPlaneCreation 一致（0 左下、1 右下、2 右上、3 左上）。
    // 展开曲面中 y=0 是近处、y=SurfaceH 是远处，接缝就落在 y=SurfaceH 上。
    static Plane ground()
    {
        Plane plane;
        plane.surfaceGroup = 0;
        plane.corner[0] = QPointF(60, 420);
        plane.corner[1] = QPointF(560, 420);
        plane.corner[2] = QPointF(470, 300);
        plane.corner[3] = QPointF(150, 300);
        plane.surfaceCorner[0] = QPointF(0, 0);
        plane.surfaceCorner[1] = QPointF(SurfaceW, 0);
        plane.surfaceCorner[2] = QPointF(SurfaceW, SurfaceH);
        plane.surfaceCorner[3] = QPointF(0, SurfaceH);
        return plane;
    }

    // 从地面上边（edge=2）向上挤出的墙面。墙面 corner[0..1] 与地面 corner[2..3] 重合。
    static Plane wall(const Plane &floor, const QSize &background)
    {
        const QPointF seamMid = (floor.corner[3] + floor.corner[2]) / 2.0;
        QPointF normal;
        if (!perpendicularDirection(floor, seamMid, background, &normal))
            return Plane();
        if (normal.y() > 0)
            normal = -normal;
        return makePerpendicularPlane(floor, 2, seamMid + normal * 180, seamMid, background);
    }

    // 沿墙面左侧（edge=1，近端是共享端点 corner[1]）向外延长的拖动参数
    static Plane extendLeft(const Plane &face, qreal amount)
    {
        const QPointF leftMid = (face.corner[1] + face.corner[2]) / 2.0;
        const QPointF rightMid = (face.corner[0] + face.corner[3]) / 2.0;
        QPointF axis = leftMid - rightMid;
        const qreal length = QLineF(QPointF(), axis).length();
        if (length < Epsilon)
            return face;
        axis /= length;
        return resizePlaneAlongEdge(face, 1, leftMid + axis * amount, leftMid);
    }

    static FloatingImage spanningImage(const Plane &floor, const Plane &face, int hostFace)
    {
        FloatingImage image;
        image.image = QImage(int(ImageW), int(ImageH), QImage::Format_ARGB32_Premultiplied);
        image.image.fill(QColor(170, 80, 60));
        image.attached = true;
        image.hostFace = hostFace;
        // 位置在展开曲面坐标里跨越接缝 y=SurfaceH
        image.position = QPointF(ImageX, ImageY);
        for (const Plane &p : {floor, face}) {
            Facet facet;
            for (int i = 0; i < 4; ++i) {
                facet.corner[i] = p.corner[i];
                facet.surfaceCorner[i] = p.surfaceCorner[i];
            }
            image.faces.append(facet);
        }
        return image;
    }

    // 接缝上被跨缝图像实际覆盖到的采样点，两端各留 30 的余量避开轮廓边缘
    static QVector<QPointF> seamSamples()
    {
        QVector<QPointF> points;
        for (int i = 0; i <= 7; ++i)
            points.append(QPointF(ImageX + 30 + i / 7.0 * (ImageW - 60), SurfaceH));
        return points;
    }

    static QPointF seamToCanvas(const Plane &floor, const QPointF &seam, bool *ok = nullptr)
    {
        QPointF result;
        const bool valid = surfaceMapping(floor).toCanvas(seam, &result);
        if (ok)
            *ok = valid;
        return result;
    }

private slots:
    void extendingSharedEdgeKeepsUnfoldedMapping()
    {
        const QSize background(1200, 800);
        const Plane floor = ground();
        QVERIFY(isValidPlane(floor));
        const Plane face = wall(floor, background);
        QVERIFY(isValidPlane(face));
        QVERIFY(QLineF(face.corner[0], floor.corner[2]).length() < 0.01);
        QVERIFY(QLineF(face.corner[1], floor.corner[3]).length() < 0.01);

        const Plane extended = extendLeft(face, 90);
        QVERIFY(isValidPlane(extended));

        // 延长必须真的生效：墙面变宽了
        const qreal widthBefore = QLineF(face.corner[1], face.corner[0]).length();
        const qreal widthAfter = QLineF(extended.corner[1], extended.corner[0]).length();
        QVERIFY2(widthAfter > widthBefore + 1.0,
                 qPrintable(QString("width %1 -> %2").arg(widthBefore).arg(widthAfter)));
        // 共享端点 corner[0] 没有被动过，仍然贴着地面
        QVERIFY(QLineF(extended.corner[0], floor.corner[2]).length() < 0.01);

        // 关键不变量：接缝上同一个曲面坐标，地面与墙面投到同一个画布点
        for (int i = 0; i <= 10; ++i) {
            const QPointF seam(i / 10.0 * SurfaceW, SurfaceH);
            QPointF fromFloor, fromWall;
            QVERIFY(surfaceMapping(floor).toCanvas(seam, &fromFloor));
            QVERIFY(surfaceMapping(extended).toCanvas(seam, &fromWall));
            QVERIFY2(QLineF(fromFloor, fromWall).length() < 0.01,
                     qPrintable(QString("seam %1: floor %2,%3 wall %4,%5")
                                    .arg(i).arg(fromFloor.x()).arg(fromFloor.y())
                                    .arg(fromWall.x()).arg(fromWall.y())));
        }
        // 展开映射本身不能被重新标定：延长前后同一曲面坐标落在同一处
        for (int i = 0; i <= 10; ++i) {
            const QPointF seam(i / 10.0 * SurfaceW, SurfaceH);
            QPointF before, after;
            QVERIFY(surfaceMapping(face).toCanvas(seam, &before));
            QVERIFY(surfaceMapping(extended).toCanvas(seam, &after));
            QVERIFY2(QLineF(before, after).length() < 0.01,
                     qPrintable(QString("remapped seam %1 by %2")
                                    .arg(i).arg(QLineF(before, after).length())));
        }
    }

    void extendingFreeEdgeAlsoKeepsUnfoldedMapping()
    {
        const QSize background(1200, 800);
        const Plane floor = ground();
        const Plane face = wall(floor, background);
        QVERIFY(isValidPlane(face));

        // 外侧边（edge=2，两端都不共享）同样要保持展开参数化
        const QPointF topMid = (face.corner[2] + face.corner[3]) / 2.0;
        const QPointF bottomMid = (face.corner[1] + face.corner[0]) / 2.0;
        QPointF axis = topMid - bottomMid;
        axis /= QLineF(QPointF(), axis).length();
        const Plane grown = resizePlaneAlongEdge(face, 2, topMid + axis * 70, topMid);
        QVERIFY(isValidPlane(grown));

        for (int i = 0; i <= 10; ++i) {
            const QPointF seam(i / 10.0 * SurfaceW, SurfaceH);
            QPointF before, after;
            QVERIFY(surfaceMapping(face).toCanvas(seam, &before));
            QVERIFY(surfaceMapping(grown).toCanvas(seam, &after));
            QVERIFY2(QLineF(before, after).length() < 0.01,
                     qPrintable(QString("seam %1 drifted %2")
                                    .arg(i).arg(QLineF(before, after).length())));
        }
    }

    void floatingImageStaysContinuousAcrossExtendedSeam()
    {
        const QSize background(1200, 800);
        const Plane floor = ground();
        const Plane face = wall(floor, background);
        const Plane extended = extendLeft(face, 90);
        QVERIFY(isValidPlane(extended));

        for (int hostFace : {0, 1}) {
            const FloatingImage image = spanningImage(floor, extended, hostFace);
            const QPainterPath outline = SceneRenderer::floatingImageOutline(image);
            QPainterPathStroker stroker;
            stroker.setWidth(1);
            const QPainterPath border = stroker.createStroke(outline);
            // 接缝中段：地面与墙面在这里相接，轮廓不能被撕开
            for (const QPointF &seam : seamSamples()) {
                bool ok = false;
                const QPointF onCanvas = seamToCanvas(floor, seam, &ok);
                QVERIFY(ok);
                QVERIFY2(outline.contains(onCanvas),
                         qPrintable(QString("host %1 seam %2,%3 missing on canvas %4,%5")
                                        .arg(hostFace).arg(seam.x()).arg(seam.y())
                                        .arg(onCanvas.x()).arg(onCanvas.y())));
                QVERIFY2(!border.intersects(QRectF(onCanvas - QPointF(2, 2), QSizeF(4, 4))),
                         qPrintable(QString("host %1 seam %2 torn").arg(hostFace).arg(seam.x())));
            }
        }
    }

    void renderedSpanIsContinuousAcrossExtendedSeam()
    {
        const QSize background(1200, 800);
        const Plane floor = ground();
        const Plane face = wall(floor, background);
        const Plane extended = extendLeft(face, 90);
        QVERIFY(isValidPlane(extended));

        CanvasDocument document;
        QImage canvas(1200, 800, QImage::Format_ARGB32_Premultiplied);
        canvas.fill(Qt::transparent);
        document.setBackground(canvas);
        document.addFloatingImage(spanningImage(floor, extended, 0).image);
        document.setImage(0, spanningImage(floor, extended, 0));
        document.setSelectedImage(-1);

        QImage rendered = canvas.copy();
        QPainter painter(&rendered);
        SceneRenderer(document).render(painter, 1, false);
        painter.end();

        // 沿接缝逐点采样：两侧都必须有内容，且不能出现空隙
        for (const QPointF &seam : seamSamples()) {
            bool ok = false;
            const QPointF onCanvas = seamToCanvas(floor, seam, &ok);
            QVERIFY(ok);
            for (const qreal offset : {-6.0, -3.0, 3.0, 6.0}) {
                const QPoint probe((onCanvas + QPointF(0, offset)).toPoint());
                QVERIFY2(qAlpha(rendered.pixel(probe)) > 0,
                         qPrintable(QString("gap at seam %1 offset %2 (%3,%4)")
                                        .arg(seam.x()).arg(offset).arg(probe.x()).arg(probe.y())));
            }
        }
    }

    // 对照组：证明这个 bug 真实存在。只改 corner 而留下旧的 surfaceCorner
    // 会把整张展开图重新标定，接缝两侧随即错位——这正是用户看到的向左偏移。
    void staleSurfaceCornerWouldBreakTheSeam()
    {
        const QSize background(1200, 800);
        const Plane floor = ground();
        const Plane face = wall(floor, background);
        const Plane extended = extendLeft(face, 90);
        QVERIFY(isValidPlane(extended));

        Plane stale = extended;
        for (int i = 0; i < 4; ++i)
            stale.surfaceCorner[i] = face.surfaceCorner[i];

        qreal worstStale = 0, worstFixed = 0;
        for (const QPointF &seam : seamSamples()) {
            QPointF fromFloor, stalePoint, fixedPoint;
            QVERIFY(surfaceMapping(floor).toCanvas(seam, &fromFloor));
            QVERIFY(surfaceMapping(stale).toCanvas(seam, &stalePoint));
            QVERIFY(surfaceMapping(extended).toCanvas(seam, &fixedPoint));
            worstStale = qMax(worstStale, QLineF(fromFloor, stalePoint).length());
            worstFixed = qMax(worstFixed, QLineF(fromFloor, fixedPoint).length());
        }
        QVERIFY2(worstStale > 5.0,
                 qPrintable(QString("expected a visible seam break, got only %1").arg(worstStale)));
        QVERIFY2(worstFixed < 0.01,
                 qPrintable(QString("seam still off by %1 after the fix").arg(worstFixed)));
    }
};

QTEST_MAIN(PerpendicularPlaneTests)
#include "perpendicularplanetests.moc"
