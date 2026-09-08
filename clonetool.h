#pragma once

#include "clonestampengine.h"
#include "planemath.h"

class QPainter;

class CloneTool
{
public:
    void setDiameter(int value) { m_engine.setDiameter(value); }
    void setHardness(int value) { m_engine.setHardness(value); }
    void setOpacity(int value) { m_engine.setOpacity(value); }
    void setAligned(bool aligned);
    bool hasSource() const { return m_hasSource; }
    QPointF marker() const { return m_marker; }
    void resetSource();
    bool pickSource(const QVector<Plane> &planes, const QPointF &point);
    QRect begin(QImage &layer, const QImage &source, const Plane &target, const QPointF &point);
    QRect move(QImage &layer, const QPointF &point);
    void hover(const QVector<Plane> &planes, const QPointF &point);
    void end(const QVector<Plane> &planes, const QPointF &point);
    void cancel();

    // 在光标处预览即将仿制的内容（所见即所得）：复用引擎的逐像素采样，
    // 与真实落笔完全一致，但不修改任何交互状态（源点/偏移/是否落笔）。
    void renderPreview(QPainter &painter, const QImage &source,
                       const QVector<Plane> &planes, const QPointF &point);

private:
    CloneStampEngine m_engine;
    ProjectiveMapping m_sourceMapping, m_targetMapping;
    bool m_sourceOnPlane = false;
    bool m_hasSource = false, m_hasOffset = false, m_aligned = true, m_drawing = false;
    QPointF m_source, m_offset, m_marker;
    QPointF originalMarker() const;
};
