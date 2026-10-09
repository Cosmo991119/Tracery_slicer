# 花窗 `GeneratedPaintPlan` 实施设计

## 目标

解决花窗编辑页在笔刷结束、碎片密度或折射参数调整后生成缓慢的问题，同时保持以下边界：

- 不创建 `GLModel`，不创建或注册额外 `GLVolume`，不增加绘制 pass。
- 不调用旧覆盖层接口，也不恢复旧覆盖层数据或专用切片路径。
- 花窗结果只写入标准 `ModelVolume::mmu_segmentation_facets`，由正常 MMU 涂色渲染和 `multi_material_segmentation_by_painting()` 切片。
- 生成期间使用的 CPU 数据在提交普通 MMU 涂色后立即释放。

## 现状与瓶颈

当前生成器已经将花窗碎片保存在 CPU 侧，并在烘焙后清理；它不会生成新的可渲染覆盖层。慢点位于烘焙阶段：

1. `TriangleSelector::paint_generated()` 将受影响区域细分成叶子三角面。
2. 对每个叶子三角面的中心点，`bake_fragments_to_mmu_painting()` 逐个遍历全部花窗碎片。
3. 对每一个碎片调用一次 `AABBMesh::squared_distance()`，找出最近的碎片并判定是否命中。

因此主要复杂度约为：

```text
细分叶子数量 × 花窗碎片数量 × 单碎片 AABB 查询成本
```

碎片数量上限可以达到 2048；在大选区或高密度时，全量扫描会阻塞编辑 UI。

相关现有位置：

- 花窗生成和烘焙：`src/slic3r/GUI/Gizmos/GLGizmoTracery.cpp`
- 当前全量碎片扫描：`bake_fragments_to_mmu_painting()`
- 标准细分/写色：`src/libslic3r/TriangleSelector.cpp` 的 `TriangleSelector::paint_generated()`

## 设计概览

`GeneratedPaintPlan` 是一次生成操作的短生命周期 CPU 查询计划。它不是模型，不保存，不参与渲染。

```text
CPU 花窗碎片 + 耗材 ID
  -> GeneratedPaintPlan：每个模型体的碎片与空间索引
  -> TriangleSelector::paint_generated() 查询 plan
  -> mmu_segmentation_facets.set(selector)
  -> 释放 GeneratedPaintPlan
```

空间索引只负责缩小候选范围；最终命中仍使用既有的 `AABBMesh::squared_distance()` 精确判断。因此索引不会改变花窗边界或耗材归属规则。

## 数据结构

以下结构建议放在 `GLGizmoTracery.cpp` 的匿名命名空间中，不作为旧接口或跨模块 API 暴露。

```cpp
struct GridCell {
    int x;
    int y;
    int z;

    bool operator==(const GridCell&) const = default;
};

struct GridCellHash {
    size_t operator()(const GridCell& cell) const noexcept;
};

struct PaintFragment {
    unsigned int              filament_id = 0;
    BoundingBoxf3             bounds;
    std::unique_ptr<AABBMesh> surface_aabb;
};

struct VolumePaintPlan {
    BoundingBoxf3 painted_bounds;
    float cell_size = 1.0f;

    std::vector<PaintFragment> fragments;
    std::unordered_map<GridCell, std::vector<uint32_t>, GridCellHash> cells;

    void add_fragment(unsigned int filament_id,
                      const indexed_triangle_set& surface);
    void build_spatial_index(float distance_epsilon);
    EnforcerBlockerType classify(const Vec3f& local_point) const;
};

struct GeneratedPaintPlan {
    ObjectID object_id;
    std::vector<VolumePaintPlan> volumes;
};
```

字段说明：

| 字段 | 含义 | 是否持久化 |
| --- | --- | --- |
| `surface_aabb` | 一个花窗碎片的 CPU 三角面距离查询结构 | 否 |
| `filament_id` | 碎片最终对应的普通或混色耗材 ID | 否 |
| `bounds` | 碎片在模型局部坐标中的包围盒 | 否 |
| `cells` | `GridCell -> 候选碎片下标列表` | 否 |
| `mmu_segmentation_facets` | 最终的普通多色涂色结果 | 是 |

`GeneratedPaintPlan` 不含 `GLModel`、VBO、VAO、纹理、`GLVolume` 或任何渲染状态；不写入 3MF，也不保存在 gizmo 存档中。

## 建立空间索引

### 格网尺寸

每个模型体单独建立格网。格子边长使用当前碎片目标边长 `target_edge`，约为 2.5--6.5 mm；不要使用 `max_leaf_edge`，后者仅用于 `TriangleSelector` 的细分精度。

这个尺度与碎片尺寸接近，通常每个格子只含很少的碎片。曲面折叠而使多个碎片投影到同一局部空间时，候选数会增加，但仍会保留正确结果。

### 插入规则

对每个 `PaintFragment`：

1. 从其 `indexed_triangle_set` 计算局部坐标包围盒。
2. 将包围盒在所有方向扩展 `sqrt(surface_epsilon_sq)`。
3. 计算该扩展包围盒覆盖的所有 `GridCell`。
4. 向每个格子的候选列表写入该碎片的下标。

包围盒扩展与现有距离阈值相同，保证位于边界或受浮点误差影响的点不会因格子筛选而漏掉真正可能命中的碎片。

## 精确分类函数

`classify()` 的输入和输出保持与 `TriangleSelector::paint_generated()` 的回调兼容：输入是模型局部坐标点，输出是 `EnforcerBlockerType`。

