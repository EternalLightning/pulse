// SPDX-License-Identifier: Apache-2.0
// Pulse project; see LICENSE and NOTICE for licensing and attribution.
// Modified by EternalLightning for this independent distribution, including licensing notices.
// Existing third-party notices and license terms remain applicable.

#pragma once
#include <string_view>

namespace pulse::text {

inline constexpr std::wstring_view kKnownTextExtensions[] = {
    L".txt", L".md", L".log", L".json", L".xml", L".yaml", L".yml",
    L".ini", L".cfg", L".conf", L".csv", L".tsv", L".cpp", L".c",
    L".h", L".hpp", L".cc", L".cxx", L".cs", L".java", L".js",
    L".jsx", L".ts", L".tsx", L".py", L".rs", L".go", L".php",
    L".html", L".htm", L".css", L".scss", L".sql", L".ps1", L".bat",
    L".cmd", L".sh", L".qml", L".cmake", L".toml", L".properties",
    L".tex", L".cls", L".sty", L".bib", L".ltx", L".dtx", L".ins", L".bst",
    L".markdown", L".mdown", L".rst", L".adoc", L".org", L".text",
    L".jsonc", L".jsonl", L".ndjson", L".yarnrc", L".editorconfig", L".env",
    L".config", L".inf", L".reg", L".psm1", L".psd1", L".zsh", L".fish",
    L".cjs", L".mjs", L".vue", L".svelte", L".less", L".sass", L".svg",
    L".xaml", L".xsl", L".xslt", L".xsd", L".resx", L".rc", L".rcinc",
    L".hh", L".hxx", L".inl", L".ipp", L".ixx", L".cppm", L".def",
    L".kt", L".kts", L".swift", L".dart", L".rb", L".lua", L".pl", L".pm",
    L".r", L".m", L".jl", L".f", L".f90", L".f95", L".v", L".sv",
    L".vh", L".svh", L".vhd", L".vhdl", L".sp", L".cir", L".spice",
    L".s", L".asm", L".gradle", L".pro", L".pri", L".prf", L".mk",
    L".dockerfile", L".gitignore", L".gitattributes", L".gitmodules",
    L".srt", L".vtt", L".ass", L".lrc", L".po", L".pot", L".ics"
};

} // namespace pulse::text
