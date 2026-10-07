# 花窗：移除旧覆盖层与单路径涂色重构计划

## 目标与边界

本次重构解决花窗生成缓慢、额外渲染几何可能重叠，以及旧覆盖层链路残留的问题。

完成后应满足：

- 花窗模块不得创建 `GLModel`。
- 花窗模块不得持有 `m_overlay_volumes`，也不得向画布注册任何独立可渲染几何。
- 花窗效果只写入标准 `mmu_segmentation_facets`，由普通多色涂装渲染与切片路径处理。
- 不保留或调用旧覆盖层接口、旧花窗专用切片覆盖、旧颜色路由接口。
- 不兼容历史“旧覆盖层花窗”3MF；不读取、不迁移、不写出其 `Metadata/stained_glass.bin` 数据。
- 保持不含花窗数据的历史普通工程可正常打开、编辑、保存和切片。

## 目标运行链路

### 当前链路

```text
笔刷结束 / 调整碎片参数
  → capture_mask_and_generate()
  → rebuild_colored_overlay()
      → 细分选区、计算碎片、创建 GLModel 与 m_overlay_volumes
  → apply_stained_glass_colors()
      → 建立混色配方
      → 通过碎片表面烘焙 mmu_segmentation_facets
      → 清理部分临时覆盖层

渲染 / 切片
  → 标准 MMU 涂色路径
  + 历史 tracery_facets 的覆盖层 / 专用分割 / 颜色路由路径
```

该链路的问题是：同一个生成结果先转换为可渲染的 `GLModel`，随后又被转换为普通 MMU 涂色；前者在完成烘焙后没有业务价值，却带来显存分配、画布特殊分支和与原模型重叠绘制的风险。旧 `tracery_facets` 还会让切片进入第二套覆盖逻辑。

### 目标链路

```text
笔刷结束 / 点击“重新生成”
  → 读取选区与原始颜色快照
  → CPU 生成短生命周期的 GeneratedPaintPlan
  → 生成或复用混色耗材配方
  → 直接烘焙 mmu_segmentation_facets
  → 释放 GeneratedPaintPlan

渲染
  → GLVolumeCollection 的标准模型 / 标准 MMU 涂色路径，仅绘制一次

切片
  → multi_material_segmentation_by_painting()
```

`GeneratedPaintPlan` 是纯 CPU 分类数据：可暂时包含碎片三角面、耗材 ID 和查询加速结构，用来判定待涂装点属于哪一个碎片。它不是 `GLModel`，不保存在 `m_overlay_volumes`，不进入画布，也不参与额外绘制 pass。烘焙结束后立即释放。

## 实施内容

### 1. 花窗生成器改为生成并直接烘焙

将当前连续调用的：

```cpp
rebuild_colored_overlay();
apply_stained_glass_colors();
```

改为单个业务入口：

```cpp
generate_and_bake_stained_glass();
```

其职责按顺序为：

1. 捕获当前选择器数据与原始 MMU 涂色快照。
2. 在 CPU 生成碎片及碎片到混色耗材 ID 的映射。
3. 生成或复用 CMYW 混色配方。
4. 将映射烘焙到 `TriangleSelector`，写回 `ModelVolume::mmu_segmentation_facets`。
5. 更新普通模型显示、耗材面板和切片状态。
6. 释放本次生成使用的临时 CPU 数据。

现有的细分、连通区域、种子、Voronoi 裁剪和颜色选择算法可以保留；改变的是其输出形态：输出 `GeneratedPaintPlan`，而非每碎片一个 `GLModel`。

烘焙查询不能继续对每个 `TriangleSelector` 采样点遍历全部碎片。`GeneratedPaintPlan` 应建立空间索引（例如规则网格或包围盒层级），使采样点只测试附近候选碎片。这样可将当前近似“涂装叶节点数 × 碎片数”的距离查询缩小为局部候选集查询。

### 2. 移除额外渲染几何和画布特殊路径

删除花窗中的以下状态和实现：

- `OverlayFragment`、`OverlayVolume`、`m_overlay_volumes`、`m_overlay_object_id`。
- 每碎片 `GLModel` 的创建、初始化、着色和渲染代码。
- `GLGizmoTracery::render_colored_overlay()`。
- `GLGizmoTracery::render_surface_mask()`。
- `GLGizmoTracery::render_overlay_geometry()`。
- `GLGizmoTracery::overlay_object_index()`。

删除 `GLGizmosManager` 的以下转发接口：

- `render_tracery_overlay()`。
- `render_tracery_surface_mask()`。
- `tracery_overlay_object_index()`。

