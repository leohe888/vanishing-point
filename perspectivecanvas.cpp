#include "perspectivecanvas.h"

#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QImageReader>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QResizeEvent>
#include <QTransform>
#include <QVector3D>
#include <QUrl>
#include <QtMath>

namespace {
// Texture resolution is fixed so drawing quality is independent of the source
// image size and of the current canvas zoom level.
constexpr int TextureSize = 1024;
// Keep history bounded because a snapshot can contain several large textures.
constexpr int MaxHistoryStates = 40;
constexpr qreal Epsilon = 1e-6;

QPolygonF planePolygon(const QPointF corner[4])
{
    // The corner order is preserved: callers must provide clockwise or
    // counter-clockwise points, never a crossed polygon.
    return QPolygonF{corner[0], corner[1], corner[2], corner[3]};
}

QColor over(const QColor &bottom, const QColor &top)
{
    // QColor's composition helpers are convenient for QPainter, but this
    // manual source-over operation is needed for per-pixel texture painting.
    const qreal a = top.alphaF();
    const qreal outA = a + bottom.alphaF() * (1.0 - a);
    if (outA < Epsilon)
        return Qt::transparent;
    QColor result;
    result.setRgbF((top.redF() * a + bottom.redF() * bottom.alphaF() * (1.0 - a)) / outA,
                   (top.greenF() * a + bottom.greenF() * bottom.alphaF() * (1.0 - a)) / outA,
                   (top.blueF() * a + bottom.blueF() * bottom.alphaF() * (1.0 - a)) / outA,
                   outA);
    return result;
}
}

PerspectiveCanvas::PerspectiveCanvas(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAcceptDrops(true);
    setMinimumSize(500, 400);

    m_background = QImage(1200, 800, QImage::Format_ARGB32);
    QPainter p(&m_background);
    p.fillRect(m_background.rect(), QColor("#26292d"));
    updateViewTransform();
    resetHistory();
}

bool PerspectiveCanvas::loadImage(const QString &fileName)
{
    QImage image(fileName);
    if (image.isNull())
        return false;
    m_background = image.convertToFormat(QImage::Format_ARGB32);
    m_hasLoadedImage = true;
    emit documentAvailabilityChanged(true);
    m_planes.clear();
    m_pastedImage = QImage();
    m_pastedImagePosition = QPointF();
    m_pastedImageAttached = false;
    m_pastedSurfaceGroup = -1;
    m_pastedHostPlane = -1;
    m_creationPoints.clear();
    m_selectedPlane = -1;
    m_hasCloneSource = false;
    resetHistory();
    updateViewTransform();
    update();
    emit statusMessage(tr("图像已打开。请依次点击四个点创建透视平面。"), 5000);
    return true;
}

bool PerspectiveCanvas::saveResult(const QString &fileName) const
{
    QImage result(m_background.size(), QImage::Format_ARGB32);
    result.fill(Qt::white);
    QPainter painter(&result);
    renderScene(painter, false);
    painter.end();
    return result.save(fileName);
}

void PerspectiveCanvas::dragEnterEvent(QDragEnterEvent *event)
{
    // A dropped image has two meanings: before a document exists it becomes
    // the background image; afterwards it becomes the movable floating image.
    // Accept only local image files so arbitrary URLs are not swallowed.
    if (!event->mimeData()->hasUrls())
        return;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile() && !QImageReader::imageFormat(url.toLocalFile()).isEmpty()) {
            event->acceptProposedAction();
            return;
        }
    }
}

void PerspectiveCanvas::dropEvent(QDropEvent *event)
{
    if (!event->mimeData()->hasUrls())
        return;

    // With no document loaded, the first valid dropped file opens the image
    // directly. loadImage() also resets planes, history and view transform.
    if (!m_hasLoadedImage) {
        for (const QUrl &url : event->mimeData()->urls()) {
            if (!url.isLocalFile() || QImageReader::imageFormat(url.toLocalFile()).isEmpty())
                continue;
            if (loadImage(url.toLocalFile()))
                event->acceptProposedAction();
            else
                emit statusMessage(tr("无法打开拖入的图像"), 3500);
            return;
        }
        return;
    }

    for (const QUrl &url : event->mimeData()->urls()) {
        if (!url.isLocalFile() || QImageReader::imageFormat(url.toLocalFile()).isEmpty())
            continue;
        const QImage image(url.toLocalFile());
        if (image.isNull()) {
            emit statusMessage(tr("无法读取拖入的图像"), 3500);
            return;
        }
        setFloatingImage(image, tr("图像已放到画布左上角，可拖入透视平面"));
        event->acceptProposedAction();
        return;
    }
}

void PerspectiveCanvas::clearPainting()
{
    if (m_planes.isEmpty())
        return;
    for (Plane &plane : m_planes)
        plane.paint.fill(Qt::transparent);
    commitHistory();
    update();
    emit statusMessage(tr("已清除所有平面上的绘画内容"), 3000);
}

PerspectiveCanvas::CanvasState PerspectiveCanvas::captureState() const
{
    return CanvasState{m_planes, m_selectedPlane, m_pastedImage, m_pastedImagePosition,
                       m_pastedImageAttached, m_pastedSurfaceGroup, m_pastedHostPlane};
}

void PerspectiveCanvas::pasteClipboardImage()
{
    // QClipboard performs the platform-specific conversion from common image
    // formats (PNG, BMP, etc.) to QImage.
    const QClipboard *clipboard = QApplication::clipboard();
    const QImage image = clipboard ? clipboard->image() : QImage();
    if (image.isNull()) {
        emit statusMessage(tr("剪切板中没有可粘贴的图像"), 2500);
        return;
    }
    if (!m_hasLoadedImage) {
        emit statusMessage(tr("请先打开一张图片，再粘贴图像"), 3000);
        return;
    }

    setFloatingImage(image, tr("图像已粘贴到画布左上角"));
}

