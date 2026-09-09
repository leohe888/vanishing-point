#pragma once

#include "planemath.h"

class PlaneEditTool
{
public:
    void begin(const Plane &plane, const QPointF &press, int handle, int edge, bool extrude,
               const QSize &canvasSize, bool rotate = false, int rotationEdge = -1);
    bool update(const QPointF &point, Plane *result);
    const Plane &start() const { return m_start; }
    bool extruding() const { return m_extrude; }
    bool rotating() const { return m_rotate; }
    int edge() const { return m_edge; }

private:
    Plane m_start;
    QPointF m_press;
    int m_handle = -1, m_edge = -1;
    bool m_extrude = false;
    bool m_rotate = false;
    int m_rotationEdge = -1;
    qreal m_lastPointerAngle = 0.0;
    qreal m_accumulatedRotation = 0.0;
    QSize m_canvasSize;
};
