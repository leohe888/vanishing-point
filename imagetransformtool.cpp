#include "imagetransformtool.h"
#include "imagegeometry.h"
#include <QLineF>

bool ImageTransformTool::begin(const FloatingImage &image, const QPointF &canvasPoint, int handle, Mode mode)
{
    if (handle < 0 || handle >= 8)
        return false;
    m_start = image;
    const QPointF control = FloatingImageMath::controlPoints(image)[handle];
    FloatingImageMath::toCanvas(image, control, &m_face);
    if (!FloatingImageMath::fromCanvas(image, canvasPoint, &m_press, m_face))
        return false;
    m_offset = m_press - control;
    m_handle = handle;
    m_mode = mode;
    return true;
}

void ImageTransformTool::beginMove(const FloatingImage &image, const QPointF &offset)
{
    m_start = image;
    m_offset = offset;
    m_face = -1;
    m_mode = Mode::Move;
}

bool ImageTransformTool::update(const QPointF &point, bool shift, bool alt, FloatingImage *result) const
{
    if (!result || m_mode == Mode::Idle)
        return false;
    QPointF surface;
    if (!FloatingImageMath::fromCanvas(m_start, point, &surface, m_face))
        return false;
    if (m_mode == Mode::Rotate)
        *result = FloatingImageMath::rotated(m_start, m_press, surface, shift);
    else if (m_mode == Mode::Scale)
        *result = FloatingImageMath::resized(m_start, m_handle, surface - m_offset, shift, alt);
    else {
        *result = m_start;
        result->position = surface - m_offset;
    }
    return true;
}

int ImageTransformTool::handleAt(const FloatingImage &image, const QPointF &point, qreal viewScale)
{
    const auto geometry = ImageGeometry::get(image);
    const auto &controls = geometry->controls();
    for (int i = 0; i < controls.size(); ++i)
        if (QLineF(point, controls[i]).length() <= 8 / viewScale)
            return i;
    return -1;
}

int ImageTransformTool::rotationCornerAt(const FloatingImage &image, const QPointF &point, qreal viewScale)
{
    const auto geometry = ImageGeometry::get(image);
    const auto &controls = geometry->controls();
    for (int i = 0; i < 4; ++i) {
        const qreal distance = QLineF(point, controls[i]).length() * viewScale;
        if (distance > 8 && distance <= 24 && !geometry->outline().contains(point))
            return i;
    }
    return -1;
}