void PerspectiveCanvas::setFloatingImage(const QImage &image, const QString &statusText)
{
    m_pastedImage = image.convertToFormat(QImage::Format_ARGB32);
    m_pastedImagePosition = QPointF(0, 0);
    m_pastedImageAttached = false;
    m_pastedSurfaceGroup = -1;
    m_pastedHostPlane = -1;
    commitHistory();
    update();
    emit statusMessage(statusText, 3000);
}

void PerspectiveCanvas::rotateFloatingImage()
{
    if (m_pastedImage.isNull()) {
        emit statusMessage(tr("请先粘贴或拖入一张浮动图像"), 2500);
        return;
    }
    // Rotate the bitmap itself, preserving its current top-left position in
    // either canvas coordinates or the shared unfolded surface coordinates.
    m_pastedImage = m_pastedImage.transformed(QTransform().rotate(90),
                                               Qt::SmoothTransformation);
    commitHistory();
    update();
    emit statusMessage(tr("浮动图像已顺时针旋转 90°"), 2200);
}

void PerspectiveCanvas::flipFloatingImageHorizontal()
{
    if (m_pastedImage.isNull()) {
        emit statusMessage(tr("请先粘贴或拖入一张浮动图像"), 2500);
        return;
    }
    m_pastedImage = m_pastedImage.mirrored(true, false);
    commitHistory();
    update();
    emit statusMessage(tr("浮动图像已水平翻转"), 2200);
}

void PerspectiveCanvas::flipFloatingImageVertical()
{
    if (m_pastedImage.isNull()) {
        emit statusMessage(tr("请先粘贴或拖入一张浮动图像"), 2500);
        return;
    }
    m_pastedImage = m_pastedImage.mirrored(false, true);
    commitHistory();
    update();
    emit statusMessage(tr("浮动图像已垂直翻转"), 2200);
}

void PerspectiveCanvas::restoreState(const CanvasState &state)
{
    m_planes = state.planes;
    m_selectedPlane = state.selectedPlane;
    m_pastedImage = state.pastedImage;
    m_pastedImagePosition = state.pastedImagePosition;
    m_pastedImageAttached = state.pastedImageAttached;
    m_pastedSurfaceGroup = state.pastedSurfaceGroup;
    m_pastedHostPlane = state.pastedHostPlane;
    m_creationPoints.clear();
    m_dragging = m_drawing = m_extruding = false;
    m_draggingPastedImage = false;
    m_hasExtrudePreview = false;
    m_stateChanged = false;
    update();
}

void PerspectiveCanvas::resetHistory()
{
    m_history.clear();
    m_history.append(captureState());
    m_historyIndex = 0;
    m_stateChanged = false;
    emit canUndoChanged(false);
    emit canRedoChanged(false);
}

void PerspectiveCanvas::commitHistory()
{
    while (m_history.size() > m_historyIndex + 1)
        m_history.removeLast();
    m_history.append(captureState());
    ++m_historyIndex;
    if (m_history.size() > MaxHistoryStates) {
        m_history.removeFirst();
        --m_historyIndex;
    }
    m_stateChanged = false;
    emit canUndoChanged(m_historyIndex > 0);
    emit canRedoChanged(false);
}

void PerspectiveCanvas::undo()
{
    if (m_historyIndex <= 0)
        return;
    restoreState(m_history[--m_historyIndex]);
    emit canUndoChanged(m_historyIndex > 0);
    emit canRedoChanged(true);
    emit statusMessage(tr("已撤销"), 1800);
}

void PerspectiveCanvas::redo()
{
    if (m_historyIndex + 1 >= m_history.size())
        return;
    restoreState(m_history[++m_historyIndex]);
    emit canUndoChanged(true);
    emit canRedoChanged(m_historyIndex + 1 < m_history.size());
    emit statusMessage(tr("已重做"), 1800);
}

void PerspectiveCanvas::setTool(Tool tool)
{
    m_tool = tool;
    m_creationPoints.clear();
    m_dragging = m_drawing = false;
    m_draggingPastedImage = false;
    setCursor(tool == EditPlane ? Qt::SizeAllCursor : Qt::CrossCursor);
    const QString messages[] = {
        tr("依次单击四个角点以创建平面"),
        tr("拖动控制点或平面；按住 Ctrl 从边缘拖出垂直于当前平面的平面"),
        tr("Alt+单击设置仿制源，然后拖动进行仿制"),
        tr("在平面内拖动进行透视绘画")
    };
    emit statusMessage(messages[tool]);
    update();
}

void PerspectiveCanvas::updateViewTransform()
{
    if (m_background.isNull())
        return;
    const qreal sx = (width() - 32.0) / m_background.width();
    const qreal sy = (height() - 32.0) / m_background.height();
    m_scale = qMin(sx, sy);
    if (m_scale <= 0)
        m_scale = 1.0;
    const QSizeF shown = QSizeF(m_background.size()) * m_scale;
    m_offset = QPointF((width() - shown.width()) / 2.0, (height() - shown.height()) / 2.0);
}

QPointF PerspectiveCanvas::toImage(const QPointF &point) const
{
    return (point - m_offset) / m_scale;
}

QPointF PerspectiveCanvas::toWidget(const QPointF &point) const
{
    return point * m_scale + m_offset;
}

void PerspectiveCanvas::resizeEvent(QResizeEvent *)
{
    updateViewTransform();
}

void PerspectiveCanvas::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor("#191b1e"));
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    painter.translate(m_offset);
    painter.scale(m_scale, m_scale);
    renderScene(painter, true);
}

