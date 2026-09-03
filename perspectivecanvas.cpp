#include "perspectivecanvas.h"

#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QImageReader>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QResizeEvent>
#include <QTransform>
#include <QVector3D>
#include <QUrl>
#include <QtMath>

namespace {
// 纹理分辨率固定不变，使绘画质量与源图像尺寸及当前画布缩放级别无关。
constexpr int TextureSize = 1024;
// 历史快照可能包含多张大纹理，因此限制历史数量以防内存失控。
constexpr int MaxHistoryStates = 40;
constexpr qreal Epsilon = 1e-6; // 浮点比较用的极小量

// 把 4 个角点组装为多边形。
QPolygonF planePolygon(const QPointF corner[4])
{
    // 角点顺序保持不变：调用方必须按顺时针或逆时针提供四个点，
    // 绝不能是交叉的多边形。
    return QPolygonF{corner[0], corner[1], corner[2], corner[3]};
}

// 手工实现的 source-over 颜色合成（上层 top 叠在下层 bottom 上）。
QColor over(const QColor &bottom, const QColor &top)
{
    // QColor 的合成辅助函数便于 QPainter 使用，但逐像素的纹理绘画
    // 需要这里的手工 source-over 运算。
    const qreal a = top.alphaF();
    const qreal outA = a + bottom.alphaF() * (1.0 - a);
    if (outA < Epsilon)
        return Qt::transparent;
    QColor result;
    result.setRgbF((top.redF() * a + bottom.redF() * bottom.alphaF() * (1.0 - a)) / outA,
                   (top.greenF() * a + bottom.greenF() * bottom.alphaF() * (1.0 - a)) / outA,
                   (top.blueF() * a + bottom.blueF() * bottom.alphaF() * (1.0 - a)) / outA,
                   outA);
    return result;
}
}

// 构造函数：初始化默认背景并重置历史
PerspectiveCanvas::PerspectiveCanvas(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAcceptDrops(true);
    setMinimumSize(500, 400);

    m_background = QImage(1200, 800, QImage::Format_ARGB32);
    QPainter p(&m_background);
    p.fillRect(m_background.rect(), QColor("#26292d"));
    updateViewTransform();
    resetHistory();
}

// 从文件加载背景图像，并清空平面、浮动图像与历史记录
bool PerspectiveCanvas::loadImage(const QString &fileName)
{
    QImage image(fileName);
    if (image.isNull())
        return false;
    m_background = image.convertToFormat(QImage::Format_ARGB32);
    m_hasLoadedImage = true;
    emit documentAvailabilityChanged(true);
    m_planes.clear();
    m_pastedImage = QImage();
    m_pastedImagePosition = QPointF();
    m_pastedImageAttached = false;
    m_pastedSurfaceGroup = -1;
    m_pastedHostPlane = -1;
    m_creationPoints.clear();
    m_selectedPlane = -1;
    m_hasCloneSource = false;
    resetHistory();
    updateViewTransform();
    update();
    emit statusMessage(tr("图像已打开。请依次点击四个点创建透视平面。"), 5000);
    return true;
}

// 将当前场景（背景 + 各平面绘画 + 浮动图像）导出为白底图像文件
bool PerspectiveCanvas::saveResult(const QString &fileName) const
{
    QImage result(m_background.size(), QImage::Format_ARGB32);
    result.fill(Qt::white);
    QPainter painter(&result);
    renderScene(painter, false);
    painter.end();
    return result.save(fileName);
}

// 拖放进入事件：判断拖入内容是否为可用的本地图像文件
void PerspectiveCanvas::dragEnterEvent(QDragEnterEvent *event)
{
    // 拖入的图像有两种含义：文档尚未打开时它成为背景图像；
    // 之后则成为可移动的浮动图像。
    // 只接受本地图像文件，避免吞掉任意 URL。
    if (!event->mimeData()->hasUrls())
        return;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile() && !QImageReader::imageFormat(url.toLocalFile()).isEmpty()) {
            event->acceptProposedAction();
            return;
        }
    }
}

// 拖放放下事件：打开图像或创建浮动图像
void PerspectiveCanvas::dropEvent(QDropEvent *event)
{
    if (!event->mimeData()->hasUrls())
        return;

    // 未加载文档时，第一个有效的拖入文件直接作为背景图像打开。
    // loadImage() 同时会重置平面、历史与视图变换。
    if (!m_hasLoadedImage) {
        for (const QUrl &url : event->mimeData()->urls()) {
            if (!url.isLocalFile() || QImageReader::imageFormat(url.toLocalFile()).isEmpty())
                continue;
            if (loadImage(url.toLocalFile()))
                event->acceptProposedAction();
            else
                emit statusMessage(tr("无法打开拖入的图像"), 3500);
            return;
        }
        return;
    }

    for (const QUrl &url : event->mimeData()->urls()) {
        if (!url.isLocalFile() || QImageReader::imageFormat(url.toLocalFile()).isEmpty())
            continue;
        const QImage image(url.toLocalFile());
        if (image.isNull()) {
            emit statusMessage(tr("无法读取拖入的图像"), 3500);
            return;
        }
        setFloatingImage(image, tr("图像已放到画布左上角，可拖入透视平面"));
        event->acceptProposedAction();
        return;
    }
}

// 清除所有平面上的绘画内容（不影响平面几何本身）
void PerspectiveCanvas::clearPainting()
{
    if (m_planes.isEmpty())
        return;
    for (Plane &plane : m_planes)
        plane.paint.fill(Qt::transparent);
    commitHistory();
    update();
    emit statusMessage(tr("已清除所有平面上的绘画内容"), 3000);
}

// 捕获当前状态为一份历史快照
PerspectiveCanvas::CanvasState PerspectiveCanvas::captureState() const
{
    return CanvasState{m_planes, m_selectedPlane, m_pastedImage, m_pastedImagePosition,
                       m_pastedImageAttached, m_pastedSurfaceGroup, m_pastedHostPlane};
}

// 把剪贴板中的图像粘贴为新的浮动图像
void PerspectiveCanvas::pasteClipboardImage()
{
    // QClipboard 会完成从常见图像格式（PNG、BMP 等）到 QImage 的
    // 平台相关转换。
    const QClipboard *clipboard = QApplication::clipboard();
    const QImage image = clipboard ? clipboard->image() : QImage();
    if (image.isNull()) {
        emit statusMessage(tr("剪切板中没有可粘贴的图像"), 2500);
        return;
    }
    if (!m_hasLoadedImage) {
        emit statusMessage(tr("请先打开一张图片，再粘贴图像"), 3000);
        return;
    }

    setFloatingImage(image, tr("图像已粘贴到画布左上角"));
}

// 设置新的浮动图像：放到画布左上角并重置其吸附状态
void PerspectiveCanvas::setFloatingImage(const QImage &image, const QString &statusText)
{
    m_pastedImage = image.convertToFormat(QImage::Format_ARGB32);
    m_pastedImagePosition = QPointF(0, 0);
    m_pastedImageAttached = false;
    m_pastedSurfaceGroup = -1;
    m_pastedHostPlane = -1;
    commitHistory();
    update();
    emit statusMessage(statusText, 3000);
}

// 将浮动图像顺时针旋转 90°
void PerspectiveCanvas::rotateFloatingImage()
{
    if (m_pastedImage.isNull()) {
        emit statusMessage(tr("请先粘贴或拖入一张浮动图像"), 2500);
        return;
    }
    // 直接旋转位图本身，保持其左上角位置不变（无论该位置处于
    // 画布坐标还是共享展开曲面坐标系中）。
    m_pastedImage = m_pastedImage.transformed(QTransform().rotate(90),
                                               Qt::SmoothTransformation);
    commitHistory();
    update();
    emit statusMessage(tr("浮动图像已顺时针旋转 90°"), 2200);
}

