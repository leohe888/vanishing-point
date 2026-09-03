#include "perspectivecanvas.h"

#include "scenerenderer.h"

#include <QApplication>
#include <QClipboard>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QImageReader>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QResizeEvent>

using namespace PlaneMath;

// 构造函数：初始化默认背景并转发文档模型的历史信号
PerspectiveCanvas::PerspectiveCanvas(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAcceptDrops(true);
    setMinimumSize(500, 400);

    QImage background(1200, 800, QImage::Format_ARGB32);
    QPainter p(&background);
    p.fillRect(background.rect(), QColor("#26292d"));
    m_doc.setBackground(background);
    updateViewTransform();

    // 文档模型不知道任何控件，把它的历史信号转发给界面使用者。
    connect(&m_doc, &CanvasDocument::canUndoChanged,
            this, &PerspectiveCanvas::canUndoChanged);
    connect(&m_doc, &CanvasDocument::canRedoChanged,
            this, &PerspectiveCanvas::canRedoChanged);
    connect(&m_doc, &CanvasDocument::documentAvailabilityChanged,
            this, &PerspectiveCanvas::documentAvailabilityChanged);
}

// 从文件加载背景图像，并清空交互状态、仿制源与视图变换
bool PerspectiveCanvas::loadImage(const QString &fileName)
{
    if (!m_doc.loadImage(fileName))
        return false;
    m_creationPoints.clear();
    m_paint.clearCloneSource();
    updateViewTransform();
    update();
    emit statusMessage(tr("图像已打开。请依次点击四个点创建透视平面。"), 5000);
    return true;
}

// 将当前场景（背景 + 各平面绘画 + 浮动图像）导出为白底图像文件
bool PerspectiveCanvas::saveResult(const QString &fileName) const
{
    QImage result(m_doc.background().size(), QImage::Format_ARGB32);
    result.fill(Qt::white);
    QPainter painter(&result);
    SceneRenderer(m_doc).render(painter, 1.0, false);
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
    if (!m_doc.hasLoadedImage()) {
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
        dropFloatingImage(image, tr("图像已放到画布左上角，可拖入透视平面"));
        event->acceptProposedAction();
        return;
    }
}

// 清除所有平面上的绘画内容（不影响平面几何本身）
void PerspectiveCanvas::clearPainting()
{
    if (m_doc.planes().isEmpty())
        return;
    m_doc.clearPainting();
    update();
    emit statusMessage(tr("已清除所有平面上的绘画内容"), 3000);
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
    if (!m_doc.hasLoadedImage()) {
        emit statusMessage(tr("请先打开一张图片，再粘贴图像"), 3000);
        return;
    }

    dropFloatingImage(image, tr("图像已粘贴到画布左上角"));
}

// 把拖入或粘贴的图像设置为新的浮动图像
void PerspectiveCanvas::dropFloatingImage(const QImage &image, const QString &statusText)
{
    m_doc.setFloatingImage(image);
    update();
    emit statusMessage(statusText, 3000);
}

// 将浮动图像顺时针旋转 90°
void PerspectiveCanvas::rotateFloatingImage()
{
    if (!m_doc.rotateFloatingImage()) {
        emit statusMessage(tr("请先粘贴或拖入一张浮动图像"), 2500);
        return;
    }
    update();
    emit statusMessage(tr("浮动图像已顺时针旋转 90°"), 2200);
}

// 将浮动图像水平翻转
void PerspectiveCanvas::flipFloatingImageHorizontal()
{
    if (!m_doc.flipFloatingImage(true, false)) {
        emit statusMessage(tr("请先粘贴或拖入一张浮动图像"), 2500);
        return;
    }
    update();
    emit statusMessage(tr("浮动图像已水平翻转"), 2200);
}

// 将浮动图像垂直翻转
void PerspectiveCanvas::flipFloatingImageVertical()
{
    if (!m_doc.flipFloatingImage(false, true)) {
        emit statusMessage(tr("请先粘贴或拖入一张浮动图像"), 2500);
        return;
    }
    update();
    emit statusMessage(tr("浮动图像已垂直翻转"), 2200);
}

// 撤销：回退到上一份快照并清空进行中的交互
void PerspectiveCanvas::undo()
{
    if (!m_doc.undo())
        return;
    cancelInteraction();
    emit statusMessage(tr("已撤销"), 1800);
}

// 重做：前进到下一份快照并清空进行中的交互
void PerspectiveCanvas::redo()
{
    if (!m_doc.redo())
        return;
    cancelInteraction();
    emit statusMessage(tr("已重做"), 1800);
}

