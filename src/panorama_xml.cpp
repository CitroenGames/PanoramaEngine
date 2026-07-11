#include "ui/panorama/panorama_xml.hpp"

#include "panorama_string_util.hpp"
#include "ui/panorama/panorama_log.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace panorama
{
namespace
{
using strings::to_lower;

// XML whitespace is the spec's fixed set (space/tab/newline/CR), intentionally
// narrower than strings::is_space's locale set.
bool is_whitespace(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

std::string strip_whitespace_copy(std::string_view text)
{
    std::size_t first = 0;
    while (first < text.size() && is_whitespace(text[first]))
    {
        ++first;
    }
    std::size_t last = text.size();
    while (last > first && is_whitespace(text[last - 1U]))
    {
        --last;
    }
    return std::string(text.substr(first, last - first));
}

// Tracks `{{ ... }}` data-binding expressions while scanning text data so a `<`
// inside an expression is treated as data, not markup. Returns an error message
// or nullptr.
const char* parse_data_brackets(bool& inside_brackets, bool& inside_string, char c, char previous)
{
    if (inside_brackets)
    {
        if (c == '\'')
        {
            inside_string = !inside_string;
        }
        if (!inside_string)
        {
            if (c == '}' && previous == '}')
            {
                inside_brackets = false;
            }
            else if (c == '{' && previous == '{')
            {
                return "Nested double curly brackets are illegal.";
            }
            else if (previous == '}' && c != '}' && c != '\'')
            {
                return "Single closing curly bracket encountered, use double curly brackets to close an expression.";
            }
            else if (previous == '/' && c == '>')
            {
                return "Closing double curly brackets not found, XML end node encountered first.";
            }
            else if (previous == '<' && c == '/')
            {
                return "Closing double curly brackets not found, XML end node encountered first.";
            }
        }
    }
    else
    {
        if (c == '{' && previous == '{')
        {
            inside_brackets = true;
        }
        else if (c == '}' && previous == '}')
        {
            return "Closing double curly brackets encountered outside an expression.";
        }
    }
    return nullptr;
}

void append_utf8(std::string& out, unsigned long code_point)
{
    if (code_point < 0x80U)
    {
        out += static_cast<char>(code_point);
    }
    else if (code_point < 0x800U)
    {
        out += static_cast<char>(0xC0U | (code_point >> 6U));
        out += static_cast<char>(0x80U | (code_point & 0x3FU));
    }
    else if (code_point < 0x10000U)
    {
        out += static_cast<char>(0xE0U | (code_point >> 12U));
        out += static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU));
        out += static_cast<char>(0x80U | (code_point & 0x3FU));
    }
    else
    {
        out += static_cast<char>(0xF0U | (code_point >> 18U));
        out += static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU));
        out += static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU));
        out += static_cast<char>(0x80U | (code_point & 0x3FU));
    }
}
}

const std::string* PanoramaXmlAttributes::find(std::string_view key) const
{
    for (const value_type& item : items_)
    {
        if (item.first == key)
        {
            return &item.second;
        }
    }
    return nullptr;
}

void PanoramaXmlAttributes::set(std::string_view key, std::string value)
{
    for (value_type& item : items_)
    {
        if (item.first == key)
        {
            item.second = std::move(value);
            return;
        }
    }
    items_.emplace_back(std::string(key), std::move(value));
}

void PanoramaXmlSaxParser::register_cdata_tag(std::string_view tag)
{
    if (!tag.empty())
    {
        cdata_tags_.insert(to_lower(tag));
    }
}

void PanoramaXmlSaxParser::parse(std::string_view xml, std::string_view source_name)
{
    source_ = xml;
    source_name_ = std::string(source_name);
    index_ = 0;
    open_tag_depth_ = 0;
    data_.clear();

    read_header();
    read_body();

    if (open_tag_depth_ > 0)
    {
        pano_log_warning("XML parse error in '{}': unclosed elements", source_name_.empty() ? "<memory>" : source_name_);
    }

    source_ = {};
    data_.clear();
}

void PanoramaXmlSaxParser::read_header()
{
    if (peek_string("<?"))
    {
        std::string temp;
        (void)find_string(">", temp);
    }
}

