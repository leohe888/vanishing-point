# 架构导览

> 面向**想改这份代码**的人。如果你只是想知道软件怎么用，请看 [README.md](../README.md)。
>
> 这份文档回答三个问题：代码分成几层、核心约定是什么、想改某个功能该看哪个文件。

---

## 1. 分层

| 层 | 类型 | 依赖 | 职责 |
| --- | --- | --- | --- |
| `VanishingCore` | 静态库 | Qt Core / Gui | 全部纯逻辑：几何、文档模型、引擎、工具状态、渲染 |
| `VanishingWidgets` | 静态库 | Qt Widgets + Core | 界面层：画布控件、主窗口 |
| `VanisingPoint` | 可执行 | Widgets | `main.cpp`，程序入口 |

**关键约束：`VanishingCore` 不引入任何 Widgets 头文件。** 这让它可以在无界面环境下被测试链接（见 `tests/`），也强制了"逻辑与界面分离"。

Core 内部再分五组：

| 组 | 文件 | 职责 |
| --- | --- | --- |
| 几何与数学 | `projectivemapping.*`、`planemath.*` | 单应变换的封装；平面几何、透视构造、命中测试 |
| 文档模型 | `canvasdocument.*`、`floatingimagemath.*`、`imagegeometry.*` | 背景 / 平面 / 绘画层 / 浮动图像 / 历史 |
| 绘制引擎 | `paintengine.*`、`clonestampengine.*` | 生成像素：软边笔触、仿制采样 |
| 工具状态 | `clonetool.*`、`planeedittool.*`、`imagetransformtool.*` | 各工具的拖动状态机，**不含界面代码** |
| 渲染 | `scenerenderer.*`、`scenecontentcache.*` | 场景合成、视口静态内容缓存 |

---

## 2. 核心约定：内容与平面解耦

这是整个项目的设计基石，**平面只描述透视规则，不持有任何内容**。

```cpp
struct Facet {
    QPointF corner[4];        // 画布上的四边形（图像坐标）
    QPointF surfaceCorner[4]; // 展开曲面上的对应四边形
};

struct Plane : Facet {
    int surfaceGroup = -1;   // 所属展开曲面分组
    quint8 lockedEdges = 0;  // 与相邻平面共用的边（位掩码）
    QString name;
};
```

注意 `Plane` 里**没有任何图像或笔迹**。内容的存放方式：

| 内容 | 存在哪 | 直接后果 |
| --- | --- | --- |
| 画笔笔迹 | 烘焙进画布同尺寸的 `paintLayer` | 平面怎么改、甚至删掉，笔迹都原样保留 |
| 浮动图像 | 各自携带吸附瞬间的几何快照 `faces` | 同上，图片也不受影响 |

```cpp
struct FloatingImage {
    QImage image;
    QPointF position;      // 未吸附 = 画布坐标；已吸附 = 展开曲面坐标
    QPointF scale;
    qreal rotation;
    bool attached = false; // 是否已吸附到某个展开曲面
    QVector<Facet> faces;  // 吸附瞬间曲面分组的几何快照（严格快照）
    int hostFace = -1;     // 宿主面在 faces 中的索引
};
```

**为什么值得这么绕**：如果内容挂在平面上，调整一个平面的形状就会把上面画的东西一起拉变形；而真实工作流里，你经常要反复微调平面来对准照片。解耦之后，平面可以随便增删改，已有内容岿然不动——代价是每次渲染都要临时"借用"平面的透视规则把内容投回画布。

**展开曲面（surface group）**：相邻的平面（例如从一条边拖出的垂直平面）共享一套摊平后的 2D 曲面坐标。这样同一张浮动图像可以跨越接缝连续显示，而每个面仍用各自的单应变换投影回画布。

---

## 3. 三套坐标系统 ⚠️ 最容易出错的地方

同一个点在这个项目里有三种写法。搞混它们是本项目最常见的 bug 来源。