// 清空一切进行中的交互状态（撤销/重做/Esc 取消后调用）
void PerspectiveCanvas::cancelInteraction()
{
    m_creationPoints.clear();
    m_dragging = m_drawing = m_extruding = false;
    m_draggingPastedImage = false;
    m_hasExtrudePreview = false;
    m_stateChanged = false;
    update();
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
    const QImage &background = m_doc.background();
    if (background.isNull())
        return;
    const qreal sx = (width() - 32.0) / background.width();
    const qreal sy = (height() - 32.0) / background.height();
    m_scale = qMin(sx, sy);
    if (m_scale <= 0)
        m_scale = 1.0;
    const QSizeF shown = QSizeF(background.size()) * m_scale;
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
    SceneRenderer renderer(m_doc);
    renderer.render(painter, m_scale, true, m_creationPoints,
                    m_hasExtrudePreview ? &m_extrudePreview : nullptr,
                    m_tool == EditPlane);
}

// 命中测试：判断某个画布坐标是否落在浮动图像上。
// 已吸附时可返回该点在浮动图像内的局部坐标及其所在平面索引。
bool PerspectiveCanvas::pastedImageAt(const QPointF &canvasPoint, QPointF *imagePoint,
                                      int *planeIndex) const
{
    if (m_doc.pastedImage().isNull())
        return false;
    const QRectF imageRect(QPointF(0, 0), QSizeF(m_doc.pastedImage().size()));
    if (!m_doc.pastedImageAttached()) {
        const QPointF local = canvasPoint - m_doc.pastedImagePosition();
        if (!imageRect.contains(local))
            return false;
        if (imagePoint)
            *imagePoint = local;
        if (planeIndex)
            *planeIndex = -1;
        return true;
    }

    // 优先选择指针实际所在的面，这样共享边将归属当前显示在顶部的面。
    for (int i = m_doc.planes().size() - 1; i >= 0; --i) {
        const Plane &plane = m_doc.planes()[i];
        if (plane.surfaceGroup != m_doc.pastedSurfaceGroup() ||
            !planePolygon(plane.corner).containsPoint(canvasPoint, Qt::OddEvenFill))
            continue;
        bool ok = false;
        const QPointF local = planeToSurface(plane, canvasPoint, &ok) -
                              m_doc.pastedImagePosition();
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
    const int hostPlane = m_doc.pastedHostPlane();
    if (hostPlane >= 0 && hostPlane < m_doc.planes().size()) {
        bool ok = false;
        const QPointF local = planeToSurface(m_doc.planes()[hostPlane], canvasPoint, &ok) -
                              m_doc.pastedImagePosition();
        if (ok && imageRect.contains(local)) {
            if (imagePoint)
                *imagePoint = local;
            if (planeIndex)
                *planeIndex = hostPlane;
            return true;
        }
    }
    return false;
}

// 用当前 4 个创建角点生成平面；有效时追加到文档并进入编辑工具
void PerspectiveCanvas::finishPlaneCreation()
{
    Plane plane;
    for (int i = 0; i < 4; ++i)
        plane.corner[i] = m_creationPoints[i];
    // 展开曲面以角点平均边长初始化为一个矩形。
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
    plane.surfaceGroup = m_doc.nextSurfaceGroupId();
    plane.paint = QImage(TextureSize, TextureSize, QImage::Format_ARGB32);
    plane.paint.fill(Qt::transparent);
    plane.name = tr("平面 %1").arg(m_doc.planes().size() + 1);
    if (isValidPlane(plane)) {
        m_doc.planes().append(plane);
        m_doc.setSelectedPlane(m_doc.planes().size() - 1);
        m_doc.commitHistory();
        emit statusMessage(tr("平面已创建，已自动进入编辑工具。"), 4000);
        emit toolChangeRequested(EditPlane);
    } else {
        emit statusMessage(tr("无法创建：四个点必须依次组成非交叉的凸四边形，请重新设置。"), 5000);
    }
    m_creationPoints.clear();
}

// 鼠标按下事件：按当前工具分派（拖动浮动图像/创建平面/编辑平面/落笔）
void PerspectiveCanvas::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    setFocus();
    if (!m_doc.hasLoadedImage()) {
        emit statusMessage(tr("请先打开一张图片，然后再创建平面或进行编辑。"), 3500);
        return;
    }
    const QPointF point = toImage(event->position());
    if (!m_doc.background().rect().contains(point.toPoint()))
        return;

    QPointF grabbedImagePoint;
    int grabbedPlane = -1;
    if (pastedImageAt(point, &grabbedImagePoint, &grabbedPlane)) {
        m_draggingPastedImage = true;
        m_pastedDragStartPosition = m_doc.pastedImagePosition();
        m_pastedDragStartAttached = m_doc.pastedImageAttached();
        m_pastedDragStartSurfaceGroup = m_doc.pastedSurfaceGroup();
        m_pastedDragStartHostPlane = m_doc.pastedHostPlane();
        m_pastedDragOffset = grabbedImagePoint;
        m_pressImagePoint = point;
        m_stateChanged = false;
        setCursor(Qt::ClosedHandCursor);
        return;
    }

    if (m_tool == CreatePlane) {
        m_creationPoints.append(point);
        if (m_creationPoints.size() == 4) {
            finishPlaneCreation();
        } else {
            emit statusMessage(tr("已设置 %1/4 个角点").arg(m_creationPoints.size()), 2000);
        }
        update();
        return;
    }

    if (m_tool == EditPlane) {
        int candidate = m_doc.selectedPlane();
        if (candidate >= 0) {
            m_dragHandle = handleAt(m_doc.planes()[candidate], point, 10.0 / m_scale);
            m_dragEdge = edgeAt(m_doc.planes()[candidate], point, 9.0 / m_scale);
        }
        if (m_dragHandle < 0 && candidate >= 0 && m_dragEdge < 0 &&
            !planePolygon(m_doc.planes()[candidate].corner).containsPoint(point, Qt::OddEvenFill)) {
            candidate = -1;
        }
        if (candidate < 0) {
            candidate = planeAt(m_doc.planes(), point);
            if (candidate >= 0) {
                m_dragHandle = handleAt(m_doc.planes()[candidate], point, 10.0 / m_scale);
                m_dragEdge = edgeAt(m_doc.planes()[candidate], point, 9.0 / m_scale);
            }
        }
        m_doc.setSelectedPlane(candidate);
        if (candidate >= 0) {
            m_dragStartPlane = m_doc.planes()[candidate];
            m_pressImagePoint = point;
            m_lastImagePoint = point;
            m_dragging = true;
            m_extruding = (event->modifiers() & Qt::ControlModifier) && m_dragEdge >= 0;
            if (m_extruding) {
                m_extrudePreview = makePerpendicularPlane(m_doc.planes()[candidate], m_dragEdge,
                                                           point, m_pressImagePoint,
                                                           m_doc.background().size());
                m_hasExtrudePreview = true;
            }
        }
        update();
        return;
    }

    const int planeIndex = planeAt(m_doc.planes(), point);
    if (planeIndex < 0) {
        emit statusMessage(tr("请在一个透视平面内绘制"), 2500);
        return;
    }
    m_doc.setSelectedPlane(planeIndex);
    bool ok = false;
    const QPointF uv = planeToUv(m_doc.planes()[planeIndex], point, &ok);
    if (!ok)
        return;
    if (m_tool == StampTool && (event->modifiers() & Qt::AltModifier)) {
        m_paint.setCloneSource(uv, planeIndex);
        emit statusMessage(tr("仿制源已设置。现在可单击并拖动进行仿制。"), 3500);
        update();
        return;
    }
    if (m_tool == StampTool && !m_paint.hasCloneSource()) {
        emit statusMessage(tr("请先按住 Alt 并在平面中单击，以设置仿制源"), 3500);
        return;
    }
    m_drawing = true;
    m_lastImagePoint = point;
    m_paint.beginStroke(m_doc.planes(), m_doc.background(), planeIndex, uv,
                        m_tool == StampTool);
    m_stateChanged = true;
    update();
}