同步清理 `GLCanvas3D` 中与花窗 overlay index、花窗专用渲染诊断和特殊模型分支的耦合。花窗工具激活时，画布仍通过标准 `m_volumes.render()` 渲染模型；笔刷选择器高亮可以保留，因为它属于编辑工具的选区提示，而非花窗碎片几何。

预期影响：

- 同一表面不会被“原模型 + 花窗覆盖网格”重复绘制，消除重叠面、深度竞争和 polygon offset 依赖。
- 重新打开花窗工具只恢复选择器和已经烘焙的普通涂色，不为预览重建碎片网格。
- 花窗效果以每次生成提交后的普通 MMU 涂色显示；不提供独立的碎片网格半成品预览。

### 3. 删除旧覆盖层模型、切片与配置链路

删除旧模型数据：

- `TraceryPatch`。
- `TraceryAnnotation`。
- `ModelVolume::tracery_facets`。
- 相关的复制、变换、时间戳、Cereal 序列化和清理代码。

删除旧切片数据适配：

- `ModelVolumeFacetsInfo::tracery`。
- `stained_glass_segmentation_by_painting()`。
- `PrintObjectSlice` 中普通 MMU 分割后再计算旧花窗覆盖、做差集和合并的逻辑。
- `PrintApply` 中针对旧花窗构建虚拟耗材路由的逻辑。
- `Print::m_stained_glass_routes` 与 `stained_glass_route()`。
- `stained_glass_color_routes` 配置项、预设同步与脏状态豁免。

花窗新逻辑只使用已经写入 `mmu_segmentation_facets` 的最终耗材 ID，因此切片只需标准 `multi_material_segmentation_by_painting()`。这既移除了第二套分割/覆盖计算，也避免旧覆盖数据覆盖普通涂色的优先级问题。

### 4. 文件格式策略

新版本不再写出或读取 `Metadata/stained_glass.bin`。

明确的兼容性约束如下：

- 含有旧花窗覆盖层数据的历史 3MF 不做兼容处理；其旧花窗效果不会恢复。
- 不含花窗数据的历史普通 3MF 必须继续兼容。由于这类文件本来不依赖 `stained_glass.bin`，删除花窗元数据读取器不应影响标准模型、普通颜色涂装和其他项目配置的加载。
- 删除模型内旧花窗字段时，通用历史归档仍按原字段位置读取一个一次性的废弃载荷并立即丢弃；新保存只写空占位以维持普通历史归档的记录布局。该载荷不会重新引入运行时 `tracery_facets` 接口，也不会恢复旧花窗效果。

## 实施顺序

1. 定义纯 CPU `GeneratedPaintPlan`，将花窗生成结果直接烘焙为 `mmu_segmentation_facets`。
2. 删除花窗 `GLModel`、`m_overlay_volumes` 和所有额外绘制入口；确保画布只走标准模型渲染。
3. 删除旧花窗专用切片覆盖、颜色路由与相关配置，使切片只走普通 MMU 分割。
4. 删除 `tracery_facets` 模型字段以及旧花窗 3MF 元数据读写代码。
5. 增加并验证无花窗历史文件的回归测试。
6. 编译并运行针对花窗、普通 MMU 涂色和历史普通工程的验证。

每一步应保持可编译；旧字段删除放在切片和 GUI 调用点完全移除之后，以降低中间状态的编译与行为风险。

## 验证与验收

### 自动化与静态检查

- 使用一条“功能引入前、没有花窗数据”的历史 3MF fixture 作为回归样本。
- 测试该 fixture 可加载、编辑、保存和切片。
- 保存后的归档中不包含 `Metadata/stained_glass.bin`。
- 静态检索确认不存在 `m_overlay_volumes`、`render_tracery_overlay`、`render_tracery_surface_mask`、`tracery_overlay_object_index`、`tracery_facets`、`stained_glass_segmentation_by_painting` 和 `stained_glass_color_routes`。

### 手工回归

- 新建花窗：绘制、擦除、调整密度、调整折射、重新生成、关闭并重新打开工具。
- 普通多色涂装与花窗连续使用，确认二者均以标准 MMU 涂色显示。
- 观察花窗编辑完成后的模型：没有闪烁、重影、z-fighting 或额外覆盖面。
- 切片后检查耗材分配与预览，确认没有旧花窗覆盖或颜色重路由。
- 加载新增的无花窗旧 3MF fixture，确认行为与普通项目一致。

### 性能观测

为生成、空间索引构建、烘焙、模型刷新分别记录耗时，以及工作三角形数、碎片数和候选查询数量。目标是确认移除渲染网格后，瓶颈集中在纯 CPU 生成和烘焙；后续异步化与防抖可在此基础上独立推进，不与本次接口清理耦合。
