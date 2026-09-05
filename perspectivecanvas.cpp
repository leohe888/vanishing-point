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
#include <QTimer>

using namespace PlaneMath;

// 构造函数：初始化默认背景并转发文档模型的历史信号
PerspectiveCanvas::PerspectiveCanvas(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAcceptDrops(true);
    setMinimumSize(500, 400);
    auto *antsTimer = new QTimer(this);
    antsTimer->setInterval(80);
    connect(antsTimer, &QTimer::timeout, this, [this] {
        if (isVisible() && m_doc.selectedImage() >= 0 && m_doc.selectedImage() < m_doc.images().size()) {
            m_antsPhase = int(m_antsPhase + 1) % 8;
            update();
        }
    });
    antsTimer->start();

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

// 从文件加载背景图像，并清空交互状态与视图变换
bool PerspectiveCanvas::loadImage(const QString &fileName)
{
    if (!m_doc.loadImage(fileName))
        return false;
    cancelInteraction();
    m_hasCloneSource = m_hasCloneOffset = false;
    m_creationPoints.clear();
    updateViewTransform();
    update();
    emit statusMessage(tr("图像已打开。请依次点击四个点创建透视平面。"), 5000);
    return true;
}

// 将当前场景（背景 + 绘画层 + 浮动图像）导出为白底图像文件
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

// 清除绘画层上的绘画内容（不影响平面几何与浮动图像）
void PerspectiveCanvas::clearPainting()
{
    if (!m_doc.hasPaintContent()) {
        emit statusMessage(tr("当前没有可清除的绘画内容"), 2500);
        return;
    }
    m_doc.clearPainting();
    update();
    emit statusMessage(tr("已清除绘画内容"), 3000);
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
    m_doc.addFloatingImage(image);
    update();
    emit statusMessage(statusText, 3000);
}

// 将当前浮动图像顺时针旋转 90°
void PerspectiveCanvas::rotateFloatingImage()
{
    if (!m_doc.rotateImage(m_doc.selectedImage())) {
        emit statusMessage(tr("请先粘贴或拖入一张浮动图像"), 2500);
        return;
    }
    update();
    emit statusMessage(tr("浮动图像已顺时针旋转 90°"), 2200);
}

// 将当前浮动图像水平翻转
void PerspectiveCanvas::flipFloatingImageHorizontal()
{
    if (!m_doc.flipImage(m_doc.selectedImage(), true, false)) {
        emit statusMessage(tr("请先粘贴或拖入一张浮动图像"), 2500);
        return;
    }
    update();
    emit statusMessage(tr("浮动图像已水平翻转"), 2200);
}

// 将当前浮动图像垂直翻转
void PerspectiveCanvas::flipFloatingImageVertical()
{
    if (!m_doc.flipImage(m_doc.selectedImage(), false, true)) {
        emit statusMessage(tr("请先粘贴或拖入一张浮动图像"), 2500);
        return;
    }
    update();
    emit statusMessage(tr("浮动图像已垂直翻转"), 2200);
}

// 撤销：回退到上一份状态并清空进行中的交互
void PerspectiveCanvas::undo()
{
    if (m_drawing && m_stateChanged)
        m_doc.commitHistory();
    if (!m_doc.undo())
        return;
    cancelInteraction();
    emit statusMessage(tr("已撤销"), 1800);
}

// 重做：前进到下一份状态并清空进行中的交互
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
    m_clone.endStroke();
    if (!m_cloneAligned)
        m_hasCloneOffset = false;
    if (m_hasCloneSource)
        m_cloneMarker = m_cloneSourceToCanvas.map(m_cloneSource);
    m_creationPoints.clear();
    m_dragging = m_drawing = m_extruding = false;
    m_draggingImage = -1;
    m_hasExtrudePreview = false;
    m_brushPlaneIndex = -1;
    m_hoverPlane = -1;
    m_stateChanged = false;
    update();
}

// 切换当前工具，并清理进行中的交互状态、更新光标与提示
void PerspectiveCanvas::setTool(Tool tool)
{
    if (m_drawing && m_stateChanged)
        m_doc.commitHistory();
    cancelInteraction();
    m_tool = tool;
    m_creationPoints.clear();
    m_dragging = m_drawing = false;
    m_draggingImage = -1;
    m_hoverPlane = -1;
    setCursor(tool == EditPlane ? Qt::SizeAllCursor : Qt::CrossCursor);
    const QString messages[] = {
        tr("依次单击四个角点以创建平面"),
        tr("拖动控制点或平面；按住 Ctrl 从边缘拖出垂直于当前平面的平面"),
        tr("在平面内拖动进行透视绘画，笔触可延伸到平面之外"),
        tr("Alt+左键设置源点；在透视平面内拖动仿制。对齐时源点持续跟随光标")
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
                    m_tool == EditPlane, m_hoverPlane, m_antsPhase);
    if (m_tool == CloneStampTool && m_hasCloneSource) {
        painter.resetTransform();
        const QPointF center = toWidget(m_cloneMarker);
        painter.setPen(QPen(QColor("#12351e"), 4));
        painter.drawLine(center + QPointF(-9, 0), center + QPointF(9, 0));
        painter.drawLine(center + QPointF(0, -9), center + QPointF(0, 9));
        painter.setPen(QPen(QColor("#43ff76"), 2));
        painter.drawLine(center + QPointF(-9, 0), center + QPointF(9, 0));
        painter.drawLine(center + QPointF(0, -9), center + QPointF(0, 9));
    }
}