// 将浮动图像水平翻转
void PerspectiveCanvas::flipFloatingImageHorizontal()
{
    if (m_pastedImage.isNull()) {
        emit statusMessage(tr("请先粘贴或拖入一张浮动图像"), 2500);
        return;
    }
    m_pastedImage = m_pastedImage.mirrored(true, false);
    commitHistory();
    update();
    emit statusMessage(tr("浮动图像已水平翻转"), 2200);
}

// 将浮动图像垂直翻转
void PerspectiveCanvas::flipFloatingImageVertical()
{
    if (m_pastedImage.isNull()) {
        emit statusMessage(tr("请先粘贴或拖入一张浮动图像"), 2500);
        return;
    }
    m_pastedImage = m_pastedImage.mirrored(false, true);
    commitHistory();
    update();
    emit statusMessage(tr("浮动图像已垂直翻转"), 2200);
}

// 恢复到指定的历史快照，并清空一切进行中的交互状态
void PerspectiveCanvas::restoreState(const CanvasState &state)
{
    m_planes = state.planes;
    m_selectedPlane = state.selectedPlane;
    m_pastedImage = state.pastedImage;
    m_pastedImagePosition = state.pastedImagePosition;
    m_pastedImageAttached = state.pastedImageAttached;
    m_pastedSurfaceGroup = state.pastedSurfaceGroup;
    m_pastedHostPlane = state.pastedHostPlane;
    m_creationPoints.clear();
    m_dragging = m_drawing = m_extruding = false;
    m_draggingPastedImage = false;
    m_hasExtrudePreview = false;
    m_stateChanged = false;
    update();
}

// 清空历史并以当前状态作为初始快照（用于加载新文档）
void PerspectiveCanvas::resetHistory()
{
    m_history.clear();
    m_history.append(captureState());
    m_historyIndex = 0;
    m_stateChanged = false;
    emit canUndoChanged(false);
    emit canRedoChanged(false);
}

// 提交一次状态变更：丢弃旧的重做分支，追加新快照并裁剪历史长度
void PerspectiveCanvas::commitHistory()
{
    while (m_history.size() > m_historyIndex + 1)
        m_history.removeLast();
    m_history.append(captureState());
    ++m_historyIndex;
    if (m_history.size() > MaxHistoryStates) {
        m_history.removeFirst();
        --m_historyIndex;
    }
    m_stateChanged = false;
    emit canUndoChanged(m_historyIndex > 0);
    emit canRedoChanged(false);
}

// 撤销：回退到上一份快照
void PerspectiveCanvas::undo()
{
    if (m_historyIndex <= 0)
        return;
    restoreState(m_history[--m_historyIndex]);
    emit canUndoChanged(m_historyIndex > 0);
    emit canRedoChanged(true);
    emit statusMessage(tr("已撤销"), 1800);
}

// 重做：前进到下一份快照
void PerspectiveCanvas::redo()
{
    if (m_historyIndex + 1 >= m_history.size())
        return;
    restoreState(m_history[++m_historyIndex]);
    emit canUndoChanged(true);
    emit canRedoChanged(m_historyIndex + 1 < m_history.size());
    emit statusMessage(tr("已重做"), 1800);
}

// 切换当前工具，并清理进行中的交互状态、更新光标与提示
void PerspectiveCanvas::setTool(Tool tool)
{
    m_tool = tool;
    m_creationPoints.clear();
    m_dragging = m_drawing = false;
    m_draggingPastedImage = false;
    setCursor(tool == EditPlane ? Qt::SizeAllCursor : Qt::CrossCursor);
    const QString messages[] = {
        tr("依次单击四个角点以创建平面"),
        tr("拖动控制点或平面；按住 Ctrl 从边缘拖出垂直于当前平面的平面"),
        tr("Alt+单击设置仿制源，然后拖动进行仿制"),
        tr("在平面内拖动进行透视绘画")
    };
    emit statusMessage(messages[tool]);
    update();
}

// 根据控件尺寸计算“适应窗口”的缩放比例与居中偏移
void PerspectiveCanvas::updateViewTransform()
{
    if (m_background.isNull())
        return;
    const qreal sx = (width() - 32.0) / m_background.width();
    const qreal sy = (height() - 32.0) / m_background.height();
    m_scale = qMin(sx, sy);
    if (m_scale <= 0)
        m_scale = 1.0;
    const QSizeF shown = QSizeF(m_background.size()) * m_scale;
    m_offset = QPointF((width() - shown.width()) / 2.0, (height() - shown.height()) / 2.0);
}

// 控件坐标 -> 原始图像坐标
QPointF PerspectiveCanvas::toImage(const QPointF &point) const
{
    return (point - m_offset) / m_scale;
}

// 原始图像坐标 -> 控件坐标
QPointF PerspectiveCanvas::toWidget(const QPointF &point) const
{
    return point * m_scale + m_offset;
}

// 尺寸变化时重新计算视图变换
void PerspectiveCanvas::resizeEvent(QResizeEvent *)
{
    updateViewTransform();
}

// 绘制整个画布：设置抗锯齿与平滑缩放，按视图变换渲染场景
void PerspectiveCanvas::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), QColor("#191b1e"));
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    painter.translate(m_offset);
    painter.scale(m_scale, m_scale);
    renderScene(painter, true);
}

// 渲染完整场景。showGuides 为 true 时额外绘制编辑辅助元素。
void PerspectiveCanvas::renderScene(QPainter &painter, bool showGuides) const
{
    // 画布背景属于文档输出的一部分，总是被渲染。showGuides 标志
    // 只控制下方这些编辑器叠加层的绘制。
    painter.drawImage(QPointF(0, 0), m_background);
    if (showGuides && !m_hasLoadedImage && m_planes.isEmpty() && m_creationPoints.isEmpty()) {
        painter.save();
        painter.setPen(QColor("#89919b"));
        QFont placeholderFont = painter.font();
        placeholderFont.setPointSize(20);
        painter.setFont(placeholderFont);
        painter.drawText(m_background.rect(), Qt::AlignCenter,
                         tr("请打开一张图片开始操作"));
        painter.restore();
    }
    // 先绘制每个平面的绘画层。剪贴板图像在被显式移除之前，
    // 始终作为一个可直接移动的顶层图层存在。
    for (const Plane &plane : m_planes) {
        renderProjectedImage(painter, plane, plane.paint);
    }
    renderPastedImage(painter);

    if (!showGuides)
        return;
    for (int i = 0; i < m_planes.size(); ++i)
        drawPlaneGuides(painter, m_planes[i], i == m_selectedPlane);
    if (m_hasExtrudePreview)
        drawPlaneGuides(painter, m_extrudePreview, true);

    painter.save();
    painter.setPen(QPen(QColor("#4bc3ff"), 2.0 / m_scale));
    painter.setBrush(QColor("#4bc3ff"));
    for (int i = 0; i < m_creationPoints.size(); ++i) {
        const QPointF &point = m_creationPoints[i];
        painter.drawEllipse(point, 4.5 / m_scale, 4.5 / m_scale);
        if (i > 0)
            painter.drawLine(m_creationPoints[i - 1], point);
    }
    painter.restore();
}

