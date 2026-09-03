#include "canvasdocument.h"

#include <QTransform>

namespace {
// 历史快照可能包含多张大纹理，因此限制历史数量以防内存失控。
constexpr int MaxHistoryStates = 40;
}

// 构造函数：以空文档的初始状态作为第一份历史快照
CanvasDocument::CanvasDocument(QObject *parent) : QObject(parent)
{
    resetHistory();
}

// 从文件加载背景图像，并清空平面与浮动图像、重置历史记录
bool CanvasDocument::loadImage(const QString &fileName)
{
    QImage image(fileName);
    if (image.isNull())
        return false;
    m_background = image.convertToFormat(QImage::Format_ARGB32);
    m_hasLoadedImage = true;
    emit documentAvailabilityChanged(true);
    m_planes.clear();
    m_paintLayer = QImage(m_background.size(), QImage::Format_ARGB32_Premultiplied);
    m_paintLayer.fill(Qt::transparent);
    m_pastedImages.clear();
    m_activePastedImage = -1;
    m_selectedPlane = -1;
    resetHistory();
    return true;
}

// 设置背景图像而不重置历史（仅在画布初始化默认背景时使用）
void CanvasDocument::setBackground(const QImage &image)
{
    m_background = image.convertToFormat(QImage::Format_ARGB32);
    m_paintLayer = QImage(m_background.size(), QImage::Format_ARGB32_Premultiplied);
    m_paintLayer.fill(Qt::transparent);
}

// 分配一个新的展开曲面分组号（比现有最大分组号大 1）
int CanvasDocument::nextSurfaceGroupId() const
{
    int nextSurfaceGroup = 0;
    for (const Plane &existing : m_planes)
        nextSurfaceGroup = qMax(nextSurfaceGroup, existing.surfaceGroup + 1);
    return nextSurfaceGroup;
}

// 清除所有平面上的绘画内容（不影响平面几何本身）
void CanvasDocument::clearPainting()
{
    if (m_paintLayer.isNull()) return;
    m_paintLayer.fill(Qt::transparent);
    commitHistory();
}

// 删除指定平面，并修正浮动图像宿主索引与选中索引。
// 若宿主平面被删除，则尝试在同组内另寻宿主；找不到时浮动图像
// 脱离曲面并回到画布左上角。
void CanvasDocument::removePlane(int removedPlane)
{
    if (removedPlane < 0 || removedPlane >= m_planes.size())
        return;
    m_planes.removeAt(removedPlane);
    for (PastedImage &image : m_pastedImages) {
        if (image.hostPlane > removedPlane) {
            --image.hostPlane;
        } else if (image.hostPlane == removedPlane) {
            image.hostPlane = -1;
            for (int i = 0; i < m_planes.size(); ++i) {
                if (m_planes[i].surfaceGroup == image.surfaceGroup) {
                    image.hostPlane = i;
                    break;
                }
            }
            if (image.hostPlane < 0) {
                image.attached = false;
                image.surfaceGroup = -1;
                image.position = QPointF(0, 0);
            }
        }
    }
    m_selectedPlane = qMin(m_selectedPlane, m_planes.size() - 1);
    commitHistory();
}

// 设置新的浮动图像：放到画布左上角并重置其吸附状态
void CanvasDocument::setFloatingImage(const QImage &image)
{
    m_pastedImages.append(PastedImage{image.convertToFormat(QImage::Format_ARGB32), QPointF(0, 0), false, -1, -1});
    m_activePastedImage = m_pastedImages.size() - 1;
    commitHistory();
}

// 将浮动图像顺时针旋转 90°。
// 直接旋转位图本身，保持其左上角位置不变（无论该位置处于
// 画布坐标还是共享展开曲面坐标系中）。
bool CanvasDocument::rotateFloatingImage()
{
    if (!hasFloatingImage())
        return false;
    m_pastedImages[m_activePastedImage].image = m_pastedImages[m_activePastedImage].image.transformed(QTransform().rotate(90),
                                               Qt::SmoothTransformation);
    commitHistory();
    return true;
}