| 坐标 | 含义 | 谁在用 | 映射 |
| --- | --- | --- | --- |
| **画布坐标** | 图像像素，最终真实坐标 | 一切几何的最终落点 | — |
| **展开曲面坐标** | 同一曲面分组共享的摊平空间 | 图章、浮动图像、跨接缝内容 | `surfaceMapping()` |
| **归一化 UV** | 单位正方形 `(0,0)~(1,1)` 参数化 | 画笔、绘画层 | `uvMapping()` |

对应关系（`planemath.h`）：

```cpp
ProjectiveMapping surfaceMapping(const Facet &); // surfaceCorner 四边形 ↔ corner 四边形
ProjectiveMapping uvMapping(const Facet &);      // 单位正方形    ↔ corner 四边形

QPointF planeToSurface(const Facet &, const QPointF &, bool *ok = nullptr);
QPointF planeToUv(const Facet &, const QPointF &, bool *ok = nullptr);
QPointF uvToPlane(const Facet &, const QPointF &);
```

**口诀：跟画笔、绘画层打交道用 UV；跟图章、浮动图像、跨平面接缝打交道用 surface。**

`ProjectiveMapping` 是这三套坐标的公共底座：

```cpp
ProjectiveMapping(const QPolygonF &domain, const QPolygonF &canvas);
bool toCanvas(const QPointF &, QPointF *result) const;   // domain → canvas
bool fromCanvas(const QPointF &, QPointF *result) const; // canvas → domain
static bool mapVisible(const QTransform &, const QPointF &point,
                       const QPointF &reference, QPointF *result);
```

`mapVisible` 是处理**无穷远点与地平线**的关键：射影变换的分母会变号，越过极点线的点理论上映射到"背面"，必须拒绝掉，否则会出现坐标系翻转的诡异图形。所有变换都必须经过它。

---

## 4. 一次操作的数据流

```
鼠标事件 → 工具分派 → 引擎计算 → 写入文档 → 重绘
```

1. **鼠标事件**：`perspectivecanvas.cpp` 的 `mousePressEvent` / `mouseMoveEvent` / `mouseReleaseEvent` 接收，先用 `toImage()` 转成画布坐标。
2. **工具分派**：按 `Tool` 枚举（当前工具）+ `Gesture` 枚举（进行中的手势）决定行为。
3. **引擎计算**：调用对应引擎做透视采样，产出像素，并返回**脏矩形**（本次改动影响的范围）。
4. **写入文档**：用编辑事务包裹，更新模型数据。
5. **重绘**：`update()` → `paintEvent` → `SceneRenderer` 合成 + 内容缓存。

```cpp
enum Tool { CreatePlane, EditPlane, BrushTool, CloneStampTool, TransformTool, MarqueeTool };
enum class Gesture { Idle, Plane, Image, Brush, Clone, Selection };
```

`paintEvent` 的绘制顺序（后面覆盖前面）：

1. `m_contentCache.get(...)` — 缓存的静态内容（背景 + 绘画层 + 浮动图像）
2. `translate(m_offset)` + `scale(m_scale)` — 应用视图变换
3. `SceneRenderer::render(...)` — 平面辅助线、网格、控制点、创建预览
4. 画笔光标预览（`m_paint.applyDab`）
5. 图章光标预览（`m_cloneTool.renderPreview`，所见即所得）
6. 选区蚂蚁线
7. 变换控制点
8. 图章源点十字标记

---

## 5. 模块职责速查

| 想改什么 | 看哪里 |
| --- | --- |
| 画笔手感、笔触形状 | `paintengine.cpp` |
| 图章/仿制逻辑 | `clonestampengine.cpp` + `clonetool.cpp` |
| 平面创建、垂直平面拖出 | `planemath.cpp` + `planeedittool.cpp` |
| 浮动图像移动、缩放、旋转 | `floatingimagemath.cpp` + `imagetransformtool.cpp` |
| 撤销 / 重做 | `canvasdocument.cpp` 的 `HistoryEntry` |
| 屏幕上的线、控制点怎么画 | `scenerenderer.cpp` |
| 鼠标由谁响应 | `perspectivecanvas.cpp` 的 `mousePressEvent` |
| 快捷键、工具栏 | `mainwindow.cpp` |