void PerspectiveCanvas::setCloneAligned(bool aligned)
{
    m_cloneAligned = aligned;
    if (!m_drawing) {
        m_hasCloneOffset = false;
        if (m_hasCloneSource)
            m_cloneMarker = m_cloneSourceToCanvas.map(m_cloneSource);
    }
    update();
}

void PerspectiveCanvas::updateCloneMarker(const QPointF &point)
{
    if (!m_hasCloneSource)
        return;
    m_cloneMarker = m_cloneSourceToCanvas.map(m_cloneSource);
    if (m_hasCloneOffset && (m_drawing || m_cloneAligned)) {
        if (!m_drawing) {
            const int index = planeAt(m_doc.planes(), point);
            if (index >= 0) {
                const Facet &face = m_doc.planes()[index];
                QTransform target;
                if (QTransform::quadToQuad(planePolygon(face.surfaceCorner), planePolygon(face.corner), target))
                    m_cloneTargetToCanvas = target;
            }
        }
        bool ok = false;
        const QTransform inverse = m_cloneTargetToCanvas.inverted(&ok);
        if (ok)
            m_cloneMarker = m_cloneSourceToCanvas.map(inverse.map(point) + m_cloneOffset);
    }
}

void PerspectiveCanvas::beginClone(const QPointF &point, bool pickSource)
{
    const int index = planeAt(m_doc.planes(), point);
    if (pickSource) {
        // 平面外也可取样：此时源图像像素就是展开坐标。
        m_cloneSourceToCanvas = QTransform();
        if (index >= 0) {
            const Facet &face = m_doc.planes()[index];
            if (!QTransform::quadToQuad(planePolygon(face.surfaceCorner), planePolygon(face.corner),
                                        m_cloneSourceToCanvas))
                return;
        }
        m_cloneSource = m_cloneSourceToCanvas.inverted().map(point);
        m_cloneMarker = point;
        m_hasCloneSource = true;
        m_hasCloneOffset = false;
        m_stateChanged = false;
        emit statusMessage(tr("源点已设置，左键拖动进行透视仿制"), 3000);
        update();
        return;
    }
    if (!m_hasCloneSource) {
        emit statusMessage(tr("请先按住 Alt 并单击画布设置仿制源点"), 3000);
        return;
    }
    if (index < 0) {
        emit statusMessage(tr("请在一个透视平面内开始仿制"), 3000);
        return;
    }
    const Facet &face = m_doc.planes()[index];
    QTransform target;
    if (!QTransform::quadToQuad(planePolygon(face.surfaceCorner), planePolygon(face.corner), target))
        return;
    const QPointF position = target.inverted().map(point);
    if (!m_cloneAligned || !m_hasCloneOffset)
        m_cloneOffset = m_cloneSource - position;
    m_cloneTargetToCanvas = target;
    m_hasCloneOffset = true;
    QImage source(m_doc.background().size(), QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::transparent);
    {
        QPainter painter(&source);
        SceneRenderer(m_doc).render(painter, 1, false);
    }
    m_doc.setSelectedPlane(index);
    m_doc.beginPaintTransaction();
    m_drawing = true;
    const QRect dirty = m_clone.beginStroke(m_doc.paintLayer(), source, target,
                                           m_cloneSourceToCanvas, m_cloneOffset, position);
    m_doc.addPaintDirty(dirty);
    m_stateChanged = !dirty.isEmpty();
    updateCloneMarker(point);
    update();
}

