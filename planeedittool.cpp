#include "planeedittool.h"

void PlaneEditTool::begin(const Plane &plane, const QPointF &press, int handle, int edge,
                         bool extrude, const QSize &canvasSize)
{
    m_start = plane;
    m_press = press;
    m_handle = handle;
    m_edge = edge;
    m_extrude = extrude;
    m_canvasSize = canvasSize;
}

bool PlaneEditTool::update(const QPointF &point, Plane *result) const
{
    if (!result)
        return false;
    Plane candidate = m_start;
    if (m_extrude)
        candidate = PlaneMath::makePerpendicularPlane(m_start, m_edge, point, m_press, m_canvasSize);
    else if (m_handle >= 0 && m_handle < 4)
        candidate.corner[m_handle] = point;
    else if (m_handle >= 4)
        candidate = PlaneMath::resizePlaneAlongEdge(m_start, m_handle - 4, point, m_press);
    else if (!PlaneMath::movePlaneOnSurface(m_start, point, m_press, &candidate))
        return false;
    if (!PlaneMath::isValidPlane(candidate))
        return false;
    *result = candidate;
    return true;
}