`perspectivecanvas.cpp` 有 1400+ 行，**不要从头读**——它是按工具分派的大分支，应该带着"我要找某个功能"的目的去搜方法名。

---

## 6. 历史与事务

历史条目采用**混合策略**：

```cpp
struct HistoryEntry {
    QVector<Plane> planes;          // 结构：整份快照
    int selectedPlane;
    QVector<FloatingImage> images;  // 结构：整份快照
    int selectedImage;
    QRect paintRect;                // 绘画层脏区域（空 = 本次无绘画变更）
    QImage paintBefore;             // 脏区域旧像素
    QImage paintAfter;              // 脏区域新像素
};
```

- **结构（平面 + 浮动图像）**：体积小、位图靠隐式共享，直接整份拷贝入栈。
- **绘画层**：整张大图。若整份入栈，几十步历史就会失控，因此**只记录脏矩形的前后像素增量**。

两种事务，注意区分：

| 事务 | API | 用途 |
| --- | --- | --- |
| 结构编辑 | `beginEdit()` / `commitEdit(changed)` / `cancelEdit()` | 平面增删改；保留操作前的 COW 快照，Esc 可完整回退 |
| 绘画 | `beginPaintTransaction()` / `addPaintDirty(rect)` | 累积本次绘制的脏矩形，提交时存入历史 |

`m_historyIndex` 指向当前可见状态；撤销后再做新编辑会丢弃旧的重做分支（与主流编辑器一致）。

---

## 7. 阅读顺序建议

从地基往上，不要跳：

1. **`planemath.h`** — 数据结构 + 坐标变换，注释是项目里最全的
2. **`projectivemapping.h`** — 单应映射，三套坐标的数学基础（只有 59 行）
3. **`canvasdocument.h`** — 模型层，看完就知道项目里有哪些数据
4. **`perspectivecanvas.h`** — 只看 `Tool` 枚举和私有方法名列表，就知道软件能干什么
5. 具体引擎（`paintengine` / `clonestampengine`）最后按需深入

---

## 8. 常见陷阱

- **坐标空间串台**：拿 `planeToUv` 当 surface 用（或反之），是最容易犯的错。症状是内容出现在错误位置或整体偏移。写新功能前先确认你要的是哪一套。
- **绕过 `mapVisible`**：直接用 `QTransform::map` 做射影变换，遇到极点/地平线会产生翻转的诡异图形。
- **改动 `corner` 后忘记同步 `surfaceCorner`**：会让整张展开图重新标定，相邻平面在接缝处把同一个曲面坐标送到不同画布点，跨缝内容就会错位（见 `resizePlaneAlongEdge` 里的详细注释）。
- **给 `Q_OBJECT` 类改成员布局后没做干净重建**：旧的 moc 产物会让元对象系统与真实类布局不一致，典型症状是启动即崩（异常码 `0xc0000374`）。必须清掉 `*_autogen/` 和目标文件后全量重建。

---

## 9. 测试

测试在 `tests/` 目录，用 Qt Test，随主项目一起构建（`add_subdirectory(tests)`）：

```bash
cd build/<配置目录>
cmake --build . --target tst_planemath tst_projectivemapping
ctest --output-on-failure
```

纯逻辑用 `QTEST_APPLESS_MAIN`（连 `QCoreApplication` 都不建），链接 `VanishingCore + Qt::Test`。加新测试：新建 `tests/tst_xxx.cpp`，然后在 `tests/CMakeLists.txt` 里复制一个 `qt_add_executable` + `add_test` 块。

> Windows 注意：在 Git Bash 里直接 `./tst_xxx.exe` 跑，stdout 是空的（Qt Test 检测到 stdout 是管道时走 `WriteConsoleW`）。退出码仍然可信（0 = 通过），看详细结果用 `-o <路径>,txt`，或用 `ctest`。