// 命中测试：判断某个画布坐标是否落在某张浮动图像上（从最上层开始）。
// 已吸附时返回该点在图像内的局部坐标。
bool PerspectiveCanvas::floatingImageAt(const QPointF &canvasPoint, int *imageIndex,
                                        QPointF *imagePoint) const
{
    for (int idx = m_doc.images().size() - 1; idx >= 0; --idx) {
        const FloatingImage &img = m_doc.images()[idx];
        if (img.image.isNull())
            continue;
        const QRectF imageRect(QPointF(0, 0), QSizeF(img.image.size()));
        if (!img.attached || img.faces.isEmpty()) {
            const QPointF local = canvasPoint - img.position;
            if (imageRect.contains(local)) {
                if (imageIndex)
                    *imageIndex = idx;
                if (imagePoint)
                    *imagePoint = local;
                return true;
            }
            continue;
        }
        // 优先选择指针实际所在的面，这样共享边将归属当前显示在顶部的面。
        for (int f = img.faces.size() - 1; f >= 0; --f) {
            const Facet &face = img.faces[f];
            if (!planePolygon(face.corner).containsPoint(canvasPoint, Qt::OddEvenFill))
                continue;
            bool ok = false;
            const QPointF local = planeToSurface(face, canvasPoint, &ok) - img.position;
            if (ok && imageRect.contains(local)) {
                if (imageIndex)
                    *imageIndex = idx;
                if (imagePoint)
                    *imagePoint = local;
                return true;
            }
        }
        // 宿主投影有意延伸到其有限网格之外，因此其可见的延伸部分
        // 也必须保持可拖动。
        if (img.hostFace >= 0 && img.hostFace < img.faces.size()) {
            bool ok = false;
            const QPointF local = planeToSurface(img.faces[img.hostFace], canvasPoint, &ok) -
                                  img.position;
            if (ok && imageRect.contains(local)) {
                if (imageIndex)
                    *imageIndex = idx;
                if (imagePoint)
                    *imagePoint = local;
                return true;
            }
        }
    }
    return false;
}

// 把指定浮动图像吸附到目标平面所在的曲面分组：拷贝该分组的全部面片
// 几何作为严格快照，此后平面增删改都不再影响这张图像。
void PerspectiveCanvas::attachImageToPlane(int imageIndex, int planeIndex,
                                           const QPointF &canvasPoint)
{
    const Plane &host = m_doc.planes()[planeIndex];
    QVector<Facet> faces;
    int hostFace = -1;
    for (int i = 0; i < m_doc.planes().size(); ++i) {
        const Plane &p = m_doc.planes()[i];
        if (p.surfaceGroup != host.surfaceGroup)
            continue;
        Facet f;
        for (int c = 0; c < 4; ++c) {
            f.corner[c] = p.corner[c];
            f.surfaceCorner[c] = p.surfaceCorner[c];
        }
        if (i == planeIndex)
            hostFace = faces.size();
        faces.append(f);
    }
    bool ok = false;
    const QPointF surfacePoint = planeToSurface(host, canvasPoint, &ok);
    if (!ok)
        return;
    m_doc.attachImage(imageIndex, faces, hostFace, surfacePoint - m_imageDragOffset);
}