// 用单应变换把纹理投影到平面的四边形上
void PerspectiveCanvas::renderProjectedImage(QPainter &painter, const Plane &plane,
                                               const QImage &texture) const
{
    if (texture.isNull())
        return;
    const QPolygonF source{QPointF(0, 0), QPointF(texture.width(), 0),
                           QPointF(texture.width(), texture.height()), QPointF(0, texture.height())};
    // quadToQuad() 生成所需的单应变换，使纹理的每个像素都落在四点
    // 平面上正确的透视位置。
    QTransform projection;
    if (!QTransform::quadToQuad(source, planePolygon(plane.corner), projection))
        return;
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setWorldTransform(projection, true);
    painter.drawImage(QPointF(0, 0), texture);
    painter.restore();
}

// 归一化 UV 坐标 -> 平面上的图像坐标
QPointF PerspectiveCanvas::uvToPlane(const Plane &plane, const QPointF &uv) const
{
    const QPolygonF unit{QPointF(0, 0), QPointF(1, 0), QPointF(1, 1), QPointF(0, 1)};
    QTransform transform;
    if (!QTransform::quadToQuad(unit, planePolygon(plane.corner), transform))
        return {};
    return transform.map(uv);
}

// 平面上的图像坐标 -> 归一化 UV 坐标（ok 返回变换是否有效）
QPointF PerspectiveCanvas::planeToUv(const Plane &plane, const QPointF &point, bool *ok) const
{
    QTransform transform;
    const QPolygonF unit{QPointF(0, 0), QPointF(1, 0), QPointF(1, 1), QPointF(0, 1)};
    const bool valid = QTransform::quadToQuad(planePolygon(plane.corner), unit, transform);
    if (ok)
        *ok = valid;
    return valid ? transform.map(point) : QPointF();
}

// 平面上的图像坐标 -> 该平面所属分组的共享展开曲面坐标
QPointF PerspectiveCanvas::planeToSurface(const Plane &plane, const QPointF &point,
                                           bool *ok) const
{
    QTransform transform;
    const bool valid = QTransform::quadToQuad(planePolygon(plane.corner),
                                               planePolygon(plane.surfaceCorner), transform);
    if (ok)
        *ok = valid;
    return valid ? transform.map(point) : QPointF();
}

// 渲染浮动图像：未吸附时直接绘制；已吸附时按宿主平面及相邻面的
// 单应变换分段投影，使图像可以跨越共享接缝。
void PerspectiveCanvas::renderPastedImage(QPainter &painter) const
{
    if (m_pastedImage.isNull())
        return;
    if (!m_pastedImageAttached) {
        painter.drawImage(m_pastedImagePosition, m_pastedImage);
        return;
    }

    int host = m_pastedHostPlane;
    if (host < 0 || host >= m_planes.size() ||
        m_planes[host].surfaceGroup != m_pastedSurfaceGroup) {
        host = -1;
        for (int i = 0; i < m_planes.size(); ++i) {
            if (m_planes[i].surfaceGroup == m_pastedSurfaceGroup) {
                host = i;
                break;
            }
        }
    }
    if (host < 0) {
        painter.drawImage(m_pastedImagePosition, m_pastedImage);
        return;
    }

    const QRectF imageRect(QPointF(0, 0), QSizeF(m_pastedImage.size()));
    QPainterPath hostClip;
    hostClip.addRect(imageRect);

    auto sourcePolygon = [this](const Plane &plane) {
        QPolygonF polygon;
        for (const QPointF &corner : plane.surfaceCorner)
            polygon << corner - m_pastedImagePosition;
        return polygon;
    };
    auto polygonPath = [](const QPolygonF &polygon) {
        QPainterPath path;
        path.addPolygon(polygon);
        path.closeSubpath();
        return path;
    };

    // 先从宿主投影中减去属于其他面的像素，再用那个面的单应变换重绘。
    // 这样接缝处不会出现重影，同时图像仍可延伸到有限网格之外。
    for (int i = 0; i < m_planes.size(); ++i) {
        if (i == host || m_planes[i].surfaceGroup != m_pastedSurfaceGroup)
            continue;
        hostClip = hostClip.subtracted(polygonPath(sourcePolygon(m_planes[i])));
    }

    auto drawFace = [this, &painter, &sourcePolygon](const Plane &plane,
                                                     const QPainterPath &clip) {
        const QPolygonF source = sourcePolygon(plane);
        QTransform projection;
        if (!QTransform::quadToQuad(source, planePolygon(plane.corner), projection))
            return;
        painter.save();
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.setWorldTransform(projection, true);
        // 裁剪路径表达在源图像坐标系中，因此要先把图像到画布的变换
        // 安装到 QPainter，再传入该路径。
        painter.setClipPath(clip, Qt::IntersectClip);
        painter.drawImage(QPointF(0, 0), m_pastedImage);
        painter.restore();
    };

    drawFace(m_planes[host], hostClip);
    for (int i = 0; i < m_planes.size(); ++i) {
        if (i == host || m_planes[i].surfaceGroup != m_pastedSurfaceGroup)
            continue;
        drawFace(m_planes[i], polygonPath(sourcePolygon(m_planes[i])));
    }
}

// 命中测试：判断某个画布坐标是否落在浮动图像上。
// 已吸附时可返回该点在浮动图像内的局部坐标及其所在平面索引。
bool PerspectiveCanvas::pastedImageAt(const QPointF &canvasPoint, QPointF *imagePoint,
                                       int *planeIndex) const
{
    if (m_pastedImage.isNull())
        return false;
    const QRectF imageRect(QPointF(0, 0), QSizeF(m_pastedImage.size()));
    if (!m_pastedImageAttached) {
        const QPointF local = canvasPoint - m_pastedImagePosition;
        if (!imageRect.contains(local))
            return false;
        if (imagePoint)
            *imagePoint = local;
        if (planeIndex)
            *planeIndex = -1;
        return true;
    }

    // 优先选择指针实际所在的面，这样共享边将归属当前显示在顶部的面。
    for (int i = m_planes.size() - 1; i >= 0; --i) {
        const Plane &plane = m_planes[i];
        if (plane.surfaceGroup != m_pastedSurfaceGroup ||
            !planePolygon(plane.corner).containsPoint(canvasPoint, Qt::OddEvenFill))
            continue;
        bool ok = false;
        const QPointF local = planeToSurface(plane, canvasPoint, &ok) -
                              m_pastedImagePosition;
        if (ok && imageRect.contains(local)) {
            if (imagePoint)
                *imagePoint = local;
            if (planeIndex)
                *planeIndex = i;
            return true;
        }
    }

    // 宿主投影有意延伸到其有限网格之外，因此其可见的延伸部分
    // 也必须保持可拖动。
    if (m_pastedHostPlane >= 0 && m_pastedHostPlane < m_planes.size()) {
        bool ok = false;
        const QPointF local = planeToSurface(m_planes[m_pastedHostPlane], canvasPoint, &ok) -
                              m_pastedImagePosition;
        if (ok && imageRect.contains(local)) {
            if (imagePoint)
                *imagePoint = local;
            if (planeIndex)
                *planeIndex = m_pastedHostPlane;
            return true;
        }
    }
    return false;
}

// 返回平面的 8 个控制点：4 个角点在前，4 个边中点在后
QVector<QPointF> PerspectiveCanvas::handles(const Plane &plane) const
{
    return {plane.corner[0], plane.corner[1], plane.corner[2], plane.corner[3],
            (plane.corner[0] + plane.corner[1]) / 2.0,
            (plane.corner[1] + plane.corner[2]) / 2.0,
            (plane.corner[2] + plane.corner[3]) / 2.0,
            (plane.corner[3] + plane.corner[0]) / 2.0};
}

