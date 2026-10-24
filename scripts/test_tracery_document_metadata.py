#!/usr/bin/env python3
"""Check the export metadata reader independently of the geometry/UI libraries."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include "libslic3r/TraceryDocumentMetadata.hpp"
#include <cassert>
#include <stdexcept>
int main() {
    using Slic3r::tracery_document_version;
    assert(tracery_document_version(R"({"version":1})") == 1);
    assert(tracery_document_version(R"({"base":{"version":999},"label":"version:1","version":2})") == 2);
    assert(tracery_document_version(R"({"version":2,"nested":{"version":1}})") == 2);
    assert(tracery_document_version(R"({"version":3,"through":[],"channels":[]})") == 3);
    for (const auto* bad : {"{}", "[]", "null", R"({"nested":{"version":2}})",
            R"({"version":4})", R"({"version":-1})", R"({"version":"2"})", R"({"version":2.5})",
            R"({"version":null})", R"({"version":true})", R"({"version":{}})", R"({"version":[]})",
            R"({"version":1,"version":2})", R"({"version":2,})", R"({"version":2} garbage)"}) {
        bool rejected = false;
        try { tracery_document_version(bad); } catch (const std::exception&) { rejected = true; }
        assert(rejected);
    }
    std::string large = "{\"channels\":[";
    for (int i=0; i<100000; ++i) large += "[1,2,3],";
    large += "[1,2,3]],\"version\":2}";
    assert(tracery_document_version(large) == 2);
}
'''
with tempfile.TemporaryDirectory(prefix="tracery-metadata-") as directory:
    source = Path(directory) / "test.cpp"
    binary = Path(directory) / "test"
    source.write_text(SOURCE)
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "src"), "-I", str(ROOT / "deps_src"), str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("Tracery export metadata checks passed")
