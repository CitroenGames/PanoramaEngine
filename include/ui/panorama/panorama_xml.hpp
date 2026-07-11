#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

// Self-contained SAX-style XML parser for Panorama .xml layout files.
//
// This parser preserves the XML behavior Panorama layouts rely on:
//   - `<?...>` header skipped, `<!--...-->` comments skipped (without splitting
//     surrounding text data), `<![CDATA[...]]>` sections passed through as data.
//   - Registered CDATA tags (e.g. <style>, <script>) capture their raw body —
//     including markup — until the matching case-insensitive close tag.
//   - Attribute values may be double-quoted, single-quoted, or bare words, and
//     have their XML character entities decoded; text data is delivered
//     verbatim (the consumer re-escapes it).
//   - Text containing `{{ ... }}` data-binding expressions keeps `<` inside the
//     brackets as data instead of treating it as markup.
//   - Parsing stops once the root element closes; trailing content is ignored.
namespace openstrike
{
// Attribute list preserving document order.
class PanoramaXmlAttributes
{
public:
    using value_type = std::pair<std::string, std::string>;
    using const_iterator = std::vector<value_type>::const_iterator;

    // Returns the value for `key`, or nullptr when absent.
    [[nodiscard]] const std::string* find(std::string_view key) const;
    void set(std::string_view key, std::string value);

    [[nodiscard]] const_iterator begin() const noexcept { return items_.begin(); }
    [[nodiscard]] const_iterator end() const noexcept { return items_.end(); }
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }

private:
    std::vector<value_type> items_;
};

enum class PanoramaXmlDataType
{
    Text,
    CData,
};

class PanoramaXmlSaxParser
{
public:
    virtual ~PanoramaXmlSaxParser() = default;

    // Tags registered here have their body captured raw (markup included) until
    // the matching close tag, and delivered via handle_data(..., CData).
    void register_cdata_tag(std::string_view tag);

    // Parses `xml`, invoking the handlers below. `source_name` is only used to
    // label parse warnings (e.g. the document path).
    void parse(std::string_view xml, std::string_view source_name = {});

protected:
    virtual void handle_element_start(const std::string& name, const PanoramaXmlAttributes& attributes) = 0;
    virtual void handle_element_end(const std::string& name) = 0;
    virtual void handle_data(const std::string& data, PanoramaXmlDataType type) = 0;

private:
    void read_header();
    void read_body();
    [[nodiscard]] bool read_open_tag();
    [[nodiscard]] bool read_close_tag();
    [[nodiscard]] bool read_attributes(PanoramaXmlAttributes& attributes);
    [[nodiscard]] bool read_cdata(const char* tag_terminator);

    [[nodiscard]] bool at_end() const noexcept { return index_ >= source_.size(); }
    [[nodiscard]] char look() const noexcept { return source_[index_]; }
    void next() noexcept { ++index_; }
    [[nodiscard]] bool find_word(std::string& word, const char* terminators);
    [[nodiscard]] bool find_string(std::string_view needle, std::string& data, bool escape_brackets = false);
    [[nodiscard]] bool peek_string(std::string_view needle, bool consume = true);

    std::unordered_set<std::string> cdata_tags_;
    std::string_view source_;
    std::string source_name_;
    std::size_t index_ = 0;
    int open_tag_depth_ = 0;
    std::string data_;
};

// Decodes the XML character entities `&lt;` `&gt;` `&amp;` `&quot;` and
// numeric `&#...;` / `&#x...;` references (code points emitted as UTF-8).
// Unrecognized sequences pass through unchanged.
[[nodiscard]] std::string decode_xml_entities(std::string_view text);
}