void PerspectiveCanvas::renderScene(QPainter &painter, bool showGuides) const
{
    // The canvas background is part of the document output and is always
    // rendered. The showGuides flag controls only editor overlays below.
    painter.drawImage(QPointF(0, 0), m_background);
    if (showGuides && !m_hasLoadedImage && m_planes.isEmpty() && m_creationPoints.isEmpty()) {
        painter.save();
        painter.setPen(QColor("#89919b"));
        QFont placeholderFont = painter.font();
        placeholderFont.setPointSize(20);
        painter.setFont(placeholderFont);
        painter.drawText(m_background.rect(), Qt::AlignCenter,
                         tr("请打开一张图片开始操作"));
        painter.restore();
    }
    // Draw each plane's paint layer first. The clipboard image remains a
    // top-level, directly movable layer until it is explicitly removed.
    for (const Plane &plane : m_planes) {
        renderProjectedImage(painter, plane, plane.paint);
    }
    renderPastedImage(painter);

    if (!showGuides)
        return;
    for (int i = 0; i < m_planes.size(); ++i)
        drawPlaneGuides(painter, m_planes[i], i == m_selectedPlane);
    if (m_hasExtrudePreview)
        drawPlaneGuides(painter, m_extrudePreview, true);

    painter.save();
    painter.setPen(QPen(QColor("#4bc3ff"), 2.0 / m_scale));
    painter.setBrush(QColor("#4bc3ff"));
    for (int i = 0; i < m_creationPoints.size(); ++i) {
        const QPointF &point = m_creationPoints[i];
        painter.drawEllipse(point, 4.5 / m_scale, 4.5 / m_scale);
        if (i > 0)
            painter.drawLine(m_creationPoints[i - 1], point);
    }
    painter.restore();
}

void PerspectiveCanvas::renderProjectedImage(QPainter &painter, const Plane &plane,
                                               const QImage &texture) const
{
    if (texture.isNull())
        return;
    const QPolygonF source{QPointF(0, 0), QPointF(texture.width(), 0),
                           QPointF(texture.width(), texture.height()), QPointF(0, texture.height())};
    // quadToQuad() produces the homography that gives every texture pixel its
    // correct perspective position on the four-point plane.
    QTransform projection;
    if (!QTransform::quadToQuad(source, planePolygon(plane.corner), projection))
        return;
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setWorldTransform(projection, true);
    painter.drawImage(QPointF(0, 0), texture);
    painter.restore();
}

QPointF PerspectiveCanvas::uvToPlane(const Plane &plane, const QPointF &uv) const
{
    const QPolygonF unit{QPointF(0, 0), QPointF(1, 0), QPointF(1, 1), QPointF(0, 1)};
    QTransform transform;
    if (!QTransform::quadToQuad(unit, planePolygon(plane.corner), transform))
        return {};
    return transform.map(uv);
}

QPointF PerspectiveCanvas::planeToUv(const Plane &plane, const QPointF &point, bool *ok) const
{
    QTransform transform;
    const QPolygonF unit{QPointF(0, 0), QPointF(1, 0), QPointF(1, 1), QPointF(0, 1)};
    const bool valid = QTransform::quadToQuad(planePolygon(plane.corner), unit, transform);
    if (ok)
        *ok = valid;
    return valid ? transform.map(point) : QPointF();
}

QPointF PerspectiveCanvas::planeToSurface(const Plane &plane, const QPointF &point,
                                           bool *ok) const
{
    QTransform transform;
    const bool valid = QTransform::quadToQuad(planePolygon(plane.corner),
                                               planePolygon(plane.surfaceCorner), transform);
    if (ok)
        *ok = valid;
    return valid ? transform.map(point) : QPointF();
}

void PerspectiveCanvas::renderPastedImage(QPainter &painter) const
{
    if (m_pastedImage.isNull())
        return;
    if (!m_pastedImageAttached) {
        painter.drawImage(m_pastedImagePosition, m_pastedImage);
        return;
    }

    int host = m_pastedHostPlane;
    if (host < 0 || host >= m_planes.size() ||
        m_planes[host].surfaceGroup != m_pastedSurfaceGroup) {
        host = -1;
        for (int i = 0; i < m_planes.size(); ++i) {
            if (m_planes[i].surfaceGroup == m_pastedSurfaceGroup) {
                host = i;
                break;
            }
        }
    }
    if (host < 0) {
        painter.drawImage(m_pastedImagePosition, m_pastedImage);
        return;
    }

    const QRectF imageRect(QPointF(0, 0), QSizeF(m_pastedImage.size()));
    QPainterPath hostClip;
    hostClip.addRect(imageRect);

    auto sourcePolygon = [this](const Plane &plane) {
        QPolygonF polygon;
        for (const QPointF &corner : plane.surfaceCorner)
            polygon << corner - m_pastedImagePosition;
        return polygon;
    };
    auto polygonPath = [](const QPolygonF &polygon) {
        QPainterPath path;
        path.addPolygon(polygon);
        path.closeSubpath();
        return path;
    };

    // Pixels belonging to another face are removed from the host projection,
    // then redrawn with that face's homography. This avoids a doubled image at
    // the seam while allowing the image to extend beyond the finite grid.
    for (int i = 0; i < m_planes.size(); ++i) {
        if (i == host || m_planes[i].surfaceGroup != m_pastedSurfaceGroup)
            continue;
        hostClip = hostClip.subtracted(polygonPath(sourcePolygon(m_planes[i])));
    }

    auto drawFace = [this, &painter, &sourcePolygon](const Plane &plane,
                                                     const QPainterPath &clip) {
        const QPolygonF source = sourcePolygon(plane);
        QTransform projection;
        if (!QTransform::quadToQuad(source, planePolygon(plane.corner), projection))
            return;
        painter.save();
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.setWorldTransform(projection, true);
        // The clip is expressed in source-image coordinates, so install the
        // image-to-canvas transform before giving the path to QPainter.
        painter.setClipPath(clip, Qt::IntersectClip);
        painter.drawImage(QPointF(0, 0), m_pastedImage);
        painter.restore();
    };

    drawFace(m_planes[host], hostClip);
    for (int i = 0; i < m_planes.size(); ++i) {
        if (i == host || m_planes[i].surfaceGroup != m_pastedSurfaceGroup)
            continue;
        drawFace(m_planes[i], polygonPath(sourcePolygon(m_planes[i])));
    }
}