// 命中测试：返回距离点最近的控制点索引（考虑视图缩放后的拾取半径）
int PerspectiveCanvas::handleAt(const Plane &plane, const QPointF &point) const
{
    const QVector<QPointF> hs = handles(plane);
    const qreal radius = 10.0 / m_scale;
    for (int i = 0; i < hs.size(); ++i) {
        if (QLineF(hs[i], point).length() <= radius)
            return i;
    }
    return -1;
}

// 计算点 p 到线段 ab 的距离；t 返回最近点在线段上的参数化位置（0~1）
qreal PerspectiveCanvas::distanceToSegment(const QPointF &p, const QPointF &a,
                                            const QPointF &b, qreal *t)
{
    const QPointF d = b - a;
    const qreal len2 = QPointF::dotProduct(d, d);
    const qreal amount = len2 < Epsilon ? 0.0
                                        : qBound(0.0, QPointF::dotProduct(p - a, d) / len2, 1.0);
    if (t)
        *t = amount;
    return QLineF(p, a + d * amount).length();
}

// 校验平面是否为可用的单应变换目标：必须是非交叉的凸四边形，
// 且不能过于退化（边过短、面积过小或分母过零）。
bool PerspectiveCanvas::isValidPlane(const Plane &plane)
{
    // 射影变换把单位正方形映射为简单的凸四边形。
    // 必须在进入 quadToQuad() 之前拒绝凹形、自交叉和近乎退化的
    // 配置，否则变换的极点可能穿过平面。
    qreal windingSign = 0.0;
    qreal twiceArea = 0.0;
    for (int i = 0; i < 4; ++i) {
        const QPointF a = plane.corner[i];
        const QPointF b = plane.corner[(i + 1) % 4];
        const QPointF c = plane.corner[(i + 2) % 4];
        if (QLineF(a, b).length() < 8.0)
            return false;
        const QPointF ab = b - a;
        const QPointF bc = c - b;
        const qreal cross = ab.x() * bc.y() - ab.y() * bc.x();
        if (qAbs(cross) < 4.0)
            return false;
        const qreal sign = cross > 0.0 ? 1.0 : -1.0;
        if (i == 0)
            windingSign = sign;
        else if (sign != windingSign)
            return false;
        twiceArea += a.x() * b.y() - b.x() * a.y();
    }
    if (qAbs(twiceArea) < 100.0)
        return false;

    const QPolygonF unit{QPointF(0, 0), QPointF(1, 0), QPointF(1, 1), QPointF(0, 1)};
    QTransform transform;
    if (!QTransform::quadToQuad(unit, planePolygon(plane.corner), transform))
        return false;

    // 齐次分母必须在完整的单位正方形上保持同一符号。
    // 由于它对 u/v 是线性的，只需检查四个角即可。
    qreal denominatorSign = 0.0;
    for (const QPointF &uv : unit) {
        const qreal w = transform.m13() * uv.x() + transform.m23() * uv.y() + transform.m33();
        if (!qIsFinite(w) || qAbs(w) < 1e-5)
            return false;
        const qreal sign = w > 0.0 ? 1.0 : -1.0;
        if (denominatorSign == 0.0)
            denominatorSign = sign;
        else if (sign != denominatorSign)
            return false;
    }
    return true;
}

// 命中测试：返回点靠近的边缘索引（0~3），无命中返回 -1
int PerspectiveCanvas::edgeAt(const Plane &plane, const QPointF &point) const
{
    const qreal tolerance = 9.0 / m_scale;
    for (int i = 0; i < 4; ++i) {
        if (distanceToSegment(point, plane.corner[i], plane.corner[(i + 1) % 4]) <= tolerance)
            return i;
    }
    return -1;
}

// 命中测试：返回点所在的最上层平面索引（后创建的优先），无命中返回 -1
int PerspectiveCanvas::planeAt(const QPointF &point) const
{
    for (int i = m_planes.size() - 1; i >= 0; --i) {
        if (planePolygon(m_planes[i].corner).containsPoint(point, Qt::OddEvenFill))
            return i;
    }
    return -1;
}

// 绘制平面的编辑辅助元素：外框、内部网格，以及选中且处于编辑
// 工具时的控制点方块。
void PerspectiveCanvas::drawPlaneGuides(QPainter &painter, const Plane &plane, bool selected) const
{
    painter.save();
    const qreal lineWidth = (selected ? 1.7 : 1.0) / m_scale;
    const QColor color = selected ? QColor(52, 195, 255, 230) : QColor(70, 155, 210, 160);
    painter.setPen(QPen(color, lineWidth));
    painter.setBrush(Qt::NoBrush);
    painter.drawPolygon(planePolygon(plane.corner));

    painter.save();
    QPainterPath planeClip;
    planeClip.addPolygon(planePolygon(plane.corner));
    planeClip.closeSubpath();
    painter.setClipPath(planeClip, Qt::IntersectClip);
    painter.setPen(QPen(QColor(65, 182, 235, selected ? 145 : 80), 0.8 / m_scale));
    constexpr int divisions = 8;
    for (int i = 1; i < divisions; ++i) {
        const qreal t = qreal(i) / divisions;
        painter.drawLine(uvToPlane(plane, QPointF(t, 0)), uvToPlane(plane, QPointF(t, 1)));
        painter.drawLine(uvToPlane(plane, QPointF(0, t)), uvToPlane(plane, QPointF(1, t)));
    }
    painter.restore();

    if (selected && m_tool == EditPlane) {
        const QVector<QPointF> hs = handles(plane);
        for (int i = 0; i < hs.size(); ++i) {
            const qreal radius = (i < 4 ? 5.5 : 4.5) / m_scale;
            painter.setPen(QPen(QColor("#0e526e"), 1.0 / m_scale));
            painter.setBrush(i < 4 ? QColor("#f3f8fa") : QColor("#4bc3ff"));
            painter.drawRect(QRectF(hs[i].x() - radius, hs[i].y() - radius,
                                    radius * 2, radius * 2));
        }
    }
    painter.restore();
}

