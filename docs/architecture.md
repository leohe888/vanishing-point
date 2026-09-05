# 分阶段重构记录

本轮保留 Qt Widgets、QPainter、原始图片及画布绘画层，逐步替换几何、交互和渲染之间的耦合。现有工具和快捷键保持，Esc 的取消行为统一为恢复本次操作前的状态。

## 阶段一：统一透视规则

- `ProjectiveMapping` 独立表示无限平面的正向、逆向单应映射和有效半平面。
- `PlaneMath::surfaceMapping` 与 `uvMapping` 将现有 `Facet` 标定快照适配为映射。保留四角快照格式，以兼容垂直平面构造、已吸附图片和历史记录；不引入第二份需要手动同步的持久化矩阵。
- 平面整体移动在展开坐标中更新有限范围，再使用原投影重建四角。
- 平面、图片、画笔和图章共用地平线及有限值判断；齐次矩阵整体取负仍表示相同几何。

## 阶段二：操作事务

- `CanvasDocument::beginEdit/commitEdit/cancelEdit` 管理一次交互的生命周期。
- 预览期间保存操作前的结构与绘画层 COW 快照；取消恢复快照，不增加历史、不丢弃重做分支。
- 提交仍使用原来的绘画脏矩形增量历史，不把每笔绘画永久保存为整张画布副本。
- 画布不再直接取得可写的平面数组、图片数组或图片引用，通过 `appendPlane/setPlane/setImage` 修改，入口校验索引和基本几何参数。
- 松开鼠标或切换工具提交；Esc 取消；撤销先提交正在进行的操作，再撤销它；重做先取消当前预览。
- 图片变换、平面编辑、垂直平面预览、画笔、图章都接入该流程。平面和绘画按 Esc 恢复是本轮有意统一的行为。

## 阶段三：工具职责与状态

- `ImageTransformTool` 维护图片移动、缩放、旋转的起始快照和交互模式。
- `PlaneEditTool` 维护平面编辑和垂直平面构造的拖动基准。
- `CloneTool` 维护图章源点、对齐偏移、取样映射和准星；`CloneStampEngine` 只负责产生像素。
- `PerspectiveCanvas` 用 `Gesture` 表示空闲、平面、图片、画笔或图章交互，替代互相叠加的绘制/拖动布尔值。
- 画布仍负责输入优先级、选择、视图变换和工具与文档之间的协调；不增加插件系统或通用编辑器框架。

## 阶段四：共享几何与缓存

- `ImageGeometry` 为一张图片生成不可变的投影片段、轮廓、控制点和命中映射。
- 渲染、蚂蚁线、变换工具的控制点拾取及画布图片命中共用该结果。
- 几何缓存按图片尺寸、位置、比例、旋转、宿主面和全部面片快照构造键；不持有原始位图，按线程隔离，最多保留 128 项。
- `SceneContentCache` 缓存当前视口的背景、绘画层和浮动图片。动画只更新动态辅助元素，静态内容未变时不重复投影位图。
- 视口尺寸、DPI、视图变换、图像像素和几何变化均使内容缓存失效；撤销与取消恢复也被缓存键覆盖。

## 模块依赖

```text
VanisingPoint
  └─ VanishingWidgets（MainWindow、PerspectiveCanvas）
       └─ VanishingCore（Qt Core / Gui）
            ├─ 文档与事务：CanvasDocument
            ├─ 透视与几何：ProjectiveMapping、PlaneMath、FloatingImageMath、ImageGeometry
            ├─ 工具：PlaneEditTool、ImageTransformTool、CloneTool
            ├─ 像素引擎：PaintEngine、CloneStampEngine
            └─ 渲染：SceneRenderer、SceneContentCache
```

应用和测试链接同一份核心及界面库，避免复制源文件列表并重复编译全部实现。

## 验证

配置 CMake 时启用 `-DVP_BUILD_TESTS=ON`，构建后运行：

```text
ctest --test-dir <构建目录> --output-on-failure
```

- `EditorRegressionTests`：保留现有平面移动、跨面轮廓、变换、图章、快捷键、历史与导出回归测试。
- `ArchitectureTests`：新增齐次映射一致性、事务取消与重做分支、共享几何与命中、缓存失效，以及平面/画笔 Esc 恢复测试。
- Windows 测试启动显式配置所选 Qt 和编译器运行库路径；离屏运行，不要求打开用户的编辑窗口。

本轮未引入 GPU、多线程绘画、项目文件格式、通用多图层栈或稳定对象 ID。这些属于未来功能范围；大图性能仍需针对真实工作负载测量，缓存测试只验证内容复用和正确失效。
