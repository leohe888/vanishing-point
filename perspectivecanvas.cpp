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
        if (isVisible() && (hasSelectedImage() || !m_selectionRect.isEmpty())) {
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
    clearSelection();
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

// 把当前选中的浮动图像按当前几何画进绘画层，并删除该浮动图像。
// 烘焙之后内容彻底并入绘画层：不再能单独移动、缩放或删除。
// 走的是与屏幕渲染完全相同的分段投影路径，因此肉眼看不到像素跳变。
void PerspectiveCanvas::bakeSelectedImage()
{
    if (!hasSelectedImage())
        return;
    const int index = m_doc.selectedImage();
    const FloatingImage image = m_doc.image(index);  // 拷贝：移除后仍需用它的几何算脏矩形
    m_doc.beginPaintTransaction();
    QPainter painter(&m_doc.paintLayer());
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    SceneRenderer(m_doc).renderFloatingImage(painter, image);
    painter.end();
    const QRect dirty = ImageGeometry::get(image)->outline().boundingRect().toAlignedRect()
                            .adjusted(-2, -2, 2, 2).intersected(m_doc.paintLayer().rect());
    m_doc.addPaintDirty(dirty);
    // 删除会顺带取消选中，并在同一步历史里记录结构变化与绘画层增量，
    // 撤销时两者一起回退。
    m_doc.removeFloatingImage(index);
    m_imageTool.reset();
    m_draggingImage = -1;
    m_gesture = Gesture::Idle;
    m_stateChanged = false;
    update();
    emit statusMessage(tr("浮动图像已合并到绘画层"), 2500);
}

void PerspectiveCanvas::clearSelection()
{
    m_selectionFaces.clear();
    m_selectionRect = QRectF();
    m_selectionStartRect = QRectF();
    m_selectionGroup = -1;
    m_selectionAction = SelectionAction::None;
    m_selectionSampleSource = QImage();
    m_selectionPaintBefore = QImage();
}

bool PerspectiveCanvas::pointToSelectionSurface(const QPointF &point, QPointF *surface) const
{
    if (!surface || m_selectionFaces.isEmpty())
        return false;
    for (int i = m_selectionFaces.size() - 1; i >= 0; --i) {
        const Facet &face = m_selectionFaces[i];
        if (!planePolygon(face.corner).containsPoint(point, Qt::OddEvenFill))
            continue;
        bool ok = false;
        *surface = planeToSurface(face, point, &ok);
        if (ok)
            return true;
    }
    bool ok = false;
    *surface = planeToSurface(m_selectionFaces.first(), point, &ok);
    return ok;
}

QPainterPath PerspectiveCanvas::selectionPath() const
{
    QPainterPath result;
    if (m_selectionRect.isEmpty())
        return result;
    QPainterPath rectangle;
    rectangle.addRect(m_selectionRect.normalized());
    for (const Facet &face : m_selectionFaces) {
        QPainterPath facePath;
        facePath.addPolygon(planePolygon(face.surfaceCorner));
        facePath.closeSubpath();
        const QPainterPath clipped = rectangle.intersected(facePath);
        const ProjectiveMapping mapping = surfaceMapping(face);
        if (!mapping.isValid())
            continue;
        for (const QPolygonF &surfacePolygon : clipped.toFillPolygons()) {
            QPolygonF canvasPolygon;
            for (const QPointF &surfacePoint : surfacePolygon) {
                QPointF canvasPoint;
                if (mapping.toCanvas(surfacePoint, &canvasPoint))
                    canvasPolygon.append(canvasPoint);
            }
            if (canvasPolygon.size() >= 3) {
                QPainterPath patch;
                patch.addPolygon(canvasPolygon);
                patch.closeSubpath();
                result = result.united(patch);
            }
        }
    }
    return result;
}

void PerspectiveCanvas::updateSelection(const QPointF &point, Qt::KeyboardModifiers modifiers)
{
    QPointF surface;
    if (!pointToSelectionSurface(point, &surface))
        return;
    if (m_selectionAction == SelectionAction::Create) {
        QPointF delta = surface - m_selectionPressSurface;
        if (modifiers & Qt::ShiftModifier) {
            const qreal side = qMax(qAbs(delta.x()), qAbs(delta.y()));
            delta.setX(delta.x() < 0 ? -side : side);
            delta.setY(delta.y() < 0 ? -side : side);
        }
        m_selectionRect = QRectF(m_selectionPressSurface,
                                 m_selectionPressSurface + delta).normalized();
    } else if (m_selectionAction == SelectionAction::Move) {
        QPointF delta = surface - m_selectionPressSurface;
        if (modifiers & Qt::ShiftModifier) {
            if (qAbs(delta.x()) >= qAbs(delta.y()))
                delta.setY(0);
            else
                delta.setX(0);
            delta.setX(qRound(delta.x() / m_gridSize) * m_gridSize);
            delta.setY(qRound(delta.y() / m_gridSize) * m_gridSize);
        }
        m_selectionRect = m_selectionStartRect.translated(delta);
    } else if (m_selectionAction == SelectionAction::Fill) {
        fillSelectionFromPoint(point);
    }
    update();
}

void PerspectiveCanvas::fillSelectionFromPoint(const QPointF &point)
{
    if (m_selectionSampleSource.isNull() || m_selectionPaintBefore.isNull())
        return;

    QPointF sourceAnchor;
    if (!pointToSelectionSurface(point, &sourceAnchor))
        return;
    const QPointF sourceOffset = sourceAnchor - m_selectionPressSurface;
    m_selectionFillOffset = sourceOffset;
    const QPainterPath targetPath = selectionPath();
    const QRect dirty = targetPath.boundingRect().toAlignedRect().adjusted(-1, -1, 1, 1)
                            .intersected(m_doc.paintLayer().rect());
    if (dirty.isEmpty())
        return;

    // Rebuild the preview from the paint layer as it was when Ctrl-drag
    // started. This prevents repeated mouse moves from sampling or stacking
    // earlier previews.
    m_doc.paintLayer() = m_selectionPaintBefore;
    QPainter painter(&m_doc.paintLayer());
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    QPainterPath selectionSurfacePath;
    selectionSurfacePath.addRect(m_selectionRect.normalized());
    // Each target/source face pair is one homographic patch. Qt rasterizes the
    // whole patch at once, replacing the old per-pixel inverse mapping loop.
    for (const Facet &targetFace : m_selectionFaces) {
        QPainterPath targetSurfacePath;
        targetSurfacePath.addPolygon(planePolygon(targetFace.surfaceCorner));
        targetSurfacePath.closeSubpath();
        const ProjectiveMapping targetMapping = surfaceMapping(targetFace);
        if (!targetMapping.isValid())
            continue;
        for (const Facet &sourceFace : m_selectionFaces) {
            QPolygonF shiftedSourceSurface;
            QPolygonF sourceCanvas;
            QPolygonF targetCanvas;
            for (int c = 0; c < 4; ++c) {
                shiftedSourceSurface.append(sourceFace.surfaceCorner[c] - sourceOffset);
                sourceCanvas.append(sourceFace.corner[c]);
                QPointF mapped;
                if (!targetMapping.toCanvas(shiftedSourceSurface.last(), &mapped)) {
                    targetCanvas.clear();
                    break;
                }
                targetCanvas.append(mapped);
            }
            if (targetCanvas.size() != 4)
                continue;

            QPainterPath sourceDomain;
            sourceDomain.addPolygon(shiftedSourceSurface);
            sourceDomain.closeSubpath();
            const QPainterPath surfacePatch = selectionSurfacePath
                                                  .intersected(targetSurfacePath)
                                                  .intersected(sourceDomain);
            if (surfacePatch.isEmpty())
                continue;

            QPainterPath canvasClip;
            for (const QPolygonF &polygon : surfacePatch.toFillPolygons()) {
                QPolygonF mappedPolygon;
                for (const QPointF &surfacePoint : polygon) {
                    QPointF mapped;
                    if (targetMapping.toCanvas(surfacePoint, &mapped))
                        mappedPolygon.append(mapped);
                }
                if (mappedPolygon.size() >= 3) {
                    canvasClip.addPolygon(mappedPolygon);
                    canvasClip.closeSubpath();
                }
            }
            const ProjectiveMapping sourceToTarget(sourceCanvas, targetCanvas);
            if (!sourceToTarget.isValid() || canvasClip.isEmpty())
                continue;
            painter.save();
            painter.setClipPath(canvasClip, Qt::IntersectClip);
            painter.setWorldTransform(sourceToTarget.forward());
            painter.drawImage(QPointF(), m_selectionSampleSource);
            painter.restore();
        }
    }
    painter.end();
    m_doc.addPaintDirty(dirty);
    m_stateChanged |= !dirty.isEmpty();
}

int PerspectiveCanvas::copySelectionToFloatingImage(const QPointF &point)
{
    const QRectF rect = m_selectionRect.normalized();
    if (rect.width() < 1 || rect.height() < 1 || m_selectionFaces.isEmpty())
        return -1;
    const QSize size(qMin(8192, qMax(1, qCeil(rect.width()))),
                     qMin(8192, qMax(1, qCeil(rect.height()))));
    QImage source(m_doc.background().size(), QImage::Format_ARGB32_Premultiplied);
    source.fill(Qt::transparent);
    {
        QPainter sourcePainter(&source);
        SceneRenderer(m_doc).render(sourcePainter, 1.0, false);
    }
    QImage extracted(size, QImage::Format_ARGB32_Premultiplied);
    extracted.fill(Qt::transparent);
    QPainter painter(&extracted);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    int hostFace = 0;
    QPointF pressSurface;
    pointToSelectionSurface(point, &pressSurface);
    for (int i = 0; i < m_selectionFaces.size(); ++i) {
        const Facet &face = m_selectionFaces[i];
        QPolygonF target;
        for (int c = 0; c < 4; ++c)
            target.append(face.surfaceCorner[c] - rect.topLeft());
        QPainterPath clip;
        clip.addPolygon(target);
        clip.closeSubpath();
        QPainterPath outputBounds;
        outputBounds.addRect(QRectF(QPointF(), QSizeF(size)));
        clip = outputBounds.intersected(clip);
        const ProjectiveMapping mapping(planePolygon(face.corner), target);
        if (!mapping.isValid() || clip.isEmpty())
            continue;
        painter.save();
        painter.setClipPath(clip);
        painter.setWorldTransform(mapping.forward());
        painter.drawImage(QPointF(), source);
        painter.restore();
        if (planePolygon(face.surfaceCorner).containsPoint(pressSurface, Qt::OddEvenFill))
            hostFace = i;
    }
    painter.end();
    return m_doc.addFloatingImageOnSurface(extracted, m_selectionFaces, hostFace, rect.topLeft());
}

// 把 Ctrl 拖动（区域克隆）的结果提取为一张浮动图像。
// 与 fillSelectionFromPoint 使用同一套「源面 → 目标」单应，只是目标由画布
// 坐标换成展开曲面上的矩形位图，因此拖动结果可以继续被移动、缩放。
int PerspectiveCanvas::cloneSelectionToFloatingImage()
{
    const QRectF rect = m_selectionRect.normalized();
    if (m_selectionSampleSource.isNull() || m_selectionFaces.isEmpty()
        || rect.width() < 1 || rect.height() < 1) {
        return -1;
    }
    const QSize size(qMin(8192, qMax(1, qCeil(rect.width()))),
                     qMin(8192, qMax(1, qCeil(rect.height()))));
    QImage extracted(size, QImage::Format_ARGB32_Premultiplied);
    extracted.fill(Qt::transparent);
    QPainter painter(&extracted);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);

    const QPointF origin = rect.topLeft();
    QPainterPath bounds;                  // 位图范围，等价于选区矩形
    bounds.addRect(QRectF(QPointF(), QSizeF(size)));
    int hostFace = 0;
    for (int i = 0; i < m_selectionFaces.size(); ++i) {
        if (planePolygon(m_selectionFaces[i].surfaceCorner)
                .containsPoint(m_selectionPressSurface, Qt::OddEvenFill)) {
            hostFace = i;
        }
    }
    // 每个「目标面 + 源面」组合是一片单应补丁，裁剪规则与填充预览保持一致，
    // 因此松手前后看到的像素不会跳变。
    for (const Facet &targetFace : m_selectionFaces) {
        QPolygonF targetQuad;
        for (int c = 0; c < 4; ++c)
            targetQuad.append(targetFace.surfaceCorner[c] - origin);
        QPainterPath targetSurface;
        targetSurface.addPolygon(targetQuad);
        targetSurface.closeSubpath();
        for (const Facet &sourceFace : m_selectionFaces) {
            QPolygonF shifted;            // 源面按拖动偏移搬到目标位置后，在位图中的四边形
            for (int c = 0; c < 4; ++c)
                shifted.append(sourceFace.surfaceCorner[c] - m_selectionFillOffset - origin);
            QPainterPath sourceDomain;
            sourceDomain.addPolygon(shifted);
            sourceDomain.closeSubpath();
            const QPainterPath clip = bounds.intersected(targetSurface).intersected(sourceDomain);
            if (clip.isEmpty())
                continue;
            QPolygonF sourceCanvas;
            for (int c = 0; c < 4; ++c)
                sourceCanvas.append(sourceFace.corner[c]);
            const ProjectiveMapping mapping(sourceCanvas, shifted);
            if (!mapping.isValid())
                continue;
            painter.save();
            painter.setClipPath(clip);
            painter.setWorldTransform(mapping.forward());
            painter.drawImage(QPointF(), m_selectionSampleSource);
            painter.restore();
        }
    }
    painter.end();
    return m_doc.addFloatingImageOnSurface(extracted, m_selectionFaces, hostFace, rect.topLeft());
}


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
    clearSelection();
    update();
}