void PanoramaXmlSaxParser::read_body()
{
    for (;;)
    {
        // Find the next open tag, accumulating text data along the way.
        if (!find_string("<", data_, true))
        {
            break;
        }

        if (peek_string("!--"))
        {
            // Comment: skip without splitting the surrounding text data.
            std::string temp;
            if (!find_string("-->", temp))
            {
                break;
            }
        }
        else if (peek_string("![CDATA["))
        {
            // CDATA section: pass everything through as data, markup included.
            if (!read_cdata(nullptr))
            {
                break;
            }
        }
        else if (peek_string("/"))
        {
            if (!read_close_tag())
            {
                break;
            }
            // Root element closed: ignore any trailing content.
            if (open_tag_depth_ == 0)
            {
                break;
            }
        }
        else
        {
            if (!read_open_tag())
            {
                break;
            }
        }
    }
}

bool PanoramaXmlSaxParser::read_open_tag()
{
    ++open_tag_depth_;

    // Opening tag: flush pending text data first.
    if (!data_.empty())
    {
        handle_data(data_, PanoramaXmlDataType::Text);
        data_.clear();
    }

    std::string tag_name;
    if (!find_word(tag_name, "/>"))
    {
        return false;
    }

    bool section_opened = false;

    if (peek_string(">"))
    {
        handle_element_start(tag_name, PanoramaXmlAttributes());
        section_opened = true;
    }
    else if (peek_string("/") && peek_string(">"))
    {
        // Empty element (<tag/>).
        handle_element_start(tag_name, PanoramaXmlAttributes());
        handle_element_end(tag_name);
        --open_tag_depth_;
    }
    else
    {
        PanoramaXmlAttributes attributes;
        if (!read_attributes(attributes))
        {
            return false;
        }

        if (peek_string(">"))
        {
            handle_element_start(tag_name, attributes);
            section_opened = true;
        }
        else if (peek_string("/") && peek_string(">"))
        {
            handle_element_start(tag_name, attributes);
            handle_element_end(tag_name);
            --open_tag_depth_;
        }
        else
        {
            return false;
        }
    }

    // A registered CDATA tag captures its raw body until the matching close tag.
    if (section_opened && cdata_tags_.count(to_lower(tag_name)) > 0)
    {
        const std::string terminator = to_lower(tag_name);
        if (!read_cdata(terminator.c_str()))
        {
            return false;
        }
        --open_tag_depth_;
        if (!data_.empty())
        {
            handle_data(data_, PanoramaXmlDataType::CData);
            data_.clear();
        }
        handle_element_end(tag_name);
    }

    return true;
}

bool PanoramaXmlSaxParser::read_close_tag()
{
    // Closing tag: flush pending text data first.
    if (!data_.empty())
    {
        handle_data(data_, PanoramaXmlDataType::Text);
        data_.clear();
    }

    std::string tag_name;
    if (!find_string(">", tag_name))
    {
        return false;
    }

    handle_element_end(strip_whitespace_copy(tag_name));
    --open_tag_depth_;
    return true;
}

bool PanoramaXmlSaxParser::read_attributes(PanoramaXmlAttributes& attributes)
{
    for (;;)
    {
        std::string attribute;
        std::string value;

        if (!find_word(attribute, "=/>"))
        {
            return false;
        }

        if (peek_string("="))
        {
            if (peek_string("\""))
            {
                if (!find_string("\"", value))
                {
                    return false;
                }
            }
            else if (peek_string("'"))
            {
                if (!find_string("'", value))
                {
                    return false;
                }
            }
            else if (!find_word(value, "/>"))
            {
                return false;
            }
        }

        attributes.set(attribute, decode_xml_entities(value));

        if (peek_string("/", false) || peek_string(">", false))
        {
            return true;
        }
    }
}

bool PanoramaXmlSaxParser::read_cdata(const char* tag_terminator)
{
    std::string cdata;
    if (tag_terminator == nullptr)
    {
        (void)find_string("]]>", cdata);
        data_ += cdata;
        return true;
    }

    for (;;)
    {
        if (!find_string("<", cdata))
        {
            return false;
        }

        if (peek_string("/", false))
        {
            std::string tag;
            if (find_string(">", tag))
            {
                const std::size_t slash_pos = tag.find('/');
                std::string tag_name =
                    strip_whitespace_copy(slash_pos == std::string::npos ? tag : tag.substr(slash_pos + 1U));
                if (to_lower(tag_name) == tag_terminator)
                {
                    data_ += cdata;
                    return true;
                }
                cdata += '<' + tag + '>';
            }
            else
            {
                cdata += '<';
            }
        }
        else
        {
            cdata += '<';
        }
    }
}