// 已吸附图像沿其快照曲面移动；无法映射（越过极点线）时返回 false。
bool PerspectiveCanvas::moveAttachedImage(int imageIndex, const QPointF &canvasPoint)
{
    const FloatingImage &img = m_doc.image(imageIndex);
    for (int f = img.faces.size() - 1; f >= 0; --f) {
        if (!planePolygon(img.faces[f].corner).containsPoint(canvasPoint, Qt::OddEvenFill))
            continue;
        bool ok = false;
        const QPointF surfacePoint = planeToSurface(img.faces[f], canvasPoint, &ok);
        if (ok) {
            m_doc.setImagePosition(imageIndex, surfacePoint - m_imageDragOffset);
            return true;
        }
    }
    if (img.hostFace >= 0 && img.hostFace < img.faces.size()) {
        bool ok = false;
        const QPointF surfacePoint = planeToSurface(img.faces[img.hostFace], canvasPoint, &ok);
        if (ok) {
            m_doc.setImagePosition(imageIndex, surfacePoint - m_imageDragOffset);
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
    const bool insideCanvas = m_doc.background().rect().contains(point.toPoint());
    QPointF grabbedImagePoint;
    int grabbedImage = -1;
    const bool hitImage = insideCanvas && floatingImageAt(point, &grabbedImage, &grabbedImagePoint);
    if (!hitImage && m_doc.selectedImage() >= 0) {
        m_doc.setSelectedImage(-1);
        update();
    }
    if (!insideCanvas)
        return;

    if (m_tool == CloneStampTool) {
        beginClone(point, event->modifiers() & Qt::AltModifier);
        return;
    }

    if (hitImage) {
        m_draggingImage = grabbedImage;
        m_doc.setSelectedImage(grabbedImage);
        m_imageDragStart = m_doc.image(grabbedImage);
        m_imageDragOffset = grabbedImagePoint;
        m_stateChanged = false;
        setCursor(Qt::ClosedHandCursor);
        update();
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

    // 画笔工具：锁定按下时所在的平面，此后整笔跟随该平面的透视，
    // 即使指针移出平面边界也能继续延伸绘制。
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
    m_brushPlaneIndex = planeIndex;
    m_brushFacet = m_doc.planes()[planeIndex];  // 拷贝面片几何作为锁定快照
    m_drawing = true;
    m_lastImagePoint = point;
    m_doc.beginPaintTransaction();
    const QRect dirty = m_paint.beginStroke(m_doc.paintLayer(), m_brushFacet, uv);
    m_doc.addPaintDirty(dirty);
    m_stateChanged = true;
    update();
}

// 鼠标移动事件：处理拖动浮动图像、编辑平面、绘制笔迹，或更新悬停光标
void PerspectiveCanvas::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF point = toImage(event->position());
    if (m_tool == CloneStampTool) {
        if (m_drawing && (event->buttons() & Qt::LeftButton)) {
            const QPointF position = m_cloneTargetToCanvas.inverted().map(point);
            const QRect dirty = m_clone.drawStrokeTo(m_doc.paintLayer(), position);
            m_doc.addPaintDirty(dirty);
            m_stateChanged |= !dirty.isEmpty();
        }
        updateCloneMarker(point);
        setCursor(Qt::CrossCursor);
        update();
        return;
    }
    if (m_draggingImage >= 0 && (event->buttons() & Qt::LeftButton)) {
        // 指针落在某个平面上时，把图像吸附到该平面的展开曲面；
        // 已吸附且可沿快照曲面映射时继续移动；其余情况脱离回画布坐标。
        const int targetPlane = planeAt(m_doc.planes(), point);
        if (targetPlane >= 0) {
            attachImageToPlane(m_draggingImage, targetPlane, point);
        } else if (!m_doc.image(m_draggingImage).attached ||
                   !moveAttachedImage(m_draggingImage, point)) {
            m_doc.detachImage(m_draggingImage, point - m_imageDragOffset);
        }
        m_stateChanged = true;
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
        bool ok = false;
        const QPointF uv = planeToUv(m_brushFacet, point, &ok);
        if (ok) {
            const QRect dirty = m_paint.drawStrokeTo(m_doc.paintLayer(), m_brushFacet, uv);
            m_doc.addPaintDirty(dirty);
        }
        m_lastImagePoint = point;
        update();
        return;
    }
    if (!m_dragging) {
        m_hoverPlane = planeAt(m_doc.planes(), point);
        updateHoverCursor(point);
    }
}

// 根据悬停位置更新鼠标光标形状（抓手/十字/方向缩放等）
void PerspectiveCanvas::updateHoverCursor(const QPointF &imagePoint)
{
    if (m_tool == CloneStampTool) {
        setCursor(Qt::CrossCursor);
        return;
    }
    if (floatingImageAt(imagePoint, nullptr, nullptr)) {
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
    if (m_tool == CloneStampTool && m_drawing) {
        const QPointF point = toImage(event->position());
        const QRect dirty = m_clone.drawStrokeTo(m_doc.paintLayer(), m_cloneTargetToCanvas.inverted().map(point));
        m_doc.addPaintDirty(dirty);
        m_stateChanged |= !dirty.isEmpty();
        m_clone.endStroke();
        m_drawing = false;
        if (!m_cloneAligned)
            m_hasCloneOffset = false;
        updateCloneMarker(point);
    }
    if (m_extruding && m_hasExtrudePreview) {
        const QRectF bounds = planePolygon(m_extrudePreview.corner).boundingRect();
        const qreal area = qAbs(bounds.width() * bounds.height());
        if (area > 100.0) {
            m_extrudePreview.name = tr("平面 %1").arg(m_doc.planes().size() + 1);
            m_doc.planes().append(m_extrudePreview);
            m_doc.setSelectedPlane(m_doc.planes().size() - 1);
            m_stateChanged = true;
            emit statusMessage(tr("已创建相邻的垂直平面"), 3000);
        }
    }
    m_dragging = m_drawing = m_extruding = false;
    m_draggingImage = -1;
    m_hasExtrudePreview = false;
    m_dragHandle = m_dragEdge = -1;
    m_brushPlaneIndex = -1;
    if (m_stateChanged)
        m_doc.commitHistory();
    m_stateChanged = false;
    const QPointF point = toImage(event->position());
    m_hoverPlane = planeAt(m_doc.planes(), point);
    updateHoverCursor(point);
    update();
}

// 键盘事件：Ctrl+V 粘贴图像、Esc 取消当前操作、Delete 删除选中平面
void PerspectiveCanvas::keyPressEvent(QKeyEvent *event)
{
    if (event->matches(QKeySequence::Paste)) {
        pasteClipboardImage();
        event->accept();
    } else if (event->key() == Qt::Key_Escape) {
        if (m_drawing && m_stateChanged)
            m_doc.commitHistory();
        // 取消拖动中的浮动图像，恢复到拖动开始时的完整快照
        if (m_draggingImage >= 0)
            m_doc.image(m_draggingImage) = m_imageDragStart;
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