bool PerspectiveCanvas::pastedImageAt(const QPointF &canvasPoint, QPointF *imagePoint,
                                       int *planeIndex) const
{
    if (m_pastedImage.isNull())
        return false;
    const QRectF imageRect(QPointF(0, 0), QSizeF(m_pastedImage.size()));
    if (!m_pastedImageAttached) {
        const QPointF local = canvasPoint - m_pastedImagePosition;
        if (!imageRect.contains(local))
            return false;
        if (imagePoint)
            *imagePoint = local;
        if (planeIndex)
            *planeIndex = -1;
        return true;
    }

    // Prefer the actual face under the pointer so shared edges use the face
    // currently visible on top.
    for (int i = m_planes.size() - 1; i >= 0; --i) {
        const Plane &plane = m_planes[i];
        if (plane.surfaceGroup != m_pastedSurfaceGroup ||
            !planePolygon(plane.corner).containsPoint(canvasPoint, Qt::OddEvenFill))
            continue;
        bool ok = false;
        const QPointF local = planeToSurface(plane, canvasPoint, &ok) -
                              m_pastedImagePosition;
        if (ok && imageRect.contains(local)) {
            if (imagePoint)
                *imagePoint = local;
            if (planeIndex)
                *planeIndex = i;
            return true;
        }
    }

    // The host projection deliberately continues outside its finite grid, so
    // its visible extension must remain draggable too.
    if (m_pastedHostPlane >= 0 && m_pastedHostPlane < m_planes.size()) {
        bool ok = false;
        const QPointF local = planeToSurface(m_planes[m_pastedHostPlane], canvasPoint, &ok) -
                              m_pastedImagePosition;
        if (ok && imageRect.contains(local)) {
            if (imagePoint)
                *imagePoint = local;
            if (planeIndex)
                *planeIndex = m_pastedHostPlane;
            return true;
        }
    }
    return false;
}

QVector<QPointF> PerspectiveCanvas::handles(const Plane &plane) const
{
    return {plane.corner[0], plane.corner[1], plane.corner[2], plane.corner[3],
            (plane.corner[0] + plane.corner[1]) / 2.0,
            (plane.corner[1] + plane.corner[2]) / 2.0,
            (plane.corner[2] + plane.corner[3]) / 2.0,
            (plane.corner[3] + plane.corner[0]) / 2.0};
}

int PerspectiveCanvas::handleAt(const Plane &plane, const QPointF &point) const
{
    const QVector<QPointF> hs = handles(plane);
    const qreal radius = 10.0 / m_scale;
    for (int i = 0; i < hs.size(); ++i) {
        if (QLineF(hs[i], point).length() <= radius)
            return i;
    }
    return -1;
}

