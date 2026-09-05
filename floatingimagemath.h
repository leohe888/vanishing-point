#pragma once

#include "canvasdocument.h"
#include <QTransform>

namespace FloatingImageMath {
QTransform imageToSpace(const FloatingImage &image);
// 几何坐标：未吸附时为画布像素，吸附后为展开曲面坐标。
QPointF toCanvas(const FloatingImage &image, const QPointF &point, int *faceIndex = nullptr);
bool fromCanvas(const FloatingImage &image, const QPointF &point, QPointF *result, int fallbackFace = -1);
QVector<QPointF> controlPoints(const FloatingImage &image);
FloatingImage resized(const FloatingImage &start, int handle, const QPointF &point,
                      bool keepAspect, bool fromCenter = false);
FloatingImage rotated(const FloatingImage &start, const QPointF &press, const QPointF &point, bool snap);
}
