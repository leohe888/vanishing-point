#include "canvasdocument.h"

#include <QPainter>
#include <QLineF>
#include <QTransform>

namespace {
// 历史条目可能包含多张大纹理的脏矩形，因此限制历史数量以防内存失控。
constexpr int MaxHistoryStates = 40;
}

// 构造函数：以空文档的初始状态作为第一份历史状态
CanvasDocument::CanvasDocument(QObject *parent) : QObject(parent)
{
    resetHistory();
}

// 从文件加载背景图像，并清空平面、浮动图像与绘画层、重置历史记录
bool CanvasDocument::loadImage(const QString &fileName)
{
    QImage image(fileName);
    if (image.isNull())
        return false;
    m_background = image.convertToFormat(QImage::Format_ARGB32);
    m_hasLoadedImage = true;
    emit documentAvailabilityChanged(true);
    m_planes.clear();
    m_images.clear();
    m_selectedPlane = -1;
    setSelectedImage(-1);
    m_paintLayer = QImage(m_background.size(), QImage::Format_ARGB32);
    m_paintLayer.fill(Qt::transparent);
    m_paintTransactionActive = false;
    resetHistory();
    return true;
}

// 设置背景图像而不重置历史（仅在画布初始化默认背景时使用）
void CanvasDocument::setBackground(const QImage &image)
{
    m_background = image.convertToFormat(QImage::Format_ARGB32);
    // 绘画层需与背景同尺寸（仅初始化时绘画层尚未建立，或尺寸不一致时重建）
    if (m_paintLayer.isNull() || m_paintLayer.size() != m_background.size()) {
        m_paintLayer = QImage(m_background.size(), QImage::Format_ARGB32);
        m_paintLayer.fill(Qt::transparent);
    }
}

// 分配一个新的展开曲面分组号（比现有最大分组号大 1）
int CanvasDocument::nextSurfaceGroupId() const
{
    int nextSurfaceGroup = 0;
    for (const Plane &existing : m_planes)
        nextSurfaceGroup = qMax(nextSurfaceGroup, existing.surfaceGroup + 1);
    return nextSurfaceGroup;
}

// 绘画层是否含有任何不透明像素（供“清除绘画”按钮判断是否有内容可清除）
bool CanvasDocument::hasPaintContent() const
{
    if (m_paintLayer.isNull())
        return false;
    for (int y = 0; y < m_paintLayer.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(m_paintLayer.constScanLine(y));
        for (int x = 0; x < m_paintLayer.width(); ++x) {
            if (qAlpha(line[x]) != 0)
                return true;
        }
    }
    return false;
}

// 清空绘画层（不影响平面几何与浮动图像）
void CanvasDocument::clearPainting()
{
    if (m_paintLayer.isNull())
        return;
    beginPaintTransaction();
    addPaintDirty(m_paintLayer.rect());
    m_paintLayer.fill(Qt::transparent);
    commitHistory();
}

// 开始一次绘画事务：浅拷贝当前绘画层作为撤销基线（隐式共享，O(1)）
void CanvasDocument::beginPaintTransaction()
{
    m_paintBefore = m_paintLayer;
    m_paintDirtyRect = QRect();
    m_paintTransactionActive = true;
}

// 累积本次绘画事务的脏矩形
void CanvasDocument::addPaintDirty(const QRect &rect)
{
    if (rect.isEmpty())
        return;
    m_paintDirtyRect = m_paintDirtyRect.isEmpty() ? rect : m_paintDirtyRect.united(rect);
}

// 删除指定平面。内容已与平面解耦：浮动图像持有自己的几何快照，
int CanvasDocument::appendPlane(const Plane &plane)
{
    if (!PlaneMath::isValidPlane(plane))
        return -1;
    m_planes.append(plane);
    return m_planes.size() - 1;
}

bool CanvasDocument::setPlane(int index, const Plane &plane)
{
    if (index < 0 || index >= m_planes.size() || !PlaneMath::isValidPlane(plane))
        return false;
    m_planes[index] = plane;
    return true;
}

bool CanvasDocument::setImage(int index, const FloatingImage &image)
{
    if (index < 0 || index >= m_images.size() || image.image.isNull()
        || !qIsFinite(image.position.x()) || !qIsFinite(image.position.y())
        || !qIsFinite(image.rotation) || !qIsFinite(image.scale.x()) || !qIsFinite(image.scale.y())
        || image.scale.x() <= 0 || image.scale.y() <= 0)
        return false;
    m_images[index] = image;
    return true;
}

