#!/usr/bin/env python3
"""Export the approved Tracery artwork. Requires Pillow 11.3.0.

Run from any directory: python3 scripts/generate_app_icons.py [--check]
The master is deliberately preserved, including its light outer margin.
Legacy filenames are retained for platform packaging and existing GUI consumers.
"""

import argparse
import io
from pathlib import Path

from PIL import Image, ImageOps


ROOT = Path(__file__).resolve().parents[1]
MASTER = ROOT / "design/app-icon/tracery-master.png"
PNG_SIZES = {
    "Snapmaker_Orca.png": 256,
    "Snapmaker_OrcaTitle.png": 256,
    "Snapmaker_Orca_32px.png": 32,
    "Snapmaker_Orca_64.png": 64,
    "Snapmaker_Orca_128px.png": 128,
    "Snapmaker_Orca-mac_128px.png": 128,
    "Snapmaker_Orca_154.png": 154,
    "Snapmaker_Orca_154_title.png": 154,
    "Snapmaker_Orca_192px.png": 192,
    "Snapmaker_Orca_192px_transparent.png": 192,
    "tracery-app.png": 1024,
}
ICO_SIZES = [(size, size) for size in (16, 24, 32, 48, 64, 128, 256)]


def exports():
    with Image.open(MASTER) as source:
        image = source.convert("RGBA")
    if image.width != image.height or image.width < 1024:
        raise ValueError("Master must be square and at least 1024 pixels")

    def resized(size):
        return image.resize((size, size), Image.Resampling.LANCZOS)

    def encoded(bitmap, fmt, **options):
        stream = io.BytesIO()
        bitmap.save(stream, format=fmt, **options)
        return stream.getvalue()

    for name, size in PNG_SIZES.items():
        yield f"resources/images/{name}", encoded(resized(size), "PNG")
    gray = ImageOps.grayscale(resized(192)).convert("RGBA")
    yield "resources/images/Snapmaker_Orca_192px_grayscale.png", encoded(gray, "PNG")
    yield "resources/web/image/logo.png", encoded(resized(512), "PNG")
    ico = encoded(resized(256), "ICO", sizes=ICO_SIZES)
    for name in ("Snapmaker_Orca.ico", "Snapmaker_OrcaTitle.ico", "Snapmaker_Orca-mac_256px.ico"):
        yield f"resources/images/{name}", ico
    icns = encoded(resized(1024), "ICNS")
    yield "resources/Icon.icns", icns
    yield "resources/images/Snapmaker_Orca.icns", icns


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Verify exports without writing")
    args = parser.parse_args()
    mismatches = []
    for relative, data in exports():
        path = ROOT / relative
        if args.check:
            if not path.is_file() or path.read_bytes() != data:
                mismatches.append(relative)
        else:
            path.write_bytes(data)
        print(f"{'Check' if args.check else 'Export'}: {relative}")
    if mismatches:
        raise SystemExit("Out-of-date icons: " + ", ".join(mismatches))


if __name__ == "__main__":
    main()
