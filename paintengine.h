#pragma once

#include "planemath.h"

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QVector>

// 笔刷引擎：在平面的 UV 纹理空间中生成画笔与仿制图章笔迹。
// 持有笔刷参数与仿制源状态，本身不依赖任何窗口部件——
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

    // —— 仿制源状态 ——
    void setCloneSource(const QPointF &uv, int planeIndex);      // 设置仿制源（Alt+单击）
    bool hasCloneSource() const { return m_hasCloneSource; }
    void clearCloneSource();                                     // 清除仿制源（加载新文档时）
    void endStroke();                                            // 一笔结束，复位笔迹标记

    // 在指定平面上从当前 UV 位置开始一笔。
    // stamp 为 true 时按仿制方式落第一个笔触点。
    void beginStroke(QVector<Plane> &planes, const QImage &background,
                     int planeIndex, const QPointF &uv, bool stamp);

    // 从上一个 UV 位置向当前图像坐标插值补间，沿笔迹均匀落下一串笔触点。
    // stamp 为 true 时按仿制偏移从源位置采样颜色。
    void drawStrokeTo(QVector<Plane> &planes, const QImage &background,
                      int planeIndex, const QPointF &imagePoint, bool stamp);

private:
    // 在纹理空间落下一个笔触点（画笔或仿制图章）。
    // stamp 为 true 时按仿制偏移从源位置采样颜色。
    void applyDab(const QVector<Plane> &planes, const QImage &background,
                  Plane &plane, const QPointF &uv, bool stamp);

    QPointF m_lastUv;                          // 最近一次笔迹的 UV 坐标
    QColor m_brushColor = QColor("#e85d4a");   // 画笔颜色
    qreal m_diameter = 42;                     // 笔刷直径（图像像素）
    qreal m_hardness = .75;                    // 笔刷硬度（0~1）
    qreal m_opacity = 1.0;                     // 笔刷不透明度（0~1）
    QPointF m_cloneSourceUv;                   // 仿制源在源平面上的 UV
    QPointF m_cloneAnchorUv;                   // 本次仿制笔迹起点的 UV（偏移锚）
    bool m_hasCloneSource = false;             // 是否已设置仿制源
    bool m_cloneStrokeStarted = false;         // 当前笔迹是否已开始
    int m_cloneSourcePlane = -1;               // 仿制源所在平面索引
};
