# 花窗：生成到普通 MMU 渲染的调用链（现状）

> 本文按当前工作区源码记录，不按设计意图推断。最后核对：2026-10-09。
>
> 关键结论：当前实现**不会渲染 `GeneratedFragment`**，最终可见颜色来自普通
> `mmu_segmentation_facets`。但是当前 bake 仍是 `AABBMesh` 最近距离 + **叶子三角形重心**
> 的分类，并不是“花窗多边形与选区叶子三角形的精确相交”。

## 调用流程图

```mermaid
flowchart TD
    A[用户完成花窗笔刷操作] --> B[GLGizmoTracery::update_model_object]
    B --> C[capture_mask_and_generate]
    C --> C1[保存 m_mask_data: 编辑选区快照]
    C --> C2[首次保存 m_stained_glass_base_paint_data]
    C --> D[generate_paint_plan]
    D --> D1[读取选区严格三角面]
    D1 --> D2[细分 / 颜色区域 / 种子 / Voronoi 裁剪]
    D2 --> D3[m_generated_fragments: CPU indexed_triangle_set]
    B --> E[apply_stained_glass_colors]
    E --> E1[生成或复用 CMYW 虚拟耗材]
    E1 --> E2[m_fragment_mixed_filament_ids]
    E2 --> F[bake_fragments_to_mmu_painting]
    F --> F1[每个 GeneratedFragment 建 AABBMesh]
    F1 --> F2[TriangleSelector::paint_generated]
    F2 --> F3[细分模型叶子三角形]
    F3 --> F4[以叶子重心查询所有 AABBMesh 最近距离]
    F4 --> F5[mmu_segmentation_facets.set(selector)]
    F5 --> F6[清除 m_generated_fragments]
    F6 --> G[GLVolume::simple_render]
    G --> G1[mmu_segmentation_facets.get_facets]
    G1 --> G2[mmuseg_models 按耗材色构建]
    G2 --> G3[普通模型 MMU 着色渲染]

    R[GLGizmoTracery::render_painter_gizmo] -.仅编辑辅助.-> R1[cut / hider / cursor]
    R -.不渲染.-> D3
```

## 入口与生成

### 1. 笔刷提交：`GLGizmoTracery::update_model_object`

位置：[GLGizmoTracery.cpp](../src/slic3r/GUI/Gizmos/GLGizmoTracery.cpp) 第 719 行附近。

```cpp
void GLGizmoTracery::update_model_object()
{
    m_selection_modified = true;
    capture_mask_and_generate();
    apply_stained_glass_colors();
    m_parent.set_as_dirty();
}
```

这意味着每一次完成笔刷操作，都会先重新生成 CPU 花窗碎片，随后立即写入普通 MMU 涂色。
“Regenerate fragments”与密度/折射滑块提交也会调用 `generate_paint_plan()` 后接
`apply_stained_glass_colors()`。

### 2. 固化选区：`GLGizmoTracery::capture_mask_and_generate`

位置：`GLGizmoTracery.cpp` 第 730 行附近。

核心代码：

```cpp
for (const std::unique_ptr<TriangleSelectorGUI>& selector : m_triangle_selectors)
    m_mask_data.emplace_back(selector->serialize());
generate_paint_plan();
```

`m_mask_data`是编辑选区的序列化快照；`m_stained_glass_base_paint_data`则保存第一次
花窗操作前的普通 MMU 涂色。后续重新生成应从这个基础涂色重新开始，避免把上一次花窗结果
当成输入色。

### 3. 生成临时碎片：`GLGizmoTracery::generate_paint_plan`

位置：`GLGizmoTracery.cpp` 第 764 行附近。

此函数的输出是：

```cpp
std::vector<std::vector<GeneratedFragment>> m_generated_fragments;
std::vector<ColorRGBA>                      m_fragment_colors;
```

`GeneratedFragment`定义在 `GLGizmoTracery.hpp`：

```cpp
struct GeneratedFragment {
    size_t              fragment_id = 0;
    indexed_triangle_set surface;
};
```

生成内部主要阶段：

1. 取 `TriangleSelector` 中 ENFORCER 状态的严格选区面；
2. 从普通 MMU 状态 / 模型颜色取得源颜色；
3. 将选区细分为 `WorkTriangle`；
4. 对连通同色区域放置 `FragmentSeed`；
5. 通过半平面裁剪在每个 `WorkTriangle` 内生成 Voronoi 碎片；
6. 将裁剪结果三角化并只保存在 `m_generated_fragments`。

这些 `indexed_triangle_set` 是 CPU 临时数据。它们不是 `GLModel`，也没有在此处上传到
OpenGL。

## 颜色配方与写入模型

### 4. 分配虚拟耗材：`GLGizmoTracery::apply_stained_glass_colors`

位置：`GLGizmoTracery.cpp` 第 1650 行附近。

该函数根据 `m_fragment_colors` 创建/复用 CMYW 混色配方，并形成以下映射：

```cpp
m_fragment_mixed_filament_ids[fragment] =
    palette_filament_ids[fragment_palette[fragment]];
```

