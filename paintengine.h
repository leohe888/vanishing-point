#pragma once

#include "planemath.h"

#include <QColor>
#include <QPointF>
#include <QVector>

// 笔刷引擎：在平面的 UV 纹理空间中生成画笔笔迹。
// 持有笔刷参数，本身不依赖任何窗口部件——
// 绘画发生在纹理上，透视效果在纹理被投影回平面时自然产生。
class PaintEngine
{
public:
    // —— 笔刷参数 ——
    void setDiameter(int value) { m_diameter = value; }          // 直径（图像像素）
    void setHardness(int value) { m_hardness = value / 100.0; }  // 硬度（0~100 -> 0~1）
    void setOpacity(int value) { m_opacity = value / 100.0; }    // 不透明度（0~100 -> 0~1）
    void setColor(const QColor &color) { m_brushColor = color; }
    QColor color() const { return m_brushColor; }

    // 在指定平面上从给定 UV 位置开始一笔，并立即落下第一个笔触点。
    void beginStroke(QVector<Plane> &planes, int planeIndex, const QPointF &uv);

    // 从上一个 UV 位置向当前图像坐标插值补间，沿笔迹均匀落下一串笔触点。
    void drawStrokeTo(QVector<Plane> &planes, int planeIndex, const QPointF &imagePoint);

private:
    // 在纹理空间落下一个笔触点。
    void applyDab(Plane &plane, const QPointF &uv);

    QPointF m_lastUv;                          // 最近一次笔迹的 UV 坐标
    QColor m_brushColor = QColor("#e85d4a");   // 画笔颜色
    qreal m_diameter = 42;                     // 笔刷直径（图像像素）
    qreal m_hardness = .75;                    // 笔刷硬度（0~1）
    qreal m_opacity = 1.0;                     // 笔刷不透明度（0~1）
};
