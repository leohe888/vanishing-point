#include "projectivemapping.h"

#include <QtMath>

ProjectiveMapping::ProjectiveMapping(const QPolygonF &domain, const QPolygonF &canvas)
{
    if (domain.size() != 4 || canvas.size() != 4)
        return;
    for (int i = 0; i < 4; ++i) {
        m_domainReference += domain[i] / 4;
        m_canvasReference += canvas[i] / 4;
    }
    if (!QTransform::quadToQuad(domain, canvas, m_forward))
        return;
    auto normalize = [](const QTransform &t, const QPointF &reference) {
        const qreal w = t.m13() * reference.x() + t.m23() * reference.y() + t.m33();
        return w >= 0 ? t : QTransform(-t.m11(), -t.m12(), -t.m13(), -t.m21(), -t.m22(), -t.m23(),
                                       -t.m31(), -t.m32(), -t.m33());
    };
    m_forward = normalize(m_forward, m_domainReference);
    m_inverse = m_forward.inverted(&m_valid);
    m_inverse = normalize(m_inverse, m_canvasReference);
    QPointF mapped;
    for (int i = 0; m_valid && i < 4; ++i)
        m_valid = mapVisible(m_forward, domain[i], m_domainReference, &mapped)
                  && mapVisible(m_inverse, canvas[i], m_canvasReference, &mapped);
}

bool ProjectiveMapping::mapVisible(const QTransform &transform, const QPointF &point,
                                   const QPointF &reference, QPointF *result)
{
    if (!result || !qIsFinite(point.x()) || !qIsFinite(point.y()))
        return false;
    auto denominator = [&transform](const QPointF &p) {
        return transform.m13() * p.x() + transform.m23() * p.y() + transform.m33();
    };
    const qreal referenceW = denominator(reference), w = denominator(point);
    if (!qIsFinite(referenceW) || !qIsFinite(w) || referenceW * w <= 0
        || qAbs(w) <= qAbs(referenceW) * 1e-6)
        return false;
    // QTransform::map 对负分母有绘制裁剪语义，几何计算必须直接做齐次除法。
    const QPointF mapped((transform.m11() * point.x() + transform.m21() * point.y() + transform.m31()) / w,
                          (transform.m12() * point.x() + transform.m22() * point.y() + transform.m32()) / w);
    if (!qIsFinite(mapped.x()) || !qIsFinite(mapped.y()))
        return false;
    *result = mapped;
    return true;
}

bool ProjectiveMapping::toCanvas(const QPointF &point, QPointF *result) const
{
    return m_valid && mapVisible(m_forward, point, m_domainReference, result);
}

bool ProjectiveMapping::fromCanvas(const QPointF &point, QPointF *result) const
{
    return m_valid && mapVisible(m_inverse, point, m_canvasReference, result);
}