// 鼠标移动事件：处理拖动浮动图像、编辑平面、绘制笔迹，或更新悬停光标
void PerspectiveCanvas::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF point = toImage(event->position());
    if (m_draggingPastedImage && (event->buttons() & Qt::LeftButton)) {
        // 指针落在某个平面上时，把图像吸附到该平面的展开曲面；
        // 落在宿主投影的网格外延伸部分时保持吸附并沿曲面移动；
        // 其余情况脱离曲面回到画布坐标。
        const int targetPlane = planeAt(m_doc.planes(), point);
        bool mapped = false;
        if (targetPlane >= 0) {
            bool ok = false;
            const QPointF surfacePoint = planeToSurface(m_doc.planes()[targetPlane], point, &ok);
            if (ok) {
                m_doc.setFloatingImagePlacement(surfacePoint - m_pastedDragOffset, true,
                                                m_doc.planes()[targetPlane].surfaceGroup,
                                                targetPlane);
                mapped = true;
            }
        }
        if (!mapped && m_doc.pastedImageAttached() && m_doc.pastedHostPlane() >= 0 &&
            m_doc.pastedHostPlane() < m_doc.planes().size()) {
            bool ok = false;
            const QPointF surfacePoint =
                planeToSurface(m_doc.planes()[m_doc.pastedHostPlane()], point, &ok);
            if (ok) {
                m_doc.setPastedImagePosition(surfacePoint - m_pastedDragOffset);
                mapped = true;
            }
        }
        if (!mapped) {
            m_doc.setFloatingImagePlacement(point - m_pastedDragOffset, false, -1, -1);
        }
        m_stateChanged = m_doc.pastedImagePosition() != m_pastedDragStartPosition ||
                         m_doc.pastedImageAttached() != m_pastedDragStartAttached ||
                         m_doc.pastedSurfaceGroup() != m_pastedDragStartSurfaceGroup ||
                         m_doc.pastedHostPlane() != m_pastedDragStartHostPlane;
        update();
        return;
    }
    if (m_tool == EditPlane && m_dragging && m_doc.selectedPlane() >= 0) {
        Plane &plane = m_doc.planes()[m_doc.selectedPlane()];
        if (m_extruding) {
            m_extrudePreview = makePerpendicularPlane(m_dragStartPlane, m_dragEdge,
                                                       point, m_pressImagePoint,
                                                       m_doc.background().size());
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
            const Plane candidate = resizePlaneAlongEdge(m_dragStartPlane, edge,
                                                         point, m_pressImagePoint);
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
        m_paint.drawStrokeTo(m_doc.planes(), m_doc.background(),
                             m_doc.selectedPlane(), point, m_tool == StampTool);
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
    if (m_doc.selectedPlane() < 0 || m_doc.selectedPlane() >= m_doc.planes().size()) {
        setCursor(Qt::ArrowCursor);
        return;
    }

    const Plane &plane = m_doc.planes()[m_doc.selectedPlane()];
    const int handle = handleAt(plane, imagePoint, 10.0 / m_scale);
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
        const QRectF bounds = planePolygon(m_extrudePreview.corner).boundingRect();
        const qreal area = qAbs(bounds.width() * bounds.height());
        if (area > 100.0) {
            m_extrudePreview.paint = QImage(TextureSize, TextureSize, QImage::Format_ARGB32);
            m_extrudePreview.paint.fill(Qt::transparent);
            m_extrudePreview.name = tr("平面 %1").arg(m_doc.planes().size() + 1);
            m_doc.planes().append(m_extrudePreview);
            m_doc.setSelectedPlane(m_doc.planes().size() - 1);
            m_stateChanged = true;
            emit statusMessage(tr("已创建相邻的垂直平面"), 3000);
        }
    }
    m_dragging = m_drawing = m_extruding = false;
    m_draggingPastedImage = false;
    m_hasExtrudePreview = false;
    m_paint.endStroke();
    m_dragHandle = m_dragEdge = -1;
    if (m_stateChanged)
        m_doc.commitHistory();
    updateHoverCursor(toImage(event->position()));
    update();
}

