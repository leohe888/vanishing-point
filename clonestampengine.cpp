#include "clonestampengine.h"
#include "projectivemapping.h"

#include <QLineF>
#include <QPainter>
#include <QtMath>
#include <cmath>

namespace {
// 双线性插值使用预乘 alpha，透明边缘不会带入黑色。
QRgb sample(const QImage &image, const QPointF &point, qreal coverage)
{
    const qreal x = point.x() - .5, y = point.y() - .5;
    const int x0 = qFloor(x), y0 = qFloor(y);
    const qreal fx = x - x0, fy = y - y0;
    qreal r = 0, g = 0, b = 0, a = 0;
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            if (!image.rect().contains(x0 + i, y0 + j))
                continue;
            const QRgb pixel = image.pixel(x0 + i, y0 + j);
            const qreal weight = (i ? fx : 1 - fx) * (j ? fy : 1 - fy) * coverage;
            r += qRed(pixel) * weight;
            g += qGreen(pixel) * weight;
            b += qBlue(pixel) * weight;
            a += qAlpha(pixel) * weight;
        }
    }
    return qRgba(qRound(r), qRound(g), qRound(b), qRound(a));
}
}

QRect CloneStampEngine::beginStroke(QImage &layer, const QImage &source,
                                   const QTransform &targetToCanvas,
                                   const QTransform &sourceToCanvas,
                                   const QPointF &offset, const QPointF &position)
{
    bool invertible = false;
    m_canvasToTarget = targetToCanvas.inverted(&invertible);
    m_source = invertible ? source.convertToFormat(QImage::Format_ARGB32_Premultiplied) : QImage();
    m_targetToCanvas = targetToCanvas;
    m_sourceToCanvas = sourceToCanvas;
    m_offset = offset;
    m_targetReference = position;
    m_canvasReference = targetToCanvas.map(position);
    m_sourceReference = position + offset;
    m_lastPosition = position;
    return applyDab(layer, position);
}

QRect CloneStampEngine::drawStrokeTo(QImage &layer, const QPointF &position)
{
    const qreal distance = QLineF(m_lastPosition, position).length();
    if (!std::isfinite(distance) || distance < 1e-6)
        return {};
    // 防止鼠标越过透视极点时产生无限量补点。
    const int count = int(qMin(10000.0, std::ceil(distance / qMax(.5, m_diameter * .12))));
    QRect dirty;
    for (int i = 1; i <= count; ++i)
        dirty = dirty.united(applyDab(layer, m_lastPosition + (position - m_lastPosition) * (qreal(i) / count)));
    m_lastPosition = position;
    return dirty;
}

QRect CloneStampEngine::applyDab(QImage &layer, const QPointF &position)
{
    if (m_source.isNull() || m_opacity <= 0)
        return {};
    const qreal radius = m_diameter / 2;
    const QRectF bounds(position.x() - radius, position.y() - radius, m_diameter, m_diameter);
    QPointF mapped;
    for (const QPointF &corner : {bounds.topLeft(), bounds.topRight(), bounds.bottomLeft(), bounds.bottomRight()})
        if (!ProjectiveMapping::mapVisible(m_targetToCanvas, corner, m_targetReference, &mapped))
            return {};
    // 先在浮点坐标中裁剪，避免接近消失线时整型溢出或分配巨大图像。
    const QRect area = m_targetToCanvas.mapRect(bounds).intersected(QRectF(layer.rect()))
                           .toAlignedRect().intersected(layer.rect());
    if (area.isEmpty())
        return {};
    QImage dab(area.size(), QImage::Format_ARGB32_Premultiplied);
    dab.fill(Qt::transparent);
    bool changed = false;
    for (int y = 0; y < area.height(); ++y) {
        auto *row = reinterpret_cast<QRgb *>(dab.scanLine(y));
        for (int x = 0; x < area.width(); ++x) {
            QPointF target, source;
            if (!ProjectiveMapping::mapVisible(m_canvasToTarget, QPointF(area.x() + x + .5, area.y() + y + .5), m_canvasReference, &target))
                continue;
            const qreal distance = QLineF(target, position).length() / radius;
            if (distance >= 1 || !ProjectiveMapping::mapVisible(m_sourceToCanvas, target + m_offset, m_sourceReference, &source)
                || source.x() < 0 || source.y() < 0
                || source.x() >= m_source.width() || source.y() >= m_source.height())
                continue;
            const qreal mask = distance <= m_hardness ? 1 : (1 - distance) / (1 - m_hardness);
            row[x] = sample(m_source, source, mask * m_opacity);
            changed |= qAlpha(row[x]) > 0;
        }
    }
    if (!changed)
        return {};
    QPainter painter(&layer);
    painter.drawImage(area.topLeft(), dab);
    return area;
}
