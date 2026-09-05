#include "perspectivecanvas.h"

#include "scenerenderer.h"
#include "floatingimagemath.h"
#include "imagegeometry.h"

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
#include <QCursor>
#include <QPixmap>

namespace {
const QCursor &rotationCursor()
{
    static const QCursor cursor = [] {
        QPixmap pixmap(32, 32);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath arrow;
        arrow.arcMoveTo(QRectF(7, 7, 18, 18), 45);
        arrow.arcTo(QRectF(7, 7, 18, 18), 45, 270);
        arrow.moveTo(22, 23);
        arrow.lineTo(22, 16);
        arrow.moveTo(22, 23);
        arrow.lineTo(15, 23);
        painter.setPen(QPen(Qt::black, 4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(arrow);
        painter.setPen(QPen(Qt::white, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(arrow);
        painter.end();
        return QCursor(pixmap, 16, 16);
    }();
    return cursor;
}
}

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
    connect(&m_doc, &CanvasDocument::imageSelectionChanged, this, [this](bool selected) {
        if (!selected && m_tool == TransformTool) {
            cancelInteraction();
            setTool(EditPlane);
            emit toolChangeRequested(EditPlane);
        }
        emit imageSelectionChanged(selected);
        update();
    });
}

// 从文件加载背景图像，并清空交互状态与视图变换
bool PerspectiveCanvas::loadImage(const QString &fileName)
{
    commitInteraction();
    if (!m_doc.loadImage(fileName))
        return false;
    cancelInteraction();
    m_cloneTool.resetSource();
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
    commitInteraction();
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
    commitInteraction();
    m_doc.addFloatingImage(image);
    update();
    emit statusMessage(statusText, 3000);
}

// 将当前浮动图像顺时针旋转 90°


// 将当前浮动图像水平翻转


// 将当前浮动图像垂直翻转


// 撤销：回退到上一份状态并清空进行中的交互
void PerspectiveCanvas::undo()
{
    commitInteraction();
    if (m_doc.undo())
        emit statusMessage(tr("已撤销"), 1800);
    update();
}

// 重做：前进到下一份状态并清空进行中的交互
void PerspectiveCanvas::redo()
{
    cancelInteraction();
    if (m_doc.redo())
        emit statusMessage(tr("已重做"), 1800);
    update();
}

// 清空一切进行中的交互状态（撤销/重做/Esc 取消后调用）
void PerspectiveCanvas::commitInteraction()
{
    m_doc.commitEdit(m_stateChanged);
    cancelInteraction();
}

void PerspectiveCanvas::cancelInteraction()
{
    m_doc.cancelEdit();
    m_imageTool.reset();
    m_cloneTool.cancel();
    m_creationPoints.clear();
    m_gesture = Gesture::Idle;
    m_draggingImage = -1;
    m_hasExtrudePreview = false;
    m_hoverPlane = -1;
    m_stateChanged = false;
    update();
}

// 切换当前工具，并清理进行中的交互状态、更新光标与提示
void PerspectiveCanvas::setTool(Tool tool)
{
    if (tool == TransformTool && !hasSelectedImage())
        return;
    commitInteraction();
    m_tool = tool;
    m_creationPoints.clear();
    m_gesture = Gesture::Idle;
    m_draggingImage = -1;
    m_hoverPlane = -1;
    setCursor(tool == EditPlane ? Qt::SizeAllCursor : Qt::CrossCursor);
    const QString messages[] = {
        tr("依次单击四个角点以创建平面"),
        tr("拖动平面内部沿原无限透视平面移动；拖动控制点调整；Ctrl 从边缘拖出垂直平面"),
        tr("在平面内拖动进行透视绘画，笔触可延伸到平面之外"),
        tr("Alt+左键设置源点；在透视平面内拖动仿制。对齐时源点持续跟随光标"),
        tr("拖动控制点缩放，角点外侧拖动旋转；Shift 等比缩放/15°旋转，Alt 中心缩放；Esc 取消")
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
    painter.drawPixmap(0, 0, m_contentCache.get(m_doc, size(), devicePixelRatioF(), m_scale, m_offset));
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    painter.translate(m_offset);
    painter.scale(m_scale, m_scale);
    SceneRenderer renderer(m_doc);
    renderer.render(painter, m_scale, true, m_creationPoints,
                    m_hasExtrudePreview ? &m_extrudePreview : nullptr,
                    m_tool == EditPlane, m_hoverPlane, m_antsPhase, false);
    if (m_tool == TransformTool && hasSelectedImage()) {
        const FloatingImage &image = m_doc.image(m_doc.selectedImage());
        painter.save();
        painter.resetTransform();
        painter.setPen(QPen(QColor("#1769aa"), 1));
        painter.setBrush(Qt::white);
        const auto geometry = ImageGeometry::get(image);
        for (const QPointF &point : geometry->controls()) {
            const QPointF center = toWidget(point);
            painter.drawRect(QRectF(center - QPointF(4, 4), QSizeF(8, 8)));
        }
        painter.restore();
    }
    if (m_tool == CloneStampTool && m_cloneTool.hasSource()) {
        painter.resetTransform();
        const QPointF center = toWidget(m_cloneTool.marker());
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
    m_cloneTool.setAligned(aligned);
    update();
}

void PerspectiveCanvas::updateCloneMarker(const QPointF &point)
{
    m_cloneTool.hover(m_doc.planes(), point);
}

void PerspectiveCanvas::beginClone(const QPointF &point, bool pickSource)
{
    if (pickSource) {
        if (m_cloneTool.pickSource(m_doc.planes(), point))
            emit statusMessage(tr("源点已设置，左键拖动进行透视仿制"), 3000);
        update();
        return;
    }
    if (!m_cloneTool.hasSource()) {
        emit statusMessage(tr("请先按住 Alt 并单击画布设置仿制源点"), 3000);
        return;
    }
    const int index = planeAt(m_doc.planes(), point);
    if (index < 0) {
        emit statusMessage(tr("请在一个透视平面内开始仿制"), 3000);
        return;
    }
    QImage source(m_doc.background().size(), QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::transparent);
    {
        QPainter painter(&source);
        SceneRenderer(m_doc).render(painter, 1, false);
    }
    m_doc.setSelectedPlane(index);
    m_doc.beginEdit();
    m_doc.beginPaintTransaction();
    m_gesture = Gesture::Clone;
    const QRect dirty = m_cloneTool.begin(m_doc.paintLayer(), source, m_doc.planes()[index], point);
    m_doc.addPaintDirty(dirty);
    m_stateChanged = !dirty.isEmpty();
    updateCloneMarker(point);
    update();
}

// 命中测试：判断某个画布坐标是否落在某张浮动图像上（从最上层开始）。
// 已吸附时返回该点在图像内的局部坐标。
bool PerspectiveCanvas::floatingImageAt(const QPointF &canvasPoint, int *imageIndex, QPointF *imagePoint) const
{
    for (int index = m_doc.images().size() - 1; index >= 0; --index) {
        if (ImageGeometry::get(m_doc.image(index))->hitTest(canvasPoint, imagePoint)) {
            if (imageIndex)
                *imageIndex = index;
            return true;
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
    m_doc.attachImage(imageIndex, faces, hostFace, surfacePoint - m_imageTool.grabOffset());
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
            m_doc.setImagePosition(imageIndex, surfacePoint - m_imageTool.grabOffset());
            return true;
        }
    }
    if (img.hostFace >= 0 && img.hostFace < img.faces.size()) {
        bool ok = false;
        const QPointF surfacePoint = planeToSurface(img.faces[img.hostFace], canvasPoint, &ok);
        if (ok) {
            m_doc.setImagePosition(imageIndex, surfacePoint - m_imageTool.grabOffset());
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
        m_doc.beginEdit();
        m_doc.appendPlane(plane);
        m_doc.setSelectedPlane(m_doc.planes().size() - 1);
        m_doc.commitEdit(true);
        emit statusMessage(tr("平面已创建，已自动进入编辑工具。"), 4000);
        emit toolChangeRequested(EditPlane);
    } else {
        emit statusMessage(tr("无法创建：四个点必须依次组成非交叉的凸四边形，请重新设置。"), 5000);
    }
    m_creationPoints.clear();
}

// 鼠标按下事件：按当前工具分派（拖动浮动图像/创建平面/编辑平面/落笔）
int PerspectiveCanvas::imageTransformHandleAt(const QPointF &point) const
{
    return m_tool == TransformTool && hasSelectedImage()
        ? ImageTransformTool::handleAt(m_doc.image(m_doc.selectedImage()), point, m_scale) : -1;
}

int PerspectiveCanvas::imageRotationCornerAt(const QPointF &point) const
{
    return m_tool == TransformTool && hasSelectedImage()
        ? ImageTransformTool::rotationCornerAt(m_doc.image(m_doc.selectedImage()), point, m_scale) : -1;
}



void PerspectiveCanvas::updateImageTransform(const QPointF &point, Qt::KeyboardModifiers modifiers)
{
    FloatingImage image;
    if (!m_imageTool.update(point, modifiers & Qt::ShiftModifier, modifiers & Qt::AltModifier, &image))
        return;
    m_doc.setImage(m_draggingImage, image);
    const FloatingImage &start = m_imageTool.start();
    m_stateChanged = image.position != start.position || image.scale != start.scale || image.rotation != start.rotation;
    update();
}

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
    const int transformHandle = imageTransformHandleAt(point);
    const int rotationCorner = transformHandle < 0 ? imageRotationCornerAt(point) : -1;
    if (rotationCorner >= 0 || transformHandle >= 0) {
        const auto mode = rotationCorner >= 0 ? ImageTransformTool::Mode::Rotate : ImageTransformTool::Mode::Scale;
        if (!m_imageTool.begin(m_doc.image(m_doc.selectedImage()), point,
                               rotationCorner >= 0 ? rotationCorner : transformHandle, mode))
            return;
        m_doc.beginEdit();
        m_draggingImage = m_doc.selectedImage();
        m_gesture = Gesture::Image;
        m_stateChanged = false;
        if (rotationCorner >= 0)
            setCursor(rotationCursor());
        return;
    }
    const bool wasTransformTool = m_tool == TransformTool;
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
        m_gesture = Gesture::Image;
        m_doc.setSelectedImage(grabbedImage);
        m_doc.beginEdit();
        m_imageTool.beginMove(m_doc.image(grabbedImage), grabbedImagePoint);
        m_stateChanged = false;
        setCursor(Qt::ClosedHandCursor);
        update();
        return;
    }
    if (wasTransformTool)
        return;

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
            m_doc.beginEdit();
            m_gesture = Gesture::Plane;
            const bool extruding = (event->modifiers() & Qt::ControlModifier) && m_dragEdge >= 0;
            m_planeTool.begin(m_doc.planes()[candidate], point, m_dragHandle, m_dragEdge,
                              extruding, m_doc.background().size());
            if (extruding)
                m_hasExtrudePreview = m_planeTool.update(point, &m_extrudePreview);
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
    m_brushFacet = m_doc.planes()[planeIndex];  // 拷贝面片几何作为锁定快照
    m_gesture = Gesture::Brush;
    m_doc.beginEdit();
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
    if (m_imageTool.transforming() && m_draggingImage >= 0 && (event->buttons() & Qt::LeftButton)) {
        updateImageTransform(point, event->modifiers());
        return;
    }
    if (m_tool == CloneStampTool) {
        if (drawing() && (event->buttons() & Qt::LeftButton)) {
            const QRect dirty = m_cloneTool.move(m_doc.paintLayer(), point);
            m_doc.addPaintDirty(dirty);
            m_stateChanged |= !dirty.isEmpty();
        }
        updateCloneMarker(point);
        setCursor(Qt::CrossCursor);
        update();
        return;
    }
    if (m_draggingImage >= 0 && (event->buttons() & Qt::LeftButton)) {
        if (m_tool == TransformTool && m_imageTool.start().attached) {
            QPointF surface;
            if (FloatingImageMath::fromCanvas(m_imageTool.start(), point, &surface)) {
                m_doc.setImagePosition(m_draggingImage, surface - m_imageTool.grabOffset());
                m_stateChanged = m_doc.image(m_draggingImage).position != m_imageTool.start().position;
                update();
            }
            return;
        }
        // 指针落在某个平面上时，把图像吸附到该平面的展开曲面；
        // 已吸附且可沿快照曲面映射时继续移动；其余情况脱离回画布坐标。
        const int targetPlane = planeAt(m_doc.planes(), point);
        if (targetPlane >= 0) {
            attachImageToPlane(m_draggingImage, targetPlane, point);
        } else if (!m_doc.image(m_draggingImage).attached ||
                   !moveAttachedImage(m_draggingImage, point)) {
            m_doc.detachImage(m_draggingImage, point - m_imageTool.grabOffset());
        }
        m_stateChanged = true;
        update();
        return;
    }
    if (m_tool == EditPlane && m_gesture == Gesture::Plane && m_doc.selectedPlane() >= 0) {
        Plane candidate;
        const bool valid = m_planeTool.update(point, &candidate);
        if (m_planeTool.extruding()) {
            m_hasExtrudePreview = valid;
            if (valid)
                m_extrudePreview = candidate;
        } else if (valid) {
            m_doc.setPlane(m_doc.selectedPlane(), candidate);
            m_stateChanged = planePolygon(candidate.corner) != planePolygon(m_planeTool.start().corner);
        }
        update();
        return;
    }
    if (drawing() && (event->buttons() & Qt::LeftButton)) {
        bool ok = false;
        const QPointF uv = planeToUv(m_brushFacet, point, &ok);
        if (ok) {
            const QRect dirty = m_paint.drawStrokeTo(m_doc.paintLayer(), m_brushFacet, uv);
            m_doc.addPaintDirty(dirty);
        }
        update();
        return;
    }
    if (m_gesture != Gesture::Plane) {
        m_hoverPlane = planeAt(m_doc.planes(), point);
        updateHoverCursor(point);
    }
}

// 根据悬停位置更新鼠标光标形状（抓手/十字/方向缩放等）
void PerspectiveCanvas::updateHoverCursor(const QPointF &imagePoint)
{
    if (imageRotationCornerAt(imagePoint) >= 0) {
        setCursor(rotationCursor());
        return;
    }
    const int transformHandle = imageTransformHandleAt(imagePoint);
    if (transformHandle >= 0) {
        const FloatingImage &image = m_doc.image(m_doc.selectedImage());
        const auto points = FloatingImageMath::controlPoints(image);
        const int opposite = transformHandle < 4 ? (transformHandle + 2) % 4 : 4 + (transformHandle - 4 + 2) % 4;
        const QPointF axis = FloatingImageMath::toCanvas(image, points[transformHandle])
                             - FloatingImageMath::toCanvas(image, points[opposite]);
        if (qAbs(axis.x()) < qAbs(axis.y()) * .42)
            setCursor(Qt::SizeVerCursor);
        else if (qAbs(axis.y()) < qAbs(axis.x()) * .42)
            setCursor(Qt::SizeHorCursor);
        else
            setCursor(axis.x() * axis.y() >= 0 ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor);
        return;
    }
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
    if (m_gesture == Gesture::Plane && m_doc.selectedPlane() >= 0) {
        Plane candidate;
        const bool valid = m_planeTool.update(toImage(event->position()), &candidate);
        if (m_planeTool.extruding()) {
            m_hasExtrudePreview = valid;
            if (valid)
                m_extrudePreview = candidate;
        } else if (valid) {
            m_doc.setPlane(m_doc.selectedPlane(), candidate);
            m_stateChanged = planePolygon(candidate.corner) != planePolygon(m_planeTool.start().corner);
        }
    }
    if (m_imageTool.transforming() && m_draggingImage >= 0)
        updateImageTransform(toImage(event->position()), event->modifiers());
    m_imageTool.reset();
    if (m_tool == CloneStampTool && drawing()) {
        const QPointF point = toImage(event->position());
        const QRect dirty = m_cloneTool.move(m_doc.paintLayer(), point);
        m_doc.addPaintDirty(dirty);
        m_stateChanged |= !dirty.isEmpty();
        m_cloneTool.end(m_doc.planes(), point);
        m_gesture = Gesture::Idle;
        updateCloneMarker(point);
    }
    if (m_gesture == Gesture::Plane && m_planeTool.extruding() && m_hasExtrudePreview) {
        const QRectF bounds = planePolygon(m_extrudePreview.corner).boundingRect();
        const qreal area = qAbs(bounds.width() * bounds.height());
        if (area > 100.0) {
            m_extrudePreview.name = tr("平面 %1").arg(m_doc.planes().size() + 1);
            const int index = m_doc.appendPlane(m_extrudePreview);
            if (index >= 0) {
                m_doc.setSelectedPlane(index);
                m_stateChanged = true;
                emit statusMessage(tr("已创建相邻的垂直平面"), 3000);
            }
        }
    }
    m_gesture = Gesture::Idle;
    m_draggingImage = -1;
    m_hasExtrudePreview = false;
    m_dragHandle = m_dragEdge = -1;
    m_doc.commitEdit(m_stateChanged);
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
        cancelInteraction();
    } else if (event->key() == Qt::Key_Delete && m_tool == EditPlane && m_doc.selectedPlane() >= 0) {
        commitInteraction();
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