// 将浮动图像水平或垂直翻转
bool CanvasDocument::flipFloatingImage(bool horizontal, bool vertical)
{
    if (!hasFloatingImage())
        return false;
    m_pastedImages[m_activePastedImage].image = m_pastedImages[m_activePastedImage].image.mirrored(horizontal, vertical);
    commitHistory();
    return true;
}

// 一次性更新浮动图像的完整放置状态（位置 + 吸附信息）
void CanvasDocument::setFloatingImagePlacement(const QPointF &position, bool attached,
                                               int surfaceGroup, int hostPlane)
{
    if (!hasFloatingImage()) return;
    auto &item = m_pastedImages[m_activePastedImage]; item.position = position; item.attached = attached; item.surfaceGroup = surfaceGroup; item.hostPlane = hostPlane;
}

// 已吸附状态下仅移动浮动图像位置（仍处于展开曲面坐标系中）
void CanvasDocument::setPastedImagePosition(const QPointF &position)
{
    if (hasFloatingImage()) m_pastedImages[m_activePastedImage].position = position;
}

// 撤销：回退到上一份快照
bool CanvasDocument::undo()
{
    if (m_historyIndex <= 0)
        return false;
    restoreState(m_history[--m_historyIndex]);
    emit canUndoChanged(m_historyIndex > 0);
    emit canRedoChanged(true);
    return true;
}

// 重做：前进到下一份快照
bool CanvasDocument::redo()
{
    if (m_historyIndex + 1 >= m_history.size())
        return false;
    restoreState(m_history[++m_historyIndex]);
    emit canUndoChanged(true);
    emit canRedoChanged(m_historyIndex + 1 < m_history.size());
    return true;
}

// 清空历史并以当前状态作为初始快照（用于加载新文档）
void CanvasDocument::resetHistory()
{
    m_history.clear();
    m_history.append(captureState());
    m_historyIndex = 0;
    emit canUndoChanged(false);
    emit canRedoChanged(false);
}

// 提交一次状态变更：丢弃旧的重做分支，追加新快照并裁剪历史长度
void CanvasDocument::commitHistory()
{
    while (m_history.size() > m_historyIndex + 1)
        m_history.removeLast();
    m_history.append(captureState());
    ++m_historyIndex;
    if (m_history.size() > MaxHistoryStates) {
        m_history.removeFirst();
        --m_historyIndex;
    }
    emit canUndoChanged(m_historyIndex > 0);
    emit canRedoChanged(false);
}

// 捕获当前状态为一份历史快照
CanvasDocument::CanvasState CanvasDocument::captureState() const
{
    return CanvasState{m_planes, m_selectedPlane, m_paintLayer, m_pastedImages, m_activePastedImage};
}

// 恢复到指定的历史快照（仅文档数据；进行中的交互状态由画布自行清理）
void CanvasDocument::restoreState(const CanvasState &state)
{
    m_planes = state.planes;
    m_selectedPlane = state.selectedPlane;
    m_paintLayer = state.paintLayer;
    m_pastedImages = state.pastedImages;
    m_activePastedImage = state.activePastedImage;
}

const QImage &CanvasDocument::pastedImage() const { static const QImage empty; return hasFloatingImage() ? m_pastedImages[m_activePastedImage].image : empty; }
QPointF CanvasDocument::pastedImagePosition() const { return hasFloatingImage() ? m_pastedImages[m_activePastedImage].position : QPointF(); }
bool CanvasDocument::pastedImageAttached() const { return hasFloatingImage() && m_pastedImages[m_activePastedImage].attached; }
int CanvasDocument::pastedSurfaceGroup() const { return hasFloatingImage() ? m_pastedImages[m_activePastedImage].surfaceGroup : -1; }
int CanvasDocument::pastedHostPlane() const { return hasFloatingImage() ? m_pastedImages[m_activePastedImage].hostPlane : -1; }
