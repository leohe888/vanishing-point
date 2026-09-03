#pragma once

#include <QImage>
#include <QPointF>
#include <QPolygonF>
#include <QString>
#include <QVector>

// 一个平面保存其在画布和展开曲面上的几何信息，以及一张供画笔
// 使用的透明绘画纹理。
struct Plane {
    QPointF corner[4];
    // 相邻平面共享一个展开的 2D 曲面。这些坐标允许同一张浮动图像
    // 跨越接缝，而每个面仍使用各自到画布的单应变换。
    QPointF surfaceCorner[4];
    int surfaceGroup = -1;  // 所属的展开曲面分组（共享曲面的相邻平面同组）
    QImage paint;           // 该平面的绘画纹理（UV 空间）
    QString name;           // 显示用的平面名称
};

// 平面几何与透视构造算法的纯函数集合。
// 这些函数不持有任何状态，所需的上下文（视图缩放、背景尺寸、
// 按下起点等）一律由调用方作为参数提供，因此可以独立测试。
namespace PlaneMath {

// 纹理分辨率固定不变，使绘画质量与源图像尺寸及当前画布缩放级别无关。
constexpr int TextureSize = 1024;
constexpr qreal Epsilon = 1e-6; // 浮点比较用的极小量

// —— 基础几何 ——
QPolygonF planePolygon(const QPointF corner[4]);   // 把 4 个角点组装为多边形
QVector<QPointF> handles(const Plane &plane);      // 平面的 4 个角点 + 4 个边中点
// 计算点 p 到线段 ab 的距离；t 返回最近点在线段上的参数化位置（0~1）
qreal distanceToSegment(const QPointF &p, const QPointF &a,
                        const QPointF &b, qreal *t = nullptr);
bool isValidPlane(const Plane &plane);             // 校验平面是否为有效的凸四边形

// —— 坐标变换（单应） ——
QPointF uvToPlane(const Plane &plane, const QPointF &uv);          // 归一化 UV -> 图像坐标
QPointF planeToUv(const Plane &plane, const QPointF &point,        // 图像坐标 -> 归一化 UV
                 bool *ok = nullptr);
QPointF planeToSurface(const Plane &plane, const QPointF &point,   // 图像坐标 -> 共享展开曲面坐标
                       bool *ok = nullptr);

// —— 命中测试（tolerance 为图像坐标系下的拾取半径） ——
int planeAt(const QVector<Plane> &planes, const QPointF &point);        // 点所在的最上层平面
int handleAt(const Plane &plane, const QPointF &point, qreal tolerance); // 控制点索引
int edgeAt(const Plane &plane, const QPointF &point, qreal tolerance);   // 边缘索引

// —— 平面构造算法（pressPoint 为本次拖动的按下起点） ——
// 沿某条边方向缩放平面：只改变该边到对边的距离，保持透视关系不变
Plane resizePlaneAlongEdge(const Plane &source, int edge,
                           const QPointF &dragPoint, const QPointF &pressPoint);
// 由平面法线恢复投影后的第三个消失方向（backgroundSize 用于估计焦距）
bool perpendicularDirection(const Plane &source, const QPointF &atPoint,
                            const QSize &backgroundSize, QPointF *direction);
// 从源平面的一条边拖出与之垂直的新平面（Ctrl+拖动边缘）
Plane makePerpendicularPlane(const Plane &source, int edge,
                             const QPointF &dragPoint, const QPointF &pressPoint,
                             const QSize &backgroundSize);

} // namespace PlaneMath
