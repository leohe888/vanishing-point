#include "planeedittool.h"

#include <cmath>
#include <QtMath>

void PlaneEditTool::begin(const Plane &plane, const QPointF &press, int handle, int edge,
                         bool extrude, const QSize &canvasSize, bool rotate, int rotationEdge)
{
    m_start = plane;
    m_press = press;
    m_handle = handle;
    m_edge = edge;
    m_extrude = extrude;
    m_canvasSize = canvasSize;
    m_rotate = rotate;
    m_rotationEdge = rotationEdge;
    if (m_rotate && rotationEdge >= 0 && rotationEdge < 4) {
        const QPointF seamMid = (plane.corner[rotationEdge] +
                                 plane.corner[(rotationEdge + 1) % 4]) / 2.0;
        const QPointF v = press - seamMid;
        m_lastPointerAngle = std::atan2(v.y(), v.x());
        m_accumulatedRotation = 0.0;
    }
}

bool PlaneEditTool::update(const QPointF &point, Plane *result)
{
    if (!result)
        return false;
    Plane candidate = m_start;
    if (m_extrude)
        candidate = PlaneMath::makePerpendicularPlane(m_start, m_edge, point, m_press, m_canvasSize);
    else if (m_rotate) {
        const int edge = m_rotationEdge;
        const QPointF seamMid = (m_start.corner[edge] + m_start.corner[(edge + 1) % 4]) / 2.0;
        const QPointF v = point - seamMid;
        if (QLineF(QPointF(), v).length() < 1e-4)
            return false;
        const qreal currentAngle = std::atan2(v.y(), v.x());
        qreal delta = currentAngle - m_lastPointerAngle;
        while (delta > M_PI)
            delta -= 2.0 * M_PI;
        while (delta < -M_PI)
            delta += 2.0 * M_PI;
        m_accumulatedRotation += qRadiansToDegrees(delta);
        m_lastPointerAngle = currentAngle;
        candidate = PlaneMath::rotateChildPlane(m_start, edge,
                                                 m_start.relativeAngle + m_accumulatedRotation);
    }
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
