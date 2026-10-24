# Full-depth vector stained glass

Development branch: `codex/tracery-full-through-slicing`.
Based on `462d900886` of `codex/tracery-density-performance`.
Do not merge until the user has verified the preview build.

## 预览版验收

1. 使用原项目的副本，进入花窗的“贯穿”模式。
2. 勾选“完整贯穿内部”，再涂色；已有选区可以使用“重新生成”。
3. 切片后查看模型中间层，确认颜色分区覆盖通道内部，边界保持多边形线段。
4. 检查擦除、连续涂色、撤销/重做，以及保存再打开；同时核对其他零件和其他盘。
5. 有空腔或前后不相连的实体时，通道应在第一次离开实体的位置停止。

复选框默认关闭，改变它只影响后续操作；重新生成会按当前模式处理选区。
启用完整贯穿的项目保存为 V3，请用本预览版打开，避免用旧版覆盖保存。
本分支在用户确认效果之前不合并。

## User behavior

In Stained glass → Through, enable **Full depth material** (完整贯穿内部).
The default remains disabled. Ordinary surface and two-sided vector painting keep
their existing paths. Changing the checkbox affects subsequent operations;
regenerating applies the currently selected mode to the regenerated selection.

Full-depth entries are stored in the volume's local coordinates. They assign a
material/virtual recipe inside the solid without modifying its triangle mesh or
creating modifier meshes. Entry directions follow source face normals, as in the
existing through mode. A single entry stops at the first outward crossing, so a
disconnected body behind a gap is not painted. Negative volumes also act as exits.
Disconnected, non-coplanar entry patches may have different directions.

Full-depth materials take precedence over ordinary surface segmentation inside
their channels. Their display-only front/back regions do not enter the surface
segmentation pass. Existing source-selection eligibility and plate palette rules
remain in force; this feature does not introduce a new global black override.
The base material and infill density remain in effect outside a channel.

Later full-depth strokes replace earlier strokes in overlapping regions. Erase
removes full-depth paint in its projected footprint, revealing the ordinary/base
material rather than an older full-depth stroke. Clear-all removes all channels.
Existing through front/back preview is reused; the sliced preview is authoritative
for side faces and variable-thickness channels.

## Data and call path

- `GLGizmoTracery::apply_stained_glass_colors()` retains the legacy front/back
  generation for preview. `build_voronoi_2d_data(..., front_only=true)` captures
  only entries, preventing a back projection from becoming a second channel.
- `TraceryDocument::apply_vector_edit(..., through_entry)` publishes entry
  operations and a separate surface-only slicing channel atomically alongside
  normal vector editing data. Operations store local entry polygons, directions,
  material IDs, and erase flags, with no derived solid mesh.
- V1/V2 documents remain readable. Ordinary vector documents keep V2. Documents
  with full-depth operations use V3 and require this feature-capable build when
  reopened; use a project copy when evaluating the preview.
- `PrintObject::slice_volumes()` retains each target part's ownership after
  multipart/negative/modifier clipping. This avoids painting another part with
  the same material or an overlapping footprint.
- `slice_tracery_through()` slices convex entry sweeps directly from edge-plane
  intersections. AABB candidates provide outward exit faces. Swept exit-face
  shadows remove all positions after the first exit, including sloping exits.
  No centroid-only depth estimate and no generated triangulated solid is used.
- Channel masks are clipped to target ownership and composed after surface-only
  material offsets, before local-Z planning and `apply_mm_segmentation()`.
  Virtual recipes are then handled by the existing material expansion code.
- Source geometry, transforms, document bytes and material remaps participate in
  the existing model copying, undo/redo and slice invalidation mechanisms.

## Verification

```
cmake --build build-through --config Release --target tracery_through_tests tracery_document_tests
ctest --test-dir build-through -C Release -R '^tracery_(through|vector|document|voronoi)$' --timeout 90 --output-on-failure
TRACERY_PERF_TRACE=1 build-through/tests/libslic3r/Release/tracery_through_tests '[TraceryThroughPerformance]'
```

Coverage includes all interior layers, first exit before a second solid, sloping
exit, horizontal direction, mirrored placement, repaint/erase, recipe remapping,
3MF reopening, mixed recipes, later part ownership and negative cavities.
Performance measurements cover target slicing, spatial index, channel sections,
operation composition, exit candidates and entry patch counts. Timings are
reported rather than used as machine-dependent pass/fail thresholds.

Interactive acceptance: compare checkbox off/on on the user's window model;
inspect a middle layer; repeat paint/erase, change density and regenerate, undo
and redo, save/reopen, and verify unaffected parts/plates. No printer validation
is implied by unit or slicing tests.

## Preview verification (2026-10-24)

- Release arm64 application and both test executables built successfully.
- CTest `tracery_vector`, `tracery_voronoi`, `tracery_through`: 3/3 passed,
  respectively 24/3/11 cases (26,675/188,071/117 assertions).
- Document lifecycle excluding `WindowBrush` and `WindowDense`: 11 cases,
  158 assertions passed. The full 13-case document suite passed once under LLDB,
  but the normal CTest run timed out at 90 seconds. The WindowBrush path was
  narrowed to a SIGSEGV in `__gmpn_mul_basecase`, followed by Catch's signal handler.
  A separate existing density build was independently observed at the same stack.
  Increasing the test process stack limit did not resolve it. This legacy exact
  mesh reconstruction instability remains unresolved and is **not** counted as a
  passing regression; the new full-depth vector/slice path does not call rebuild.
- Performance fixture: 400 cells × 25 layers, all 25 area assertions passed.
  Two runs measured about 56 ms and 867 ms while other builds/tests were active;
  these are diagnostics, not a stable benchmark or a speedup claim.
- All translations passed `msgfmt --check-format`. The standalone application
  passed ad-hoc signature verification and `--help` startup (exit 0).
- Interactive painting, undo/redo, and the user's actual model still need visual
  acceptance before merge. Physical printing has not been tested.

Logs are bundled beside the application in `build-through/releases/verification/`.
