# 花窗多色区域笔刷实施计划

## 目标

将花窗碎片作为一组带颜色的 CPU 区域笔刷，按与普通涂色相同的“完整覆盖 / 部分覆盖 / 不相交”语义写入模型。

本方案解决两项问题：

1. 当前 `paint_generated()` 只以叶子三角形重心取样；花窗边界穿过叶子但未经过重心时，可能漏涂或产生边界台阶。
2. 当前烘焙对每个叶子三角形扫描全部花窗碎片，并调用每个碎片的 `AABBMesh::squared_distance()`；大选区、高密度时复杂度接近“叶子数乘碎片数”。

## 不可违反的边界

- 不创建 `GLModel`、`GLVolume`、VBO、VAO、纹理或任何可供花窗渲染调用的额外几何。
- 不使用、恢复或调用旧覆盖层接口、旧花窗保存字段、花窗专用切片接口或颜色路由接口。
- 不采用任何覆盖层显示方案；花窗完成后仅由正常模型多色涂色渲染。
- 最终结果只能写入 `ModelVolume::mmu_segmentation_facets`，必须与普通 MMU 涂色一样进入 `multi_material_segmentation_by_painting()`。
- 所有中间碎片、局部二维区域和索引均是一次生成操作的 CPU 临时数据；写入完成立即释放，不序列化到 3MF 或 gizmo 存档。

## 当前问题

当前 `GLGizmoTracery::bake_fragments_to_mmu_painting()` 为每个花窗碎片建立一个 `AABBMesh`。随后 `TriangleSelector::paint_generated()` 将模型面细分到叶子，并用叶子重心调用回调。回调遍历所有碎片，找到距离重心最近的碎片：

```text
每个叶子中心
  -> 遍历全部花窗碎片
  -> 对每个碎片调用 AABBMesh::squared_distance()
  -> 最近碎片距离 <= epsilon 时写入其耗材 ID
```

`AABBMesh` 能加速点到单个碎片内部三角面的距离查询，但当前没有碎片级的区域筛选。高密度时碎片可达 2048，且叶子边长约为 0.18--0.55 mm，距离查询总量会很大。

重心分类还具有固有精度问题：当边界仅切过叶子一角而不经过重心时，整个叶子可能保留原色。

## 方案概览

逻辑上，每个花窗碎片都是一支带固定颜色的笔刷；所有碎片合起来是多色复合笔刷。

```text
花窗生成的碎片表面 + 最终耗材 ID
  -> 按模型体和原始模型面建立多色区域笔刷计划
  -> 对每个原始模型面的叶子三角形递归分类
       无交集                 保留普通涂色
       被同一颜色完整覆盖      直接写入该颜色
       部分覆盖或多色覆盖      继续细分
       达到精度下限            按最大覆盖面积写入
  -> mmu_segmentation_facets.set(selector)
  -> 释放笔刷计划和碎片
```

实现上不能真的将每个碎片调用一次现有 `select_patch()`：该接口依赖鼠标命中的起始面，一次只能写一种颜色，且重复扫描整个模型会造成新的性能问题。应复用普通涂色的递归相交语义，但按原始模型面批处理全部局部颜色区域。

## 为什么使用二维区域相交

花窗碎片和待写入的叶子三角形位于同一个原始模型面上。直接使用一般三维三角形相交会遇到共面重叠的退化情形，不能可靠地识别覆盖面积。

每个原始模型面应定义稳定的局部二维投影：

```text
original triangle (a, b, c)
  origin = a
  u      = normalize(b - a)
  v      = normalize(normal(a,b,c) cross u)
  3D point -> (dot(point-a, u), dot(point-a, v))
```

该面上的花窗碎片三角面和 `TriangleSelector` 叶子三角形均投影到同一二维坐标系。当前生成器输出的是只在公共边接触的三角形分割，因此实现以凸三角形裁剪计算 `leaf ∩ brush_triangle` 的面积；不需要创建额外的多边形几何或进行三维最近面查询。若将来的生成器允许不同碎片出现正面积重叠，再将每色区域先做布尔并集。

## 临时数据结构

以下结构放在 `GLGizmoTracery.cpp` 的匿名命名空间中，或者放在仅被该 gizmo 使用的私有实现文件中。它们不属于模型、渲染器和序列化数据。

