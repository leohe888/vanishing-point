#include "paintengine.h"

#include <QLineF>
#include <QtMath>

namespace {
using PlaneMath::TextureSize;

// 手工实现的 source-over 颜色合成（上层 top 叠在下层 bottom 上）。
// QColor 的合成辅助函数便于 QPainter 使用，但逐像素的纹理绘画
// 需要这里的手工 source-over 运算。
QColor over(const QColor &bottom, const QColor &top)
{
    const qreal a = top.alphaF();
    const qreal outA = a + bottom.alphaF() * (1.0 - a);
    if (outA < PlaneMath::Epsilon)
        return Qt::transparent;
    QColor result;
    result.setRgbF((top.redF() * a + bottom.redF() * bottom.alphaF() * (1.0 - a)) / outA,
                   (top.greenF() * a + bottom.greenF() * bottom.alphaF() * (1.0 - a)) / outA,
                   (top.blueF() * a + bottom.blueF() * bottom.alphaF() * (1.0 - a)) / outA,
                   outA);
    return result;
}
}

// 设置仿制源（Alt+单击）：记录源平面与其上的 UV 位置
void PaintEngine::setCloneSource(const QPointF &uv, int planeIndex)
{
    m_cloneSourceUv = uv;
    m_cloneSourcePlane = planeIndex;
    m_hasCloneSource = true;
    m_cloneStrokeStarted = false;
}

// 清除仿制源（加载新文档时）
void PaintEngine::clearCloneSource()
{
    m_hasCloneSource = false;
    m_cloneSourcePlane = -1;
    m_cloneStrokeStarted = false;
}

// 一笔结束，复位笔迹标记
void PaintEngine::endStroke()
{
    m_cloneStrokeStarted = false;
}

// 在指定平面上从当前 UV 位置开始一笔
void PaintEngine::beginStroke(QVector<Plane> &planes, const QImage &background,
                              int planeIndex, const QPointF &uv, bool stamp)
{
    m_lastUv = uv;
    if (stamp) {
        m_cloneAnchorUv = uv;
        m_cloneStrokeStarted = true;
    }
    applyDab(planes, background, planes[planeIndex], uv, stamp);
}

// 从上一个 UV 位置向当前位置插值补间，沿笔迹均匀落下一串笔触点
void PaintEngine::drawStrokeTo(QVector<Plane> &planes, const QImage &background,
                               int planeIndex, const QPointF &point, bool stamp)
{
    Plane &plane = planes[planeIndex];
    bool ok = false;
    const QPointF uv = PlaneMath::planeToUv(plane, point, &ok);
    if (!ok || uv.x() < 0 || uv.x() > 1 || uv.y() < 0 || uv.y() > 1)
        return;
    const qreal planeWidth = (QLineF(plane.corner[0], plane.corner[1]).length() +
                              QLineF(plane.corner[3], plane.corner[2]).length()) / 2.0;
    const qreal textureDiameter = m_diameter * TextureSize / qMax(40.0, planeWidth);
    const qreal step = qMax(1.0, textureDiameter * 0.18) / TextureSize;
    const qreal distance = QLineF(m_lastUv, uv).length();
    const int count = qMax(1, int(qCeil(distance / step)));
    for (int i = 1; i <= count; ++i)
        applyDab(planes, background, plane, m_lastUv + (uv - m_lastUv) * (qreal(i) / count), stamp);
    m_lastUv = uv;
}

// 在平面的纹理空间落下一个笔触点（画笔或仿制图章）。
// stamp 为 true 时按仿制偏移从源位置采样颜色。
void PaintEngine::applyDab(const QVector<Plane> &planes, const QImage &background,
                           Plane &plane, const QPointF &uv, bool stamp)
{
    // 笔刷尺寸从图像像素换算到平面的归一化纹理，使一个笔触点在
    // 透视作用下保持视觉上的一致大小。
    const qreal planeWidth = (QLineF(plane.corner[0], plane.corner[1]).length() +
                              QLineF(plane.corner[3], plane.corner[2]).length()) / 2.0;
    const qreal radius = qBound(1.0, m_diameter * TextureSize /
                                      (2.0 * qMax(40.0, planeWidth)), 300.0);
    const QPointF center(uv.x() * (TextureSize - 1), uv.y() * (TextureSize - 1));
    const int left = qMax(0, int(qFloor(center.x() - radius)));
    const int right = qMin(TextureSize - 1, int(qCeil(center.x() + radius)));
    const int top = qMax(0, int(qFloor(center.y() - radius)));
    const int bottom = qMin(TextureSize - 1, int(qCeil(center.y() + radius)));
    const qreal softStart = qBound(0.0, m_hardness, 1.0);

    for (int y = top; y <= bottom; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(plane.paint.scanLine(y));
        for (int x = left; x <= right; ++x) {
            const qreal d = qSqrt(qPow(x - center.x(), 2) + qPow(y - center.y(), 2)) / radius;
            if (d > 1.0)
                continue;
            qreal falloff = 1.0;
            if (d > softStart)
                falloff = (1.0 - d) / qMax(0.001, 1.0 - softStart);
            falloff = falloff * falloff * (3.0 - 2.0 * falloff);

            QColor source = m_brushColor;
            if (stamp) {
                const QPointF destinationUv(qreal(x) / (TextureSize - 1),
                                            qreal(y) / (TextureSize - 1));
                const QPointF sourceUv = m_cloneSourceUv + (destinationUv - m_cloneAnchorUv);
                if (sourceUv.x() < 0 || sourceUv.x() > 1 || sourceUv.y() < 0 || sourceUv.y() > 1 ||
                    m_cloneSourcePlane < 0 || m_cloneSourcePlane >= planes.size())
                    continue;
                const Plane &sourcePlane = planes[m_cloneSourcePlane];
                const QPointF imagePoint = PlaneMath::uvToPlane(sourcePlane, sourceUv);
                const int ix = qBound(0, qRound(imagePoint.x()), background.width() - 1);
                const int iy = qBound(0, qRound(imagePoint.y()), background.height() - 1);
                source = QColor::fromRgba(background.pixel(ix, iy));
                const int tx = qBound(0, qRound(sourceUv.x() * (TextureSize - 1)), TextureSize - 1);
                const int ty = qBound(0, qRound(sourceUv.y() * (TextureSize - 1)), TextureSize - 1);
                source = over(source, QColor::fromRgba(sourcePlane.paint.pixel(tx, ty)));
            }
            source.setAlphaF(source.alphaF() * m_opacity * falloff);
            line[x] = over(QColor::fromRgba(line[x]), source).rgba();
        }
    }
}