随后直接调用：

```cpp
bake_fragments_to_mmu_painting();
```

### 5. 当前 bake 实现：`GLGizmoTracery::bake_fragments_to_mmu_painting`

位置：`GLGizmoTracery.cpp` 第 1871 行附近。

它为每个临时碎片构建空间查询结构：

```cpp
struct GeneratedPaint {
    unsigned int filament_id;
    std::unique_ptr<AABBMesh> surface;
};

generated.push_back({ filament, std::make_unique<AABBMesh>(fragment.surface) });
```

然后调用旧的“点 → 颜色”接口：

```cpp
selector.paint_generated(region, max_leaf_edge, [&generated](const Vec3f &point) {
    double nearest = std::numeric_limits<double>::max();
    unsigned int filament = 0;
    for (const GeneratedPaint &paint : generated) {
        const double distance = paint.surface->squared_distance(point.cast<double>());
        if (distance < nearest) {
            nearest = distance;
            filament = paint.filament_id;
        }
    }
    return nearest <= surface_epsilon_sq ? EnforcerBlockerType(filament)
                                         : EnforcerBlockerType::NONE;
});
```

最后才写回普通模型：

```cpp
updated |= volume->mmu_segmentation_facets.set(selector);
m_generated_fragments.clear();
```

### 6. 当前点分类的实际行为：`TriangleSelector::paint_generated`

位置：[TriangleSelector.cpp](../src/libslic3r/TriangleSelector.cpp) 第 966 行附近。

对进入 region 的模型三角形递归细分，直到边长不大于 `max_leaf_edge`。**每个叶子只取重心**：

```cpp
const Vec3f center = (a + b + c) / 3.f;
const EnforcerBlockerType state = classify(center);
if (state != EnforcerBlockerType::NONE)
    m_triangles[triangle_idx].set_state(state);
```

因此当前代码的分类语义是：

`模型叶子重心` → `遍历所有花窗碎片 AABB` → `最近表面距离` → `一种耗材色`

它**不是**：

`模型叶子三角形` ∩ `花窗彩色多边形` → 根据精确覆盖关系决定细分或写色。

这也是当前性能问题的直接来源：每个叶子重心都要扫描所有 `GeneratedPaint`，每次扫描都调用
一次 AABB 最近距离查询。

## 普通 MMU 的渲染调用链

### 7. 编辑器渲染：`GLGizmoTracery::render_painter_gizmo`

位置：`GLGizmoTracery.cpp` 第 348 行附近。

```cpp
void GLGizmoTracery::render_painter_gizmo()
{
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));
    m_c->object_clipper()->render_cut();
    m_c->instances_hider()->render_cut();
    if (m_paint_mode == PaintMode::SURFACE)
        render_cursor();
    glsafe(::glDisable(GL_BLEND));
}
```

此函数不读取 `m_generated_fragments`，不绘制花窗碎片。它仍会绘制编辑辅助内容：切面、隐藏实例
遮罩和鼠标光标。这些辅助内容不应承担花窗颜色显示。

### 8. 正常模型颜色渲染：`GLVolume::simple_render`

位置：[3DScene.cpp](../src/slic3r/GUI/3DScene.cpp) 第 651 行附近。

当 `mmu_segmentation_facets.timestamp()`改变时，普通渲染器重建每种耗材对应的 GL 模型：

```cpp
model_volume->mmu_segmentation_facets.get_facets(*model_volume, its_per_color);
mmuseg_models.resize(its_per_color.size());
for (int idx = 0; idx < its_per_color.size(); idx++)
    mmuseg_models[idx].init_from(its_per_color[idx]);
```

随后它只渲染 `mmuseg_models[idx]`。这就是花窗完成写入后应该使用的唯一可见颜色路径。

## 与目标方案的差异清单

| 目标 | 当前源码 | 结论 |
|---|---|---|
| 不渲染额外花窗几何 | `GeneratedFragment`未进入 `render_painter_gizmo`，bake 后清空 | 已满足 |
| 不使用旧覆盖层 | 未见花窗覆盖层 GLModel 路径 | 已满足 |
| 不使用任何覆盖层 | 正常颜色来自 `mmuseg_models` | 已满足；编辑辅助仍存在 |
| 写入普通模型 MMU | `mmu_segmentation_facets.set(selector)` | 已满足 |
| 多边形与模型叶子三角形精确相交 | `paint_generated(... classify(center))` + `AABBMesh::squared_distance` | **未满足** |
| 不使用 AABB 最近距离逐点分类 | 每个叶子重心遍历 `generated` | **未满足** |

## 建议的替换边界

只替换步骤 5 与步骤 6：

```text
GeneratedFragment 三角形
  → 按 source_triangle 分组为彩色 2D brush 三角形
  → 与对应的模型叶子三角形精确裁剪
  → 全覆盖单色：写该 MMU 色
  → 部分覆盖或多色：继续细分
  → 不相交：保持基础 MMU 色
```

替换后仍保留步骤 4、`mmu_segmentation_facets.set(selector)` 和步骤 8；不新增任何可渲染
花窗网格或覆盖层。