// 键盘事件：Ctrl+V 粘贴图像、Esc 取消当前操作、Delete 删除选中平面
void PerspectiveCanvas::keyPressEvent(QKeyEvent *event)
{
    if (event->matches(QKeySequence::Paste)) {
        pasteClipboardImage();
        event->accept();
    } else if (event->key() == Qt::Key_Escape) {
        // 取消拖动中的浮动图像，恢复到拖动开始时的放置状态
        if (m_draggingPastedImage) {
            m_doc.setFloatingImagePlacement(m_pastedDragStartPosition,
                                            m_pastedDragStartAttached,
                                            m_pastedDragStartSurfaceGroup,
                                            m_pastedDragStartHostPlane);
        }
        cancelInteraction();
    } else if (event->key() == Qt::Key_Delete && m_tool == EditPlane && m_doc.selectedPlane() >= 0) {
        m_doc.removePlane(m_doc.selectedPlane());
        update();
    } else {
        QWidget::keyPressEvent(event);
    }
}

// Tab / Shift+Tab：在平面之间循环切换选中（仅编辑平面工具）
bool PerspectiveCanvas::focusNextPrevChild(bool next)
{
    if (m_tool != EditPlane || m_doc.planes().isEmpty())
        return QWidget::focusNextPrevChild(next);

    const int planeCount = m_doc.planes().size();
    int selected = m_doc.selectedPlane();
    if (selected < 0) {
        selected = next ? 0 : planeCount - 1;
    } else if (next) {
        selected = (selected + 1) % planeCount;
    } else {
        selected = (selected - 1 + planeCount) % planeCount;
    }
    m_doc.setSelectedPlane(selected);
    update();
    emit statusMessage(tr("已选择：%1（%2/%3）")
                           .arg(m_doc.planes()[selected].name)
                           .arg(selected + 1)
                           .arg(planeCount),
                       2500);
    return true;
}