// 鼠标按下事件：按当前工具分派（拖动浮动图像/创建平面/编辑平面/落笔）
void PerspectiveCanvas::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    setFocus();
    if (!m_hasLoadedImage) {
        emit statusMessage(tr("请先打开一张图片，然后再创建平面或进行编辑。"), 3500);
        return;
    }
    const QPointF point = toImage(event->position());
    if (!m_background.rect().contains(point.toPoint()))
        return;

    QPointF grabbedImagePoint;
    int grabbedPlane = -1;
    if (pastedImageAt(point, &grabbedImagePoint, &grabbedPlane)) {
        m_draggingPastedImage = true;
        m_pastedDragStartPosition = m_pastedImagePosition;
        m_pastedDragStartAttached = m_pastedImageAttached;
        m_pastedDragStartSurfaceGroup = m_pastedSurfaceGroup;
        m_pastedDragStartHostPlane = m_pastedHostPlane;
        m_pastedDragOffset = grabbedImagePoint;
        m_pressImagePoint = point;
        m_stateChanged = false;
        setCursor(Qt::ClosedHandCursor);
        return;
    }

    if (m_tool == CreatePlane) {
        m_creationPoints.append(point);
        if (m_creationPoints.size() == 4) {
            Plane plane;
            for (int i = 0; i < 4; ++i)
                plane.corner[i] = m_creationPoints[i];
            const qreal surfaceWidth = qMax(1.0,
                (QLineF(plane.corner[0], plane.corner[1]).length() +
                 QLineF(plane.corner[3], plane.corner[2]).length()) / 2.0);
            const qreal surfaceHeight = qMax(1.0,
                (QLineF(plane.corner[0], plane.corner[3]).length() +
                 QLineF(plane.corner[1], plane.corner[2]).length()) / 2.0);
            plane.surfaceCorner[0] = QPointF(0, 0);
            plane.surfaceCorner[1] = QPointF(surfaceWidth, 0);
            plane.surfaceCorner[2] = QPointF(surfaceWidth, surfaceHeight);
            plane.surfaceCorner[3] = QPointF(0, surfaceHeight);
            int nextSurfaceGroup = 0;
            for (const Plane &existing : m_planes)
                nextSurfaceGroup = qMax(nextSurfaceGroup, existing.surfaceGroup + 1);
            plane.surfaceGroup = nextSurfaceGroup;
            plane.paint = QImage(TextureSize, TextureSize, QImage::Format_ARGB32);
            plane.paint.fill(Qt::transparent);
            plane.name = tr("平面 %1").arg(m_planes.size() + 1);
            if (isValidPlane(plane)) {
                m_planes.append(plane);
                m_selectedPlane = m_planes.size() - 1;
                commitHistory();
                emit statusMessage(tr("平面已创建，已自动进入编辑工具。"), 4000);
                emit toolChangeRequested(EditPlane);
            } else {
                emit statusMessage(tr("无法创建：四个点必须依次组成非交叉的凸四边形，请重新设置。"), 5000);
            }
            m_creationPoints.clear();
        } else {
            emit statusMessage(tr("已设置 %1/4 个角点").arg(m_creationPoints.size()), 2000);
        }
        update();
        return;
    }

    if (m_tool == EditPlane) {
        int candidate = m_selectedPlane;
        if (candidate >= 0) {
            m_dragHandle = handleAt(m_planes[candidate], point);
            m_dragEdge = edgeAt(m_planes[candidate], point);
        }
        if (m_dragHandle < 0 && candidate >= 0 && m_dragEdge < 0 &&
            !planePolygon(m_planes[candidate].corner).containsPoint(point, Qt::OddEvenFill)) {
            candidate = -1;
        }
        if (candidate < 0) {
            candidate = planeAt(point);
            if (candidate >= 0) {
                m_dragHandle = handleAt(m_planes[candidate], point);
                m_dragEdge = edgeAt(m_planes[candidate], point);
            }
        }
        m_selectedPlane = candidate;
        if (candidate >= 0) {
            m_dragStartPlane = m_planes[candidate];
            m_pressImagePoint = point;
            m_lastImagePoint = point;
            m_dragging = true;
            m_extruding = (event->modifiers() & Qt::ControlModifier) && m_dragEdge >= 0;
            if (m_extruding) {
                m_extrudePreview = makePerpendicularPlane(m_planes[candidate], m_dragEdge, point);
                m_hasExtrudePreview = true;
            }
        }
        update();
        return;
    }

    const int planeIndex = planeAt(point);
    if (planeIndex < 0) {
        emit statusMessage(tr("请在一个透视平面内绘制"), 2500);
        return;
    }
    m_selectedPlane = planeIndex;
    bool ok = false;
    const QPointF uv = planeToUv(m_planes[planeIndex], point, &ok);
    if (!ok)
        return;
    if (m_tool == StampTool && (event->modifiers() & Qt::AltModifier)) {
        m_cloneSourceUv = uv;
        m_cloneSourcePlane = planeIndex;
        m_hasCloneSource = true;
        m_cloneStrokeStarted = false;
        emit statusMessage(tr("仿制源已设置。现在可单击并拖动进行仿制。"), 3500);
        update();
        return;
    }
    if (m_tool == StampTool && !m_hasCloneSource) {
        emit statusMessage(tr("请先按住 Alt 并在平面中单击，以设置仿制源"), 3500);
        return;
    }
    m_drawing = true;
    m_lastImagePoint = point;
    m_lastUv = uv;
    if (m_tool == StampTool) {
        m_cloneAnchorUv = uv;
        m_cloneStrokeStarted = true;
    }
    applyDab(m_planes[planeIndex], uv, m_tool == StampTool);
    m_stateChanged = true;
    update();
}

// 鼠标移动事件：处理拖动浮动图像、编辑平面、绘制笔迹，或更新悬停光标
void PerspectiveCanvas::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF point = toImage(event->position());
    if (m_draggingPastedImage && (event->buttons() & Qt::LeftButton)) {
        const int targetPlane = planeAt(point);
        bool mapped = false;
        if (targetPlane >= 0) {
            bool ok = false;
            const QPointF surfacePoint = planeToSurface(m_planes[targetPlane], point, &ok);
            if (ok) {
                m_pastedImageAttached = true;
                m_pastedSurfaceGroup = m_planes[targetPlane].surfaceGroup;
                m_pastedHostPlane = targetPlane;
                m_pastedImagePosition = surfacePoint - m_pastedDragOffset;
                mapped = true;
            }
        }
        if (!mapped && m_pastedImageAttached && m_pastedHostPlane >= 0 &&
            m_pastedHostPlane < m_planes.size()) {
            bool ok = false;
            const QPointF surfacePoint =
                planeToSurface(m_planes[m_pastedHostPlane], point, &ok);
            if (ok) {
                m_pastedImagePosition = surfacePoint - m_pastedDragOffset;
                mapped = true;
            }
        }
        if (!mapped) {
            m_pastedImageAttached = false;
            m_pastedSurfaceGroup = -1;
            m_pastedHostPlane = -1;
            m_pastedImagePosition = point - m_pastedDragOffset;
        }
        m_stateChanged = m_pastedImagePosition != m_pastedDragStartPosition ||
                         m_pastedImageAttached != m_pastedDragStartAttached ||
                         m_pastedSurfaceGroup != m_pastedDragStartSurfaceGroup ||
                         m_pastedHostPlane != m_pastedDragStartHostPlane;
        update();
        return;
    }
    if (m_tool == EditPlane && m_dragging && m_selectedPlane >= 0) {
        Plane &plane = m_planes[m_selectedPlane];
        if (m_extruding) {
            m_extrudePreview = makePerpendicularPlane(m_dragStartPlane, m_dragEdge,
                                                       point);
            m_hasExtrudePreview = isValidPlane(m_extrudePreview);
        } else if (m_dragHandle >= 0 && m_dragHandle < 4) {
            Plane candidate = m_dragStartPlane;
            candidate.corner[m_dragHandle] = point;
            if (isValidPlane(candidate))
            {
                plane = candidate;
                m_stateChanged = true;
            }
        } else if (m_dragHandle >= 4) {
            const int edge = m_dragHandle - 4;
            const Plane candidate = resizePlaneAlongEdge(m_dragStartPlane, edge, point);
            if (isValidPlane(candidate))
            {
                plane = candidate;
                m_stateChanged = true;
            }
        } else {
            const QPointF delta = point - m_pressImagePoint;
            for (int i = 0; i < 4; ++i)
                plane.corner[i] = m_dragStartPlane.corner[i] + delta;
            m_stateChanged = !delta.isNull();
        }
        m_lastImagePoint = point;
        update();
        return;
    }
    if (m_drawing && (event->buttons() & Qt::LeftButton)) {
        drawStrokeTo(point, m_tool == StampTool);
        update();
        return;
    }
    if (!m_dragging)
        updateHoverCursor(point);
}

