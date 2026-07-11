#include "ui/panorama/panorama_dom.hpp"

#include "panorama_string_util.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <utility>

namespace openstrike
{
namespace
{
// Process-global lifetime-observer registry (see panorama_dom.hpp). Unlocked by
// design: the DOM is single-threaded, observers are few and long-lived.
std::vector<PanoramaNodeLifetimeObserver*>& node_lifetime_observers()
{
    static std::vector<PanoramaNodeLifetimeObserver*> observers;
    return observers;
}

// Tree guard (see panorama_dom.hpp). Same single-threaded-DOM assumption as the
// observer registry above.
struct TreeGuardDestroyedRecord
{
    const void* ptr = nullptr;
    std::string label;   // "tag#id" of the node at destruction time
    std::string parent;  // "tag#id" of node->parent at destruction time
    std::string context; // mutation context active when it died
    std::uint64_t seq = 0;
};

struct TreeGuardState
{
    std::string mutation_context;
    std::vector<TreeGuardDestroyedRecord> ring; // capped circular buffer
    std::size_t ring_next = 0;
    std::uint64_t destroy_seq = 0;
    static constexpr std::size_t kRingCapacity = 512;
};

TreeGuardState& tree_guard_state()
{
    static TreeGuardState state;
    return state;
}

std::string tree_guard_label(const PanoramaNode& node)
{
    std::string label = node.tag.empty() ? "<?>" : node.tag;
    if (!node.id.empty())
    {
        label += "#" + node.id;
    }
    return label;
}

void tree_guard_record_destroyed(const PanoramaNode& node)
{
    TreeGuardState& state = tree_guard_state();
    TreeGuardDestroyedRecord record;
    record.ptr = &node;
    record.label = tree_guard_label(node);
    record.parent = node.parent != nullptr ? tree_guard_label(*node.parent) : "(no parent)";
    record.context = state.mutation_context;
    record.seq = ++state.destroy_seq;
    if (state.ring.size() < TreeGuardState::kRingCapacity)
    {
        state.ring.push_back(std::move(record));
    }
    else
    {
        state.ring[state.ring_next] = std::move(record);
        state.ring_next = (state.ring_next + 1) % TreeGuardState::kRingCapacity;
    }
}

const TreeGuardDestroyedRecord* tree_guard_find_destroyed(const void* ptr)
{
    // Newest match wins: an address can be freed and reused several times.
    const TreeGuardState& state = tree_guard_state();
    const TreeGuardDestroyedRecord* best = nullptr;
    for (const TreeGuardDestroyedRecord& record : state.ring)
    {
        if (record.ptr == ptr && (best == nullptr || record.seq > best->seq))
        {
            best = &record;
        }
    }
    return best;
}

using panorama_strings::is_space;
using panorama_strings::starts_with;
using panorama_strings::to_lower;
using panorama_strings::trim;

template <std::size_t Count>
bool tag_in(std::string_view tag, const std::array<std::string_view, Count>& tags)
{
    return std::find(tags.begin(), tags.end(), tag) != tags.end();
}

void set_node_class(PanoramaNode& node, std::string_view klass, bool enabled)
{
    const auto it = std::find(node.classes.begin(), node.classes.end(), klass);
    if (enabled && it == node.classes.end())
    {
        node.classes.emplace_back(klass);
        node.mark_style_dirty();
    }
    else if (!enabled && it != node.classes.end())
    {
        node.classes.erase(it);
        node.mark_style_dirty();
    }
}

bool has_text_entry_input(const PanoramaNode& node)
{
    if (!node.text.empty())
    {
        return true;
    }
    const auto value = node.attributes.find("value");
    return value != node.attributes.end() && !value->second.empty();
}

PanoramaNode* direct_child_by_id(PanoramaNode& node, std::string_view id)
{
    for (const auto& child : node.children)
    {
        if (child->id == id)
        {
            return child.get();
        }
    }
    return nullptr;
}

// Decodes the handful of XML entities Panorama layouts actually use.
std::string decode_entities(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();)
    {
        if (text[i] != '&')
        {
            out.push_back(text[i]);
            ++i;
            continue;
        }
        const std::size_t semi = text.find(';', i);
        if (semi == std::string_view::npos)
        {
            out.push_back('&');
            ++i;
            continue;
        }
        const std::string_view entity = text.substr(i + 1, semi - i - 1);
        if (entity == "amp")
        {
            out.push_back('&');
        }
        else if (entity == "lt")
        {
            out.push_back('<');
        }
        else if (entity == "gt")
        {
            out.push_back('>');
        }
        else if (entity == "quot")
        {
            out.push_back('"');
        }
        else if (entity == "apos")
        {
            out.push_back('\'');
        }
        else if (!entity.empty() && entity.front() == '#')
        {
            // Numeric character reference; emit the low byte (sufficient for the
            // ASCII/Latin glyphs Panorama menus use).
            const bool hex = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
            const std::string digits(entity.substr(hex ? 2 : 1));
            try
            {
                const long code = std::stol(digits, nullptr, hex ? 16 : 10);
                if (code > 0 && code < 0x110000)
                {
                    out.push_back(static_cast<char>(code & 0xFF));
                }
            }
            catch (...)
            {
                // Leave malformed numeric refs out rather than throwing.
            }
        }
        else
        {
            // Unknown entity: keep it verbatim so nothing is silently lost.
            out.push_back('&');
            out.append(entity);
            out.push_back(';');
        }
        i = semi + 1;
    }
    return out;
}

using Attributes = std::vector<std::pair<std::string, std::string>>;

const std::string* find_attr(const Attributes& attrs, std::string_view key)
{
    for (const auto& [name, value] : attrs)
    {
        if (name == key)
        {
            return &value;
        }
    }
    return nullptr;
}

enum class Section
{
    Normal,
    Styles,
    Scripts,
    Snippets,
};

class XmlParser
{
public:
    explicit XmlParser(std::string_view xml) : xml_(xml)
    {
        doc_.root = std::make_unique<PanoramaNode>();
        doc_.root->tag = "root";
        doc_.root->tag_lower = "root";
        open_.push_back(doc_.root.get());
    }