// 删除指定平面。内容已与平面解耦：浮动图像持有自己的几何快照，
// 绘画层独立于平面，因此删除平面无需修正任何内容。
void CanvasDocument::removePlane(int index)
{
    if (index < 0 || index >= m_planes.size())
        return;
    // If this plane was created by extrusion, release every matching edge on
    // its neighbours before removing it so their shared-edge handles return.
    const Plane removed = m_planes[index];
    for (int other = 0; other < m_planes.size(); ++other) {
        if (other == index)
            continue;
        if (m_planes[other].parentPlane == index) {
            m_planes[other].parentPlane = -1;
            m_planes[other].parentEdge = -1;
            m_planes[other].relativeAngle = 90.0;
            m_planes[other].angleAdjusted = false;
        } else if (m_planes[other].parentPlane > index) {
            --m_planes[other].parentPlane;
        }
        for (int edge = 0; edge < 4; ++edge) {
            const QPointF a = m_planes[other].corner[edge];
            const QPointF b = m_planes[other].corner[(edge + 1) % 4];
            for (int removedEdge = 0; removedEdge < 4; ++removedEdge) {
                const QPointF ra = removed.corner[removedEdge];
                const QPointF rb = removed.corner[(removedEdge + 1) % 4];
                if ((QLineF(a, ra).length() < 0.01 && QLineF(b, rb).length() < 0.01) ||
                    (QLineF(a, rb).length() < 0.01 && QLineF(b, ra).length() < 0.01)) {
                    m_planes[other].lockedEdges &= quint8(~(1u << edge));
                    break;
                }
            }
        }
    }
    m_planes.removeAt(index);
    if (m_selectedPlane > index)
        --m_selectedPlane;
    else if (m_selectedPlane == index)
        m_selectedPlane = m_planes.isEmpty() ? -1 : qMin(index, m_planes.size() - 1);
    commitHistory();
}

// 追加一张浮动图像到画布左上角，返回其索引
int CanvasDocument::addFloatingImage(const QImage &image)
{
    FloatingImage floating;
    floating.image = image.convertToFormat(QImage::Format_ARGB32);
    floating.position = QPointF(0, 0);
    floating.attached = false;
    floating.hostFace = -1;
    m_images.append(floating);
    setSelectedImage(m_images.size() - 1);
    commitHistory();
    return m_selectedImage;
}

int CanvasDocument::addFloatingImageOnSurface(const QImage &image,
                                               const QVector<Facet> &faces,
                                               int hostFace,
                                               const QPointF &surfacePosition)
{
    if (image.isNull() || faces.isEmpty() || hostFace < 0 || hostFace >= faces.size())
        return -1;
    FloatingImage floating;
    floating.image = image.convertToFormat(QImage::Format_ARGB32);
    floating.position = surfacePosition;
    floating.attached = true;
    floating.faces = faces;
    floating.hostFace = hostFace;
    m_images.append(floating);
    setSelectedImage(m_images.size() - 1);
    commitHistory();
    return m_selectedImage;
}

void CanvasDocument::lockPlaneEdge(int index, int edge)
{
    if (index >= 0 && index < m_planes.size() && edge >= 0 && edge < 4)
        m_planes[index].lockedEdges |= quint8(1u << edge);
}

// 删除浮动图像，并将选中项移动到删除位置上的下一张（若无则为上一张）。
void CanvasDocument::removeFloatingImage(int index)
{
    if (index < 0 || index >= m_images.size())
        return;

    m_images.removeAt(index);
    int nextSelection = m_selectedImage;
    if (m_selectedImage == index)
        nextSelection = m_images.isEmpty() ? -1 : qMin(index, m_images.size() - 1);
    else if (m_selectedImage > index)
        --nextSelection;
    setSelectedImage(nextSelection);
    commitHistory();
}

// 仅移动图像位置（不改变吸附状态）
void CanvasDocument::setSelectedImage(int index)
{
    index = index >= 0 && index < m_images.size() ? index : -1;
    if (m_selectedImage == index)
        return;
    m_selectedImage = index;
    emit imageSelectionChanged(index >= 0);
}

void CanvasDocument::setImagePosition(int index, const QPointF &position)
{
    if (index < 0 || index >= m_images.size())
        return;
    m_images[index].position = position;
}

// 把图像吸附到一组几何快照上（严格快照：此后平面增删改不再影响它）
void CanvasDocument::attachImage(int index, const QVector<Facet> &faces, int hostFace,
                                 const QPointF &surfacePosition)
{
    if (index < 0 || index >= m_images.size())
        return;
    FloatingImage &img = m_images[index];
    img.faces = faces;
    img.hostFace = hostFace;
    img.position = surfacePosition;
    img.attached = true;
}

// 让图像脱离曲面，回到画布坐标
void CanvasDocument::detachImage(int index, const QPointF &canvasPosition)
{
    if (index < 0 || index >= m_images.size())
        return;
    FloatingImage &img = m_images[index];
    img.faces.clear();
    img.hostFace = -1;
    img.position = canvasPosition;
    img.attached = false;
}

// 将指定浮动图像顺时针旋转 90°（直接旋转位图，位置不变）
bool CanvasDocument::rotateImage(int index)
{
    if (index < 0 || index >= m_images.size() || m_images[index].image.isNull())
        return false;
    m_images[index].image = m_images[index].image.transformed(QTransform().rotate(90),
                                                               Qt::SmoothTransformation);
    m_images[index].scale = QPointF(m_images[index].scale.y(), m_images[index].scale.x());
    commitHistory();
    return true;
}