```cpp
struct PlaneProjector {
    Vec3d origin;
    Vec3d u;
    Vec3d v;

    Vec2d project(const Vec3f& local_point) const;
};

struct BrushTriangle {
    uint32_t             original_facet_id;
    uint32_t             fragment_id;
    EnforcerBlockerType  color;
    std::array<Vec2d, 3> triangle;
    BoundingBoxf         bounds;
};

struct FacetBrushes {
    PlaneProjector projector;
    std::vector<BrushTriangle> triangles;
    // 仅用于候选筛选；不能作为最终覆盖判定，也不参与写入。
    SpatialGrid2d grid;
};

struct GeneratedBrushPlan {
    ObjectID object_id;
    std::vector<FacetBrushes> per_volume_per_original_facet;
};
```

实际存储可按 `volume_index -> original_facet_id -> FacetBrushes` 分层，避免为未被花窗覆盖的原始面分配空对象。

### 来源面 ID

当前生成阶段的 `SurfaceTriangle` 与 `WorkTriangle` 缺少原始模型面 ID。需要为 `TriangleSelector` 增加只读枚举叶子的接口，或在导出花窗选区时携带 `Triangle::source_triangle`：

```cpp
struct SelectorLeaf {
    int source_triangle;
    EnforcerBlockerType state;
    std::array<Vec3f, 3> vertices;
};
```

花窗生成的每个工作三角面、裁剪多边形和最终 `BrushTriangle` 都必须保留这个 ID。这样写回时只访问该原始面及其派生叶子，不扫描整个模型。

## 笔刷计划的建立

在 CMYW 配方已经生成，且每个 `fragment_id` 已映射到普通 MMU 耗材 ID 后：

1. 从实时 `TriangleSelector` 导出 T-joint 安全的选区叶三角面；这是唯一允许着色的实际网格。
2. 遍历现有 CPU 花窗碎片三角面，以 `(volume_index, original_facet_id)` 找到同面的选区叶。
3. 将花窗三角面与选区叶投影到该原始面的局部二维坐标，先计算 `fragment_polygon ∩ selected_leaf`；没有交集不得进入笔刷计划。
4. 将每个交集多边形重新三角化，以 `(volume_index, original_facet_id, color)` 分组。
5. 保留生成器已经给出的非重叠三角形分割；同色三角面不需要合并，分类时累加它们与叶子的精确相交面积。
6. 为每个 `FacetBrushes` 建立二维格网。索引只保存可能相交的三角面包围盒。

花窗碎片应当只在公共边界接触；当前生成器保持这一分割不变量。若将来引入可正面积重叠的生成路径，必须先增加显式重叠验证和每色布尔并集，不能通过顺序覆盖掩盖问题。

## TriangleSelector 新入口

新增一个与普通笔刷递归逻辑平行的接口，而不是继续扩展重心回调：

```cpp
enum class RegionCoverageKind {
    Outside,
    FullSingleColor,
    Partial
};

struct RegionCoverage {
    RegionCoverageKind kind = RegionCoverageKind::Outside;
    EnforcerBlockerType color = EnforcerBlockerType::NONE;
    // 只在最小叶子需要收敛时使用。
    std::map<EnforcerBlockerType, double> covered_area;
};

void TriangleSelector::paint_generated_regions(
    float max_leaf_edge,
    const std::function<RegionCoverage(
        int source_triangle,
        const std::array<Vec3f, 3>& leaf)>& classify_region);
```

实现要求：

- 沿用 `split_triangle()`、邻接关系、序列化和普通涂色的状态树。
- 子叶继承父叶已有状态；`Outside` 不得写入 `NONE`，而是保留继承的普通涂色。
- `FullSingleColor` 直接设置颜色，且不再细分该子树。
- `Partial` 仅在边长大于 `max_leaf_edge` 时继续细分。
- 到达最小叶子后，选择覆盖面积最大的颜色；无覆盖面积时不修改状态。
- 面积相同或近似相同时，以最小稳定 `fragment_id` 决胜，使保存、重开和重复生成结果一致。
- 仍调用 `remove_useless_children()` 和 `garbage_collect()`；这两个操作只能在子叶状态完全一致时压缩树，不能丢弃颜色。

## 区域分类

对一个叶子三角形：

