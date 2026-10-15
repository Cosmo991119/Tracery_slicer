// Standalone palette regression test (no GUI dependencies).
#include "slic3r/GUI/TraceryTheme.hpp"
#include <cassert>
#include <cmath>
#include <initializer_list>

static double luminance(unsigned rgb)
{
    auto linear = [](unsigned channel) {
        double value = channel / 255.0;
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear((rgb >> 16) & 255) + 0.7152 * linear((rgb >> 8) & 255) + 0.0722 * linear(rgb & 255);
}

int main()
{
    using namespace Slic3r::GUI::TraceryTheme;
    assert(ACCENT == 0xA78BDA);
    for (unsigned background : {ACCENT, HOVER, PRESSED}) {
        assert(button_text(background, 0xFFFFFF) == ON_ACCENT);
        double contrast = (luminance(background) + 0.05) / (luminance(ON_ACCENT) + 0.05);
        assert(contrast >= 4.5);
    }
    // Neutral/disabled buttons and destructive actions keep their existing text.
    assert(button_text(0xDFDFDF, 0x6B6A6A) == 0x6B6A6A);
    assert(button_text(0x3E3E45, 0xFFFFFF) == 0xFFFFFF);
    assert(button_text(0xE14747, 0xFFFFFF) == 0xFFFFFF);
}