qreal PerspectiveCanvas::distanceToSegment(const QPointF &p, const QPointF &a,
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

bool PerspectiveCanvas::isValidPlane(const Plane &plane)
{
    // A projective transform maps the unit square to a simple convex quad.
    // Reject concave, self-intersecting and nearly singular configurations before
    // they reach quadToQuad(), otherwise its pole can pass through the plane.
    qreal windingSign = 0.0;
    qreal twiceArea = 0.0;
    for (int i = 0; i < 4; ++i) {
        const QPointF a = plane.corner[i];
        const QPointF b = plane.corner[(i + 1) % 4];
        const QPointF c = plane.corner[(i + 2) % 4];
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
    if (!QTransform::quadToQuad(unit, planePolygon(plane.corner), transform))
        return false;

    // The homogeneous denominator must keep one sign over the complete unit
    // square. Since it is linear in u/v, checking all corners is sufficient.
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

int PerspectiveCanvas::edgeAt(const Plane &plane, const QPointF &point) const
{
    const qreal tolerance = 9.0 / m_scale;
    for (int i = 0; i < 4; ++i) {
        if (distanceToSegment(point, plane.corner[i], plane.corner[(i + 1) % 4]) <= tolerance)
            return i;
    }
    return -1;
}

int PerspectiveCanvas::planeAt(const QPointF &point) const
{
    for (int i = m_planes.size() - 1; i >= 0; --i) {
        if (planePolygon(m_planes[i].corner).containsPoint(point, Qt::OddEvenFill))
            return i;
    }
    return -1;
}

void PerspectiveCanvas::drawPlaneGuides(QPainter &painter, const Plane &plane, bool selected) const
{
    painter.save();
    const qreal lineWidth = (selected ? 1.7 : 1.0) / m_scale;
    const QColor color = selected ? QColor(52, 195, 255, 230) : QColor(70, 155, 210, 160);
    painter.setPen(QPen(color, lineWidth));
    painter.setBrush(Qt::NoBrush);
    painter.drawPolygon(planePolygon(plane.corner));

    painter.save();
    QPainterPath planeClip;
    planeClip.addPolygon(planePolygon(plane.corner));
    planeClip.closeSubpath();
    painter.setClipPath(planeClip, Qt::IntersectClip);
    painter.setPen(QPen(QColor(65, 182, 235, selected ? 145 : 80), 0.8 / m_scale));
    constexpr int divisions = 8;
    for (int i = 1; i < divisions; ++i) {
        const qreal t = qreal(i) / divisions;
        painter.drawLine(uvToPlane(plane, QPointF(t, 0)), uvToPlane(plane, QPointF(t, 1)));
        painter.drawLine(uvToPlane(plane, QPointF(0, t)), uvToPlane(plane, QPointF(1, t)));
    }
    painter.restore();

    if (selected && m_tool == EditPlane) {
        const QVector<QPointF> hs = handles(plane);
        for (int i = 0; i < hs.size(); ++i) {
            const qreal radius = (i < 4 ? 5.5 : 4.5) / m_scale;
            painter.setPen(QPen(QColor("#0e526e"), 1.0 / m_scale));
            painter.setBrush(i < 4 ? QColor("#f3f8fa") : QColor("#4bc3ff"));
            painter.drawRect(QRectF(hs[i].x() - radius, hs[i].y() - radius,
                                    radius * 2, radius * 2));
        }
    }
    painter.restore();
}

void PerspectiveCanvas::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    setFocus();
    if (!m_hasLoadedImage) {
        emit statusMessage(tr("请先打开一张图片，然后再创建平面或进行编辑。"), 3500);
        return;
    }
    const QPointF point = toImage(event->position());
    if (!m_background.rect().contains(point.toPoint()))
        return;

    QPointF grabbedImagePoint;
    int grabbedPlane = -1;
    if (pastedImageAt(point, &grabbedImagePoint, &grabbedPlane)) {
        m_draggingPastedImage = true;
        m_pastedDragStartPosition = m_pastedImagePosition;
        m_pastedDragStartAttached = m_pastedImageAttached;
        m_pastedDragStartSurfaceGroup = m_pastedSurfaceGroup;
        m_pastedDragStartHostPlane = m_pastedHostPlane;
        m_pastedDragOffset = grabbedImagePoint;
        m_pressImagePoint = point;
        m_stateChanged = false;
        setCursor(Qt::ClosedHandCursor);
        return;
    }

    if (m_tool == CreatePlane) {
        m_creationPoints.append(point);
        if (m_creationPoints.size() == 4) {
            Plane plane;
            for (int i = 0; i < 4; ++i)
                plane.corner[i] = m_creationPoints[i];
            const qreal surfaceWidth = qMax(1.0,
                (QLineF(plane.corner[0], plane.corner[1]).length() +
                 QLineF(plane.corner[3], plane.corner[2]).length()) / 2.0);
            const qreal surfaceHeight = qMax(1.0,
                (QLineF(plane.corner[0], plane.corner[3]).length() +
                 QLineF(plane.corner[1], plane.corner[2]).length()) / 2.0);
            plane.surfaceCorner[0] = QPointF(0, 0);
            plane.surfaceCorner[1] = QPointF(surfaceWidth, 0);
            plane.surfaceCorner[2] = QPointF(surfaceWidth, surfaceHeight);
            plane.surfaceCorner[3] = QPointF(0, surfaceHeight);
            int nextSurfaceGroup = 0;
            for (const Plane &existing : m_planes)
                nextSurfaceGroup = qMax(nextSurfaceGroup, existing.surfaceGroup + 1);
            plane.surfaceGroup = nextSurfaceGroup;
            plane.paint = QImage(TextureSize, TextureSize, QImage::Format_ARGB32);
            plane.paint.fill(Qt::transparent);
            plane.name = tr("平面 %1").arg(m_planes.size() + 1);
            if (isValidPlane(plane)) {
                m_planes.append(plane);
                m_selectedPlane = m_planes.size() - 1;
                commitHistory();
                emit statusMessage(tr("平面已创建，已自动进入编辑工具。"), 4000);
                emit toolChangeRequested(EditPlane);
            } else {
                emit statusMessage(tr("无法创建：四个点必须依次组成非交叉的凸四边形，请重新设置。"), 5000);
            }
            m_creationPoints.clear();
        } else {
            emit statusMessage(tr("已设置 %1/4 个角点").arg(m_creationPoints.size()), 2000);
        }
        update();
        return;
    }

    if (m_tool == EditPlane) {
        int candidate = m_selectedPlane;
        if (candidate >= 0) {
            m_dragHandle = handleAt(m_planes[candidate], point);
            m_dragEdge = edgeAt(m_planes[candidate], point);
        }
        if (m_dragHandle < 0 && candidate >= 0 && m_dragEdge < 0 &&
            !planePolygon(m_planes[candidate].corner).containsPoint(point, Qt::OddEvenFill)) {
            candidate = -1;
        }
        if (candidate < 0) {
            candidate = planeAt(point);
            if (candidate >= 0) {
                m_dragHandle = handleAt(m_planes[candidate], point);
                m_dragEdge = edgeAt(m_planes[candidate], point);
            }
        }
        m_selectedPlane = candidate;
        if (candidate >= 0) {
            m_dragStartPlane = m_planes[candidate];
            m_pressImagePoint = point;
            m_lastImagePoint = point;
            m_dragging = true;
            m_extruding = (event->modifiers() & Qt::ControlModifier) && m_dragEdge >= 0;
            if (m_extruding) {
                m_extrudePreview = makePerpendicularPlane(m_planes[candidate], m_dragEdge, point);
                m_hasExtrudePreview = true;
            }
        }
        update();
        return;
    }

    const int planeIndex = planeAt(point);
    if (planeIndex < 0) {
        emit statusMessage(tr("请在一个透视平面内绘制"), 2500);
        return;
    }
    m_selectedPlane = planeIndex;
    bool ok = false;
    const QPointF uv = planeToUv(m_planes[planeIndex], point, &ok);
    if (!ok)
        return;
    if (m_tool == StampTool && (event->modifiers() & Qt::AltModifier)) {
        m_cloneSourceUv = uv;
        m_cloneSourcePlane = planeIndex;
        m_hasCloneSource = true;
        m_cloneStrokeStarted = false;
        emit statusMessage(tr("仿制源已设置。现在可单击并拖动进行仿制。"), 3500);
        update();
        return;
    }
    if (m_tool == StampTool && !m_hasCloneSource) {
        emit statusMessage(tr("请先按住 Alt 并在平面中单击，以设置仿制源"), 3500);
        return;
    }
    m_drawing = true;
    m_lastImagePoint = point;
    m_lastUv = uv;
    if (m_tool == StampTool) {
        m_cloneAnchorUv = uv;
        m_cloneStrokeStarted = true;
    }
    applyDab(m_planes[planeIndex], uv, m_tool == StampTool);
    m_stateChanged = true;
    update();
}

void PerspectiveCanvas::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF point = toImage(event->position());
    if (m_draggingPastedImage && (event->buttons() & Qt::LeftButton)) {
        const int targetPlane = planeAt(point);
        bool mapped = false;
        if (targetPlane >= 0) {
            bool ok = false;
            const QPointF surfacePoint = planeToSurface(m_planes[targetPlane], point, &ok);
            if (ok) {
                m_pastedImageAttached = true;
                m_pastedSurfaceGroup = m_planes[targetPlane].surfaceGroup;
                m_pastedHostPlane = targetPlane;
                m_pastedImagePosition = surfacePoint - m_pastedDragOffset;
                mapped = true;
            }
        }
        if (!mapped && m_pastedImageAttached && m_pastedHostPlane >= 0 &&
            m_pastedHostPlane < m_planes.size()) {
            bool ok = false;
            const QPointF surfacePoint =
                planeToSurface(m_planes[m_pastedHostPlane], point, &ok);
            if (ok) {
                m_pastedImagePosition = surfacePoint - m_pastedDragOffset;
                mapped = true;
            }
        }
        if (!mapped) {
            m_pastedImageAttached = false;
            m_pastedSurfaceGroup = -1;
            m_pastedHostPlane = -1;
            m_pastedImagePosition = point - m_pastedDragOffset;
        }
        m_stateChanged = m_pastedImagePosition != m_pastedDragStartPosition ||
                         m_pastedImageAttached != m_pastedDragStartAttached ||
                         m_pastedSurfaceGroup != m_pastedDragStartSurfaceGroup ||
                         m_pastedHostPlane != m_pastedDragStartHostPlane;
        update();
        return;
    }
    if (m_tool == EditPlane && m_dragging && m_selectedPlane >= 0) {
        Plane &plane = m_planes[m_selectedPlane];
        if (m_extruding) {
            m_extrudePreview = makePerpendicularPlane(m_dragStartPlane, m_dragEdge,
                                                       point);
            m_hasExtrudePreview = isValidPlane(m_extrudePreview);
        } else if (m_dragHandle >= 0 && m_dragHandle < 4) {
            Plane candidate = m_dragStartPlane;
            candidate.corner[m_dragHandle] = point;
            if (isValidPlane(candidate))
            {
                plane = candidate;
                m_stateChanged = true;
            }
        } else if (m_dragHandle >= 4) {
            const int edge = m_dragHandle - 4;
            const Plane candidate = resizePlaneAlongEdge(m_dragStartPlane, edge, point);
            if (isValidPlane(candidate))
            {
                plane = candidate;
                m_stateChanged = true;
            }
        } else {
            const QPointF delta = point - m_pressImagePoint;
            for (int i = 0; i < 4; ++i)
                plane.corner[i] = m_dragStartPlane.corner[i] + delta;
            m_stateChanged = !delta.isNull();
        }
        m_lastImagePoint = point;
        update();
        return;
    }
    if (m_drawing && (event->buttons() & Qt::LeftButton)) {
        drawStrokeTo(point, m_tool == StampTool);
        update();
        return;
    }
    if (!m_dragging)
        updateHoverCursor(point);
}

void PerspectiveCanvas::updateHoverCursor(const QPointF &imagePoint)
{
    if (pastedImageAt(imagePoint)) {
        setCursor(Qt::OpenHandCursor);
        return;
    }
    if (m_tool != EditPlane) {
        setCursor(Qt::CrossCursor);
        return;
    }
    if (m_selectedPlane < 0 || m_selectedPlane >= m_planes.size()) {
        setCursor(Qt::ArrowCursor);
        return;
    }

    const Plane &plane = m_planes[m_selectedPlane];
    const int handle = handleAt(plane, imagePoint);
    if (handle >= 4) {
        const int edge = handle - 4;
        const QPointF edgeMidpoint = (plane.corner[edge] +
                                      plane.corner[(edge + 1) % 4]) / 2.0;
        const QPointF oppositeMidpoint = (plane.corner[(edge + 2) % 4] +
                                          plane.corner[(edge + 3) % 4]) / 2.0;
        const QPointF axis = edgeMidpoint - oppositeMidpoint;
        const qreal ax = qAbs(axis.x());
        const qreal ay = qAbs(axis.y());

        if (ax < ay * 0.42)
            setCursor(Qt::SizeVerCursor);
        else if (ay < ax * 0.42)
            setCursor(Qt::SizeHorCursor);
        else if (axis.x() * axis.y() >= 0.0)
            setCursor(Qt::SizeFDiagCursor);
        else
            setCursor(Qt::SizeBDiagCursor);
        return;
    }
    if (handle >= 0) {
        setCursor(Qt::CrossCursor);
        return;
    }
    if (planePolygon(plane.corner).containsPoint(imagePoint, Qt::OddEvenFill))
        setCursor(Qt::SizeAllCursor);
    else
        setCursor(Qt::ArrowCursor);
}

void PerspectiveCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    if (m_extruding && m_hasExtrudePreview) {
        const qreal area = qAbs(QPolygonF(planePolygon(m_extrudePreview.corner)).boundingRect().width() *
                                QPolygonF(planePolygon(m_extrudePreview.corner)).boundingRect().height());
        if (area > 100.0) {
            m_extrudePreview.paint = QImage(TextureSize, TextureSize, QImage::Format_ARGB32);
            m_extrudePreview.paint.fill(Qt::transparent);
            m_extrudePreview.name = tr("平面 %1").arg(m_planes.size() + 1);
            m_planes.append(m_extrudePreview);
            m_selectedPlane = m_planes.size() - 1;
            m_stateChanged = true;
            emit statusMessage(tr("已创建相邻的垂直平面"), 3000);
        }
    }
    m_dragging = m_drawing = m_extruding = false;
    m_draggingPastedImage = false;
    m_hasExtrudePreview = false;
    m_cloneStrokeStarted = false;
    m_dragHandle = m_dragEdge = -1;
    if (m_stateChanged)
        commitHistory();
    updateHoverCursor(toImage(event->position()));
    update();
}