    PanoramaDocument run()
    {
        while (pos_ < xml_.size())
        {
            if (xml_[pos_] == '<')
            {
                handle_markup();
            }
            else
            {
                handle_text();
            }
        }
        return std::move(doc_);
    }

private:
    void handle_markup()
    {
        if (starts_with(rest(), "<!--"))
        {
            const std::size_t end = xml_.find("-->", pos_ + 4);
            pos_ = end == std::string_view::npos ? xml_.size() : end + 3;
            return;
        }
        if (starts_with(rest(), "<![CDATA["))
        {
            const std::size_t end = xml_.find("]]>", pos_ + 9);
            const std::size_t content_end = end == std::string_view::npos ? xml_.size() : end;
            emit_text(xml_.substr(pos_ + 9, content_end - (pos_ + 9)), /*raw=*/true);
            pos_ = end == std::string_view::npos ? xml_.size() : end + 3;
            return;
        }
        if (starts_with(rest(), "<?"))
        {
            const std::size_t end = xml_.find("?>", pos_ + 2);
            pos_ = end == std::string_view::npos ? xml_.size() : end + 2;
            return;
        }
        if (starts_with(rest(), "<!"))
        {
            const std::size_t end = xml_.find('>', pos_ + 2);
            pos_ = end == std::string_view::npos ? xml_.size() : end + 1;
            return;
        }
        if (pos_ + 1 < xml_.size() && xml_[pos_ + 1] == '/')
        {
            parse_end_tag();
            return;
        }
        parse_start_tag();
    }

    void parse_end_tag()
    {
        pos_ += 2; // skip "</"
        const std::size_t name_start = pos_;
        while (pos_ < xml_.size() && xml_[pos_] != '>' && !is_space(xml_[pos_]))
        {
            ++pos_;
        }
        const std::string name = to_lower(trim(xml_.substr(name_start, pos_ - name_start)));
        const std::size_t close = xml_.find('>', pos_);
        pos_ = close == std::string_view::npos ? xml_.size() : close + 1;
        handle_element_end(name);
    }

    void parse_start_tag()
    {
        ++pos_; // skip '<'
        const std::size_t name_start = pos_;
        while (pos_ < xml_.size() && xml_[pos_] != '>' && xml_[pos_] != '/' && !is_space(xml_[pos_]))
        {
            ++pos_;
        }
        const std::string name(xml_.substr(name_start, pos_ - name_start));

        Attributes attrs;
        parse_attributes(attrs);

        bool self_closing = false;
        if (pos_ < xml_.size() && xml_[pos_] == '/')
        {
            self_closing = true;
            ++pos_;
        }
        if (pos_ < xml_.size() && xml_[pos_] == '>')
        {
            ++pos_;
        }
        handle_element_start(name, attrs, self_closing);
    }

    void parse_attributes(Attributes& attrs)
    {
        while (pos_ < xml_.size())
        {
            while (pos_ < xml_.size() && is_space(xml_[pos_]))
            {
                ++pos_;
            }
            if (pos_ >= xml_.size() || xml_[pos_] == '>' || xml_[pos_] == '/')
            {
                return;
            }
            const std::size_t key_start = pos_;
            while (pos_ < xml_.size() && xml_[pos_] != '=' && xml_[pos_] != '>' && xml_[pos_] != '/' && !is_space(xml_[pos_]))
            {
                ++pos_;
            }
            std::string key = to_lower(xml_.substr(key_start, pos_ - key_start));
            while (pos_ < xml_.size() && is_space(xml_[pos_]))
            {
                ++pos_;
            }
            std::string value;
            if (pos_ < xml_.size() && xml_[pos_] == '=')
            {
                ++pos_;
                while (pos_ < xml_.size() && is_space(xml_[pos_]))
                {
                    ++pos_;
                }
                if (pos_ < xml_.size() && (xml_[pos_] == '"' || xml_[pos_] == '\''))
                {
                    const char quote = xml_[pos_];
                    ++pos_;
                    const std::size_t value_start = pos_;
                    while (pos_ < xml_.size() && xml_[pos_] != quote)
                    {
                        ++pos_;
                    }
                    value = decode_entities(xml_.substr(value_start, pos_ - value_start));
                    if (pos_ < xml_.size())
                    {
                        ++pos_; // closing quote
                    }
                }
                else
                {
                    const std::size_t value_start = pos_;
                    while (pos_ < xml_.size() && !is_space(xml_[pos_]) && xml_[pos_] != '>' && xml_[pos_] != '/')
                    {
                        ++pos_;
                    }
                    value = decode_entities(xml_.substr(value_start, pos_ - value_start));
                }
            }
            if (!key.empty())
            {
                attrs.emplace_back(std::move(key), std::move(value));
            }
        }
    }

    void handle_text()
    {
        const std::size_t start = pos_;
        while (pos_ < xml_.size() && xml_[pos_] != '<')
        {
            ++pos_;
        }
        emit_text(xml_.substr(start, pos_ - start), /*raw=*/false);
    }

    void emit_text(std::string_view text, bool raw)
    {
        if (capturing_style_)
        {
            style_buffer_ += raw ? std::string(text) : decode_entities(text);
            return;
        }
        if (section_ != Section::Normal)
        {
            return;
        }
        const std::string decoded = raw ? std::string(text) : decode_entities(text);
        if (trim(decoded).empty())
        {
            return;
        }
        if (!open_.empty())
        {
            if (!open_.back()->text.empty())
            {
                open_.back()->text.push_back(' ');
            }
            open_.back()->text += trim(decoded);
        }
    }

