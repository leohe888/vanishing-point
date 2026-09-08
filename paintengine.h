#pragma once

#include "planemath.h"

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QRect>

class QPainter;

// 笔刷引擎：在透视面片的归一化 UV 空间中生成软边笔触，并直接烘焙到
// 画布同尺寸的绘画层（画布/图像坐标系）上。笔触跟随面片的单应变换自然
// 产生透视缩短，且可以越过面片边界向外延伸——它只依赖面片提供的透视
// 规则，与平面本身是否持有内容无关。绘画层由 CanvasDocument 持有。
class PaintEngine
{
public:
    // —— 笔刷参数 ——
    void setDiameter(int value) { m_diameter = value; }          // 直径（图像像素）
    void setHardness(int value) { m_hardness = value / 100.0; }  // 硬度（0~100 -> 0~1）
    void setOpacity(int value) { m_opacity = value / 100.0; }    // 不透明度（0~100 -> 0~1）
    void setColor(const QColor &color) { m_brushColor = color; }
    QColor color() const { return m_brushColor; }

    // 在指定面片上从给定 UV 位置开始一笔，并立即落下第一个笔触点。
    // 返回本次落笔在画布上影响到的矩形（供历史脏矩形累积）。
    QRect beginStroke(QImage &paintLayer, const Facet &facet, const QPointF &uv);

    // 从上一个 UV 位置向目标 UV 插值补间，沿笔迹均匀落下一串笔触点。
    // 返回本次补间在画布上影响到的矩形。
    QRect drawStrokeTo(QImage &paintLayer, const Facet &facet, const QPointF &uv);

    // 在指定面片的给定 UV 位置处落下一个笔触点。
    // 既有笔触绘制（写入绘画层），也可以画到任意 QPainter 上用作光标预览——
    // 区别只是不做"替换"的世界变换，而是与 painter 当前的变换相乘，
    // 因此在画布（已经叠加了视图缩放与平移）的 painter 上也能正确跟随光标。
    QRect applyDab(QPainter &painter, const Facet &facet, const QPointF &uv) const;

private:
    QPointF m_lastUv;                          // 最近一次笔迹的 UV 坐标
    QColor m_brushColor = QColor("#e85d4a");   // 画笔颜色
    qreal m_diameter = 42;                     // 笔刷直径（图像像素）
    qreal m_hardness = .75;                    // 笔刷硬度（0~1）
    qreal m_opacity = 1.0;                     // 笔刷不透明度（0~1）
};