PerspectiveCanvas::Plane PerspectiveCanvas::resizePlaneAlongEdge(const Plane &source, int edge,
                                                                  const QPointF &dragPoint) const
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

    // An edge resize has one degree of freedom. Ignore sideways pointer motion
    // and retain only movement along the plane's existing extension axis.
    QPointF extensionAxis = edgeMidpoint - oppositeMidpoint;
    qreal axisLength = QLineF(QPointF(), extensionAxis).length();
    if (axisLength < Epsilon) {
        extensionAxis = QPointF(-(b - a).y(), (b - a).x());
        axisLength = QLineF(QPointF(), extensionAxis).length();
    }
    if (axisLength < Epsilon)
        return result;
    extensionAxis /= axisLength;
    const qreal extension = QPointF::dotProduct(dragPoint - m_pressImagePoint, extensionAxis);
    const QPointF targetPoint = edgeMidpoint + extensionAxis * extension;

    // The resized edge must retain the original edge-direction vanishing point.
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
        // Parallel edges are the limiting case with a vanishing point at infinity.
        resizedEdge = QLineF(targetPoint, targetPoint + (b - a));
    }

    // Each endpoint is constrained to its existing side line. This is what
    // keeps a vertical plane vertical while only changing its height.
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

bool PerspectiveCanvas::perpendicularDirection(const Plane &source, const QPointF &atPoint,
                                                QPointF *direction) const
{
    // Recover the two vanishing points of the source plane in homogeneous
    // image coordinates. Homogeneous form also covers parallel line families.
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

    const qreal cx = m_background.width() / 2.0;
    const qreal cy = m_background.height() / 2.0;
    const qreal imageExtent = qMax(m_background.width(), m_background.height());
    qreal focalLength = imageExtent * 1.2;

    // When both vanishing points are finite and the two grid axes represent
    // orthogonal world directions, their orthogonality determines focal length.
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

    // Project the 3D normal through K. This is the third vanishing point shared
    // by every plane perpendicular to the source plane.
    const qreal projectedX = focalLength * normal.x() + cx * normal.z();
    const qreal projectedY = focalLength * normal.y() + cy * normal.z();
    QPointF projectedDirection;
    if (qAbs(normal.z()) > 1e-6) {
        const QPointF perpendicularVanishingPoint(projectedX / normal.z(),
                                                  projectedY / normal.z());
        projectedDirection = perpendicularVanishingPoint - atPoint;
    } else {
        // A zero homogeneous w means the third vanishing point is at infinity.
        projectedDirection = QPointF(projectedX, projectedY);
    }

    const qreal length = QLineF(QPointF(), projectedDirection).length();
    if (!qIsFinite(length) || length < Epsilon)
        return false;
    *direction = projectedDirection / length;
    return true;
}

