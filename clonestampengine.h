#pragma once

#include <QImage>
#include <QPointF>
#include <QTransform>

// 在展开平面中平移采样位置；源与目标分别使用自己的透视变换。
class CloneStampEngine
{
public:
    void setDiameter(int value) { m_diameter = qMax(1, value); }
    void setHardness(int value) { m_hardness = qBound(0, value, 100) / 100.0; }
    void setOpacity(int value) { m_opacity = qBound(0, value, 100) / 100.0; }
    QRect beginStroke(QImage &layer, const QImage &source,
                      const QTransform &targetToCanvas, const QTransform &sourceToCanvas,
                      const QPointF &offset, const QPointF &position);
    QRect drawStrokeTo(QImage &layer, const QPointF &position);
    void endStroke() { m_source = QImage(); }

    // —— 光标预览支持 ——
    // 把预览所需的源、变换与参考点写入引擎；不写 m_lastPosition、不进入
    // 落笔状态，因此可与真实落笔共用同一台引擎而互不干扰。
    void setPreview(const QImage &source, const QTransform &targetToCanvas,
                    const QTransform &sourceToCanvas, const QPointF &offset,
                    const QPointF &position);
    // 计算 position 处一个笔触点对应的画布矩形（已裁到源图范围）。
    QRect dabRect(const QPointF &position) const;
    // 把 position 处一个笔触点的源采样写入 dab（dab 尺寸须等于 dabRect）。
    // 返回是否真的写入了非透明像素。
    bool renderDab(QImage &dab, const QRect &dabRect, const QPointF &position) const;

private:
    QRect applyDab(QImage &layer, const QPointF &position);
    QImage m_source;
    QTransform m_targetToCanvas, m_canvasToTarget, m_sourceToCanvas;
    QPointF m_offset, m_lastPosition;
    QPointF m_targetReference, m_canvasReference, m_sourceReference;
    qreal m_diameter = 42, m_hardness = .75, m_opacity = 1;
};
