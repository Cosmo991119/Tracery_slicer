#ifndef slic3r_GUI_TraceryTheme_hpp_
#define slic3r_GUI_TraceryTheme_hpp_

namespace Slic3r::GUI::TraceryTheme {

inline constexpr unsigned ACCENT = 0xB69AE8;
inline constexpr unsigned HOVER = 0xC8B2EF;
inline constexpr unsigned PRESSED = 0xA083D4;
inline constexpr unsigned ON_ACCENT = 0x30213F;

// Only branded button fills override the label color. In particular, leave
// disabled, neutral and warning/error buttons unchanged in both themes.
inline constexpr unsigned button_text(unsigned background, unsigned fallback)
{
    return background == ACCENT || background == HOVER || background == PRESSED ? ON_ACCENT : fallback;
}

} // namespace Slic3r::GUI::TraceryTheme

#endif
