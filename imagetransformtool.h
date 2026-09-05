#pragma once

#include "floatingimagemath.h"

// 一次图片操作的状态独立于 QWidget、文档及撤销历史。
class ImageTransformTool
{
public:
    enum class Mode { Idle, Move, Scale, Rotate };
    bool begin(const FloatingImage &image, const QPointF &canvasPoint, int handle, Mode mode);
    void beginMove(const FloatingImage &image, const QPointF &offset);
    bool update(const QPointF &canvasPoint, bool shift, bool alt, FloatingImage *result) const;
    void reset() { m_mode = Mode::Idle; }
    Mode mode() const { return m_mode; }
    bool transforming() const { return m_mode == Mode::Scale || m_mode == Mode::Rotate; }
    const FloatingImage &start() const { return m_start; }
    QPointF grabOffset() const { return m_offset; }
    static int handleAt(const FloatingImage &image, const QPointF &point, qreal viewScale);
    static int rotationCornerAt(const FloatingImage &image, const QPointF &point, qreal viewScale);

private:
    Mode m_mode = Mode::Idle;
    FloatingImage m_start;
    int m_handle = -1, m_face = -1;
    QPointF m_offset, m_press;
};