    void handle_element_start(const std::string& name, const Attributes& attrs, bool self_closing)
    {
        const std::string low = to_lower(name);

        if (section_ == Section::Styles)
        {
            if (low == "include")
            {
                if (const std::string* src = find_attr(attrs, "src"))
                {
                    doc_.stylesheet_includes.push_back(*src);
                }
            }
            else if (low == "style")
            {
                capturing_style_ = true;
                style_buffer_.clear();
                if (self_closing)
                {
                    capturing_style_ = false;
                }
            }
            return;
        }
        if (section_ == Section::Scripts)
        {
            if (low == "include")
            {
                if (const std::string* src = find_attr(attrs, "src"))
                {
                    doc_.script_includes.push_back(*src);
                }
            }
            return;
        }

        if (low == "styles" && section_ == Section::Normal)
        {
            section_ = Section::Styles;
            return;
        }
        if (low == "scripts" && section_ == Section::Normal)
        {
            section_ = Section::Scripts;
            return;
        }
        if (low == "snippets" && section_ == Section::Normal)
        {
            section_ = Section::Snippets;
            return;
        }
        if (low == "root")
        {
            return; // wrapper; our synthetic root already exists
        }

        if (section_ == Section::Snippets && !building_snippet_)
        {
            if (low == "snippet")
            {
                const std::string* snippet_id = find_attr(attrs, "name");
                if (snippet_id == nullptr)
                {
                    snippet_id = find_attr(attrs, "id");
                }
                building_snippet_ = true;
                snippet_name_ = snippet_id != nullptr ? *snippet_id : std::string();
                snippet_root_ = std::make_unique<PanoramaNode>();
                snippet_root_->tag = "snippet";
                snippet_root_->tag_lower = "snippet";
                open_.push_back(snippet_root_.get());
                snippet_base_depth_ = open_.size();
                if (self_closing)
                {
                    finalize_snippet();
                }
            }
            return;
        }

        // Normal element (top-level content, or content inside a snippet).
        PanoramaNode* node = create_node(name, low, attrs);
        if (!self_closing)
        {
            open_.push_back(node);
        }
    }

    PanoramaNode* create_node(const std::string& name, const std::string& low, const Attributes& attrs)
    {
        auto owned = std::make_unique<PanoramaNode>();
        PanoramaNode* node = owned.get();
        node->tag = name;
        node->tag_lower = low;
        node->parent = open_.empty() ? nullptr : open_.back();

        for (const auto& [key, value] : attrs)
        {
            if (key == "id")
            {
                node->id = value;
            }
            else if (key == "class")
            {
                std::istringstream stream(value);
                std::string klass;
                while (stream >> klass)
                {
                    node->classes.push_back(klass);
                }
            }
            else if (key == "style")
            {
                node->inline_style = value;
            }
            else if (key == "text")
            {
                node->text = value;
            }
            else
            {
                node->attributes[key] = value;
            }
        }

        if (node->parent != nullptr)
        {
            node->parent->children.push_back(std::move(owned));
        }
        else
        {
            // Should not happen: open_ always has at least the synthetic root.
            doc_.root->children.push_back(std::move(owned));
        }
        return node;
    }

    void handle_element_end(const std::string& low)
    {
        if (capturing_style_)
        {
            if (low == "style")
            {
                doc_.inline_styles.push_back(style_buffer_);
                style_buffer_.clear();
                capturing_style_ = false;
            }
            return;
        }
        if (section_ == Section::Styles)
        {
            if (low == "styles")
            {
                section_ = Section::Normal;
            }
            return;
        }
        if (section_ == Section::Scripts)
        {
            if (low == "scripts")
            {
                section_ = Section::Normal;
            }
            return;
        }
        if (section_ == Section::Snippets)
        {
            if (building_snippet_)
            {
                if (low == "snippet" && open_.size() == snippet_base_depth_)
                {
                    finalize_snippet();
                    return;
                }
                pop_open();
                return;
            }
            if (low == "snippets")
            {
                section_ = Section::Normal;
            }
            return;
        }

        if (low == "root")
        {
            return;
        }
        pop_open();
    }

    void finalize_snippet()
    {
        // Pop the snippet root marker off the open stack.
        if (!open_.empty() && open_.back() == snippet_root_.get())
        {
            open_.pop_back();
        }
        if (!snippet_name_.empty())
        {
            doc_.snippets[snippet_name_] = std::move(snippet_root_);
        }
        snippet_root_.reset();
        building_snippet_ = false;
        snippet_name_.clear();
        snippet_base_depth_ = 0;
    }

    void pop_open()
    {
        // Never pop the synthetic document root.
        if (open_.size() > 1)
        {
            open_.pop_back();
        }
    }

    [[nodiscard]] std::string_view rest() const { return xml_.substr(pos_); }

    std::string_view xml_;
    std::size_t pos_ = 0;
    PanoramaDocument doc_;
    std::vector<PanoramaNode*> open_;
    Section section_ = Section::Normal;

    bool capturing_style_ = false;
    std::string style_buffer_;

    bool building_snippet_ = false;
    std::string snippet_name_;
    std::unique_ptr<PanoramaNode> snippet_root_;
    std::size_t snippet_base_depth_ = 0;
};
}

void panorama_add_node_lifetime_observer(PanoramaNodeLifetimeObserver& observer)
{
    auto& observers = node_lifetime_observers();
    if (std::find(observers.begin(), observers.end(), &observer) == observers.end())
    {
        observers.push_back(&observer);
    }
}

void panorama_remove_node_lifetime_observer(PanoramaNodeLifetimeObserver& observer)
{
    auto& observers = node_lifetime_observers();
    observers.erase(std::remove(observers.begin(), observers.end(), &observer), observers.end());
}

PanoramaScopedNodeWatch::PanoramaScopedNodeWatch(std::vector<PanoramaNode*> nodes)
    : nodes_(std::move(nodes))
{
    panorama_add_node_lifetime_observer(*this);
}

PanoramaScopedNodeWatch::~PanoramaScopedNodeWatch()
{
    panorama_remove_node_lifetime_observer(*this);
}

void PanoramaScopedNodeWatch::on_panorama_node_destroyed(PanoramaNode& node)
{
    std::replace(nodes_.begin(), nodes_.end(), &node, static_cast<PanoramaNode*>(nullptr));
}