bool PanoramaXmlSaxParser::find_word(std::string& word, const char* terminators)
{
    while (!at_end())
    {
        const char c = look();

        if (is_whitespace(c))
        {
            if (word.empty())
            {
                next();
                continue;
            }
            return true;
        }

        if (terminators != nullptr && std::strchr(terminators, c) != nullptr)
        {
            return !word.empty();
        }

        word += c;
        next();
    }
    return false;
}

bool PanoramaXmlSaxParser::find_string(std::string_view needle, std::string& data, bool escape_brackets)
{
    const char first_char = needle.front();
    bool in_brackets = false;
    bool in_string = false;
    char previous = 0;

    while (!at_end())
    {
        const char c = look();

        if (escape_brackets)
        {
            if (const char* error = parse_data_brackets(in_brackets, in_string, c, previous))
            {
                pano_log_warning("XML parse error in '{}': {}", source_name_.empty() ? "<memory>" : source_name_, error);
                return false;
            }
        }

        if (c == first_char && !in_brackets && peek_string(needle))
        {
            return true;
        }

        data += c;
        previous = c;
        next();
    }
    return false;
}

bool PanoramaXmlSaxParser::peek_string(std::string_view needle, bool consume)
{
    const std::size_t start_index = index_;
    bool success = true;
    std::size_t i = 0;
    while (i < needle.size())
    {
        if (at_end())
        {
            success = false;
            break;
        }

        const char c = look();

        // Seek past whitespace until the first character matches.
        if (i == 0 && is_whitespace(c))
        {
            next();
        }
        else
        {
            if (c != needle[i])
            {
                success = false;
                break;
            }
            ++i;
            next();
        }
    }

    if (!consume || !success)
    {
        index_ = start_index;
    }
    return success;
}

std::string decode_xml_entities(std::string_view text)
{
    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size();)
    {
        if (text[i] == '&')
        {
            const std::string_view rest = text.substr(i);
            if (rest.substr(0, 4) == "&lt;")
            {
                result += '<';
                i += 4;
                continue;
            }
            if (rest.substr(0, 4) == "&gt;")
            {
                result += '>';
                i += 4;
                continue;
            }
            if (rest.substr(0, 5) == "&amp;")
            {
                result += '&';
                i += 5;
                continue;
            }
            if (rest.substr(0, 6) == "&quot;")
            {
                result += '"';
                i += 6;
                continue;
            }
            if (rest.substr(0, 2) == "&#")
            {
                const bool hex = rest.size() > 2 && (rest[2] == 'x' || rest[2] == 'X');
                const std::size_t digits_begin = hex ? 3U : 2U;
                std::size_t digits = 0;
                unsigned long code_point = 0;
                while (digits_begin + digits < rest.size() && digits < 8U)
                {
                    const char c = rest[digits_begin + digits];
                    unsigned long digit_value = 0;
                    if (c >= '0' && c <= '9')
                    {
                        digit_value = static_cast<unsigned long>(c - '0');
                    }
                    else if (hex && c >= 'a' && c <= 'f')
                    {
                        digit_value = static_cast<unsigned long>(c - 'a') + 10U;
                    }
                    else if (hex && c >= 'A' && c <= 'F')
                    {
                        digit_value = static_cast<unsigned long>(c - 'A') + 10U;
                    }
                    else
                    {
                        break;
                    }
                    code_point = code_point * (hex ? 16U : 10U) + digit_value;
                    ++digits;
                }

                const std::size_t semicolon = digits_begin + digits;
                if (digits > 0 && semicolon < rest.size() && rest[semicolon] == ';' && code_point != 0 &&
                    code_point <= 0x10FFFFU)
                {
                    append_utf8(result, code_point);
                    i += semicolon + 1U;
                    continue;
                }
            }
        }
        result += text[i];
        ++i;
    }
    return result;
}
}
