#pragma once

#include "canvasdocument.h"
#include <QPainterPath>
#include <QSharedPointer>

struct ImagePatch {
    ProjectiveMapping mapping;
    QPainterPath clip;       // 源位图坐标
    QPainterPath canvasClip;
};

// 内容渲染、选中轮廓、控制点和命中测试共用的不可变投影结果。
class ImageGeometry
{
public:
    static QByteArray key(const FloatingImage &image);
    static QSharedPointer<const ImageGeometry> get(const FloatingImage &image);
    const QVector<ImagePatch> &patches() const { return m_patches; }
    const QPainterPath &outline() const { return m_outline; }
    const QVector<QPointF> &controls() const { return m_controls; }
    bool hitTest(const QPointF &point, QPointF *spaceOffset = nullptr) const;

private:
    explicit ImageGeometry(const FloatingImage &image);
    QVector<ImagePatch> m_patches;
    QPainterPath m_outline;
    QVector<QPointF> m_controls;
    QTransform m_imageToSpace;
    QPointF m_position;
};
