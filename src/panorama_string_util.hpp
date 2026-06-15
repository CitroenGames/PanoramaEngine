#pragma once

#include <cctype>
#include <string>
#include <string_view>

// Internal string helpers shared by the PanoramaEngine translation units (the
// same handful used to be re-implemented per file under slightly different
// names). Not part of the public API: include/ stays the library surface, this
// header lives with the sources.
//
// panorama_xml.cpp intentionally keeps its own is_whitespace/strip helpers:
// XML whitespace is the spec's space/tab/newline/carriage-return set, not the
// locale-aware std::isspace set used here.
namespace openstrike::panorama_strings
{
inline bool is_space(char ch)
{
    return std::isspace(static_cast<unsigned char>(ch)) != 0;
}

inline std::string to_lower(std::string_view text)
{
    std::string lowered(text);
    for (char& ch : lowered)
    {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return lowered;
}

inline std::string trim(std::string_view text)
{
    std::size_t first = 0;
    while (first < text.size() && is_space(text[first]))
    {
        ++first;
    }
    std::size_t last = text.size();
    while (last > first && is_space(text[last - 1]))
    {
        --last;
    }
    return std::string(text.substr(first, last - first));
}

inline bool starts_with(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}
}
