#include "clonetool.h"

QPointF CloneTool::originalMarker() const
{
    QPointF marker = m_source;
    if (m_sourceOnPlane)
        m_sourceMapping.toCanvas(m_source, &marker);
    return marker;
}

void CloneTool::resetSource()
{
    cancel();
    m_hasSource = m_hasOffset = false;
}

void CloneTool::setAligned(bool aligned)
{
    m_aligned = aligned;
    if (!m_drawing) {
        m_hasOffset = false;
        m_marker = originalMarker();
    }
}

bool CloneTool::pickSource(const QVector<Plane> &planes, const QPointF &point)
{
    const int index = PlaneMath::planeAt(planes, point);
    m_sourceOnPlane = index >= 0;
    m_source = point;
    if (m_sourceOnPlane) {
        m_sourceMapping = PlaneMath::surfaceMapping(planes[index]);
        if (!m_sourceMapping.fromCanvas(point, &m_source))
            return false;
    }
    m_marker = point;
    m_hasSource = true;
    m_hasOffset = false;
    return true;
}

QRect CloneTool::begin(QImage &layer, const QImage &source, const Plane &target, const QPointF &point)
{
    m_targetMapping = PlaneMath::surfaceMapping(target);
    QPointF position;
    if (!m_hasSource || !m_targetMapping.fromCanvas(point, &position))
        return {};
    if (!m_aligned || !m_hasOffset)
        m_offset = m_source - position;
    m_hasOffset = m_drawing = true;
    return m_engine.beginStroke(layer, source, m_targetMapping.forward(),
                                m_sourceOnPlane ? m_sourceMapping.forward() : QTransform(), m_offset, position);
}

QRect CloneTool::move(QImage &layer, const QPointF &point)
{
    QPointF position;
    if (!m_drawing || !m_targetMapping.fromCanvas(point, &position))
        return {};
    return m_engine.drawStrokeTo(layer, position);
}

void CloneTool::hover(const QVector<Plane> &planes, const QPointF &point)
{
    if (!m_hasSource)
        return;
    m_marker = originalMarker();
    if (!m_hasOffset || (!m_drawing && !m_aligned))
        return;
    if (!m_drawing) {
        const int index = PlaneMath::planeAt(planes, point);
        if (index >= 0)
            m_targetMapping = PlaneMath::surfaceMapping(planes[index]);
    }
    QPointF position;
    if (!m_targetMapping.fromCanvas(point, &position))
        return;
    const QPointF source = position + m_offset;
    if (m_sourceOnPlane)
        m_sourceMapping.toCanvas(source, &m_marker);
    else
        m_marker = source;
}

void CloneTool::end(const QVector<Plane> &planes, const QPointF &point)
{
    m_engine.endStroke();
    m_drawing = false;
    if (!m_aligned)
        m_hasOffset = false;
    hover(planes, point);
}

void CloneTool::cancel()
{
    m_engine.endStroke();
    m_drawing = false;
    if (!m_aligned)
        m_hasOffset = false;
    m_marker = originalMarker();
}
