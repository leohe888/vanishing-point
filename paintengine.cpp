#include "paintengine.h"

#include <QLineF>
#include <QPainter>
#include <QPolygonF>
#include <QRadialGradient>
#include <QTransform>
#include <QtMath>

using namespace PlaneMath;

// 在面片透视下于 UV 位置落下一个软边笔触点，返回画布脏矩形。
QRect PaintEngine::applyDab(QPainter &painter, const Facet &facet, const QPointF &uv)
{
    // 把画笔直径换算到归一化 UV 空间：用面片的平均水平边长估计
    // UV -> 画布 的局部尺度。这样同一个笔触点在透视下保持一致的视觉粗细。
    const qreal planeWidth = (QLineF(facet.corner[0], facet.corner[1]).length() +
                              QLineF(facet.corner[3], facet.corner[2]).length()) / 2.0;
    const qreal radiusUv = (m_diameter / 2.0) / qMax(40.0, planeWidth);

    const QPolygonF unit{QPointF(0, 0), QPointF(1, 0), QPointF(1, 1), QPointF(0, 1)};
    QTransform uvToCanvas;
    if (!QTransform::quadToQuad(unit, planePolygon(facet.corner), uvToCanvas))
        return QRect();

    // 软边圆点在 UV 空间用径向渐变定义，随面片单应变换投影到画布。
    // 硬度的含义沿用旧实现：半径 softStart 以内完全不透明，向外平滑淡出。
    QRadialGradient gradient(uv, radiusUv);
    QColor core = m_brushColor;
    core.setAlphaF(m_opacity);
    gradient.setColorAt(0.0, core);
    if (m_hardness < 1.0) {
        gradient.setColorAt(qBound(0.0, m_hardness, 1.0), core);
        QColor edge = m_brushColor;
        edge.setAlphaF(0.0);
        gradient.setColorAt(1.0, edge);
    } else {
        gradient.setColorAt(1.0, core);
    }

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setWorldTransform(uvToCanvas);
    painter.setPen(Qt::NoPen);
    painter.setBrush(gradient);
    painter.drawEllipse(QPointF(uv), radiusUv, radiusUv);
    painter.restore();

    // 返回受影响画布矩形：圆的外接框经单应变换后的包围盒，略微外扩抗锯齿余量。
    const QRectF uvBounds(uv.x() - radiusUv, uv.y() - radiusUv, radiusUv * 2.0, radiusUv * 2.0);
    return uvToCanvas.mapRect(uvBounds).toAlignedRect().adjusted(-1, -1, 1, 1);
}

// 从给定 UV 位置开始一笔，并立即落下第一个笔触点。
QRect PaintEngine::beginStroke(QImage &paintLayer, const Facet &facet, const QPointF &uv)
{
    m_lastUv = uv;
    QPainter painter(&paintLayer);
    return applyDab(painter, facet, uv);
}

// 从上一 UV 位置向目标 UV 插值补间，沿笔迹均匀落下一串笔触点。
QRect PaintEngine::drawStrokeTo(QImage &paintLayer, const Facet &facet, const QPointF &uv)
{
    const qreal planeWidth = (QLineF(facet.corner[0], facet.corner[1]).length() +
                              QLineF(facet.corner[3], facet.corner[2]).length()) / 2.0;
    const qreal radiusUv = (m_diameter / 2.0) / qMax(40.0, planeWidth);
    // 步长约为笔刷半径的 1/3，保证快速拖动时笔迹连续无断点。
    const qreal step = qMax(0.001, radiusUv * 0.35);
    const qreal distance = QLineF(m_lastUv, uv).length();
    const int count = qMax(1, int(qCeil(distance / step)));

    QPainter painter(&paintLayer);
    QRect dirty;
    for (int i = 1; i <= count; ++i) {
        const QPointF uvi = m_lastUv + (uv - m_lastUv) * (qreal(i) / qreal(count));
        const QRect r = applyDab(painter, facet, uvi);
        dirty = dirty.isEmpty() ? r : dirty.united(r);
    }
    m_lastUv = uv;
    return dirty;
}
