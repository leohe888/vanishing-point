#include "canvasdocument.h"
#include "imagegeometry.h"
#include "scenecontentcache.h"
#include "perspectivecanvas.h"
#include "imagetransformtool.h"

#include <QPainter>
#include <QTest>
#include <QTemporaryDir>
#include <QMouseEvent>

class ArchitectureTests : public QObject
{
    Q_OBJECT
private:
    static Plane plane()
    {
        Plane result;
        const QPolygonF corners{QPointF(80, 80), QPointF(300, 80), QPointF(340, 320), QPointF(40, 320)};
        const QPolygonF surface{QPointF(0, 0), QPointF(200, 0), QPointF(200, 200), QPointF(0, 200)};
        for (int i = 0; i < 4; ++i) {
            result.corner[i] = corners[i];
            result.surfaceCorner[i] = surface[i];
        }
        return result;
    }
    static QImage background()
    {
        QImage result(400, 400, QImage::Format_ARGB32);
        result.fill(Qt::white);
        return result;
    }
    static void mouse(PerspectiveCanvas &canvas, QEvent::Type type, QPointF point,
                      Qt::MouseButton button, Qt::MouseButtons buttons,
                      Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        const QPointF local = point * 1.17 + QPointF(16, 16);
        QMouseEvent event(type, local, local, button, buttons, modifiers);
        QApplication::sendEvent(&canvas, &event);
    }

private slots:
    void extrusionUsesReleasePosition()
    {
        QTemporaryDir dir;
        QVERIFY(background().save(dir.filePath("background.png")));
        PerspectiveCanvas canvas;
        canvas.resize(500, 500);
        canvas.show();
        QVERIFY(canvas.loadImage(dir.filePath("background.png")));
        const Plane source = plane();
        for (const QPointF &point : PlaneMath::planePolygon(source.corner)) {
            mouse(canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
            mouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
        }
        canvas.setTool(PerspectiveCanvas::EditPlane);
        const QPointF press = (source.corner[0] + source.corner[1]) / 2;
        QPointF direction;
        QVERIFY(PlaneMath::perpendicularDirection(source, press, QSize(400, 400), &direction));
        mouse(canvas, QEvent::MouseButtonPress, press, Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
        // 中间没有 mouseMove：释放事件本身也必须更新构造结果。
        mouse(canvas, QEvent::MouseButtonRelease, press + direction * 60, Qt::LeftButton, Qt::NoButton);
        canvas.undo();
        QVERIFY(canvas.hasSelectedPlane()); // 仅撤销新建的面，原面仍在
        mouse(canvas, QEvent::MouseButtonPress, press, Qt::LeftButton, Qt::LeftButton, Qt::ControlModifier);
        mouse(canvas, QEvent::MouseMove, press + direction * 60, Qt::NoButton, Qt::LeftButton);
        // 最后又回到共享边释放，不能提交之前缓存的有效预览。
        mouse(canvas, QEvent::MouseButtonRelease, press, Qt::LeftButton, Qt::NoButton);
        canvas.undo();
        QVERIFY(!canvas.hasSelectedPlane());
    }

    void mappingUsesReferenceHalfPlane()
    {
        const auto mapping = PlaneMath::surfaceMapping(plane());
        QVERIFY(mapping.isValid());
        const QPointF point(40, 150);
        QPointF canvas, recovered;
        QVERIFY(mapping.toCanvas(point, &canvas));
        QVERIFY(mapping.fromCanvas(canvas, &recovered));
        QVERIFY(QLineF(point, recovered).length() < .001);
        const QTransform t = mapping.forward();
        const QTransform negative(-t.m11(), -t.m12(), -t.m13(), -t.m21(), -t.m22(), -t.m23(), -t.m31(), -t.m32(), -t.m33());
        QPointF equivalent;
        QVERIFY(ProjectiveMapping::mapVisible(negative, point, QPointF(100, 100), &equivalent));
        QVERIFY(QLineF(canvas, equivalent).length() < .001);
        QVERIFY(!ProjectiveMapping::mapVisible(t, QPointF(qQNaN(), 0), QPointF(100, 100), &equivalent));
        QVERIFY(!ProjectiveMapping().toCanvas(point, &equivalent));
    }

    void transactionCancelCommitAndRedoBranch()
    {
        CanvasDocument doc;
        doc.setBackground(background());
        doc.appendPlane(plane());
        doc.resetHistory();
        const QImage before = doc.paintLayer();
        doc.beginEdit();
        doc.beginPaintTransaction();
        Plane changed = doc.planes()[0];
        changed.corner[0] += QPointF(20, 0);
        QVERIFY(doc.setPlane(0, changed));
        doc.paintLayer().setPixelColor(30, 40, Qt::red);
        doc.addPaintDirty(QRect(30, 40, 1, 1));
        doc.cancelEdit();
        QCOMPARE(doc.planes()[0].corner[0], plane().corner[0]);
        QCOMPARE(doc.paintLayer(), before);
        QVERIFY(!doc.canUndo());
        doc.beginEdit();
        doc.beginPaintTransaction();
        doc.paintLayer().setPixelColor(30, 40, Qt::red);
        doc.addPaintDirty(QRect(30, 40, 1, 1));
        doc.commitEdit(true);
        QVERIFY(doc.undo());
        QCOMPARE(doc.paintLayer(), before);
        QVERIFY(doc.canRedo());
        doc.beginEdit();
        changed = doc.planes()[0];
        changed.corner[0] += QPointF(10, 0);
        QVERIFY(doc.setPlane(0, changed));
        doc.cancelEdit();
        QVERIFY(doc.canRedo());
        QVERIFY(doc.redo());
        QCOMPARE(doc.paintLayer().pixelColor(30, 40), QColor(Qt::red));
        QVERIFY(!doc.redo());
        doc.beginEdit();
        doc.commitEdit(false);
        QVERIFY(doc.undo());
        QVERIFY(!doc.canUndo());
    }

    void sharedGeometryCacheAndHitTesting()
    {
        FloatingImage image;
        image.image = QImage(100, 80, QImage::Format_ARGB32);
        image.image.fill(Qt::red);
        image.position = QPointF(100, 100);
        image.rotation = 37;
        const auto first = ImageGeometry::get(image);
        QCOMPARE(ImageGeometry::get(image).data(), first.data());
        QCOMPARE(first->controls().size(), 8);
        const QPointF inside = FloatingImageMath::imageToSpace(image).map(QPointF(20, 30));
        QPointF offset;
        QVERIFY(first->hitTest(inside, &offset));
        QVERIFY(first->outline().contains(inside));
        QVERIFY(QLineF(offset, inside - image.position).length() < .001);
        image.scale = QPointF(2, 2);
        QVERIFY(ImageGeometry::get(image).data() != first.data());
        image.scale = QPointF(1, 1);
        QCOMPARE(ImageGeometry::get(image).data(), first.data());
        image.attached = true;
        image.faces = {plane()};
        image.hostFace = 0;
        const auto attached = ImageGeometry::get(image);
        QVERIFY(attached.data() != first.data());
        const QPointF projected = FloatingImageMath::toCanvas(image, inside);
        QVERIFY(attached->hitTest(projected));
        image.faces[0].corner[0] += QPointF(3, 0);
        QVERIFY(ImageGeometry::get(image).data() != attached.data());
    }

    void contentCacheInvalidation()
    {
        CanvasDocument doc;
        doc.setBackground(background());
        SceneContentCache cache;
        const QImage original = cache.get(doc, QSize(400, 400), 1, 1, {}).toImage();
        QCOMPARE(cache.renderCount(), 1);
        cache.get(doc, QSize(400, 400), 1, 1, {});
        QCOMPARE(cache.renderCount(), 1);
        doc.setSelectedPlane(-1);
        cache.get(doc, QSize(400, 400), 1, 1, {});
        QCOMPARE(cache.renderCount(), 1);
        doc.beginEdit();
        doc.beginPaintTransaction();
        doc.paintLayer().setPixelColor(10, 10, Qt::red);
        doc.addPaintDirty(QRect(10, 10, 1, 1));
        const QImage painted = cache.get(doc, QSize(400, 400), 1, 1, {}).toImage();
        QVERIFY(painted != original);
        doc.cancelEdit();
        QCOMPARE(cache.get(doc, QSize(400, 400), 1, 1, {}).toImage(), original);
        const int renders = cache.renderCount();
        cache.get(doc, QSize(200, 200), 2, .5, {});
        QCOMPARE(cache.renderCount(), renders + 1);
        QImage sprite(20, 20, QImage::Format_ARGB32);
        sprite.fill(Qt::red);
        const int index = doc.addFloatingImage(sprite);
        const QImage withImage = cache.get(doc, QSize(400, 400), 1, 1, {}).toImage();
        QCOMPARE(withImage.pixelColor(5, 5), QColor(Qt::red));
        const int imageRenders = cache.renderCount();
        doc.setSelectedImage(-1);
        QCOMPARE(cache.get(doc, QSize(400, 400), 1, 1, {}).toImage(), withImage);
        QCOMPARE(cache.renderCount(), imageRenders);
        doc.beginEdit();
        FloatingImage moved = doc.image(index);
        moved.position = QPointF(100, 100);
        QVERIFY(doc.setImage(index, moved));
        doc.commitEdit(true);
        const QImage movedPixels = cache.get(doc, QSize(400, 400), 1, 1, {}).toImage();
        QCOMPARE(movedPixels.pixelColor(105, 105), QColor(Qt::red));
        QVERIFY(doc.undo());
        QCOMPARE(cache.get(doc, QSize(400, 400), 1, 1, {}).toImage(), withImage);
    }

    void escapeRestoresPlaneAndBrush()
    {
        QTemporaryDir dir;
        QVERIFY(background().save(dir.filePath("background.png")));
        PerspectiveCanvas canvas;
        canvas.resize(500, 500);
        canvas.show();
        QVERIFY(canvas.loadImage(dir.filePath("background.png")));
        for (const QPointF &point : PlaneMath::planePolygon(plane().corner)) {
            mouse(canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
            mouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
        }
        canvas.setTool(PerspectiveCanvas::EditPlane);
        const QImage before = canvas.grab().toImage();
        mouse(canvas, QEvent::MouseButtonPress, QPointF(150, 150), Qt::LeftButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseMove, QPointF(180, 200), Qt::NoButton, Qt::LeftButton);
        QVERIFY(canvas.grab().toImage() != before);
        QTest::keyClick(&canvas, Qt::Key_Escape);
        QCOMPARE(canvas.grab().toImage(), before);
        canvas.setTool(PerspectiveCanvas::BrushTool);
        mouse(canvas, QEvent::MouseButtonPress, QPointF(150, 150), Qt::LeftButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseMove, QPointF(180, 200), Qt::NoButton, Qt::LeftButton);
        QTest::keyClick(&canvas, Qt::Key_Escape);
        QVERIFY(canvas.saveResult(dir.filePath("result.png")));
        QCOMPARE(QImage(dir.filePath("result.png")), background());
    }
};

QTEST_MAIN(ArchitectureTests)
#include "architecturetests.moc"
