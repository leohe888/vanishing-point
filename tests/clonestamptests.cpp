#include "clonestampengine.h"
#include "perspectivecanvas.h"
#include "mainwindow.h"
#include "scenerenderer.h"

#include <QApplication>
#include <QCheckBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPathStroker>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

class CloneStampTests : public QObject
{
    Q_OBJECT
private:
    static QImage gradient()
    {
        QImage image(400, 400, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < 400; ++y)
            for (int x = 0; x < 400; ++x)
                image.setPixel(x, y, qRgb(x / 2, y / 2, 80));
        return image;
    }
    static QPoint widgetPoint(QPointF p) { return (p * 1.17 + QPointF(16, 16)).toPoint(); }
    static void mouse(PerspectiveCanvas &canvas, QEvent::Type type, QPointF p,
                      Qt::MouseButton button, Qt::MouseButtons buttons,
                      Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        QMouseEvent event(type, widgetPoint(p), widgetPoint(p), button, buttons, modifiers);
        QApplication::sendEvent(&canvas, &event);
    }
    static void click(PerspectiveCanvas &canvas, QPointF p, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
    {
        mouse(canvas, QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton, modifiers);
        mouse(canvas, QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton, modifiers);
    }
    static bool greenAt(PerspectiveCanvas &canvas, QPointF p)
    {
        const QColor color = canvas.grab().toImage().pixelColor(widgetPoint(p));
        return color.green() > 220 && color.red() < 100 && color.blue() < 160;
    }

private slots:
    void floatingImageOutlineAcrossPlanes()
    {
        FloatingImage image;
        image.image = QImage(240, 60, QImage::Format_ARGB32_Premultiplied);
        image.image.fill(QColor(170, 80, 60));
        image.position = QPointF(30, 20);
        QCOMPARE(SceneRenderer::floatingImageOutline(image).boundingRect(), QRectF(30, 20, 240, 60));
        image.attached = true;
        image.hostFace = 0;
        const QVector<QPolygonF> corners{
            {QPointF(20, 30), QPointF(120, 30), QPointF(120, 130), QPointF(20, 130)},
            {QPointF(120, 30), QPointF(200, 50), QPointF(190, 120), QPointF(120, 130)},
            {QPointF(200, 50), QPointF(260, 20), QPointF(250, 150), QPointF(190, 120)}
        };
        for (int i = 0; i < corners.size(); ++i) {
            Facet face;
            const QPolygonF surface{QPointF(i * 100, 0), QPointF((i + 1) * 100, 0),
                                    QPointF((i + 1) * 100, 100), QPointF(i * 100, 100)};
            for (int j = 0; j < 4; ++j) {
                face.corner[j] = corners[i][j];
                face.surfaceCorner[j] = surface[j];
            }
            image.faces.append(face);
        }
        const QPainterPath outline = SceneRenderer::floatingImageOutline(image);
        QPainterPathStroker stroker;
        stroker.setWidth(1);
        const QPainterPath border = stroker.createStroke(outline);
        for (const QPointF &seam : {QPointF(120, 80), QPointF(195, 85)}) {
            QVERIFY2(outline.contains(seam), qPrintable(QString("seam %1,%2; outline %3 elements")
                .arg(seam.x()).arg(seam.y()).arg(outline.elementCount())));
            QVERIFY(!border.intersects(QRectF(seam - QPointF(2, 2), QSizeF(4, 4))));
        }

        CanvasDocument document;
        QImage background(320, 200, QImage::Format_ARGB32_Premultiplied);
        background.fill(Qt::transparent);
        document.setBackground(background);
        document.images().append(image);
        document.setSelectedImage(0);
        auto render = [&](bool guides, qreal phase) {
            QImage result = background.copy();
            QPainter painter(&result);
            SceneRenderer(document).render(painter, 1, guides, {}, nullptr, false, -1, phase);
            painter.end();
            return result;
        };
        const QImage content = render(false, 0);
        // 用实际渲染的像素验证三个平面上的外轮廓，边缘抗锯齿区域除外。
        for (int y = 0; y < content.height(); y += 3)
            for (int x = 0; x < content.width(); x += 3) {
                const QPointF point(x + .5, y + .5);
                if (!border.contains(point))
                    QCOMPARE(qAlpha(content.pixel(x, y)) > 0, outline.contains(point));
            }
        QVERIFY(render(true, 0) != render(true, 3));
        QCOMPARE(render(false, 0), render(false, 3));
        document.setSelectedImage(-1);
        QCOMPARE(render(true, 0), render(true, 3));

        // 宿主平面外的图片仍有轮廓；切换宿主后接缝也不能变成边框。
        image.position = QPointF(-20, 20);
        QVERIFY(SceneRenderer::floatingImageOutline(image).contains(QPointF(5, 80)));
        image.position = QPointF(30, 20);
        image.hostFace = 1;
        QVERIFY(!stroker.createStroke(SceneRenderer::floatingImageOutline(image)).contains(QPointF(120, 80)));
        image.faces.resize(1);
        image.hostFace = 0;
        QCOMPARE(SceneRenderer::floatingImageOutline(image).boundingRect(), QRectF(50, 50, 240, 60));
    }

    void perspectiveSampling()
    {
        CloneStampEngine engine;
        engine.setDiameter(60);
        engine.setHardness(100);
        const QImage source = gradient();
        QImage layer(source.size(), source.format());
        layer.fill(Qt::transparent);
        QTransform target(1, 0, .001, 0, 1, .002, 20, 10, 1);
        QTransform sourceTransform(1, 0, .0005, 0, 1, .001, 5, 7, 1);
        const QPointF center(150, 150), offset(-60, -30);
        QVERIFY(!engine.beginStroke(layer, source, target, sourceTransform, offset, center).isEmpty());
        const QPoint pixel = target.map(center).toPoint();
        const QPointF expected = sourceTransform.map(target.inverted().map(QPointF(pixel) + QPointF(.5, .5)) + offset);
        const QColor actual = layer.pixelColor(pixel);
        QVERIFY(qAbs(actual.red() - expected.x() / 2) < 2);
        QVERIFY(qAbs(actual.green() - expected.y() / 2) < 2);
        QCOMPARE(actual.alpha(), 255);
        QCOMPARE(layer.pixelColor(0, 0).alpha(), 0);
    }

    void maskAndSnapshot()
    {
        CloneStampEngine engine;
        engine.setDiameter(40);
        engine.setHardness(0);
        engine.setOpacity(50);
        QImage source(200, 200, QImage::Format_ARGB32_Premultiplied);
        source.fill(Qt::red);
        QImage layer(source.size(), source.format());
        layer.fill(Qt::transparent);
        engine.beginStroke(layer, source, {}, {}, {}, QPointF(50.5, 50.5));
        QCOMPARE(layer.pixelColor(50, 50).alpha(), 128);
        QVERIFY(layer.pixelColor(65, 50).alpha() < 40);
        QCOMPARE(layer.pixelColor(71, 50).alpha(), 0);
        source.fill(Qt::blue);
        engine.drawStrokeTo(layer, QPointF(100.5, 50.5));
        QCOMPARE(layer.pixelColor(100, 50).red(), 255);
        QCOMPARE(layer.pixelColor(100, 50).blue(), 0);
        engine.endStroke();
        layer.fill(Qt::transparent);
        QVERIFY(engine.beginStroke(layer, source, {}, {}, QPointF(-1000, 0), QPointF(50, 50)).isEmpty());
    }

    void alignmentHistoryAndExport()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QImage background = gradient();
        QVERIFY(background.save(dir.filePath("source.png")));
        PerspectiveCanvas canvas;
        canvas.resize(500, 500);
        canvas.show();
        QVERIFY(canvas.loadImage(dir.filePath("source.png")));
        for (QPointF p : {QPointF(40, 40), QPointF(360, 40), QPointF(360, 360), QPointF(40, 360)})
            click(canvas, p);
        canvas.setTool(PerspectiveCanvas::CloneStampTool);
        canvas.setBrushHardness(100);
        click(canvas, QPointF(100, 100), Qt::AltModifier);
        QVERIFY(greenAt(canvas, QPointF(100, 100)));
        click(canvas, QPointF(200, 200));
        mouse(canvas, QEvent::MouseMove, QPointF(240, 220), Qt::NoButton, Qt::NoButton);
        QVERIFY(greenAt(canvas, QPointF(140, 120)));
        QVERIFY(canvas.saveResult(dir.filePath("painted.png")));
        QImage painted(dir.filePath("painted.png"));
        QVERIFY(qAbs(painted.pixelColor(200, 200).red() - 50) < 2);
        canvas.undo();
        QVERIFY(canvas.saveResult(dir.filePath("undo.png")));
        QCOMPARE(QImage(dir.filePath("undo.png")).pixelColor(200, 200), background.pixelColor(200, 200));
        canvas.redo();
        QVERIFY(canvas.saveResult(dir.filePath("redo.png")));
        QCOMPARE(QImage(dir.filePath("redo.png")).pixelColor(200, 200), painted.pixelColor(200, 200));
        click(canvas, QPointF(240, 220));
        QVERIFY(canvas.saveResult(dir.filePath("aligned.png")));
        const QColor alignedPixel = QImage(dir.filePath("aligned.png")).pixelColor(240, 220);
        QVERIFY(qAbs(alignedPixel.red() - 70) < 2);
        QVERIFY(qAbs(alignedPixel.green() - 60) < 2);
        canvas.setCloneAligned(false);
        click(canvas, QPointF(100, 100), Qt::AltModifier);
        mouse(canvas, QEvent::MouseButtonPress, QPointF(250, 250), Qt::LeftButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseMove, QPointF(270, 250), Qt::NoButton, Qt::LeftButton);
        QVERIFY(greenAt(canvas, QPointF(120, 100)));
        mouse(canvas, QEvent::MouseButtonRelease, QPointF(270, 250), Qt::LeftButton, Qt::NoButton);
        QVERIFY(greenAt(canvas, QPointF(100, 100)));
        mouse(canvas, QEvent::MouseMove, QPointF(290, 270), Qt::NoButton, Qt::NoButton);
        QVERIFY(greenAt(canvas, QPointF(100, 100)));
        click(canvas, QPointF(300, 300));
        QVERIFY(canvas.saveResult(dir.filePath("unaligned.png")));
        QVERIFY(qAbs(QImage(dir.filePath("unaligned.png")).pixelColor(300, 300).red() - 50) < 2);
        QVERIFY(canvas.loadImage(dir.filePath("source.png")));
        QVERIFY(!greenAt(canvas, QPointF(100, 100)));
    }

    void shortcutAndOptions()
    {
        QTemporaryDir dir;
        QVERIFY(gradient().save(dir.filePath("source.png")));
        MainWindow window;
        window.show();
        QVERIFY(window.findChild<PerspectiveCanvas *>()->loadImage(dir.filePath("source.png")));
        window.activateWindow();
        QTest::qWait(30);
        QTest::keyClick(&window, Qt::Key_S);
        bool selected = false;
        for (auto *button : window.findChildren<QToolButton *>())
            if (button->text().contains("(S)"))
                selected = button->isChecked();
        QVERIFY(selected);
        QVERIFY(window.findChild<QCheckBox *>()->isVisible());
        QVERIFY(window.findChild<QCheckBox *>()->isChecked());
    }
};

QTEST_MAIN(CloneStampTests)
#include "clonestamptests.moc"
