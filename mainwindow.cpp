#include "mainwindow.h"
#include "perspectivecanvas.h"

#include <QAction>
#include <QAbstractButton>
#include <QButtonGroup>
#include <QColorDialog>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QStatusBar>
#include <QShortcut>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    buildUi();
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi()
{
    setWindowTitle(tr("消失点 / Vanishing Point"));
    resize(1280, 820);

    auto *fileBar = addToolBar(tr("文件"));
    fileBar->setMovable(false);
    auto *openAction = fileBar->addAction(tr("打开图像"));
    auto *saveAction = fileBar->addAction(tr("导出结果"));
    fileBar->addSeparator();
    auto *undoAction = fileBar->addAction(tr("撤销"));
    auto *redoAction = fileBar->addAction(tr("重做"));
    undoAction->setEnabled(false);
    redoAction->setEnabled(false);
    undoAction->setShortcuts(QKeySequence::keyBindings(QKeySequence::Undo));
    QList<QKeySequence> redoShortcuts = QKeySequence::keyBindings(QKeySequence::Redo);
    if (!redoShortcuts.contains(QKeySequence("Ctrl+Shift+Z")))
        redoShortcuts.append(QKeySequence("Ctrl+Shift+Z"));
    redoAction->setShortcuts(redoShortcuts);
    fileBar->addSeparator();
    auto *clearPaintAction = fileBar->addAction(tr("清除绘画"));

    auto *root = new QWidget(this);
    auto *rootLayout = new QHBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);
    auto *side = new QFrame(root);
    side->setObjectName("sidePanel");
    side->setFixedWidth(238);
    auto *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(14, 16, 14, 16);
    sideLayout->setSpacing(10);
    auto *title = new QLabel(tr("透视工具"), side);
    title->setObjectName("sectionTitle");
    sideLayout->addWidget(title);

    m_tools = new QButtonGroup(this);
    m_tools->setExclusive(true);
    const QStringList toolNames{tr("创建平面  (C)"), tr("编辑平面  (V)"),
                                tr("图章工具  (S)"), tr("画笔工具  (B)")};
    for (int i = 0; i < toolNames.size(); ++i) {
        auto *button = new QToolButton(side);
        button->setText(toolNames[i]);
        button->setCheckable(true);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setMinimumHeight(36);
        m_tools->addButton(button, i);
        sideLayout->addWidget(button);
    }
    m_tools->button(0)->setChecked(true);

    auto addSlider = [side, sideLayout](const QString &name, int minimum, int maximum,
                                        int value, QWidget **rowOutput) {
        auto *row = new QWidget(side);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 7, 0, 0);
        auto *number = new QLabel(QString::number(value), row);
        number->setMinimumWidth(35);
        number->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        layout->addWidget(new QLabel(name, row));
        layout->addStretch();
        layout->addWidget(number);
        sideLayout->addWidget(row);
        auto *slider = new QSlider(Qt::Horizontal, side);
        slider->setRange(minimum, maximum);
        slider->setValue(value);
        sideLayout->addWidget(slider);
        *rowOutput = row;
        QObject::connect(slider, &QSlider::valueChanged, number,
                         [number](int v) { number->setText(QString::number(v)); });
        return slider;
    };

    sideLayout->addSpacing(8);
    m_brushTitle = new QLabel(tr("笔刷设置"), side);
    m_brushTitle->setObjectName("sectionTitle");
    sideLayout->addWidget(m_brushTitle);
    m_diameter = addSlider(tr("直径"), 2, 200, 42, &m_diameterRow);
    m_hardness = addSlider(tr("硬度"), 0, 100, 75, &m_hardnessRow);
    m_opacity = addSlider(tr("不透明度"), 1, 100, 100, &m_opacityRow);

    m_colorRow = new QWidget(side);
    auto *colorLayout = new QHBoxLayout(m_colorRow);
    colorLayout->setContentsMargins(0, 8, 0, 0);
    colorLayout->addWidget(new QLabel(tr("画笔颜色"), m_colorRow));
    colorLayout->addStretch();
    m_colorSwatch = new QLabel(m_colorRow);
    m_colorSwatch->setFixedSize(44, 26);
    m_colorSwatch->setStyleSheet("background:#e85d4a; border:1px solid #777; border-radius:3px;");
    colorLayout->addWidget(m_colorSwatch);
    auto *colorButton = new QPushButton(tr("选择"), m_colorRow);
    colorLayout->addWidget(colorButton);
    sideLayout->addWidget(m_colorRow);

    auto *hint = new QLabel(tr("创建：依次点击四个角点\n编辑：拖动控制点；Ctrl+拖边创建垂直面\n图章：Alt+单击设置源点\n画笔/图章：拖动绘制"), side);
    hint->setWordWrap(true);
    hint->setObjectName("hint");
    sideLayout->addStretch();
    sideLayout->addWidget(hint);

    m_canvas = new PerspectiveCanvas(root);
    rootLayout->addWidget(side);
    rootLayout->addWidget(m_canvas, 1);
    setCentralWidget(root);

    setStyleSheet(R"(
        QMainWindow { background:#25282c; }
        QStatusBar { background:#202328; color:#f2f5f7; border-top:1px solid #111315;
                     min-height:25px; padding-left:6px; }
        QStatusBar QLabel { color:#f2f5f7; background:transparent; }
        QStatusBar::item { border:none; }
        #sidePanel { background:#30343a; border-right:1px solid #15171a; }
        #sectionTitle { color:#f1f3f5; font-size:15px; font-weight:600; }
        #hint { color:#aeb4bc; }
        QLabel { color:#d7dbe0; }
        QToolButton { color:#dfe3e8; background:#3b4047; border:1px solid #4a5058;
                      border-radius:4px; text-align:left; padding-left:12px; }
        QToolButton:hover { background:#464c54; }
        QToolButton:checked { color:white; background:#1769aa; border-color:#2785d0; }
        QPushButton { color:#e8eaed; background:#42474e; border:1px solid #555b64;
                      border-radius:3px; padding:4px 8px; }
        QSlider::groove:horizontal { height:4px; background:#555a62; border-radius:2px; }
        QSlider::handle:horizontal { width:14px; margin:-5px 0; background:#4aa3df; border-radius:7px; }
    )");

    connect(m_tools, &QButtonGroup::idClicked, m_canvas, [this](int id) {
        updateToolOptions(id);
        m_canvas->setTool(static_cast<PerspectiveCanvas::Tool>(id));
    });
    connect(m_canvas, &PerspectiveCanvas::toolChangeRequested, this,
            [this](PerspectiveCanvas::Tool tool) {
                if (QAbstractButton *button = m_tools->button(static_cast<int>(tool)))
                    button->click();
            });
    const QList<QKeySequence> shortcuts{QKeySequence("C"), QKeySequence("V"),
                                        QKeySequence("S"), QKeySequence("B")};
    for (int i = 0; i < shortcuts.size(); ++i) {
        auto *shortcut = new QShortcut(shortcuts[i], this);
        connect(shortcut, &QShortcut::activated, m_tools->button(i), &QAbstractButton::click);
    }
    connect(m_diameter, &QSlider::valueChanged, m_canvas, &PerspectiveCanvas::setBrushDiameter);
    connect(m_hardness, &QSlider::valueChanged, m_canvas, &PerspectiveCanvas::setBrushHardness);
    connect(m_opacity, &QSlider::valueChanged, m_canvas, &PerspectiveCanvas::setBrushOpacity);
    connect(colorButton, &QPushButton::clicked, this, &MainWindow::chooseColor);
    connect(openAction, &QAction::triggered, this, [this] {
        const QString file = QFileDialog::getOpenFileName(this, tr("打开图像"), {},
            tr("图像 (*.png *.jpg *.jpeg *.bmp *.webp);;所有文件 (*)"));
        if (!file.isEmpty() && !m_canvas->loadImage(file))
            QMessageBox::warning(this, tr("打开失败"), tr("无法读取该图像。"));
    });
    connect(saveAction, &QAction::triggered, this, [this] {
        const QString file = QFileDialog::getSaveFileName(this, tr("导出结果"), "vanishing-point.png",
                                                          tr("PNG 图像 (*.png);;JPEG 图像 (*.jpg)"));
        if (!file.isEmpty() && !m_canvas->saveResult(file))
            QMessageBox::warning(this, tr("保存失败"), tr("无法写入目标文件。"));
    });
    connect(clearPaintAction, &QAction::triggered, m_canvas, &PerspectiveCanvas::clearPainting);
    connect(undoAction, &QAction::triggered, m_canvas, &PerspectiveCanvas::undo);
    connect(redoAction, &QAction::triggered, m_canvas, &PerspectiveCanvas::redo);
    connect(m_canvas, &PerspectiveCanvas::canUndoChanged, undoAction, &QAction::setEnabled);
    connect(m_canvas, &PerspectiveCanvas::canRedoChanged, redoAction, &QAction::setEnabled);
    connect(m_canvas, &PerspectiveCanvas::statusMessage, statusBar(), &QStatusBar::showMessage);
    updateToolOptions(PerspectiveCanvas::CreatePlane);
    statusBar()->showMessage(tr("使用创建平面工具依次点击四个点"));
}

void MainWindow::updateToolOptions(int toolId)
{
    const bool isPaintingTool = toolId == PerspectiveCanvas::StampTool ||
                                toolId == PerspectiveCanvas::BrushTool;
    const bool isBrushTool = toolId == PerspectiveCanvas::BrushTool;

    m_brushTitle->setVisible(isPaintingTool);
    m_diameterRow->setVisible(isPaintingTool);
    m_diameter->setVisible(isPaintingTool);
    m_hardnessRow->setVisible(isPaintingTool);
    m_hardness->setVisible(isPaintingTool);
    m_opacityRow->setVisible(isPaintingTool);
    m_opacity->setVisible(isPaintingTool);
    m_colorRow->setVisible(isBrushTool);
}

void MainWindow::chooseColor()
{
    const QColor color = QColorDialog::getColor(m_canvas->brushColor(), this, tr("选择画笔颜色"));
    if (!color.isValid())
        return;
    m_canvas->setBrushColor(color);
    m_colorSwatch->setStyleSheet(QString("background:%1; border:1px solid #777; border-radius:3px;")
                                     .arg(color.name()));
}