// 将指定浮动图像水平或垂直翻转
bool CanvasDocument::flipImage(int index, bool horizontal, bool vertical)
{
    if (index < 0 || index >= m_images.size() || m_images[index].image.isNull())
        return false;
    m_images[index].image = m_images[index].image.mirrored(horizontal, vertical);
    commitHistory();
    return true;
}

// 撤销：回退到上一状态（结构 + 绘画层脏矩形反演）
bool CanvasDocument::undo()
{
    if (m_historyIndex <= 0)
        return false;
    const HistoryEntry &leaving = m_history[m_historyIndex];
    --m_historyIndex;
    restoreStructure(m_history[m_historyIndex]);
    if (!leaving.paintRect.isEmpty())
        applyPaint(leaving.paintRect, leaving.paintBefore);
    emit canUndoChanged(m_historyIndex > 0);
    emit canRedoChanged(true);
    return true;
}

// 重做：前进到下一状态
bool CanvasDocument::redo()
{
    if (m_historyIndex + 1 >= m_history.size())
        return false;
    ++m_historyIndex;
    const HistoryEntry &target = m_history[m_historyIndex];
    restoreStructure(target);
    if (!target.paintRect.isEmpty())
        applyPaint(target.paintRect, target.paintAfter);
    emit canUndoChanged(true);
    emit canRedoChanged(m_historyIndex + 1 < m_history.size());
    return true;
}

// 清空历史并以当前状态作为初始状态（用于加载新文档）
void CanvasDocument::resetHistory()
{
    m_editActive = false;
    m_editBefore = HistoryEntry();
    m_editPaintBefore = QImage();
    m_history.clear();
    HistoryEntry initial;
    initial.planes = m_planes;
    initial.selectedPlane = m_selectedPlane;
    initial.images = m_images;
    initial.selectedImage = m_selectedImage;
    m_history.append(initial);
    m_historyIndex = 0;
    m_paintTransactionActive = false;
    m_paintBefore = QImage();
    m_paintDirtyRect = QRect();
    emit canUndoChanged(false);
    emit canRedoChanged(false);
}

// 提交一次状态变更：丢弃旧的重做分支，追加新状态并裁剪历史长度
void CanvasDocument::beginEdit()
{
    if (m_editActive)
        return;
    m_editBefore.planes = m_planes;
    m_editBefore.selectedPlane = m_selectedPlane;
    m_editBefore.images = m_images;
    m_editBefore.selectedImage = m_selectedImage;
    m_editPaintBefore = m_paintLayer;
    m_editActive = true;
}

void CanvasDocument::commitEdit(bool changed)
{
    if (!m_editActive)
        return;
    m_editActive = false;
    m_editBefore = HistoryEntry();
    m_editPaintBefore = QImage();
    if (changed)
        commitHistory();
    else {
        m_paintTransactionActive = false;
        m_paintBefore = QImage();
        m_paintDirtyRect = QRect();
    }
}

void CanvasDocument::cancelEdit()
{
    if (!m_editActive)
        return;
    // 恢复选中会同步通知 UI，先结束事务以避免信号重入。
    m_editActive = false;
    const HistoryEntry before = m_editBefore;
    m_paintLayer = m_editPaintBefore;
    m_editBefore = HistoryEntry();
    m_editPaintBefore = QImage();
    m_paintBefore = QImage();
    m_paintDirtyRect = QRect();
    m_paintTransactionActive = false;
    restoreStructure(before);
}

void CanvasDocument::commitHistory()
{
    while (m_history.size() > m_historyIndex + 1)
        m_history.removeLast();

    HistoryEntry entry;
    entry.planes = m_planes;
    entry.selectedPlane = m_selectedPlane;
    entry.images = m_images;
    entry.selectedImage = m_selectedImage;

    // 绘画层只记录本次变动的脏矩形前后像素
    if (m_paintTransactionActive && !m_paintDirtyRect.isEmpty()) {
        const QRect dirty = m_paintDirtyRect.intersected(m_paintLayer.rect());
        if (!dirty.isEmpty()) {
            entry.paintRect = dirty;
            entry.paintBefore = m_paintBefore.copy(dirty);
            entry.paintAfter = m_paintLayer.copy(dirty);
        }
    }
    m_paintTransactionActive = false;
    m_paintBefore = QImage();
    m_paintDirtyRect = QRect();

    m_history.append(entry);
    ++m_historyIndex;
    if (m_history.size() > MaxHistoryStates) {
        m_history.removeFirst();
        --m_historyIndex;
    }
    emit canUndoChanged(m_historyIndex > 0);
    emit canRedoChanged(false);
}

// 仅恢复结构部分（平面 + 浮动图像 + 选中状态）
void CanvasDocument::restoreStructure(const HistoryEntry &entry)
{
    m_planes = entry.planes;
    m_selectedPlane = entry.selectedPlane;
    m_images = entry.images;
    setSelectedImage(entry.selectedImage);
}

// 把像素直接覆盖回绘画层的指定矩形（用于脏矩形的撤销/重做）
void CanvasDocument::applyPaint(const QRect &rect, const QImage &pixels)
{
    if (rect.isEmpty() || pixels.isNull())
        return;
    QPainter painter(&m_paintLayer);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(rect.topLeft(), pixels);
}
