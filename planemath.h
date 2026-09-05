#pragma once

#include <QImage>
#include <QPointF>
#include <QPolygonF>
#include <QString>
#include <QVector>
#include "projectivemapping.h"

// 一个面片：画布上的透视四边形，以及它在展开曲面上的对应四边形。
//
// 这是本项目最基础的结构，有两种用法：
//  1. 平面（Plane 继承它）用它描述自己的几何；
//  2. 浮动图像在吸附瞬间把它拷贝一份作为快照。
//
// 快照正是内容与平面解耦的关键：内容渲染时只认自己那一份拷贝，
// 因此之后删除或修改平面都不会影响已经存在的内容。
struct Facet {
    QPointF corner[4];
    // 相邻平面共享一个展开的 2D 曲面。这些坐标允许同一张浮动图像
    // 跨越接缝，而每个面仍使用各自到画布的单应变换。
    QPointF surfaceCorner[4];
};

// 平面只描述透视规则，不持有任何内容——绘画烘焙在画布的绘画层上，
// 浮动图像各自携带几何快照。平面可以被自由增删改而不波及内容。
struct Plane : Facet {
    int surfaceGroup = -1;  // 所属的展开曲面分组（共享曲面的相邻平面同组）
    QString name;           // 显示用的平面名称
};

// 平面几何与透视构造算法的纯函数集合。
// 这些函数不持有任何状态，所需的上下文（视图缩放、背景尺寸、
// 按下起点等）一律由调用方作为参数提供，因此可以独立测试。
namespace PlaneMath {
ProjectiveMapping surfaceMapping(const Facet &facet);
ProjectiveMapping uvMapping(const Facet &facet);

// 纹理分辨率固定不变，使绘画质量与源图像尺寸及当前画布缩放级别无关。
constexpr int TextureSize = 1024;
constexpr qreal Epsilon = 1e-6; // 浮点比较用的极小量

// —— 基础几何 ——
QPolygonF planePolygon(const QPointF corner[4]);   // 把 4 个角点组装为多边形
QVector<QPointF> handles(const Facet &facet);      // 面片的 4 个角点 + 4 个边中点
// 计算点 p 到线段 ab 的距离；t 返回最近点在线段上的参数化位置（0~1）
qreal distanceToSegment(const QPointF &p, const QPointF &a,
                        const QPointF &b, qreal *t = nullptr);
bool isValidPlane(const Facet &facet);             // 校验是否为有效的凸四边形

// —— 坐标变换（单应） ——
// 这些函数接受 Facet，因此既能作用于平面，也能作用于浮动图像的几何快照。
QPointF uvToPlane(const Facet &facet, const QPointF &uv);             // 归一化 UV -> 图像坐标
QPointF planeToUv(const Facet &facet, const QPointF &point,           // 图像坐标 -> 归一化 UV
                  bool *ok = nullptr);
QPointF planeToSurface(const Facet &facet, const QPointF &point,      // 图像坐标 -> 展开曲面坐标
                       bool *ok = nullptr);

// —— 命中测试（tolerance 为图像坐标系下的拾取半径） ——
int planeAt(const QVector<Plane> &planes, const QPointF &point);        // 点所在的最上层平面
int handleAt(const Facet &facet, const QPointF &point, qreal tolerance); // 控制点索引
int edgeAt(const Facet &facet, const QPointF &point, qreal tolerance);   // 边缘索引

// —— 平面构造算法（pressPoint 为本次拖动的按下起点） ——
// 在原来的无限透视平面上平移有限四边形，同时移动展开坐标以保持单应规则。
// 越过地平线、出现极点或退化时返回 false，调用方保留最后有效位置。
bool movePlaneOnSurface(const Plane &source, const QPointF &dragPoint,
                        const QPointF &pressPoint, Plane *result);
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