PerspectiveCanvas::Plane PerspectiveCanvas::makePerpendicularPlane(const Plane &source, int edge,
                                                                    const QPointF &dragPoint) const
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
    if (!perpendicularDirection(source, midpoint, &perpendicularAtMidpoint))
        return result;

    // The pointer controls only the signed distance along the projected 3D
    // normal. Sideways motion cannot alter the perpendicular plane's angle.
    const qreal amount = QPointF::dotProduct(dragPoint - m_pressImagePoint,
                                             perpendicularAtMidpoint);
    const QPointF targetMidpoint = midpoint + perpendicularAtMidpoint * amount;
    result.corner[0] = a;
    result.corner[1] = b;

    if (qAbs(amount) < 2.0) {
        result.corner[2] = b;
        result.corner[3] = a;
        return result;
    }

    // The shared edge and the new outer edge represent the same direction in
    // 3D, so both meet at the original edge family's vanishing point.
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
            if (!perpendicularDirection(source, a, &perpendicularAtA) ||
                !perpendicularDirection(source, b, &perpendicularAtB))
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

    // Parallel source edges have their vanishing point at infinity, so the
    // outer edge remains parallel while its endpoints still follow the third
    // (perpendicular) vanishing direction.
    if (!constructedWithVanishingPoint) {
        const QLineF outerLine(targetMidpoint, targetMidpoint + (b - a));
        QPointF perpendicularAtA;
        QPointF perpendicularAtB;
        QPointF outerAtA;
        QPointF outerAtB;
        if (perpendicularDirection(source, a, &perpendicularAtA) &&
            perpendicularDirection(source, b, &perpendicularAtB) &&
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

    // Unfold the perpendicular face around the shared edge. Both faces retain
    // identical surface coordinates on the seam, while the new outer edge is
    // placed on the side opposite the source face's interior.
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

void PerspectiveCanvas::drawStrokeTo(const QPointF &point, bool stamp)
{
    if (m_selectedPlane < 0 || m_selectedPlane >= m_planes.size())
        return;
    Plane &plane = m_planes[m_selectedPlane];
    bool ok = false;
    const QPointF uv = planeToUv(plane, point, &ok);
    if (!ok || uv.x() < 0 || uv.x() > 1 || uv.y() < 0 || uv.y() > 1)
        return;
    const qreal planeWidth = (QLineF(plane.corner[0], plane.corner[1]).length() +
                              QLineF(plane.corner[3], plane.corner[2]).length()) / 2.0;
    const qreal textureDiameter = m_diameter * TextureSize / qMax(40.0, planeWidth);
    const qreal step = qMax(1.0, textureDiameter * 0.18) / TextureSize;
    const qreal distance = QLineF(m_lastUv, uv).length();
    const int count = qMax(1, int(qCeil(distance / step)));
    for (int i = 1; i <= count; ++i)
        applyDab(plane, m_lastUv + (uv - m_lastUv) * (qreal(i) / count), stamp);
    m_lastUv = uv;
    m_lastImagePoint = point;
}

void PerspectiveCanvas::applyDab(Plane &plane, const QPointF &uv, bool stamp)
{
    // Brush size is converted from image pixels to the plane's normalized
    // texture so a dab keeps a consistent apparent size under perspective.
    const qreal planeWidth = (QLineF(plane.corner[0], plane.corner[1]).length() +
                              QLineF(plane.corner[3], plane.corner[2]).length()) / 2.0;
    const qreal radius = qBound(1.0, m_diameter * TextureSize /
                                      (2.0 * qMax(40.0, planeWidth)), 300.0);
    const QPointF center(uv.x() * (TextureSize - 1), uv.y() * (TextureSize - 1));
    const int left = qMax(0, int(qFloor(center.x() - radius)));
    const int right = qMin(TextureSize - 1, int(qCeil(center.x() + radius)));
    const int top = qMax(0, int(qFloor(center.y() - radius)));
    const int bottom = qMin(TextureSize - 1, int(qCeil(center.y() + radius)));
    const qreal softStart = qBound(0.0, m_hardness, 1.0);

    for (int y = top; y <= bottom; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(plane.paint.scanLine(y));
        for (int x = left; x <= right; ++x) {
            const qreal d = qSqrt(qPow(x - center.x(), 2) + qPow(y - center.y(), 2)) / radius;
            if (d > 1.0)
                continue;
            qreal falloff = 1.0;
            if (d > softStart)
                falloff = (1.0 - d) / qMax(0.001, 1.0 - softStart);
            falloff = falloff * falloff * (3.0 - 2.0 * falloff);

            QColor source = m_brushColor;
            if (stamp) {
                const QPointF destinationUv(qreal(x) / (TextureSize - 1),
                                            qreal(y) / (TextureSize - 1));
                const QPointF sourceUv = m_cloneSourceUv + (destinationUv - m_cloneAnchorUv);
                if (sourceUv.x() < 0 || sourceUv.x() > 1 || sourceUv.y() < 0 || sourceUv.y() > 1 ||
                    m_cloneSourcePlane < 0 || m_cloneSourcePlane >= m_planes.size())
                    continue;
                const Plane &sourcePlane = m_planes[m_cloneSourcePlane];
                const QPointF imagePoint = uvToPlane(sourcePlane, sourceUv);
                const int ix = qBound(0, qRound(imagePoint.x()), m_background.width() - 1);
                const int iy = qBound(0, qRound(imagePoint.y()), m_background.height() - 1);
                source = QColor::fromRgba(m_background.pixel(ix, iy));
                const int tx = qBound(0, qRound(sourceUv.x() * (TextureSize - 1)), TextureSize - 1);
                const int ty = qBound(0, qRound(sourceUv.y() * (TextureSize - 1)), TextureSize - 1);
                source = over(source, QColor::fromRgba(sourcePlane.paint.pixel(tx, ty)));
            }
            source.setAlphaF(source.alphaF() * m_opacity * falloff);
            line[x] = over(QColor::fromRgba(line[x]), source).rgba();
        }
    }
}

void PerspectiveCanvas::keyPressEvent(QKeyEvent *event)
{
    if (event->matches(QKeySequence::Paste)) {
        pasteClipboardImage();
        event->accept();
    } else if (event->key() == Qt::Key_Escape) {
        if (m_draggingPastedImage) {
            m_pastedImagePosition = m_pastedDragStartPosition;
            m_pastedImageAttached = m_pastedDragStartAttached;
            m_pastedSurfaceGroup = m_pastedDragStartSurfaceGroup;
            m_pastedHostPlane = m_pastedDragStartHostPlane;
        }
        m_creationPoints.clear();
        m_dragging = m_drawing = m_extruding = false;
        m_draggingPastedImage = false;
        m_stateChanged = false;
        m_hasExtrudePreview = false;
        update();
    } else if (event->key() == Qt::Key_Delete && m_tool == EditPlane && m_selectedPlane >= 0) {
        const int removedPlane = m_selectedPlane;
        m_planes.removeAt(removedPlane);
        if (m_pastedHostPlane > removedPlane) {
            --m_pastedHostPlane;
        } else if (m_pastedHostPlane == removedPlane) {
            m_pastedHostPlane = -1;
            for (int i = 0; i < m_planes.size(); ++i) {
                if (m_planes[i].surfaceGroup == m_pastedSurfaceGroup) {
                    m_pastedHostPlane = i;
                    break;
                }
            }
            if (m_pastedHostPlane < 0) {
                m_pastedImageAttached = false;
                m_pastedSurfaceGroup = -1;
                m_pastedImagePosition = QPointF(0, 0);
            }
        }
        m_selectedPlane = qMin(m_selectedPlane, m_planes.size() - 1);
        commitHistory();
        update();
    } else {
        QWidget::keyPressEvent(event);
    }
}

bool PerspectiveCanvas::focusNextPrevChild(bool next)
{
    if (m_tool != EditPlane || m_planes.isEmpty())
        return QWidget::focusNextPrevChild(next);

    if (m_selectedPlane < 0) {
        m_selectedPlane = next ? 0 : m_planes.size() - 1;
    } else if (next) {
        m_selectedPlane = (m_selectedPlane + 1) % m_planes.size();
    } else {
        m_selectedPlane = (m_selectedPlane - 1 + m_planes.size()) % m_planes.size();
    }
    update();
    emit statusMessage(tr("已选择：%1（%2/%3）")
                           .arg(m_planes[m_selectedPlane].name)
                           .arg(m_selectedPlane + 1)
                           .arg(m_planes.size()),
                       2500);
    return true;
}
