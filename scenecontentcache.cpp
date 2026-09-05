#include "scenecontentcache.h"
#include "canvasdocument.h"
#include "imagegeometry.h"
#include "scenerenderer.h"

#include <QDataStream>
#include <QIODevice>
#include <QPainter>
#include <QtMath>

const QPixmap &SceneContentCache::get(const CanvasDocument &document, const QSize &viewport,
                                     qreal deviceRatio, qreal scale, const QPointF &offset)
{
    QByteArray key;
    QDataStream stream(&key, QIODevice::WriteOnly);
    stream << viewport << deviceRatio << scale << offset << document.background().cacheKey()
           << document.paintLayer().cacheKey() << qint64(document.images().size());
    for (const FloatingImage &image : document.images())
        stream << image.image.cacheKey() << ImageGeometry::key(image);
    if (key == m_key && !m_pixels.isNull())
        return m_pixels;
    m_key = key;
    m_pixels = QPixmap(qCeil(viewport.width() * deviceRatio), qCeil(viewport.height() * deviceRatio));
    m_pixels.setDevicePixelRatio(deviceRatio);
    m_pixels.fill(Qt::transparent);
    QPainter painter(&m_pixels);
    painter.translate(offset);
    painter.scale(scale, scale);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    SceneRenderer(document).render(painter, scale, false);
    ++m_renderCount;
    return m_pixels;
}
