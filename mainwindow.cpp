#include "mainwindow.h"
#include "perspectivecanvas.h"

#include <QAbstractButton>
#include <QAction>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QCoreApplication>          // QT_TRANSLATE_NOOP
#include <QFileDialog>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QSlider>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

// —— 布局常量 ——
constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 820;
constexpr int kSidePanelWidth = 238;
constexpr int kToolButtonHeight = 36;
constexpr int kNumberLabelWidth = 35;
constexpr qreal kLockedOpacity = 0.45;   // 参数行被锁定时的整体不透明度

// —— 工具表 ——
// 按钮 id 直接取 PerspectiveCanvas::Tool 的枚举值，所以这里顺序必须与枚举一致。
// 新增工具时只需补一行，按钮文案、快捷键、参数行归属都从这张表派生。
struct ToolSpec
{
    PerspectiveCanvas::Tool tool;
    const char *label;     // 用 QT_TRANSLATE_NOOP 标记，真正的取译文在 buildToolButtons 的 tr()
    const char *shortcut;
};

const ToolSpec kToolSpecs[] = {
    { PerspectiveCanvas::CreatePlane,    QT_TRANSLATE_NOOP("MainWindow", "创建平面  (C)"), "C" },
    { PerspectiveCanvas::EditPlane,      QT_TRANSLATE_NOOP("MainWindow", "编辑平面  (V)"), "V" },
    { PerspectiveCanvas::BrushTool,      QT_TRANSLATE_NOOP("MainWindow", "画笔工具  (B)"), "B" },
    { PerspectiveCanvas::CloneStampTool, QT_TRANSLATE_NOOP("MainWindow", "图章工具  (S)"), "S" },
    { PerspectiveCanvas::TransformTool,  QT_TRANSLATE_NOOP("MainWindow", "变换工具  (T)"), "T" },
    { PerspectiveCanvas::MarqueeTool,    QT_TRANSLATE_NOOP("MainWindow", "选框工具  (M)"), "M" },
};

const char *const kSwatchStyle = "background:%1; border:1px solid #777; border-radius:3px;";