// 根据悬停位置更新鼠标光标形状（抓手/十字/方向缩放等）
void PerspectiveCanvas::updateHoverCursor(const QPointF &imagePoint)
{
    if (pastedImageAt(imagePoint)) {
        setCursor(Qt::OpenHandCursor);
        return;
    }
    if (m_tool != EditPlane) {
        setCursor(Qt::CrossCursor);
        return;
    }
    if (m_selectedPlane < 0 || m_selectedPlane >= m_planes.size()) {
        setCursor(Qt::ArrowCursor);
        return;
    }

    const Plane &plane = m_planes[m_selectedPlane];
    const int handle = handleAt(plane, imagePoint);
    if (handle >= 4) {
        const int edge = handle - 4;
        const QPointF edgeMidpoint = (plane.corner[edge] +
                                      plane.corner[(edge + 1) % 4]) / 2.0;
        const QPointF oppositeMidpoint = (plane.corner[(edge + 2) % 4] +
                                          plane.corner[(edge + 3) % 4]) / 2.0;
        const QPointF axis = edgeMidpoint - oppositeMidpoint;
        const qreal ax = qAbs(axis.x());
        const qreal ay = qAbs(axis.y());

        if (ax < ay * 0.42)
            setCursor(Qt::SizeVerCursor);
        else if (ay < ax * 0.42)
            setCursor(Qt::SizeHorCursor);
        else if (axis.x() * axis.y() >= 0.0)
            setCursor(Qt::SizeFDiagCursor);
        else
            setCursor(Qt::SizeBDiagCursor);
        return;
    }
    if (handle >= 0) {
        setCursor(Qt::CrossCursor);
        return;
    }
    if (planePolygon(plane.corner).containsPoint(imagePoint, Qt::OddEvenFill))
        setCursor(Qt::SizeAllCursor);
    else
        setCursor(Qt::ArrowCursor);
}

// 鼠标释放事件：结束交互，如有变更则提交历史记录
void PerspectiveCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    if (m_extruding && m_hasExtrudePreview) {
        const qreal area = qAbs(QPolygonF(planePolygon(m_extrudePreview.corner)).boundingRect().width() *
                                QPolygonF(planePolygon(m_extrudePreview.corner)).boundingRect().height());
        if (area > 100.0) {
            m_extrudePreview.paint = QImage(TextureSize, TextureSize, QImage::Format_ARGB32);
            m_extrudePreview.paint.fill(Qt::transparent);
            m_extrudePreview.name = tr("平面 %1").arg(m_planes.size() + 1);
            m_planes.append(m_extrudePreview);
            m_selectedPlane = m_planes.size() - 1;
            m_stateChanged = true;
            emit statusMessage(tr("已创建相邻的垂直平面"), 3000);
        }
    }
    m_dragging = m_drawing = m_extruding = false;
    m_draggingPastedImage = false;
    m_hasExtrudePreview = false;
    m_cloneStrokeStarted = false;
    m_dragHandle = m_dragEdge = -1;
    if (m_stateChanged)
        commitHistory();
    updateHoverCursor(toImage(event->position()));
    update();
}

// 沿某条边方向缩放平面：只改变该边到对边的距离，保持透视关系不变
PerspectiveCanvas::Plane PerspectiveCanvas::resizePlaneAlongEdge(const Plane &source, int edge,
                                                                  const QPointF &dragPoint) const
{
    Plane result = source;
    const int next = (edge + 1) % 4;
    const int oppositeNext = (edge + 2) % 4;
    const int previous = (edge + 3) % 4;
    const QPointF a = source.corner[edge];
    const QPointF b = source.corner[next];
    const QPointF oppositeA = source.corner[oppositeNext];
    const QPointF oppositeB = source.corner[previous];
    const QPointF edgeMidpoint = (a + b) / 2.0;
    const QPointF oppositeMidpoint = (oppositeA + oppositeB) / 2.0;

    // 沿边缩放只有一个自由度。忽略指针的侧向移动，
    // 只保留沿平面既有延伸轴方向的位移量。
    QPointF extensionAxis = edgeMidpoint - oppositeMidpoint;
    qreal axisLength = QLineF(QPointF(), extensionAxis).length();
    if (axisLength < Epsilon) {
        extensionAxis = QPointF(-(b - a).y(), (b - a).x());
        axisLength = QLineF(QPointF(), extensionAxis).length();
    }
    if (axisLength < Epsilon)
        return result;
    extensionAxis /= axisLength;
    const qreal extension = QPointF::dotProduct(dragPoint - m_pressImagePoint, extensionAxis);
    const QPointF targetPoint = edgeMidpoint + extensionAxis * extension;

    // 缩放后的边必须保留原边方向的消失点。
    QPointF edgeVanishingPoint;
    const auto vpType = QLineF(a, b).intersects(QLineF(oppositeA, oppositeB),
                                                &edgeVanishingPoint);
    QLineF resizedEdge;
    if (vpType != QLineF::NoIntersection && qIsFinite(edgeVanishingPoint.x()) &&
        qIsFinite(edgeVanishingPoint.y()) &&
        QLineF(edgeVanishingPoint, targetPoint).length() > 1.0 &&
        QLineF(edgeVanishingPoint, edgeMidpoint).length() < 1e7) {
        resizedEdge = QLineF(edgeVanishingPoint, targetPoint);
    } else {
        // 对边平行是消失点位于无穷远处的极限情形。
        resizedEdge = QLineF(targetPoint, targetPoint + (b - a));
    }

    // 每个端点都被约束在它原来所在的侧边线上。
    // 这正是保证“垂直平面在缩放时只改变高度、仍然保持垂直”的原因。
    QPointF movedA;
    QPointF movedB;
    const auto aType = QLineF(a, source.corner[previous]).intersects(resizedEdge, &movedA);
    const auto bType = QLineF(b, source.corner[oppositeNext]).intersects(resizedEdge, &movedB);
    if (aType == QLineF::NoIntersection || bType == QLineF::NoIntersection ||
        !qIsFinite(movedA.x()) || !qIsFinite(movedA.y()) ||
        !qIsFinite(movedB.x()) || !qIsFinite(movedB.y()))
        return result;

    result.corner[edge] = movedA;
    result.corner[next] = movedB;
    return result;
}

