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

// 在指定平面上从给定 UV 位置开始一笔
void PaintEngine::beginStroke(QVector<Plane> &planes, int planeIndex, const QPointF &uv)
{
    m_lastUv = uv;
    applyDab(planes[planeIndex], uv);
}

// 从上一个 UV 位置向当前位置插值补间，沿笔迹均匀落下一串笔触点
void PaintEngine::drawStrokeTo(QVector<Plane> &planes, int planeIndex, const QPointF &point)
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
        applyDab(plane, m_lastUv + (uv - m_lastUv) * (qreal(i) / count));
    m_lastUv = uv;
}

// 在平面的纹理空间落下一个笔触点
void PaintEngine::applyDab(Plane &plane, const QPointF &uv)
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
            source.setAlphaF(source.alphaF() * m_opacity * falloff);
            line[x] = over(QColor::fromRgba(line[x]), source).rgba();
        }
    }
}
