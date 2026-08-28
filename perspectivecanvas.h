#pragma once

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QVector>
#include <QWidget>

class QPainter;

class PerspectiveCanvas : public QWidget
{
    Q_OBJECT
public:
    enum Tool { CreatePlane, EditPlane, StampTool, BrushTool };
    Q_ENUM(Tool)

    explicit PerspectiveCanvas(QWidget *parent = nullptr);
    bool loadImage(const QString &fileName);
    bool saveResult(const QString &fileName) const;
    bool placeImage(const QString &fileName);
    bool hasSelectedPlane() const { return m_selectedPlane >= 0 && m_selectedPlane < m_planes.size(); }
    QColor brushColor() const { return m_brushColor; }

public slots:
    void setTool(Tool tool);
    void setBrushDiameter(int value) { m_diameter = value; }
    void setBrushHardness(int value) { m_hardness = value / 100.0; }
    void setBrushOpacity(int value) { m_opacity = value / 100.0; }
    void setBrushColor(const QColor &color) { m_brushColor = color; }
    void clearPainting();
    void undo();
    void redo();

signals:
    void statusMessage(const QString &text, int timeout = 0);
    void toolChangeRequested(Tool tool);
    void canUndoChanged(bool available);
    void canRedoChanged(bool available);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    bool focusNextPrevChild(bool next) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dropEvent(QDropEvent *) override;

private:
    struct Plane {
        QPointF corner[4];
        QImage content;
        QImage paint;
        QString name;
    };

    struct CanvasState {
        QVector<Plane> planes;
        int selectedPlane = -1;
    };

    QPointF toImage(const QPointF &widgetPoint) const;
    QPointF toWidget(const QPointF &imagePoint) const;
    void updateViewTransform();
    int planeAt(const QPointF &imagePoint) const;
    int handleAt(const Plane &plane, const QPointF &imagePoint) const;
    int edgeAt(const Plane &plane, const QPointF &imagePoint) const;
    QVector<QPointF> handles(const Plane &plane) const;
    QPointF planeToUv(const Plane &plane, const QPointF &point, bool *ok = nullptr) const;
    QPointF uvToPlane(const Plane &plane, const QPointF &uv) const;
    void renderScene(QPainter &painter, bool showGuides) const;
    void renderProjectedImage(QPainter &painter, const Plane &plane, const QImage &texture) const;
    void drawPlaneGuides(QPainter &painter, const Plane &plane, bool selected) const;
    void applyDab(Plane &plane, const QPointF &uv, bool stamp);
    void drawStrokeTo(const QPointF &imagePoint, bool stamp);
    void updateHoverCursor(const QPointF &imagePoint);
    Plane makePerpendicularPlane(const Plane &source, int edge, const QPointF &dragPoint) const;
    Plane resizePlaneAlongEdge(const Plane &source, int edge, const QPointF &dragPoint) const;
    bool perpendicularDirection(const Plane &source, const QPointF &atPoint,
                                QPointF *direction) const;
    static bool isValidPlane(const Plane &plane);
    CanvasState captureState() const;
    void restoreState(const CanvasState &state);
    void resetHistory();
    void commitHistory();
    static qreal distanceToSegment(const QPointF &p, const QPointF &a, const QPointF &b,
                                   qreal *t = nullptr);

    QImage m_background;
    bool m_hasLoadedImage = false;
    QVector<Plane> m_planes;
    QVector<QPointF> m_creationPoints;
    Tool m_tool = CreatePlane;
    int m_selectedPlane = -1;
    int m_dragHandle = -1;
    int m_dragEdge = -1;
    bool m_dragging = false;
    bool m_drawing = false;
    bool m_extruding = false;
    QPointF m_pressImagePoint;
    QPointF m_lastImagePoint;
    QPointF m_lastUv;
    Plane m_dragStartPlane;
    Plane m_extrudePreview;
    bool m_hasExtrudePreview = false;
    QPointF m_cloneSourceUv;
    QPointF m_cloneAnchorUv;
    bool m_hasCloneSource = false;
    bool m_cloneStrokeStarted = false;
    int m_cloneSourcePlane = -1;
    qreal m_scale = 1.0;
    QPointF m_offset;
    int m_diameter = 42;
    qreal m_hardness = .75;
    qreal m_opacity = 1.0;
    QColor m_brushColor = QColor("#e85d4a");
    QVector<CanvasState> m_history;
    int m_historyIndex = -1;
    bool m_stateChanged = false;
};