```cpp
EnforcerBlockerType VolumePaintPlan::classify(const Vec3f& point) const
{
    const GridCell cell = to_grid_cell(point, cell_size);
    const auto found = cells.find(cell);
    if (found == cells.end())
        return EnforcerBlockerType::NONE;

    double nearest_distance = std::numeric_limits<double>::max();
    unsigned int nearest_filament = 0;

    for (uint32_t index : found->second) {
        const PaintFragment& fragment = fragments[index];
        const double distance =
            fragment.surface_aabb->squared_distance(point.cast<double>());
        if (distance < nearest_distance) {
            nearest_distance = distance;
            nearest_filament = fragment.filament_id;
        }
    }

    return nearest_distance <= surface_epsilon_sq
        ? EnforcerBlockerType(nearest_filament)
        : EnforcerBlockerType::NONE;
}
```

这里仍然执行原来的精确距离判断；区别只是不会再对所有碎片逐一计算距离。

## 调用链与函数改造

第一阶段保持调用关系最小化，只替换 `bake_fragments_to_mmu_painting()` 内的全量扫描。

```text
capture_mask_and_generate()
  -> generate_paint_plan()
  -> apply_stained_glass_colors()
      -> 生成或复用 CMYW 混色配方
      -> 为碎片取得 filament_id
      -> bake_fragments_to_mmu_painting()
          -> build_generated_paint_plan()
          -> VolumePaintPlan::build_spatial_index()
          -> TriangleSelector::paint_generated(..., VolumePaintPlan::classify)
          -> mmu_segmentation_facets.set(selector)
          -> 释放 GeneratedPaintPlan
```

建议新增或调整的私有函数：

```cpp
GeneratedPaintPlan build_generated_paint_plan(
    const std::vector<std::vector<GeneratedFragment>>& fragments,
    const std::vector<unsigned int>& fragment_filament_ids,
    float target_edge);

void bake_generated_paint_plan(
    const GeneratedPaintPlan& plan);
```

第一版也可以直接在 `bake_fragments_to_mmu_painting()` 内构建局部 `GeneratedPaintPlan`，以降低重构范围。稳定后，再将 `generate_paint_plan()`、`apply_stained_glass_colors()` 和烘焙过程收敛成一个私有入口：

```cpp
void generate_and_bake_stained_glass();
```

## 保留的标准接口

优化只替换碎片查询方式，以下标准接口保持不变：

```cpp
selector.paint_generated(region, max_leaf_edge, classify_callback);
volume->mmu_segmentation_facets.set(selector);
multi_material_segmentation_by_painting(...);
```

不得重新引入或调用以下旧接口/数据：

```text
m_overlay_volumes
render_tracery_overlay()
render_tracery_surface_mask()
tracery_overlay_object_index()
ModelVolume::tracery_facets
stained_glass_segmentation_by_painting()
stained_glass_color_routes
Metadata/stained_glass.bin
```

## 分阶段实施

### 阶段 1：空间索引（优先）

1. 在 `GLGizmoTracery.cpp` 增加局部 `GeneratedPaintPlan`、`VolumePaintPlan` 和格网辅助函数。
2. 在现有烘焙函数中把 `GeneratedFragment` 和耗材 ID 组装进 plan。
3. 使用 `VolumePaintPlan::classify()` 替换当前遍历全部 `GeneratedPaint` 的回调。
4. 保留现有碎片生成、CMYW 配方和 `TriangleSelector` 写入逻辑。
5. 烘焙结束后清空现有临时碎片和 plan。

### 阶段 2：性能观测

记录下列数据，避免凭感觉继续调整：

- 源面数量、工作三角面数量、碎片数量；
- 碎片索引构建耗时；
- `paint_generated()` 耗时和最终叶子三角面数量；
- 每次 `classify()` 的候选数总和、平均值和最大值；
- 生成、混色配方、烘焙和模型刷新各自耗时。

### 阶段 3：仅在数据证明需要时优化

- 为源颜色采样建立类似的局部索引，避免每个工作三角面遍历所有颜色源。
- 将工作三角面边索引从多次 `std::map` 查询改为预留容量的哈希容器。
- 若同步阶段仍影响交互，将不可变的 CPU 生成放到可取消的后台任务；主线程只应用最新 generation token 对应的配方和 MMU 涂色结果。

后台化不能在阶段 1 之前进行，以避免扩大线程安全、模型生命周期和取消回写风险。

## 验收标准

### 正确性

- 相同模型、选区、密度和折射参数下，优化前后的最终耗材 ID 分布一致。
- 花窗与普通多色涂装连续使用时，普通颜色与花窗颜色都正确保留。
- 编辑结束、退出并重新进入工具后，显示来自普通 MMU 涂色。
- 切片只进入标准多材料涂色分割路径。

### 渲染与旧接口

- 花窗执行期间不创建 `GLModel`，不向 `GLVolumeCollection` 增加花窗碎片。
- 模型表面仅由标准模型渲染路径绘制一次；无额外覆盖面、重影、z-fighting 或 polygon offset 依赖。
- 静态检索确认不存在旧覆盖层、专用花窗切片和旧颜色路由接口调用。

### 性能

- 记录的平均候选碎片数应显著低于总碎片数。
- `AABBMesh::squared_distance()` 调用次数应从“叶子数乘总碎片数”降为“叶子数乘局部候选数”。
- 对大选区和高密度用例，笔刷结束后的生成耗时应明显降低，且 UI 卡顿时间随碎片数增长不再接近线性全量扫描。
