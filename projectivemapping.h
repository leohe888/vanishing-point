#pragma once

#include <QPolygonF>
#include <QTransform>

// 无限射影平面的规则。有限四边形仅用于标定变换和确定可见半平面。
// 所有调用方共用同一套分母、有限值及地平线检查。
class ProjectiveMapping
{
public:
    ProjectiveMapping() = default;
    ProjectiveMapping(const QPolygonF &domain, const QPolygonF &canvas);
    bool isValid() const { return m_valid; }
    bool toCanvas(const QPointF &point, QPointF *result) const;
    bool fromCanvas(const QPointF &point, QPointF *result) const;
    const QTransform &forward() const { return m_forward; }
    const QTransform &inverse() const { return m_inverse; }
    static bool mapVisible(const QTransform &transform, const QPointF &point,
                           const QPointF &reference, QPointF *result);

private:
    QTransform m_forward, m_inverse;
    QPointF m_domainReference, m_canvasReference;
    bool m_valid = false;
};