// 切换当前工具，并清理进行中的交互状态、更新光标与提示
void PerspectiveCanvas::setTool(Tool tool)
{
    if (tool == TransformTool && !hasSelectedImage())
        return;
    commitInteraction();
    if (tool != MarqueeTool)
        clearSelection();
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
        tr("拖动控制点缩放，角点外侧拖动旋转；Shift 等比缩放/15°旋转，Alt 中心缩放；Esc 取消"),
        tr("拖动创建透视选区；Shift 正方形；Alt 拖动复制内容；Ctrl 拖动克隆为浮动图像")
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
                    m_tool == EditPlane, m_hoverPlane, m_antsPhase, false, m_gridSize,
                    m_tool == CreatePlane ? m_cursorPoint : QPointF());
    // 画笔预览：光标在某个透视平面上时，按当前画笔参数（直径/硬度/不透明度/颜色）
    // 画一个即将落下的笔触点，跟随光标移动。实际绘制时（m_gesture == Brush）这里不画，
    // 否则会和已画到绘画层的笔迹重叠。
    if (m_tool == BrushTool && m_gesture != Gesture::Brush && m_doc.hasLoadedImage()) {
        const int planeIndex = planeAt(m_doc.planes(), m_cursorPoint);
        if (planeIndex >= 0) {
            const Plane &plane = m_doc.planes()[planeIndex];
            bool ok = false;
            const QPointF uv = planeToUv(plane, m_cursorPoint, &ok);
            if (ok)
                m_paint.applyDab(painter, plane, uv);
        }
    }
    if (m_tool == MarqueeTool && !m_selectionRect.isEmpty()) {
        painter.save();
        painter.resetTransform();
        const QPainterPath outline = QTransform::fromTranslate(m_offset.x(), m_offset.y())
                                         .map(QTransform::fromScale(m_scale, m_scale)
                                                  .map(selectionPath()));
        QPen pen(Qt::white, 1);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(pen);
        painter.drawPath(outline);
        pen.setColor(Qt::black);
        pen.setDashPattern({4, 4});
        pen.setDashOffset(m_antsPhase);
        painter.setPen(pen);
        painter.drawPath(outline);
        painter.restore();
    }
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
        // 点击别处：把选中的浮动图像烘焙进绘画层，之后不再是可操作对象。
        // 这一下点击只用于确认烘焙，不再触发工具的其它动作，
        // 避免误落一笔或误建一个选区。
        bakeSelectedImage();
        updateHoverCursor(point);
        return;
    }
    if (!insideCanvas)
        return;

    if (m_tool == CloneStampTool) {
        beginClone(point, event->modifiers() & Qt::AltModifier);
        return;
    }

    if (m_tool == MarqueeTool) {
        QPointF surface;
        const bool insideSelection = !m_selectionRect.isEmpty()
                                     && selectionPath().contains(point)
                                     && pointToSelectionSurface(point, &surface);
        if (insideSelection && (event->modifiers() & Qt::AltModifier)) {
            const int imageIndex = copySelectionToFloatingImage(point);
            if (imageIndex >= 0) {
                m_draggingImage = imageIndex;
                m_gesture = Gesture::Image;
                m_imageTool.beginMove(m_doc.image(imageIndex), surface - m_selectionRect.topLeft());
                m_doc.beginEdit();
                clearSelection();
                setCursor(Qt::ClosedHandCursor);
                emit statusMessage(tr("已复制选区内容为浮动图像"), 2500);
            }
            update();
            return;
        }
        if (insideSelection) {
            m_selectionPressSurface = surface;
            m_selectionStartRect = m_selectionRect;
            m_selectionAction = (event->modifiers() & Qt::ControlModifier)
                                    ? SelectionAction::Fill : SelectionAction::Move;
            m_gesture = Gesture::Selection;
            if (m_selectionAction == SelectionAction::Fill) {
                m_selectionSampleSource = QImage(m_doc.background().size(), QImage::Format_ARGB32_Premultiplied);
                m_selectionSampleSource.fill(Qt::transparent);
                QPainter sourcePainter(&m_selectionSampleSource);
                SceneRenderer(m_doc).render(sourcePainter, 1.0, false);
                sourcePainter.end();
                m_doc.beginEdit();
                m_doc.beginPaintTransaction();
                m_selectionPaintBefore = m_doc.paintLayer();
                fillSelectionFromPoint(point);
            }
            update();
            return;
        }
        // 已选中的浮动图像（例如刚由 Ctrl 拖动生成的那张）可以直接拖动移动，
        // 未选中的图像则不拦截，仍可在其上建立新的选区。
        if (hitImage && grabbedImage == m_doc.selectedImage()) {
            m_draggingImage = grabbedImage;
            m_gesture = Gesture::Image;
            m_doc.beginEdit();
            m_imageTool.beginMove(m_doc.image(grabbedImage), grabbedImagePoint);
            m_stateChanged = false;
            setCursor(Qt::ClosedHandCursor);
            update();
            return;
        }
        const int planeIndex = planeAt(m_doc.planes(), point);
        if (planeIndex < 0) {
            clearSelection();
            update();
            return;
        }
        const Plane &host = m_doc.planes()[planeIndex];
        m_selectionFaces.clear();
        for (const Plane &plane : m_doc.planes()) {
            if (plane.surfaceGroup != host.surfaceGroup)
                continue;
            Facet face;
            for (int c = 0; c < 4; ++c) {
                face.corner[c] = plane.corner[c];
                face.surfaceCorner[c] = plane.surfaceCorner[c];
            }
            m_selectionFaces.append(face);
        }
        bool ok = false;
        m_selectionPressSurface = planeToSurface(host, point, &ok);
        if (!ok) {
            clearSelection();
            return;
        }
        m_selectionGroup = host.surfaceGroup;
        m_selectionRect = QRectF(m_selectionPressSurface, QSizeF());
        m_selectionAction = SelectionAction::Create;
        m_gesture = Gesture::Selection;
        update();
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
        auto sharedEdgesFor = [this](int index) {
            if (index < 0 || index >= m_doc.planes().size())
                return quint8(0);
            const Plane &plane = m_doc.planes()[index];
            quint8 result = plane.lockedEdges;
            for (int edge = 0; edge < 4; ++edge) {
                const QPointF a = plane.corner[edge];
                const QPointF b = plane.corner[(edge + 1) % 4];
                for (int other = 0; other < m_doc.planes().size(); ++other) {
                    if (other == index)
                        continue;
                    for (int oe = 0; oe < 4; ++oe) {
                        const QPointF oa = m_doc.planes()[other].corner[oe];
                        const QPointF ob = m_doc.planes()[other].corner[(oe + 1) % 4];
                        if ((QLineF(a, oa).length() < 0.01 && QLineF(b, ob).length() < 0.01) ||
                            (QLineF(a, ob).length() < 0.01 && QLineF(b, oa).length() < 0.01)) {
                            result |= quint8(1u << edge);
                            break;
                        }
                    }
                }
            }
            return result;
        };
        int candidate = m_doc.selectedPlane();
        if (candidate >= 0) {
            m_dragHandle = handleAt(m_doc.planes()[candidate], point, 10.0 / m_scale);
            m_dragEdge = edgeAt(m_doc.planes()[candidate], point, 9.0 / m_scale);
        }
        const quint8 sharedEdges = sharedEdgesFor(candidate);
        auto isLocked = [sharedEdges](int handle, int edge) {
            return edge >= 0 && (sharedEdges & quint8(1u << edge))
                   && (handle < 0 || handle == edge || handle == (edge + 1) % 4 || handle == 4 + edge);
        };
        if (isLocked(m_dragHandle, m_dragEdge) ||
            (m_dragHandle >= 0 && (((sharedEdges >> m_dragHandle) & 1u) ||
                                   (m_dragHandle < 4 && ((sharedEdges >> ((m_dragHandle + 3) % 4)) & 1u))))) {
            m_dragHandle = -1;
            m_dragEdge = -1;
        }
        if (m_dragHandle < 0 && candidate >= 0 && m_dragEdge < 0 &&
            !planePolygon(m_doc.planes()[candidate].corner).containsPoint(point, Qt::OddEvenFill)) {
            candidate = -1;
        }
        // Ctrl+click cycles through every plane under the cursor, from the
        // current selection toward lower (older) layers. A handle/edge hit
        // keeps its normal drag behavior, so Ctrl+drag can still extrude.
        const bool cycleSelection = (event->modifiers() & Qt::ControlModifier)
                                     && m_dragHandle < 0 && m_dragEdge < 0;
        if (cycleSelection) {
            QVector<int> hits;
            for (int i = m_doc.planes().size() - 1; i >= 0; --i) {
                if (planePolygon(m_doc.planes()[i].corner)
                        .containsPoint(point, Qt::OddEvenFill))
                    hits.append(i);
            }
            if (!hits.isEmpty()) {
                const int current = hits.indexOf(m_doc.selectedPlane());
                candidate = current >= 0 ? hits[(current + 1) % hits.size()] : hits.first();
            } else {
                candidate = -1;
            }
            m_dragHandle = candidate >= 0 ? handleAt(m_doc.planes()[candidate], point, 10.0 / m_scale) : -1;
            m_dragEdge = candidate >= 0 ? edgeAt(m_doc.planes()[candidate], point, 9.0 / m_scale) : -1;
        } else if (candidate < 0) {
            candidate = planeAt(m_doc.planes(), point);
            if (candidate >= 0) {
                m_dragHandle = handleAt(m_doc.planes()[candidate], point, 10.0 / m_scale);
                m_dragEdge = edgeAt(m_doc.planes()[candidate], point, 9.0 / m_scale);
            }
        }
        const quint8 finalSharedEdges = sharedEdgesFor(candidate);
        if ((m_dragEdge >= 0 && (finalSharedEdges & quint8(1u << m_dragEdge))) ||
            (m_dragHandle >= 4 && (finalSharedEdges & quint8(1u << (m_dragHandle - 4)))) ||
            (m_dragHandle < 4 && m_dragHandle >= 0 &&
             ((finalSharedEdges & quint8(1u << m_dragHandle)) ||
              (finalSharedEdges & quint8(1u << ((m_dragHandle + 3) % 4)))))) {
            m_dragHandle = -1;
            m_dragEdge = -1;
        }
        m_doc.setSelectedPlane(candidate);
        if (candidate >= 0) {
            // Adjacent planes created from a shared edge are locked as a pair:
            // clicking their interior may select them, but cannot translate
            // either plane as a whole. Their non-shared control points remain
            // available for shape edits.
            if (sharedEdgesFor(candidate) != 0 && m_dragHandle < 0 && m_dragEdge < 0) {
                update();
                return;
            }
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
    m_cursorPoint = point;
    // 创建平面的橡皮筋与画笔的光标预览都需要随光标移动持续重绘。
    if ((m_tool == CreatePlane && !m_creationPoints.isEmpty()) ||
        (m_tool == BrushTool && m_gesture != Gesture::Brush && m_doc.hasLoadedImage()))
        update();
    if (m_tool == MarqueeTool && m_gesture == Gesture::Selection &&
        (event->buttons() & Qt::LeftButton)) {
        updateSelection(point, event->modifiers());
        return;
    }
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
    if (m_tool == MarqueeTool && m_gesture == Gesture::Selection) {
        updateSelection(toImage(event->position()), event->modifiers());
        if (m_selectionAction == SelectionAction::Fill) {
            // 克隆结果不再烘焙进绘画层：改由一张浮动图像承载，之后还能继续移动。
            m_doc.cancelEdit();           // 丢弃拖动过程中写进绘画层的预览像素
            const int imageIndex = cloneSelectionToFloatingImage();
            if (imageIndex >= 0) {
                clearSelection();
                m_gesture = Gesture::Idle;
                m_stateChanged = false;
                emit statusMessage(tr("已把拖动结果生成为浮动图像，可直接拖动移动"), 3000);
                update();
                return;
            }
        } else {
            m_doc.commitEdit(m_stateChanged);
        }
        if (m_selectionRect.width() < 1 || m_selectionRect.height() < 1)
            clearSelection();
        m_selectionAction = SelectionAction::None;
        m_selectionSampleSource = QImage();
        m_selectionPaintBefore = QImage();
        m_gesture = Gesture::Idle;
        m_stateChanged = false;
        update();
        return;
    }
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
                const int sourceIndex = m_doc.selectedPlane();
                if (sourceIndex >= 0 && sourceIndex != index)
                    m_doc.lockPlaneEdge(sourceIndex, m_planeTool.edge());
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

// 键盘事件：Ctrl+V 粘贴图像、Esc 取消当前操作、Delete 删除选中图像/平面
void PerspectiveCanvas::keyPressEvent(QKeyEvent *event)
{
    if (event->matches(QKeySequence::Paste)) {
        pasteClipboardImage();
        event->accept();
    } else if (event->key() == Qt::Key_Escape) {
        cancelInteraction();
    } else if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)
               && hasSelectedImage()) {
        commitInteraction();
        m_doc.removeFloatingImage(m_doc.selectedImage());
        emit statusMessage(tr("已删除选中的图像"), 3000);
        update();
        event->accept();
    } else if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)
               && m_tool == EditPlane && m_doc.selectedPlane() >= 0) {
        commitInteraction();
        m_doc.removePlane(m_doc.selectedPlane());
        update();
        event->accept();
    } else {
        QWidget::keyPressEvent(event);
    }
}