// 恢复源平面法线在图像上的投影方向（即第三个消失方向）。
// 该方向被所有垂直于源平面的平面共享。
bool PerspectiveCanvas::perpendicularDirection(const Plane &source, const QPointF &atPoint,
                                                QPointF *direction) const
{
    // 在齐次图像坐标下恢复源平面的两个消失点。
    // 齐次形式同时也能覆盖平行线族（消失点在无穷远）的情形。
    auto imagePoint = [](const QPointF &p) {
        return QVector3D(float(p.x()), float(p.y()), 1.0f);
    };
    const QVector3D p0 = imagePoint(source.corner[0]);
    const QVector3D p1 = imagePoint(source.corner[1]);
    const QVector3D p2 = imagePoint(source.corner[2]);
    const QVector3D p3 = imagePoint(source.corner[3]);
    const QVector3D line01 = QVector3D::crossProduct(p0, p1);
    const QVector3D line32 = QVector3D::crossProduct(p3, p2);
    const QVector3D line03 = QVector3D::crossProduct(p0, p3);
    const QVector3D line12 = QVector3D::crossProduct(p1, p2);
    const QVector3D vanishingX = QVector3D::crossProduct(line01, line32);
    const QVector3D vanishingY = QVector3D::crossProduct(line03, line12);
    if (vanishingX.lengthSquared() < 1e-12f || vanishingY.lengthSquared() < 1e-12f)
        return false;

    const qreal cx = m_background.width() / 2.0;
    const qreal cy = m_background.height() / 2.0;
    const qreal imageExtent = qMax(m_background.width(), m_background.height());
    qreal focalLength = imageExtent * 1.2;

    // 当两个消失点均为有限值、且两条网格轴代表相互正交的世界方向时，
    // 可由正交性解出焦距。
    if (qAbs(vanishingX.z()) > 1e-6 && qAbs(vanishingY.z()) > 1e-6) {
        const QPointF vx(vanishingX.x() / vanishingX.z(),
                         vanishingX.y() / vanishingX.z());
        const QPointF vy(vanishingY.x() / vanishingY.z(),
                         vanishingY.y() / vanishingY.z());
        const qreal inferredFocalSquared =
            -QPointF::dotProduct(vx - QPointF(cx, cy), vy - QPointF(cx, cy));
        const qreal minimumFocal = imageExtent * 0.08;
        const qreal maximumFocal = imageExtent * 20.0;
        if (inferredFocalSquared > minimumFocal * minimumFocal &&
            inferredFocalSquared < maximumFocal * maximumFocal)
            focalLength = qSqrt(inferredFocalSquared);
    }

    auto cameraDirection = [cx, cy, focalLength](const QVector3D &v) {
        return QVector3D(float(v.x() - cx * v.z()),
                         float(v.y() - cy * v.z()),
                         float(focalLength * v.z())).normalized();
    };
    const QVector3D directionX = cameraDirection(vanishingX);
    const QVector3D directionY = cameraDirection(vanishingY);
    QVector3D normal = QVector3D::crossProduct(directionX, directionY);
    if (normal.lengthSquared() < 1e-10f)
        return false;
    normal.normalize();

    // 把 3D 法线经内参矩阵 K 投影回图像。这就是与源平面垂直的所有
    // 平面共享的第三个消失点。
    const qreal projectedX = focalLength * normal.x() + cx * normal.z();
    const qreal projectedY = focalLength * normal.y() + cy * normal.z();
    QPointF projectedDirection;
    if (qAbs(normal.z()) > 1e-6) {
        const QPointF perpendicularVanishingPoint(projectedX / normal.z(),
                                                  projectedY / normal.z());
        projectedDirection = perpendicularVanishingPoint - atPoint;
    } else {
        // 齐次分量 w 为零意味着第三个消失点位于无穷远处。
        projectedDirection = QPointF(projectedX, projectedY);
    }

    const qreal length = QLineF(QPointF(), projectedDirection).length();
    if (!qIsFinite(length) || length < Epsilon)
        return false;
    *direction = projectedDirection / length;
    return true;
}

// 从源平面的一条边拖出与之垂直的新平面（Ctrl+拖动边缘）
PerspectiveCanvas::Plane PerspectiveCanvas::makePerpendicularPlane(const Plane &source, int edge,
                                                                    const QPointF &dragPoint) const
{
    Plane result;
    result.surfaceGroup = source.surfaceGroup;
    result.surfaceCorner[0] = source.surfaceCorner[edge];
    result.surfaceCorner[1] = source.surfaceCorner[(edge + 1) % 4];
    result.surfaceCorner[2] = result.surfaceCorner[1];
    result.surfaceCorner[3] = result.surfaceCorner[0];
    const QPointF a = source.corner[edge];
    const QPointF b = source.corner[(edge + 1) % 4];
    const QPointF midpoint = (a + b) / 2.0;
    QPointF perpendicularAtMidpoint;
    if (!perpendicularDirection(source, midpoint, &perpendicularAtMidpoint))
        return result;

    // 指针只控制沿投影后 3D 法线方向的有符号距离。
    // 侧向移动无法改变垂直平面的角度。
    const qreal amount = QPointF::dotProduct(dragPoint - m_pressImagePoint,
                                             perpendicularAtMidpoint);
    const QPointF targetMidpoint = midpoint + perpendicularAtMidpoint * amount;
    result.corner[0] = a;
    result.corner[1] = b;

    if (qAbs(amount) < 2.0) {
        result.corner[2] = b;
        result.corner[3] = a;
        return result;
    }

    // 共享边与新的外侧边在 3D 中代表同一方向，
    // 因此二者相交于原边线族的消失点。
    const QPointF oppositeA = source.corner[(edge + 2) % 4];
    const QPointF oppositeB = source.corner[(edge + 3) % 4];
    QPointF edgeVanishingPoint;
    const QLineF::IntersectionType vpType =
        QLineF(a, b).intersects(QLineF(oppositeA, oppositeB), &edgeVanishingPoint);

    bool constructedWithVanishingPoint = false;
    if (vpType != QLineF::NoIntersection && qIsFinite(edgeVanishingPoint.x()) &&
        qIsFinite(edgeVanishingPoint.y()) &&
        QLineF(edgeVanishingPoint, (a + b) / 2.0).length() < 1e7) {
        if (QLineF(edgeVanishingPoint, targetMidpoint).length() > 1.0) {
            QPointF outerAtB;
            QPointF outerAtA;
            const QLineF outerLine(edgeVanishingPoint, targetMidpoint);
            QPointF perpendicularAtA;
            QPointF perpendicularAtB;
            if (!perpendicularDirection(source, a, &perpendicularAtA) ||
                !perpendicularDirection(source, b, &perpendicularAtB))
                return result;
            const auto bType = QLineF(b, b + perpendicularAtB).intersects(outerLine, &outerAtB);
            const auto aType = QLineF(a, a + perpendicularAtA).intersects(outerLine, &outerAtA);
            if (bType != QLineF::NoIntersection && aType != QLineF::NoIntersection &&
                qIsFinite(outerAtA.x()) && qIsFinite(outerAtA.y()) &&
                qIsFinite(outerAtB.x()) && qIsFinite(outerAtB.y())) {
                result.corner[2] = outerAtB;
                result.corner[3] = outerAtA;
                constructedWithVanishingPoint = true;
            }
        }
    }

    // 源平面的边相互平行时其消失点在无穷远处，因此外侧边保持平行，
    // 但其端点仍沿第三个（垂直）消失方向移动。
    if (!constructedWithVanishingPoint) {
        const QLineF outerLine(targetMidpoint, targetMidpoint + (b - a));
        QPointF perpendicularAtA;
        QPointF perpendicularAtB;
        QPointF outerAtA;
        QPointF outerAtB;
        if (perpendicularDirection(source, a, &perpendicularAtA) &&
            perpendicularDirection(source, b, &perpendicularAtB) &&
            QLineF(a, a + perpendicularAtA).intersects(outerLine, &outerAtA) !=
                QLineF::NoIntersection &&
            QLineF(b, b + perpendicularAtB).intersects(outerLine, &outerAtB) !=
                QLineF::NoIntersection) {
            result.corner[2] = outerAtB;
            result.corner[3] = outerAtA;
        } else {
            result.corner[2] = b;
            result.corner[3] = a;
        }
    }

    // 把垂直面绕共享边展开到曲面上。两个面在接缝处保持完全相同的
    // 曲面坐标，而新的外侧边被放置在源面内部的另一侧。
    const QPointF surfaceA = result.surfaceCorner[0];
    const QPointF surfaceB = result.surfaceCorner[1];
    const QPointF surfaceEdge = surfaceB - surfaceA;
    const qreal surfaceEdgeLength = QLineF(surfaceA, surfaceB).length();
    const qreal canvasEdgeLength = qMax(Epsilon, QLineF(a, b).length());
    QPointF outward(-surfaceEdge.y(), surfaceEdge.x());
    const qreal outwardLength = QLineF(QPointF(), outward).length();
    if (outwardLength > Epsilon)
        outward /= outwardLength;
    QPointF sourceCenter;
    for (const QPointF &corner : source.surfaceCorner)
        sourceCenter += corner;
    sourceCenter /= 4.0;
    const QPointF seamCenter = (surfaceA + surfaceB) / 2.0;
    if (QPointF::dotProduct(outward, sourceCenter - seamCenter) > 0.0)
        outward = -outward;
    const qreal canvasDepth = (QLineF(result.corner[0], result.corner[3]).length() +
                               QLineF(result.corner[1], result.corner[2]).length()) / 2.0;
    const qreal surfaceDepth = qMax(1.0, canvasDepth * surfaceEdgeLength / canvasEdgeLength);
    result.surfaceCorner[2] = surfaceB + outward * surfaceDepth;
    result.surfaceCorner[3] = surfaceA + outward * surfaceDepth;
    return result;
}

