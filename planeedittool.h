#pragma once

#include "planemath.h"

class PlaneEditTool
{
public:
    void begin(const Plane &plane, const QPointF &press, int handle, int edge, bool extrude, const QSize &canvasSize);
    bool update(const QPointF &point, Plane *result) const;
    const Plane &start() const { return m_start; }
    bool extruding() const { return m_extrude; }
    int edge() const { return m_edge; }

private:
    Plane m_start;
    QPointF m_press;
    int m_handle = -1, m_edge = -1;
    bool m_extrude = false;
    QSize m_canvasSize;
};