bool panorama_debug_tree_guard_enabled()
{
    static const bool enabled = []() {
        const char* value = std::getenv("OPENSTRIKE_PANORAMA_TREEGUARD");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

void panorama_debug_set_mutation_context(std::string context)
{
    if (panorama_debug_tree_guard_enabled())
    {
        tree_guard_state().mutation_context = std::move(context);
    }
}

const PanoramaNode* panorama_debug_scan_dead_links(const PanoramaNode& root, std::string& report)
{
    for (std::size_t i = 0; i < root.children.size(); ++i)
    {
        const PanoramaNode* child = root.children[i].get();
        const char* defect = nullptr;
        if (child == nullptr)
        {
            defect = "null child pointer";
        }
        else if (child->debug_liveness != PanoramaNode::kLivenessAlive)
        {
            defect = "child canary not alive (freed while linked)";
        }
        else if (child->parent != &root)
        {
            defect = "child parent backlink mismatch (replaced/reused while linked)";
        }
        if (defect != nullptr)
        {
            std::ostringstream out;
            out << defect << ": parent " << tree_guard_label(root) << " (" << static_cast<const void*>(&root)
                << ") child[" << i << "] = " << static_cast<const void*>(child);
            if (child != nullptr)
            {
                out << " liveness=0x" << std::hex << child->debug_liveness << std::dec;
            }
            if (const TreeGuardDestroyedRecord* record = tree_guard_find_destroyed(child))
            {
                out << " | destroyed as " << record->label << " (parent " << record->parent << ") seq="
                    << record->seq << " during context [" << record->context << "]";
            }
            else
            {
                out << " | no destruction record for this address (overwritten, or ring evicted)";
            }
            out << " | current context [" << tree_guard_state().mutation_context << "]";
            report = out.str();
            return child != nullptr ? child : &root;
        }
        if (const PanoramaNode* dead = panorama_debug_scan_dead_links(*child, report))
        {
            return dead;
        }
    }
    return nullptr;
}

PanoramaNode::~PanoramaNode()
{
    // Notify before members are destroyed: observers see a node whose identity
    // (and children) are still intact. Children notify afterwards, when their
    // own destructors run via the `children` vector.
    if (panorama_debug_tree_guard_enabled())
    {
        tree_guard_record_destroyed(*this);
    }
    for (PanoramaNodeLifetimeObserver* observer : node_lifetime_observers())
    {
        observer->on_panorama_node_destroyed(*this);
    }
    debug_liveness = kLivenessDead;
}

bool PanoramaNode::has_class(std::string_view klass) const
{
    return std::find(classes.begin(), classes.end(), klass) != classes.end();
}

void PanoramaNode::mark_style_dirty()
{
    style_dirty = true;
    // Invariant: descendant_style_dirty set => set on every ancestor too, so the
    // upward walk can stop at the first already-flagged node.
    for (PanoramaNode* ancestor = parent; ancestor != nullptr && !ancestor->descendant_style_dirty;
         ancestor = ancestor->parent)
    {
        ancestor->descendant_style_dirty = true;
    }
}

bool PanoramaNode::is_html_text() const
{
    const auto it = attributes.find("html");
    return it != attributes.end() && (it->second == "true" || it->second == "1");
}

bool panorama_node_is_leaf_content(const PanoramaNode& node)
{
    static constexpr std::array kLeafTags = {
        std::string_view("label"),
        std::string_view("image"),
        std::string_view("movie"),
    };
    return !node.text.empty() || tag_in(node.tag_lower, kLeafTags);
}

bool panorama_node_is_content_sized_control(const PanoramaNode& node)
{
    static constexpr std::array kControlTags = {
        std::string_view("button"),
        std::string_view("ccsgosteaminputaction"),
        std::string_view("csgosettingsenumdropdown"),
        std::string_view("dropdown"),
        std::string_view("dropdownmenu"),
        std::string_view("radiobutton"),
        std::string_view("textbutton"),
        std::string_view("textentry"),
        std::string_view("togglebutton"),
        std::string_view("tooltippanel"),
    };
    return tag_in(node.tag_lower, kControlTags);
}

bool panorama_node_defaults_to_content_size(const PanoramaNode& node)
{
    if (panorama_node_is_leaf_content(node) || panorama_node_is_content_sized_control(node))
    {
        return true;
    }
    return node.tag_lower == "panel" && node.parent != nullptr &&
           panorama_node_is_content_sized_control(*node.parent);
}

bool panorama_node_is_focusable_control(const PanoramaNode& node)
{
    static constexpr std::array kFocusableTags = {
        std::string_view("button"),
        std::string_view("togglebutton"),
        std::string_view("radiobutton"),
        std::string_view("textentry"),
        std::string_view("dropdown"),
        std::string_view("csgosettingsenumdropdown"),
        std::string_view("popupbutton"),
        std::string_view("iconbutton"),
    };
    return tag_in(node.tag_lower, kFocusableTags) || node.has_class("PopupButton") || node.has_class("IconButton");
}

bool panorama_node_is_radio_button(const PanoramaNode& node)
{
    return node.tag_lower == "radiobutton";
}

bool panorama_node_is_toggle_button(const PanoramaNode& node)
{
    return node.tag_lower == "togglebutton";
}

void panorama_set_node_selected(PanoramaNode& node, bool selected)
{
    // `selected` and the `checked` class travel together: scripts read/write
    // `panel.checked` (the runtime getter accepts either form), so an
    // engine-side toggle that flips only the flag would leave a stale class and
    // JS would still see the control as checked (the play menu could never
    // DESELECT a map tile once its session-settings sync had set `.checked`).
    if (node.selected != selected)
    {
        node.selected = selected;
        node.mark_style_dirty();
    }
    set_node_class(node, "checked", selected);
}

bool panorama_node_collapses_to_selected_child(const PanoramaNode& node)
{
    static constexpr std::array kCollapsedSelectionTags = {
        std::string_view("dropdown"),
        std::string_view("csgosettingsenumdropdown"),
    };
    return tag_in(node.tag_lower, kCollapsedSelectionTags);
}

namespace
{
// CS:GO's own class marking an open dropdown; reuses the file-local set_node_class.
constexpr std::string_view kDropDownOpenClass = "DropDownMenuVisible";
}

bool panorama_dropdown_is_open(const PanoramaNode& dropdown)
{
    return dropdown.has_class(kDropDownOpenClass);
}

void panorama_set_dropdown_open(PanoramaNode& dropdown, bool open)
{
    set_node_class(dropdown, kDropDownOpenClass, open);
}

void panorama_select_dropdown_option(PanoramaNode& dropdown, PanoramaNode& option)
{
    for (const auto& child : dropdown.children)
    {
        const bool is_option = child.get() == &option;
        if (child->selected != is_option)
        {
            child->selected = is_option;
            child->mark_style_dirty();
        }
        set_node_class(*child, "checked", is_option);
    }
    if (!option.id.empty())
    {
        dropdown.attributes["selected"] = option.id;
    }
    else
    {
        dropdown.attributes.erase("selected");
    }
    dropdown.mark_style_dirty(); // [selected=...] attribute selectors
}

PanoramaNode* panorama_dropdown_selected_child(const PanoramaNode& dropdown)
{
    PanoramaNode* first_visible = nullptr;
    const auto selected_id = dropdown.attributes.find("selected");
    for (const auto& child : dropdown.children)
    {
        if (!child->computed.visible)
        {
            continue;
        }
        if (selected_id != dropdown.attributes.end() && child->id == selected_id->second)
        {
            return child.get();
        }
        if (child->selected || child->has_class("checked") || child->has_class("selected"))
        {
            return child.get();
        }
        if (first_visible == nullptr)
        {
            first_visible = child.get();
        }
    }
    return first_visible;
}

void sync_panorama_text_entry_input_class(PanoramaNode& node)
{
    if (node.tag_lower == "textentry")
    {
        set_node_class(node, "HasInput", has_text_entry_input(node));
    }
}

// Valve's ToggleButton/RadioButton controls own internal children the engine
// must materialize: a `.TickBox` / `.RadioBox` panel (CS:GO styles them
// everywhere — checkbox art on map tiles via `.map-selection-btn:selected
// .TickBox`, explicit `visibility: collapse` opt-outs on the navbar tabs and
// pill toggles), and — when the control carries `text` — an internal Label so
// the text participates in the control's flow ("Open Party" sits right of the
// slider pill inside the button's translucent background). Idempotent: called
// from the per-frame control-presentation pass so JS-created controls and late
// `.text =` assignments are covered.
namespace
{
constexpr std::string_view kControlTextAttr = "__control_text";

PanoramaNode* direct_child_with_class(PanoramaNode& node, const char* klass)
{
    for (const auto& child : node.children)
    {
        if (child->has_class(klass))
        {
            return child.get();
        }
    }
    return nullptr;
}

PanoramaNode* direct_child_with_attr(PanoramaNode& node, std::string_view attr)
{
    for (const auto& child : node.children)
    {
        if (child->attributes.count(std::string(attr)) != 0)
        {
            return child.get();
        }
    }
    return nullptr;
}
}

void ensure_panorama_selection_control_internals(PanoramaNode& node)
{
    const bool is_toggle = node.tag_lower == "togglebutton";
    const bool is_radio = node.tag_lower == "radiobutton";
    if (!is_toggle && !is_radio)
    {
        return;
    }

    const char* box_class = is_toggle ? "TickBox" : "RadioBox";
    if (direct_child_with_class(node, box_class) == nullptr)
    {
        auto box = std::make_unique<PanoramaNode>();
        box->tag = "Panel";
        box->tag_lower = "panel";
        box->classes.emplace_back(box_class);
        box->parent = &node;
        node.children.insert(node.children.begin(), std::move(box));
        node.mark_style_dirty();
    }

    if (!node.text.empty())
    {
        PanoramaNode* label = direct_child_with_attr(node, kControlTextAttr);
        if (label == nullptr)
        {
            auto owned = std::make_unique<PanoramaNode>();
            owned->tag = "Label";
            owned->tag_lower = "label";
            owned->attributes.emplace(std::string(kControlTextAttr), "1");
            owned->parent = &node;
            label = owned.get();
            node.children.push_back(std::move(owned));
            node.mark_style_dirty();
        }
        if (label->text != node.text)
        {
            label->text = node.text;
            node.mark_style_dirty();
        }
    }
}

namespace
{
float read_float_attr(const PanoramaNode& node, std::string_view key, float fallback)
{
    const auto it = node.attributes.find(std::string(key));
    if (it == node.attributes.end() || it->second.empty())
    {
        return fallback;
    }
    char* end = nullptr;
    const float value = std::strtof(it->second.c_str(), &end);
    return end == it->second.c_str() ? fallback : value;
}

// Compact, round-trip-stable formatting for a slider's `value` string (the host
// reads it straight back into a convar). %.6g matches typical convar precision
// and drops trailing zeros.
std::string format_slider_value(float value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.6g", static_cast<double>(value));
    return std::string(buffer);
}

PanoramaNode& append_child_node(PanoramaNode& parent, const char* tag, const char* tag_lower, const char* id)
{
    auto owned = std::make_unique<PanoramaNode>();
    owned->tag = tag;
    owned->tag_lower = tag_lower;
    if (id != nullptr)
    {
        owned->id = id;
    }
    owned->parent = &parent;
    PanoramaNode& ref = *owned;
    parent.children.push_back(std::move(owned));
    return ref;
}
} // namespace

bool panorama_node_is_slider(const PanoramaNode& node)
{
    return node.tag_lower == "slider" || node.tag_lower == "slottedslider";
}

bool panorama_node_is_settings_slider(const PanoramaNode& node)
{
    return node.tag_lower == "csgosettingsslider";
}

void ensure_panorama_slider_internals(PanoramaNode& node)
{
    if (!panorama_node_is_slider(node))
    {
        return;
    }
    PanoramaNode* track = direct_child_by_id(node, "SliderTrack");
    if (track == nullptr)
    {
        track = &append_child_node(node, "Panel", "panel", "SliderTrack");
        append_child_node(*track, "Panel", "panel", "SliderTrackProgress");
        append_child_node(*track, "Panel", "panel", "SliderThumb");
        node.mark_style_dirty();
    }
}

void ensure_panorama_settings_slider_internals(PanoramaNode& node)
{
    if (!panorama_node_is_settings_slider(node))
    {
        return;
    }

    // settings_slider.css ships inside the native control's own template (Valve's
    // CSGOSettingsSlider loads settings_slider.xml), so the settings tab pages do
    // NOT include it. We materialize the control's DOM ourselves, so bake the few
    // layout rules that file provides as inline styles — the row flows its label /
    // slider / readout left-to-right. The track/thumb visuals come from
    // csgostyles.css, which every settings page does include. Applied once.
    if (node.attributes.count("__rowinit") == 0)
    {
        node.attributes["__rowinit"] = "1";
        node.inline_style += "flow-children: right; width: 100%; height: 48px; vertical-align: middle;";
        node.mark_style_dirty();
    }

    // Title label (#Title) carries the row's localization token (the control's
    // own `text=`); the host's localize pass resolves it like any other label.
    PanoramaNode* title = direct_child_by_id(node, "Title");
    if (title == nullptr)
    {
        title = &append_child_node(node, "Label", "label", "Title");
        title->inline_style = "width: fill-parent-flow(1.0); vertical-align: middle;";
        node.mark_style_dirty();
    }
    if (title->text != node.text)
    {
        title->text = node.text;
        node.mark_style_dirty();
    }

    // The inner slider (#Slider.HorizontalSlider) inherits the row's range so it
    // is self-contained for layout; the host binds its `value` to the convar.
    PanoramaNode* slider = direct_child_by_id(node, "Slider");
    if (slider == nullptr)
    {
        slider = &append_child_node(node, "Slider", "slider", "Slider");
        slider->classes.emplace_back("HorizontalSlider");
        slider->attributes["direction"] = "horizontal";
        slider->inline_style = "width: 25.5%; height: 36px; margin-right: 10px; vertical-align: center;";
        node.mark_style_dirty();
    }
    if (node.attributes.count("min") != 0)
    {
        slider->attributes["min"] = node.attributes.at("min");
    }
    if (node.attributes.count("max") != 0)
    {
        slider->attributes["max"] = node.attributes.at("max");
    }

    // Numeric readout (TextEntry#Value); the host fills its text from the convar
    // (e.g. a percentage). Collapsed by CSS on hidevalue rows.
    if (direct_child_by_id(node, "Value") == nullptr)
    {
        PanoramaNode& value = append_child_node(node, "TextEntry", "textentry", "Value");
        value.attributes["textmode"] = "numeric";
        value.attributes["maxchars"] = "4";
        value.inline_style = "width: 80px; text-align: center;";
        node.mark_style_dirty();
    }
}

bool panorama_node_is_settings_keybinder(const PanoramaNode& node)
{
    return node.tag_lower == "csgosettingskeybinder";
}

void ensure_panorama_settings_keybinder_internals(PanoramaNode& node)
{
    if (!panorama_node_is_settings_keybinder(node))
    {
        return;
    }

    // Build once. The tab pages author these rows childless
    // (`<CSGOSettingsKeyBinder text=.. id=.. bind=../>`), so the whole DOM is
    // materialized here, mirroring ensure_panorama_settings_slider_internals.
    if (node.attributes.count("__kbinit") != 0)
    {
        return;
    }
    node.attributes["__kbinit"] = "1";

    // settings_keybinder.css ships inside Valve's native control template and is
    // NOT included by the settings tab pages, so the layout rules that file
    // provides (BindingRow / BindingRowLabel / #LabelFXContainer /
    // BindingRowButton / ClearKeybinding) are baked as inline styles below.
    node.inline_style += "flow-children: right; width: 100%; height: 48px; vertical-align: middle;";

    // The clickable leaves carry a row-unique id (the host resolves a polled click
    // by climbing from the hit leaf to its CSGOSettingsKeyBinder ancestor, so the
    // exact id only needs to be unique + suffix-tagged bind/clear).
    const std::string base = node.id.empty() ? std::string("keybind") : node.id;

    // #title — the row's `text=` token; the host localize pass resolves it. The
    // control itself is excluded from panorama_node_paints_own_text so the token
    // is not painted twice.
    PanoramaNode& title = append_child_node(node, "Label", "label", nullptr);
    title.inline_style =
        "width: fill-parent-flow(0.8); font-size: 18px; horizontal-align: left; vertical-align: middle;";
    title.text = node.text;

    // #LabelFXContainer holds the value button + the clear button, right-aligned.
    PanoramaNode& fx = append_child_node(node, "Panel", "panel", nullptr);
    fx.inline_style =
        "width: fill-parent-flow(0.3); height: 32px; horizontal-align: right; "
        "vertical-align: middle; flow-children: right;";

    PanoramaNode& label_container = append_child_node(fx, "Panel", "panel", nullptr);
    label_container.inline_style =
        "width: fill-parent-flow(1.0); height: 100%; horizontal-align: right; "
        "vertical-align: middle; flow-children: right;";

    // The value label (BindingRowButton) shows the currently bound key; clicking it
    // starts capture. The host fills its text from the key bindings each frame.
    const std::string value_id = base + "__bind";
    PanoramaNode& value = append_child_node(label_container, "Label", "label", value_id.c_str());
    value.inline_style =
        "width: 100%; color: #8d9698; horizontal-align: right; text-align: center; "
        "font-size: 18px; vertical-align: middle;";

    // The clear button (ClearKeybinding): collapsed until the host sees a binding.
    const std::string clear_id = base + "__clear";
    PanoramaNode& clear = append_child_node(fx, "Label", "label", clear_id.c_str());
    clear.inline_style =
        "width: 24px; height: 24px; horizontal-align: right; vertical-align: middle; "
        "text-align: center; font-size: 18px; color: #8d9698;";
    clear.text = "\xE2\x9C\x95"; // U+2715 MULTIPLICATION X
    clear.visibility_override = 0;

    node.mark_style_dirty();
}

float panorama_slider_fraction(const PanoramaNode& node)
{
    const float min = read_float_attr(node, "min", 0.0F);
    const float max = read_float_attr(node, "max", 1.0F);
    const float value = read_float_attr(node, "value", min);
    const float range = max - min;
    if (range <= 0.0F)
    {
        return 0.0F;
    }
    return std::clamp((value - min) / range, 0.0F, 1.0F);
}

bool panorama_set_slider_fraction(PanoramaNode& node, float fraction)
{
    const float min = read_float_attr(node, "min", 0.0F);
    const float max = read_float_attr(node, "max", 1.0F);
    const float value = min + std::clamp(fraction, 0.0F, 1.0F) * (max - min);
    std::string formatted = format_slider_value(value);
    std::string& slot = node.attributes["value"];
    if (slot == formatted)
    {
        return false;
    }
    slot = std::move(formatted);
    node.mark_style_dirty();
    return true;
}

bool panorama_node_paints_own_text(const PanoramaNode& node)
{
    // Toggle/radio buttons render their text through the internal control
    // label (see above); the settings slider moves its `text=` into a #Title
    // child. Painting/measuring node.text on the control itself would double it.
    return node.tag_lower != "togglebutton" && node.tag_lower != "radiobutton"
        && node.tag_lower != "csgosettingsslider" && node.tag_lower != "csgosettingskeybinder";
}

// ---- overflow:scroll ---------------------------------------------------------

namespace
{
constexpr std::string_view kScrollbarAttr = "__scrollbar";

PanoramaNode* direct_scrollbar_child(PanoramaNode& node, const char* axis)
{
    for (const auto& child : node.children)
    {
        const auto it = child->attributes.find(std::string(kScrollbarAttr));
        if (it != child->attributes.end() && it->second == axis)
        {
            return child.get();
        }
    }
    return nullptr;
}

void append_scrollbar(PanoramaNode& node, const char* tag, const char* tag_lower, const char* axis)
{
    auto bar = std::make_unique<PanoramaNode>();
    bar->tag = tag;
    bar->tag_lower = tag_lower;
    bar->attributes.emplace(std::string(kScrollbarAttr), axis);
    // The bar captures pointer input over its track (Valve's scrollbars swallow
    // clicks; without this a click on the track would fall through to whatever
    // content row sits beneath the overlay).
    bar->attributes.emplace("hittest", "true");
    bar->parent = &node;

    auto thumb = std::make_unique<PanoramaNode>();
    thumb->tag = "Panel";
    thumb->tag_lower = "panel";
    thumb->classes.emplace_back("ScrollThumb");
    thumb->parent = bar.get();
    bar->children.push_back(std::move(thumb));

    // Appended last so the chrome paints over the scrolled content.
    node.children.push_back(std::move(bar));
    node.mark_style_dirty();
}
}

void ensure_panorama_scrollbar_internals(PanoramaNode& node)
{
    if (panorama_node_is_scrollbar(node))
    {
        return;
    }
    if (panorama_node_scrolls_y(node) && direct_scrollbar_child(node, "vertical") == nullptr)
    {
        append_scrollbar(node, "VerticalScrollBar", "verticalscrollbar", "vertical");
    }
    if (panorama_node_scrolls_x(node) && direct_scrollbar_child(node, "horizontal") == nullptr)
    {
        append_scrollbar(node, "HorizontalScrollBar", "horizontalscrollbar", "horizontal");
    }
}

bool panorama_node_is_scrollbar(const PanoramaNode& node)
{
    return node.attributes.count(std::string(kScrollbarAttr)) != 0;
}

bool panorama_node_scrolls_x(const PanoramaNode& node)
{
    return node.computed.overflow_scroll_x && !panorama_node_is_scrollbar(node);
}

bool panorama_node_scrolls_y(const PanoramaNode& node)
{
    return node.computed.overflow_scroll_y && !panorama_node_is_scrollbar(node);
}

bool panorama_set_scroll_offset(PanoramaNode& node, float offset_x, float offset_y)
{
    // WebCore RenderLayerScrollableArea::clampScrollOffset: constrain to
    // [minimumScrollOffset (= 0), maximumScrollOffset].
    const float clamped_x = std::clamp(offset_x, 0.0F, node.max_scroll_x);
    const float clamped_y = std::clamp(offset_y, 0.0F, node.max_scroll_y);
    const bool changed = clamped_x != node.scroll_offset_x || clamped_y != node.scroll_offset_y;
    node.scroll_offset_x = clamped_x;
    node.scroll_offset_y = clamped_y;
    return changed;
}

bool panorama_smooth_scroll_to(PanoramaNode& node, float dest_x, float dest_y)
{
    // WebCore ScrollAnimationSmooth::startAnimatedScrollToDestination /
    // retargetActiveAnimation: the destination clamps to the scroll extents; a
    // fresh start begins at rest, a retarget leaves the integrated velocity
    // untouched so the curve stays C1-continuous.
    const float clamped_x = std::clamp(dest_x, 0.0F, node.max_scroll_x);
    const float clamped_y = std::clamp(dest_y, 0.0F, node.max_scroll_y);
    auto& anim = node.scroll_anim;
    if (!anim.active)
    {
        if (clamped_x == node.scroll_offset_x && clamped_y == node.scroll_offset_y)
        {
            return false; // already there
        }
        anim.velocity_x = 0.0F;
        anim.velocity_y = 0.0F;
        anim.active = true;
        anim.dest_x = clamped_x;
        anim.dest_y = clamped_y;
        return true;
    }
    const bool moved = clamped_x != anim.dest_x || clamped_y != anim.dest_y;
    anim.dest_x = clamped_x;
    anim.dest_y = clamped_y;
    return moved;
}

void panorama_cancel_scroll_animation(PanoramaNode& node)
{
    node.scroll_anim.active = false;
    node.scroll_anim.velocity_x = 0.0F;
    node.scroll_anim.velocity_y = 0.0F;
}

float panorama_scroll_destination_x(const PanoramaNode& node)
{
    return node.scroll_anim.active ? node.scroll_anim.dest_x : node.scroll_offset_x;
}

float panorama_scroll_destination_y(const PanoramaNode& node)
{
    return node.scroll_anim.active ? node.scroll_anim.dest_y : node.scroll_offset_y;
}

namespace
{
// One axis of WebCore ScrollableArea::getRectToExposeForScrollIntoView with
// ScrollAlignment::alignToEdgeIfNeeded ({visible: NoScroll, hidden/partial:
// AlignToClosestEdge}): returns the desired visible-bounds origin on the axis.
float expose_axis_to_closest_edge(float visible_pos, float visible_size, float expose_pos, float expose_size)
{
    const float visible_max = visible_pos + visible_size;
    const float expose_max = expose_pos + expose_size;

    const bool intersects = expose_max >= visible_pos && expose_pos <= visible_max;
    bool align_to_closest_edge = true;
    if (intersects)
    {
        const float intersect_size =
            std::max(0.0F, std::min(visible_max, expose_max) - std::max(visible_pos, expose_pos));
        if (intersect_size == expose_size || intersect_size == visible_size)
        {
            // Fully visible (or filling the whole viewport): the visible
            // behavior, NoScroll.
            align_to_closest_edge = false;
        }
        // Partially visible / not intersecting: the partial/hidden behavior,
        // AlignToClosestEdge.
    }
    if (!align_to_closest_edge)
    {
        return visible_pos;
    }
    // AlignToClosestEdge resolves to the far edge when the rect lies beyond it
    // and fits; otherwise to the near edge.
    if (expose_max > visible_max && expose_size < visible_size)
    {
        return expose_max - visible_size; // AlignBottom / AlignRight
    }
    return expose_pos; // AlignTop / AlignLeft
}
}

bool panorama_scroll_ancestors_to_fit(PanoramaNode& target, bool smooth)
{
    // WebCore scrollRectToVisible walks the layer tree upward, each scrollable
    // ancestor scrolling to expose the (already adjusted) rect.
    float rect_x = target.layout.x;
    float rect_y = target.layout.y;
    const float rect_w = target.layout.width;
    const float rect_h = target.layout.height;

    bool changed = false;
    for (PanoramaNode* scroller = target.parent; scroller != nullptr; scroller = scroller->parent)
    {
        if (!panorama_node_scrolls_x(*scroller) && !panorama_node_scrolls_y(*scroller))
        {
            continue;
        }
        // Visible bounds = the overflow clip box (border box inset by the
        // border), WebCore's clientWidth/clientHeight box.
        const float vx = scroller->layout.x + std::max(0.0F, scroller->computed.border_left());
        const float vy = scroller->layout.y + std::max(0.0F, scroller->computed.border_top());
        const float vw = std::max(0.0F, scroller->layout.width - std::max(0.0F, scroller->computed.border_left()) -
            std::max(0.0F, scroller->computed.border_right()));
        const float vh = std::max(0.0F, scroller->layout.height - std::max(0.0F, scroller->computed.border_top()) -
            std::max(0.0F, scroller->computed.border_bottom()));

        // Smooth mode reasons about the offset the scroller is HEADING to (the
        // rect was laid out at the current offset, but successive fits — e.g.
        // keyboard-repeated section nav — must retarget, not re-derive from a
        // mid-flight position; WebCore retargets the active animation).
        const float from_x = smooth ? panorama_scroll_destination_x(*scroller) : scroller->scroll_offset_x;
        const float from_y = smooth ? panorama_scroll_destination_y(*scroller) : scroller->scroll_offset_y;
        const float drift_x = from_x - scroller->scroll_offset_x;
        const float drift_y = from_y - scroller->scroll_offset_y;

        // The rect was laid out at the CURRENT offset; once the pending motion
        // settles at `from`, the content (and the rect) sits `drift` further up/
        // left. Expose that settled position and retarget from `from`.
        float new_x = from_x;
        float new_y = from_y;
        if (panorama_node_scrolls_x(*scroller))
        {
            new_x += expose_axis_to_closest_edge(vx, vw, rect_x - drift_x, rect_w) - vx;
        }
        if (panorama_node_scrolls_y(*scroller))
        {
            new_y += expose_axis_to_closest_edge(vy, vh, rect_y - drift_y, rect_h) - vy;
        }

        if (smooth)
        {
            const float clamped_x = std::clamp(new_x, 0.0F, scroller->max_scroll_x);
            const float clamped_y = std::clamp(new_y, 0.0F, scroller->max_scroll_y);
            if (panorama_smooth_scroll_to(*scroller, clamped_x, clamped_y))
            {
                changed = true;
            }
            // Outer scrollers expose the rect's EVENTUAL position: this
            // scroller's full pending movement (current offset -> committed
            // destination; clamping may absorb part of the ask) shifts it.
            rect_x -= clamped_x - scroller->scroll_offset_x;
            rect_y -= clamped_y - scroller->scroll_offset_y;
            continue;
        }

        const float old_x = scroller->scroll_offset_x;
        const float old_y = scroller->scroll_offset_y;
        if (panorama_set_scroll_offset(*scroller, new_x, new_y))
        {
            // Direct manipulation: a live spring must not fight the jump.
            panorama_cancel_scroll_animation(*scroller);
            // The rect's design-space position shifts by however much the
            // scroller actually moved (clamping may absorb some of the ask);
            // outer scrollers must expose the rect's post-scroll position.
            rect_x -= scroller->scroll_offset_x - old_x;
            rect_y -= scroller->scroll_offset_y - old_y;
            changed = true;
        }
    }
    return changed;
}

void ensure_panorama_text_entry_placeholders(PanoramaNode& root, const PanoramaLocalizeCallback& localize)
{
    if (root.tag_lower == "textentry")
    {
        const auto placeholder_attr = root.attributes.find("placeholder");
        if (placeholder_attr != root.attributes.end() && !placeholder_attr->second.empty())
        {
            PanoramaNode* placeholder = direct_child_by_id(root, "PlaceholderText");
            if (placeholder == nullptr)
            {
                auto owned = std::make_unique<PanoramaNode>();
                owned->tag = "Label";
                owned->tag_lower = "label";
                owned->id = "PlaceholderText";
                owned->parent = &root;
                placeholder = owned.get();
                root.children.push_back(std::move(owned));
            }
            placeholder->text = localize ? localize(placeholder_attr->second) : placeholder_attr->second;
        }
        sync_panorama_text_entry_input_class(root);
    }

    for (const auto& child : root.children)
    {
        ensure_panorama_text_entry_placeholders(*child, localize);
    }
}

PanoramaNode* PanoramaNode::find_by_id(std::string_view target_id)
{
    if (id == target_id)
    {
        return this;
    }
    for (const auto& child : children)
    {
        if (PanoramaNode* found = child->find_by_id(target_id))
        {
            return found;
        }
    }
    return nullptr;
}

PanoramaNode* PanoramaDocument::find_by_id(std::string_view target_id) const
{
    return root ? root->find_by_id(target_id) : nullptr;
}

PanoramaDocument parse_panorama_xml(std::string_view xml)
{
    XmlParser parser(xml);
    return parser.run();
}
}
