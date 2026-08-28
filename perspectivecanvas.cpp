#include "perspectivecanvas.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QResizeEvent>
#include <QTransform>
#include <QtMath>

namespace {
constexpr int TextureSize = 1024;
constexpr qreal Epsilon = 1e-6;

QPolygonF planePolygon(const QPointF corner[4])
{
    return QPolygonF{corner[0], corner[1], corner[2], corner[3]};
}

QColor over(const QColor &bottom, const QColor &top)
{
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
    setMinimumSize(500, 400);

    m_background = QImage(1200, 800, QImage::Format_ARGB32);
    QPainter p(&m_background);
    p.fillRect(m_background.rect(), QColor("#26292d"));
    for (int y = 0; y < m_background.height(); y += 40) {
        for (int x = 0; x < m_background.width(); x += 40) {
            if (((x / 40) + (y / 40)) % 2 == 0)
                p.fillRect(x, y, 40, 40, QColor("#2d3035"));
        }
    }
    p.setPen(QColor("#69717b"));
    QFont f = p.font();
    f.setPointSize(20);
    p.setFont(f);
    p.drawText(m_background.rect(), Qt::AlignCenter,
               tr("打开一张图像，或直接在此画布上创建透视平面"));
    updateViewTransform();
}

bool PerspectiveCanvas::loadImage(const QString &fileName)
{
    QImage image(fileName);
    if (image.isNull())
        return false;
    m_background = image.convertToFormat(QImage::Format_ARGB32);
    m_planes.clear();
    m_creationPoints.clear();
    m_selectedPlane = -1;
    m_hasCloneSource = false;
    updateViewTransform();
    update();
    emit statusMessage(tr("图像已打开。请依次点击四个点创建透视平面。"), 5000);
    return true;
}

bool PerspectiveCanvas::saveResult(const QString &fileName) const
{
    QImage result(m_background.size(), QImage::Format_ARGB32);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    renderScene(painter, false);
    painter.end();
    return result.save(fileName);
}

void PerspectiveCanvas::clearPainting()
{
    for (Plane &plane : m_planes)
        plane.paint.fill(Qt::transparent);
    update();
    emit statusMessage(tr("已清除所有平面上的绘画内容"), 3000);
}

void PerspectiveCanvas::setTool(Tool tool)
{
    m_tool = tool;
    m_creationPoints.clear();
    m_dragging = m_drawing = false;
    setCursor(tool == EditPlane ? Qt::SizeAllCursor : Qt::CrossCursor);
    const QString messages[] = {
        tr("依次单击四个角点以创建平面"),
        tr("拖动控制点或平面；按住 Ctrl 从边缘拖出垂直平面"),
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
    painter.drawImage(QPointF(0, 0), m_background);
    for (const Plane &plane : m_planes)
        renderProjectedImage(painter, plane, plane.paint);

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

    painter.setPen(QPen(QColor(65, 182, 235, selected ? 145 : 80), 0.8 / m_scale));
    constexpr int divisions = 8;
    for (int i = 1; i < divisions; ++i) {
        const qreal t = qreal(i) / divisions;
        painter.drawLine(uvToPlane(plane, QPointF(t, 0)), uvToPlane(plane, QPointF(t, 1)));
        painter.drawLine(uvToPlane(plane, QPointF(0, t)), uvToPlane(plane, QPointF(1, t)));
    }

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
    const QPointF point = toImage(event->position());
    if (!m_background.rect().contains(point.toPoint()))
        return;

    if (m_tool == CreatePlane) {
        m_creationPoints.append(point);
        if (m_creationPoints.size() == 4) {
            Plane plane;
            for (int i = 0; i < 4; ++i)
                plane.corner[i] = m_creationPoints[i];
            plane.paint = QImage(TextureSize, TextureSize, QImage::Format_ARGB32);
            plane.paint.fill(Qt::transparent);
            plane.name = tr("平面 %1").arg(m_planes.size() + 1);
            m_planes.append(plane);
            m_selectedPlane = m_planes.size() - 1;
            m_creationPoints.clear();
            emit statusMessage(tr("平面已创建。可切换到编辑、图章或画笔工具。"), 4000);
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
    update();
}

void PerspectiveCanvas::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF point = toImage(event->position());
    if (m_tool == EditPlane && m_dragging && m_selectedPlane >= 0) {
        Plane &plane = m_planes[m_selectedPlane];
        if (m_extruding) {
            m_extrudePreview = makePerpendicularPlane(m_dragStartPlane, m_dragEdge, point);
            m_hasExtrudePreview = true;
        } else if (m_dragHandle >= 0 && m_dragHandle < 4) {
            plane.corner[m_dragHandle] = point;
        } else if (m_dragHandle >= 4) {
            const int edge = m_dragHandle - 4;
            const QPointF a = m_dragStartPlane.corner[edge];
            const QPointF b = m_dragStartPlane.corner[(edge + 1) % 4];
            QPointF normal(-(b - a).y(), (b - a).x());
            const qreal length = qSqrt(QPointF::dotProduct(normal, normal));
            if (length > Epsilon)
                normal /= length;
            const QPointF delta = normal * QPointF::dotProduct(point - m_pressImagePoint, normal);
            plane.corner[edge] = a + delta;
            plane.corner[(edge + 1) % 4] = b + delta;
        } else {
            const QPointF delta = point - m_pressImagePoint;
            for (int i = 0; i < 4; ++i)
                plane.corner[i] = m_dragStartPlane.corner[i] + delta;
        }
        m_lastImagePoint = point;
        update();
        return;
    }
    if (m_drawing && (event->buttons() & Qt::LeftButton)) {
        drawStrokeTo(point, m_tool == StampTool);
        update();
    }
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
            emit statusMessage(tr("已创建相邻的垂直平面"), 3000);
        }
    }
    m_dragging = m_drawing = m_extruding = false;
    m_hasExtrudePreview = false;
    m_cloneStrokeStarted = false;
    m_dragHandle = m_dragEdge = -1;
    update();
}

PerspectiveCanvas::Plane PerspectiveCanvas::makePerpendicularPlane(const Plane &source, int edge,
                                                                    const QPointF &dragPoint) const
{
    Plane result;
    const QPointF a = source.corner[edge];
    const QPointF b = source.corner[(edge + 1) % 4];
    QPointF normal(-(b - a).y(), (b - a).x());
    const qreal length = qSqrt(QPointF::dotProduct(normal, normal));
    if (length > Epsilon)
        normal /= length;
    if (QPointF::dotProduct(dragPoint - (a + b) / 2.0, normal) < 0)
        normal = -normal;
    const qreal depth = qMax(2.0, qAbs(QPointF::dotProduct(dragPoint - (a + b) / 2.0, normal)));
    const QPointF delta = normal * depth;
    result.corner[0] = a;
    result.corner[1] = b;
    result.corner[2] = b + delta;
    result.corner[3] = a + delta;
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
    if (event->key() == Qt::Key_Escape) {
        m_creationPoints.clear();
        m_dragging = m_drawing = m_extruding = false;
        m_hasExtrudePreview = false;
        update();
    } else if (event->key() == Qt::Key_Delete && m_tool == EditPlane && m_selectedPlane >= 0) {
        m_planes.removeAt(m_selectedPlane);
        m_selectedPlane = qMin(m_selectedPlane, m_planes.size() - 1);
        update();
    } else {
        QWidget::keyPressEvent(event);
    }
}
