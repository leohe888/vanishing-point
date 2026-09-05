#pragma once

#include <QPixmap>

class CanvasDocument;

// 缓存当前视口的静态内容。蚂蚁线、控制点和网格由画布另行绘制。
class SceneContentCache
{
public:
    const QPixmap &get(const CanvasDocument &document, const QSize &viewport, qreal deviceRatio,
                       qreal scale, const QPointF &offset);
    int renderCount() const { return m_renderCount; }

private:
    QByteArray m_key;
    QPixmap m_pixels;
    int m_renderCount = 0;
};