// 从上一个 UV 位置向当前位置插值补间，沿笔迹均匀落下一串笔触点
void PerspectiveCanvas::drawStrokeTo(const QPointF &point, bool stamp)
{
    if (m_selectedPlane < 0 || m_selectedPlane >= m_planes.size())
        return;
    Plane &plane = m_planes[m_selectedPlane];
    bool ok = false;
    const QPointF uv = planeToUv(plane, point, &ok);
    if (!ok || uv.x() < 0 || uv.x() > 1 || uv.y() < 0 || uv.y() > 1)
        return;
    const qreal planeWidth = (QLineF(plane.corner[0], plane.corner[1]).length() +
                              QLineF(plane.corner[3], plane.corner[2]).length()) / 2.0;
    const qreal textureDiameter = m_diameter * TextureSize / qMax(40.0, planeWidth);
    const qreal step = qMax(1.0, textureDiameter * 0.18) / TextureSize;
    const qreal distance = QLineF(m_lastUv, uv).length();
    const int count = qMax(1, int(qCeil(distance / step)));
    for (int i = 1; i <= count; ++i)
        applyDab(plane, m_lastUv + (uv - m_lastUv) * (qreal(i) / count), stamp);
    m_lastUv = uv;
    m_lastImagePoint = point;
}

// 在平面的纹理空间落下一个笔触点（画笔或仿制图章）。
// stamp 为 true 时按仿制偏移从源位置采样颜色。
void PerspectiveCanvas::applyDab(Plane &plane, const QPointF &uv, bool stamp)
{
    // 笔刷尺寸从图像像素换算到平面的归一化纹理，使一个笔触点在
    // 透视作用下保持视觉上的一致大小。
    const qreal planeWidth = (QLineF(plane.corner[0], plane.corner[1]).length() +
                              QLineF(plane.corner[3], plane.corner[2]).length()) / 2.0;
    const qreal radius = qBound(1.0, m_diameter * TextureSize /
                                      (2.0 * qMax(40.0, planeWidth)), 300.0);
    const QPointF center(uv.x() * (TextureSize - 1), uv.y() * (TextureSize - 1));
    const int left = qMax(0, int(qFloor(center.x() - radius)));
    const int right = qMin(TextureSize - 1, int(qCeil(center.x() + radius)));
    const int top = qMax(0, int(qFloor(center.y() - radius)));
    const int bottom = qMin(TextureSize - 1, int(qCeil(center.y() + radius)));
    const qreal softStart = qBound(0.0, m_hardness, 1.0);

    for (int y = top; y <= bottom; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(plane.paint.scanLine(y));
        for (int x = left; x <= right; ++x) {
            const qreal d = qSqrt(qPow(x - center.x(), 2) + qPow(y - center.y(), 2)) / radius;
            if (d > 1.0)
                continue;
            qreal falloff = 1.0;
            if (d > softStart)
                falloff = (1.0 - d) / qMax(0.001, 1.0 - softStart);
            falloff = falloff * falloff * (3.0 - 2.0 * falloff);

            QColor source = m_brushColor;
            if (stamp) {
                const QPointF destinationUv(qreal(x) / (TextureSize - 1),
                                            qreal(y) / (TextureSize - 1));
                const QPointF sourceUv = m_cloneSourceUv + (destinationUv - m_cloneAnchorUv);
                if (sourceUv.x() < 0 || sourceUv.x() > 1 || sourceUv.y() < 0 || sourceUv.y() > 1 ||
                    m_cloneSourcePlane < 0 || m_cloneSourcePlane >= m_planes.size())
                    continue;
                const Plane &sourcePlane = m_planes[m_cloneSourcePlane];
                const QPointF imagePoint = uvToPlane(sourcePlane, sourceUv);
                const int ix = qBound(0, qRound(imagePoint.x()), m_background.width() - 1);
                const int iy = qBound(0, qRound(imagePoint.y()), m_background.height() - 1);
                source = QColor::fromRgba(m_background.pixel(ix, iy));
                const int tx = qBound(0, qRound(sourceUv.x() * (TextureSize - 1)), TextureSize - 1);
                const int ty = qBound(0, qRound(sourceUv.y() * (TextureSize - 1)), TextureSize - 1);
                source = over(source, QColor::fromRgba(sourcePlane.paint.pixel(tx, ty)));
            }
            source.setAlphaF(source.alphaF() * m_opacity * falloff);
            line[x] = over(QColor::fromRgba(line[x]), source).rgba();
        }
    }
}

// 键盘事件：Ctrl+V 粘贴图像、Esc 取消当前操作、Delete 删除选中平面
void PerspectiveCanvas::keyPressEvent(QKeyEvent *event)
{
    if (event->matches(QKeySequence::Paste)) {
        pasteClipboardImage();
        event->accept();
    } else if (event->key() == Qt::Key_Escape) {
        if (m_draggingPastedImage) {
            m_pastedImagePosition = m_pastedDragStartPosition;
            m_pastedImageAttached = m_pastedDragStartAttached;
            m_pastedSurfaceGroup = m_pastedDragStartSurfaceGroup;
            m_pastedHostPlane = m_pastedDragStartHostPlane;
        }
        m_creationPoints.clear();
        m_dragging = m_drawing = m_extruding = false;
        m_draggingPastedImage = false;
        m_stateChanged = false;
        m_hasExtrudePreview = false;
        update();
    } else if (event->key() == Qt::Key_Delete && m_tool == EditPlane && m_selectedPlane >= 0) {
        const int removedPlane = m_selectedPlane;
        m_planes.removeAt(removedPlane);
        if (m_pastedHostPlane > removedPlane) {
            --m_pastedHostPlane;
        } else if (m_pastedHostPlane == removedPlane) {
            m_pastedHostPlane = -1;
            for (int i = 0; i < m_planes.size(); ++i) {
                if (m_planes[i].surfaceGroup == m_pastedSurfaceGroup) {
                    m_pastedHostPlane = i;
                    break;
                }
            }
            if (m_pastedHostPlane < 0) {
                m_pastedImageAttached = false;
                m_pastedSurfaceGroup = -1;
                m_pastedImagePosition = QPointF(0, 0);
            }
        }
        m_selectedPlane = qMin(m_selectedPlane, m_planes.size() - 1);
        commitHistory();
        update();
    } else {
        QWidget::keyPressEvent(event);
    }
}

// Tab / Shift+Tab：在平面之间循环切换选中（仅编辑平面工具）
bool PerspectiveCanvas::focusNextPrevChild(bool next)
{
    if (m_tool != EditPlane || m_planes.isEmpty())
        return QWidget::focusNextPrevChild(next);

    if (m_selectedPlane < 0) {
        m_selectedPlane = next ? 0 : m_planes.size() - 1;
    } else if (next) {
        m_selectedPlane = (m_selectedPlane + 1) % m_planes.size();
    } else {
        m_selectedPlane = (m_selectedPlane - 1 + m_planes.size()) % m_planes.size();
    }
    update();
    emit statusMessage(tr("已选择：%1（%2/%3）")
                           .arg(m_planes[m_selectedPlane].name)
                           .arg(m_selectedPlane + 1)
                           .arg(m_planes.size()),
                       2500);
    return true;
}
