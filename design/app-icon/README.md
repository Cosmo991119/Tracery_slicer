# Tracery application icon

`tracery-master.png` is the approved flower mosaic artwork. Preserve the petal
silhouette and adjoining colored fragments without adding outlines between them.
The light outer margin is part of this approved raster; exports do not redraw it.

Regenerate with Python and Pillow 11.3.0:

```sh
python3 scripts/generate_app_icons.py
python3 scripts/generate_app_icons.py --check
```

The checked-in outputs require no Python dependency during application builds.
Legacy filenames are intentional: they keep existing packaging, window icons,
desktop integration and file association references working.

| Surface | Resource |
| --- | --- |
| Windows EXE, installer and uninstaller | `resources/images/Snapmaker_Orca.ico` |
| Window/dialog title bars | `resources/images/Snapmaker_OrcaTitle.ico` |
| macOS app bundle (CMake and release packaging) | `resources/Icon.icns`, `resources/images/Snapmaker_Orca.icns` |
| macOS taskbar/Dock override | `resources/images/Snapmaker_Orca-mac_256px.ico` |
| Linux desktop/AppImage integration | `resources/images/Snapmaker_Orca.png`, `Snapmaker_Orca_{32,128,192}px.png` |
| Web setup wizard and home page | `resources/web/image/logo.png` |
| Native wizard and legacy dialogs | `resources/images/Snapmaker_Orca_192px*.png` |
| Startup splash and About dialog | `resources/images/tracery-app.png` |

ICO includes 16, 24, 32, 48, 64, 128 and 256 pixel frames; ICNS includes
standard and Retina representations through 1024 pixels. Web UI uses a 512 pixel
source for high-DPI screens. About uses a 140 DIP square instead of the previous
wide brand artwork. Printer logos, presets, account-service
branding and distinct STL/G-code document icons are outside this change.

After installing a newly built app, the OS may keep an old cached icon until the
app is relaunched or its pinned shortcut is recreated. Replacing repository
assets does not modify an already installed binary. No Release is published by
this script; any release created for this project should remain a draft.

## Application display name

The application is displayed as **Tracery slicer** in its main windows, splash,
About header, setup wizard, desktop entries and installers. macOS packaging
outputs `Tracery slicer.app`; both release scripts and GitHub packaging references
use that path. The executable inside the bundle remains `Snapmaker_Orca`.

Keep `SLIC3R_APP_KEY`, binary names, configuration directories, URL schemes,
bundle identifiers and Windows installation/registry keys unchanged for existing
installations. Upstream copyright, acknowledgments, printer branding and legacy
translated help/error messages are not a wholesale product-identity migration.
The visible name is defined in `version.inc`, `src/common_func/common_func.hpp`
and `src/libslic3r/libslic3r.h`; keep these definitions consistent.
