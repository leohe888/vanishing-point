#pragma once

#include <QImage>
#include <QPointF>
#include <QTransform>

// 在展开平面中平移采样位置；源与目标分别使用自己的透视变换。
class CloneStampEngine
{
public:
    void setDiameter(int value) { m_diameter = qMax(1, value); }
    void setHardness(int value) { m_hardness = qBound(0, value, 100) / 100.0; }
    void setOpacity(int value) { m_opacity = qBound(0, value, 100) / 100.0; }
    QRect beginStroke(QImage &layer, const QImage &source,
                      const QTransform &targetToCanvas, const QTransform &sourceToCanvas,
                      const QPointF &offset, const QPointF &position);
    QRect drawStrokeTo(QImage &layer, const QPointF &position);
    void endStroke() { m_source = QImage(); }

private:
    QRect applyDab(QImage &layer, const QPointF &position);
    QImage m_source;
    QTransform m_targetToCanvas, m_canvasToTarget, m_sourceToCanvas;
    QPointF m_offset, m_lastPosition;
    qreal m_diameter = 42, m_hardness = .75, m_opacity = 1;
};
