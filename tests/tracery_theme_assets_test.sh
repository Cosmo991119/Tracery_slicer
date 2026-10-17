#!/bin/sh
# Focused source/resource regression checks; run from the repository root.
set -eu
rg -q 'COL_ORCA.*167 / 255' src/slic3r/GUI/ImGuiWrapper.cpp
rg -q 'selColor\("#A78BDA"\)' src/slic3r/GUI/Plater.cpp
rg -q 'm_NormalColor.*167 / 255' src/slic3r/GUI/NotificationManager.cpp
rg -q 'tracery-earth-lilac' resources/web/guide/11/index.html
rg -q 'filter: url\(#tracery-earth-lilac\)' resources/web/guide/11/11.css
rg -q 'parse_ui_svg' src/slic3r/GUI/GLTexture.cpp
rg -q 'Font-atlas icons' src/slic3r/GUI/ImGuiWrapper.cpp
rg -q 'Layer-toggle textures' src/slic3r/GUI/ImGuiWrapper.cpp
for source in GLGizmoSeam GLGizmoFdmSupports GLGizmoMmuSegmentation GLGizmoTracery; do
    if rg -q '0.f, 0.59f, 0.53f' "src/slic3r/GUI/Gizmos/$source.cpp"; then
        echo "Old selected-tool accent remains in $source" >&2
        exit 1
    fi
done
rg -q 'selected \? 0.40f : 0.f' src/slic3r/GUI/Gizmos/GLGizmoTracery.cpp
rg -q 'ImGuiCol_Border, selected \? accent' src/slic3r/GUI/Gizmos/GLGizmoTracery.cpp
for source in GLGizmoSeam GLGizmoFdmSupports GLGizmoMmuSegmentation; do
    rg -q 'ImGuiCol_Button,.*218 / 255.f, 0.40f' "src/slic3r/GUI/Gizmos/$source.cpp"
done
echo 'UI accent source/resource checks passed'