1. 使用它的 `source_triangle` 找到相同原始面的 `FacetBrushes`；无条目即为 `Outside`。
2. 投影叶子到二维并查询格网，取得少量颜色区域候选。
3. 对每个候选笔刷三角面求 `leaf ∩ brush_triangle`，按颜色累加精确相交面积。
4. 若所有相交面积为零，返回 `Outside`。
5. 若仅一种颜色，且 `leaf - color_region` 为空，返回 `FullSingleColor(color)`。
6. 其余情况返回 `Partial`，并附带每种颜色的面积供最小叶子收敛使用。

这里的格网仅是宽相筛选。最终结果必须由二维布尔结果决定，不能通过包围盒、重心、最近点或候选顺序直接决定颜色。

## 写入模型

仍使用现有的普通涂色提交路径：

```cpp
TriangleSelector selector(volume->mesh());
selector.deserialize(m_stained_glass_base_paint_data[volume_index], false);
selector.paint_generated_regions(max_leaf_edge, classify_region);
volume->mmu_segmentation_facets.set(selector);
```

这保证：

- 花窗外保留生成前的普通涂色；
- 花窗内使用普通多色耗材状态；
- 正常模型渲染只绘制一次，不存在 z-fighting 或重叠面；
- 切片继续仅调用 `multi_material_segmentation_by_painting()`。

`paint_generated()` 的重心 AABB 距离回调应在该方案验证后删除或停止从花窗调用；不保留两套并行花窗写入逻辑。

## 性能设计

逻辑上是“每个彩色碎片是一支笔刷”；实现上是“每个原始模型面的一次树遍历，批量处理其局部全部颜色区域”。

```text
避免：叶子 -> 扫描所有碎片 -> 全量 AABB 最近距离
避免：碎片 -> 扫描整个模型 -> 重复树遍历
采用：叶子 -> 原始来源面 -> 局部索引候选 -> 2D 精确区域覆盖
```

大面积、单色完整覆盖的区域会在更高层级停止细分；只有颜色边界附近继续分裂。这样既避免了当前的全量 AABB 查询，也避免了不必要的内部细分。

应记录：

- 生成前后的原始面数、工作面数、碎片数和二维颜色区域数；
- 笔刷计划建立、区域分类、细分、提交各阶段耗时；
- `FullSingleColor`、`Partial`、`Outside` 的叶子数量；
- 每次区域分类的候选区域数、平均值和最大值；
- 达到最小叶子时的面积决策数量。

## 验收标准

### 正确性

- 花窗边界穿过叶子但不经过其重心时，必须判为 `Partial` 并继续细分，不得漏涂。
- 多个同色碎片共同覆盖一个叶子时，必须判为完整同色覆盖。
- 多色边界及浮点容差下的归属必须稳定，可重复生成。
- 普通涂色与花窗相邻、重叠编辑或重新进入工具后，花窗外普通颜色仍保留。
- 最终 `TriangleSplittingData` 只包含标准耗材状态，切片只走普通多色分割路径。

### 架构约束

- 静态检索确认花窗代码没有创建 `GLModel` 或花窗 `GLVolume`，没有覆盖层调用，没有旧花窗切片或旧花窗保存接口调用。
- `GeneratedBrushPlan`、二维区域与索引不保存到模型、3MF、gizmo 序列化数据或 GPU。
- 模型表面只由正常多色渲染路径绘制一次。

### 性能

- 不再存在“叶子数乘全部碎片数”的 `AABBMesh::squared_distance()` 模式。
- 内部完整覆盖区域显著少于边界区域的递归深度。
- 大选区、高密度用例中，候选区域数远小于全部碎片数，且生成后 UI 不出现与碎片总数近似线性相乘的停顿。

## 实施顺序

1. 增加来源原始面 ID 的只读枚举，并为现有花窗工作三角面传递该 ID。
2. 实现仅 CPU 使用的 `GeneratedBrushPlan`，完成局部二维投影、同色并集和格网候选索引。
3. 为 `TriangleSelector` 增加 `paint_generated_regions()`，先用纯单色矩形/三角形单元测试验证 `Outside`、`FullSingleColor` 和 `Partial`。
4. 将花窗烘焙改为该入口，保留现有 CMYW 配方与 `mmu_segmentation_facets.set(selector)`。
5. 删除花窗对 `paint_generated()` 的调用，并记录性能指标。
6. 回归保存、重开、普通涂色共存和标准切片；最后静态检查无覆盖层或额外渲染几何。