const char *const kStyleSheet = R"(
    QMainWindow { background:#25282c; }
    QStatusBar { background:#202328; color:#f2f5f7; border-top:1px solid #111315;
                 min-height:25px; padding-left:6px; }
    QStatusBar QLabel { color:#f2f5f7; background:transparent; }
    QStatusBar::item { border:none; }
    #sidePanel { background:#30343a; border-right:1px solid #15171a; }
    #sectionTitle { color:#f1f3f5; font-size:15px; font-weight:600; }
    #hint { color:#aeb4bc; }
    QLabel { color:#d7dbe0; }
    QLabel:disabled { color:#6f757c; }
    QCheckBox { color:#d7dbe0; }
    QToolButton { color:#dfe3e8; background:#3b4047; border:1px solid #4a5058;
                  border-radius:4px; text-align:left; padding-left:12px; }
    QToolButton:hover { background:#464c54; }
    QToolButton:checked { color:white; background:#1769aa; border-color:#2785d0; }
    QToolButton:disabled { color:#707780; background:#292d32; border-color:#353a40; }
    QToolBar QToolButton:disabled { color:#737981; background:transparent; border:none; }
    QPushButton { color:#e8eaed; background:#42474e; border:1px solid #555b64;
                  border-radius:3px; padding:4px 8px; }
    QPushButton:disabled { color:#6f757c; background:#2a2e33; border-color:#383d43; }
    QSlider::groove:horizontal { height:4px; background:#555a62; border-radius:2px; }
    QSlider::handle:horizontal { width:14px; margin:-5px 0; background:#4aa3df; border-radius:7px; }
    QSlider:disabled::groove:horizontal { background:#353a40; }
    QSlider:disabled::handle:horizontal { background:#626870; }
)";

QLabel *makeSectionTitle(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName("sectionTitle");
    return label;
}

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    buildUi();
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi()
{
    setWindowTitle(tr("消失点"));
    resize(kWindowWidth, kWindowHeight);

    buildFileToolBar();

    auto *root = new QWidget(this);
    // 画布先建：侧栏的初始值（如画笔颜色）直接取自画布，避免默认值在两处各写一份。
    m_canvas = new PerspectiveCanvas(root);

    auto *rootLayout = new QHBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);
    rootLayout->addWidget(buildSidePanel());
    rootLayout->addWidget(m_canvas, 1);
    setCentralWidget(root);

    applyStyleSheet();
    connectSignals();

    updateToolOptions(-1);   // -1：尚未选中任何工具，隐藏全部参数行
    statusBar()->showMessage(tr("请打开一张图片开始操作"));
}

void MainWindow::buildFileToolBar()
{
    auto *bar = addToolBar(tr("文件"));
    bar->setMovable(false);

    m_openAction = bar->addAction(tr("打开图像"));
    m_saveAction = bar->addAction(tr("导出结果"));
    bar->addSeparator();
    m_undoAction = bar->addAction(tr("撤销"));
    m_redoAction = bar->addAction(tr("重做"));

    // 没有文档时这些动作都不可用，等 documentAvailabilityChanged / canUndoChanged 再放开。
    m_saveAction->setEnabled(false);
    m_undoAction->setEnabled(false);
    m_redoAction->setEnabled(false);

    m_openAction->setShortcut(QKeySequence::Open);
    m_saveAction->setShortcut(QKeySequence::Save);
    m_undoAction->setShortcuts(QKeySequence::keyBindings(QKeySequence::Undo));

    QList<QKeySequence> redoShortcuts = QKeySequence::keyBindings(QKeySequence::Redo);
    const QKeySequence ctrlShiftZ("Ctrl+Shift+Z");
    if (!redoShortcuts.contains(ctrlShiftZ))
        redoShortcuts.append(ctrlShiftZ);
    m_redoAction->setShortcuts(redoShortcuts);
}

QFrame *MainWindow::buildSidePanel()
{
    auto *panel = new QFrame;   // 由 rootLayout 接管所有权
    panel->setObjectName("sidePanel");
    panel->setFixedWidth(kSidePanelWidth);

    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(14, 16, 14, 16);
    layout->setSpacing(10);

    layout->addWidget(makeSectionTitle(tr("透视工具"), panel));
    buildToolButtons(layout);

    layout->addSpacing(8);
    m_brushTitle = makeSectionTitle(tr("笔刷设置"), panel);
    layout->addWidget(m_brushTitle);
    m_diameter = addSliderRow(layout, tr("直径"), 2, 200, 42);
    m_hardness = addSliderRow(layout, tr("硬度"), 0, 100, 75);
    m_opacity = addSliderRow(layout, tr("不透明度"), 1, 100, 100);

    m_planeTitle = makeSectionTitle(tr("平面设置"), panel);
    layout->addWidget(m_planeTitle);
    m_gridSize = addSliderRow(layout, tr("网格大小"), 10, 200, 50);
    m_planeAngle = addSliderRow(layout, tr("角度"), 0, 360, 90);
    // 锁定态整行一起变灰：透明度特效作用在整行容器上，一次覆盖标签、数值和滑块。
    m_planeAngle.group->setGraphicsEffect(new QGraphicsOpacityEffect(m_planeAngle.group));

    m_cloneAligned = new QCheckBox(tr("对齐"), panel);
    m_cloneAligned->setChecked(true);
    m_cloneAligned->setToolTip(tr("勾选：松开鼠标后源点继续跟随；取消：每一笔从最初的源点重新取样"));
    layout->addWidget(m_cloneAligned);

    buildColorRow(layout);

    auto *hint = new QLabel(tr("创建：依次点击四个角点\n"
                               "编辑：拖动控制点；Ctrl+拖出垂直于当前平面的平面\n"
                               "画笔：在平面内拖动绘制\n"
                               "图章：Alt+左键取样，左键拖动仿制"),
                            panel);
    hint->setWordWrap(true);
    hint->setObjectName("hint");

    layout->addStretch();
    layout->addWidget(hint);

    return panel;
}

void MainWindow::buildToolButtons(QVBoxLayout *panel)
{
    m_tools = new QButtonGroup(this);
    m_tools->setExclusive(true);

    for (const ToolSpec &spec : kToolSpecs) {
        auto *button = new QToolButton(panel->parentWidget());
        button->setText(tr(spec.label));
        button->setCheckable(true);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setMinimumHeight(kToolButtonHeight);
        // 启动时尚未打开图片，先全部禁用；打开图片后由 documentAvailabilityChanged 放开。
        button->setEnabled(false);

        m_tools->addButton(button, static_cast<int>(spec.tool));
        panel->addWidget(button);
    }
}

// 在面板底部追加一行"名称 + 数值 + 滑块"，数值标签随滑块就地联动。
SliderRow MainWindow::addSliderRow(QVBoxLayout *panel, const QString &name,
                                   int minimum, int maximum, int value)
{
    QWidget *parent = panel->parentWidget();

    auto *group = new QWidget(parent);
    auto *groupLayout = new QVBoxLayout(group);
    groupLayout->setContentsMargins(0, 0, 0, 0);
    groupLayout->setSpacing(2);

    auto *row = new QWidget(group);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 7, 0, 0);

    auto *number = new QLabel(QString::number(value), row);
    number->setMinimumWidth(kNumberLabelWidth);
    number->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    rowLayout->addWidget(new QLabel(name, row));
    rowLayout->addStretch();
    rowLayout->addWidget(number);

    groupLayout->addWidget(row);

    auto *slider = new QSlider(Qt::Horizontal, group);
    slider->setRange(minimum, maximum);
    slider->setValue(value);
    groupLayout->addWidget(slider);

    panel->addWidget(group);

    connect(slider, &QSlider::valueChanged, number,
            [number](int v) { number->setText(QString::number(v)); });

    return {group, slider};
}

void MainWindow::buildColorRow(QVBoxLayout *panel)
{
    m_color = new QWidget(panel->parentWidget());
    auto *row = new QHBoxLayout(m_color);
    row->setContentsMargins(0, 8, 0, 0);
    row->addWidget(new QLabel(tr("画笔颜色"), m_color));
    row->addStretch();

    m_colorSwatch = new QLabel(m_color);
    m_colorSwatch->setFixedSize(44, 26);
    setSwatchColor(m_canvas->brushColor());   // 初始值以画布的画笔颜色为准

    auto *button = new QPushButton(tr("选择"), m_color);
    connect(button, &QPushButton::clicked, this, &MainWindow::chooseColor);

    row->addWidget(m_colorSwatch);
    row->addWidget(button);

    panel->addWidget(m_color);
}

void MainWindow::applyStyleSheet()
{
    setStyleSheet(QString(kStyleSheet));
}

void MainWindow::connectSignals()
{
    connectFileActions();
    connectEditingActions();
    connectToolSelection();
    connectBrushParameters();
    connectPlaneParameters();
    connectDocumentState();

    connect(m_canvas, &PerspectiveCanvas::statusMessage, statusBar(), &QStatusBar::showMessage);
}

void MainWindow::connectFileActions()
{
    connect(m_openAction, &QAction::triggered, this, &MainWindow::openImage);
    connect(m_saveAction, &QAction::triggered, this, &MainWindow::exportResult);
}

void MainWindow::connectEditingActions()
{
    connect(m_undoAction, &QAction::triggered, m_canvas, &PerspectiveCanvas::undo);
    connect(m_redoAction, &QAction::triggered, m_canvas, &PerspectiveCanvas::redo);

    auto *pasteShortcut = new QShortcut(QKeySequence::Paste, this);
    pasteShortcut->setContext(Qt::WindowShortcut);
    connect(pasteShortcut, &QShortcut::activated, m_canvas, &PerspectiveCanvas::pasteClipboardImage);
}

void MainWindow::connectToolSelection()
{
    // 工具按钮同时切换画布模式与可见的参数选项集。
    connect(m_tools, &QButtonGroup::idClicked, m_canvas, [this](int id) {
        updateToolOptions(id);
        m_canvas->setTool(static_cast<PerspectiveCanvas::Tool>(id));
    });
    // 画布主动请求切工具（例如创建完平面后自动进入编辑工具）。
    connect(m_canvas, &PerspectiveCanvas::toolChangeRequested, this,
            [this](PerspectiveCanvas::Tool tool) {
                if (QAbstractButton *button = m_tools->button(static_cast<int>(tool)))
                    button->click();
            });
    // 每个工具的快捷键取自工具表，点击对应按钮即可，不必重复实现切换逻辑。
    for (const ToolSpec &spec : kToolSpecs) {
        auto *shortcut = new QShortcut(QKeySequence(spec.shortcut), this);
        connect(shortcut, &QShortcut::activated, m_tools->button(static_cast<int>(spec.tool)),
                &QAbstractButton::click);
    }
}

void MainWindow::connectBrushParameters()
{
    connect(m_diameter.slider, &QSlider::valueChanged, m_canvas, &PerspectiveCanvas::setBrushDiameter);
    connect(m_hardness.slider, &QSlider::valueChanged, m_canvas, &PerspectiveCanvas::setBrushHardness);
    connect(m_opacity.slider, &QSlider::valueChanged, m_canvas, &PerspectiveCanvas::setBrushOpacity);
    connect(m_cloneAligned, &QCheckBox::toggled, m_canvas, &PerspectiveCanvas::setCloneAligned);
}

void MainWindow::connectPlaneParameters()
{
    connect(m_gridSize.slider, &QSlider::valueChanged, m_canvas, &PerspectiveCanvas::setGridSize);
    connect(m_planeAngle.slider, &QSlider::valueChanged, m_canvas,
            [this](int angle) { m_canvas->setPlaneAngle(angle); });
    // 画布端夹角变化（选中平面改变等）回写滑块，并同步锁定状态。
    connect(m_canvas, &PerspectiveCanvas::planeAngleChanged, this, [this](qreal angle, bool) {
        m_planeAngle.slider->setValue(qRound(angle));
        updatePlaneAngleState();
    });
}

void MainWindow::connectDocumentState()
{
    connect(m_canvas, &PerspectiveCanvas::canUndoChanged, m_undoAction, &QAction::setEnabled);
    connect(m_canvas, &PerspectiveCanvas::canRedoChanged, m_redoAction, &QAction::setEnabled);

    // 变换工具只在有浮动图像被选中时才有意义。
    connect(m_canvas, &PerspectiveCanvas::imageSelectionChanged, this, [this](bool selected) {
        m_tools->button(PerspectiveCanvas::TransformTool)->setEnabled(selected);
    });

    // 没有文档时禁用所有工具；文档就绪后默认进入创建平面工具。
    connect(m_canvas, &PerspectiveCanvas::documentAvailabilityChanged, this, [this](bool available) {
        m_saveAction->setEnabled(available);
        for (QAbstractButton *button : m_tools->buttons()) {
            const bool needsImage = m_tools->id(button) == PerspectiveCanvas::TransformTool;
            button->setEnabled(available && (!needsImage || m_canvas->hasSelectedImage()));
        }
        if (available && !m_tools->checkedButton())
            m_tools->button(PerspectiveCanvas::CreatePlane)->click();
        if (!available)
            updateToolOptions(-1);
    });
}

void MainWindow::updateToolOptions(int toolId)
{
    // toolId 为 -1 表示当前没有工具（尚未打开图片），此时隐藏全部参数行。
    const bool isBrush = toolId == PerspectiveCanvas::BrushTool;
    const bool isClone = toolId == PerspectiveCanvas::CloneStampTool;
    const bool showBrush = isBrush || isClone;   // 画笔和图章共用笔刷参数
    const bool showPlane = toolId == PerspectiveCanvas::CreatePlane
                           || toolId == PerspectiveCanvas::EditPlane;

    m_brushTitle->setVisible(showBrush);
    m_diameter.setVisible(showBrush);
    m_hardness.setVisible(showBrush);
    m_opacity.setVisible(showBrush);
    m_color->setVisible(isBrush);          // 只有画笔需要选色
    m_cloneAligned->setVisible(isClone);   // 只有图章需要对齐选项

    m_planeTitle->setVisible(showPlane);
    m_gridSize.setVisible(showPlane);
    m_planeAngle.setVisible(showPlane);
    updatePlaneAngleState();
}

void MainWindow::updatePlaneAngleState()
{
    const bool editable = m_canvas->canSetSelectedPlaneAngle();
    const QString reason = m_canvas->planeAngleLockReason();

    // 整行联动：setEnabled 让标签和滑块一起灰化，透明度特效再整体压暗一档。
    m_planeAngle.setEnabled(editable);
    if (auto *effect = qobject_cast<QGraphicsOpacityEffect *>(m_planeAngle.group->graphicsEffect()))
        effect->setOpacity(editable ? 1.0 : kLockedOpacity);

    m_planeAngle.slider->setToolTip(editable
                                        ? tr("调整子平面与父平面的夹角：0° 展开，90° 垂直，180° 折回")
                                        : reason);
    m_planeAngle.group->setToolTip(reason);
}

void MainWindow::openImage()
{
    const QString file = QFileDialog::getOpenFileName(this, tr("打开图像"), {},
        tr("图像 (*.png *.jpg *.jpeg *.bmp *.webp);;所有文件 (*)"));
    if (file.isEmpty())
        return;
    if (!m_canvas->loadImage(file))
        QMessageBox::warning(this, tr("打开失败"), tr("无法读取该图像。"));
}

void MainWindow::exportResult()
{
    const QString file = QFileDialog::getSaveFileName(this, tr("导出结果"),
        QStringLiteral("vanishing-point.png"), tr("PNG 图像 (*.png);;JPEG 图像 (*.jpg *.jpeg)"));
    if (file.isEmpty())
        return;
    if (!m_canvas->saveResult(file))
        QMessageBox::warning(this, tr("保存失败"), tr("无法写入目标文件。"));
}

// 打开颜色对话框，选择画笔颜色并同步更新色块预览
void MainWindow::chooseColor()
{
    const QColor color = QColorDialog::getColor(m_canvas->brushColor(), this, tr("选择画笔颜色"));
    if (!color.isValid())
        return;
    m_canvas->setBrushColor(color);
    setSwatchColor(color);
}

void MainWindow::setSwatchColor(const QColor &color)
{
    m_colorSwatch->setStyleSheet(QString(kSwatchStyle).arg(color.name()));
}

void SliderRow::setVisible(bool visible)
{
    group->setVisible(visible);
}

void SliderRow::setEnabled(bool enabled)
{
    group->setEnabled(enabled);
}
