# Tracery slicer lilac theme

The approved accent is `#B69AE8`. Hover uses `#C8B2EF`, pressed uses
`#A083D4`, and text on these fills uses `#30213F`. Small SVG marks and web
links on light surfaces use deeper lilac `#7856AD` for legibility.

Native widget accents, SVG UI accent replacement, and embedded web pages
share this palette. Existing light/dark neutral surfaces, filament presets,
model colors, and warning/error palette are not rebranded.

## Verification

Run from the repository root:

```sh
c++ -std=c++17 -Wall -Wextra -pedantic -Isrc tests/tracery_theme_test.cpp -o /tmp/tracery-theme-test
/tmp/tracery-theme-test
python3 scripts/generate_app_icons.py --check
```

The standalone test checks exact accent selection, button text contrast of
at least 4.5:1 in all three branded states, and preservation of neutral,
disabled and destructive button text. It does not replace a GUI build.

Before release, build on Windows, macOS and Linux and visually check the
welcome wizard, Slice/Print buttons, checkboxes, tabs, dropdowns, links and
progress indicators in both light and dark modes. Verify theme switching,
disabled states and printer/model colors. No full native build or runtime
visual verification was performed for this palette change.
