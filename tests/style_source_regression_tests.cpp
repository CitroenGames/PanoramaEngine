#include "ui/panorama/panorama_document_session.hpp"
#include "ui/panorama/panorama_dom.hpp"
#include "ui/panorama/panorama_resource_provider.hpp"
#include "ui/panorama/panorama_style.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace
{
void expect(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << "style/source regression failure: " << message << '\n';
        std::exit(1);
    }
}

std::unique_ptr<panorama::PanoramaNode> make_node(
    panorama::PanoramaNode& parent,
    std::string id = {})
{
    auto node = std::make_unique<panorama::PanoramaNode>();
    node->tag = "Panel";
    node->tag_lower = "panel";
    node->id = std::move(id);
    node->parent = &parent;
    return node;
}

void test_source_templates_and_batched_finalization()
{
    panorama::PanoramaDocumentSession session;
    auto provider = std::make_unique<panorama::PanoramaMemoryResourceProvider>();
    panorama::PanoramaMemoryResourceProvider* provider_view = provider.get();
    provider->add_text(
        "panorama/layout/cache.xml",
        R"xml(<root>
          <styles>
            <include src="file://{resources}/panorama/styles/cache.css"/>
            <style>.inline { height: 7px; }</style>
          </styles>
          <Panel id="Cached" class="external inline"/>
        </root>)xml");
    provider->add_text("panorama/styles/cache.css", ".external { width: 19px; }");
    session.resources().add_provider(std::move(provider), 0, "cache-fixture");

    panorama::PanoramaDocumentSessionOptions options;
    options.localize_text = false;
    expect(session.load("panorama/layout/cache.xml", options), "initial cached document load failed");
    expect(session.style_sheet().rules().size() == 2, "initial source rule count changed");
    const panorama::PanoramaDocumentSourceStats first = session.source_stats();
    const std::uint64_t first_finalizations = session.style_sheet().value_finalization_count();
    expect(first.resource_reads == 2, "initial layout/CSS resource read count changed");
    expect(first.layout_parses == 1 && first.style_parses == 2, "initial parsed-template count changed");
    expect(first_finalizations == 1, "nested source additions were not finalized as one batch");

    expect(session.load("panorama/layout/cache.xml", options), "cached document reload failed");
    const panorama::PanoramaDocumentSourceStats second = session.source_stats();
    expect(second.resource_reads == first.resource_reads, "reload reread a cached source");
    expect(second.layout_parses == first.layout_parses, "reload reparsed cached XML");
    expect(second.style_parses == first.style_parses, "reload reparsed cached CSS");
    expect(second.layout_cache_hits > first.layout_cache_hits, "layout cache hit was not recorded");
    expect(second.style_cache_hits >= first.style_cache_hits + 2, "style cache hits were not recorded");
    expect(
        session.style_sheet().value_finalization_count() == first_finalizations + 1,
        "reload did not perform exactly one stylesheet finalization");
    expect(session.style_sheet().rules().size() == 2, "reload duplicated cached rules");

    provider_view->add_text(
        "panorama/layout/cache.xml",
        R"xml(<root><Panel id="Changed"/></root>)xml");
    session.invalidate_source_cache();
    expect(session.load("panorama/layout/cache.xml", options), "invalidated document reload failed");
    expect(session.document().find_by_id("Changed") != nullptr, "explicit invalidation retained stale XML");
    expect(
        session.source_stats().layout_parses == first.layout_parses + 1,
        "explicit invalidation did not force one XML parse");
}

void test_structured_inline_declarations()
{
    panorama::PanoramaStyleSheet sheet;
    sheet.add_source("Panel { opacity: 0.5; }");
    panorama::PanoramaNode node;
    node.tag = "Panel";
    node.tag_lower = "panel";
    node.inline_style = "width: 10px; color: red !important;";

    (void)panorama::panorama_inline_style_stats_take();
    sheet.compute(node);
    const panorama::PanoramaInlineStyleStats after_raw = panorama::panorama_inline_style_stats_take();
    expect(after_raw.raw_parses == 1, "raw inline text was not parsed exactly once");

    expect(
        panorama::panorama_set_inline_style_property(node, "width", "25px"),
        "structured property write reported no change");
    expect(node.inline_style.find("width: 25px;") != std::string::npos, "compatibility serialization lost JS write");
    expect(node.inline_style.find("color: red !important;") != std::string::npos, "unrelated important declaration changed");
    expect(node.inline_style_cache.declarations.size() == 2, "structured declaration count changed");

    sheet.compute(node);
    const panorama::PanoramaInlineStyleStats after_write = panorama::panorama_inline_style_stats_take();
    expect(after_write.raw_parses == 0, "structured JS write caused cascade reparsing");
    expect(after_write.property_writes == 1, "structured property write counter changed");
    expect(std::fabs(node.computed.width.value - 25.0F) < 0.001F, "structured width did not reach computed style");

    expect(
        panorama::panorama_set_inline_style_property(node, "width", ""),
        "structured property deletion reported no change");
    expect(node.inline_style.find("width") == std::string::npos, "structured property deletion was not serialized");
}

void test_sibling_topology_and_scoped_membership()
{
    panorama::PanoramaNode root;
    root.tag = "root";
    root.tag_lower = "root";
    constexpr std::size_t kSiblingCount = 2048;
    for (std::size_t index = 0; index < kSiblingCount; ++index)
    {
        auto child = make_node(root);
        if (index == 0)
        {
            child->classes.push_back("seed");
        }
        if (index + 1 == kSiblingCount)
        {
            child->classes.push_back("target");
        }
        root.children.push_back(std::move(child));
    }
    panorama::panorama_notify_tree_structure_changed(root);
    expect(root.children.front()->previous_sibling_node == nullptr, "first sibling has a predecessor");
    for (std::size_t index = 1; index < root.children.size(); ++index)
    {
        expect(
            root.children[index]->previous_sibling_node == root.children[index - 1].get(),
            "previous-sibling topology drifted");
    }

    panorama::PanoramaStyleSheet sibling_sheet;
    sibling_sheet.add_source(".seed ~ .target { opacity: 0.25; }");
    sibling_sheet.compute(root);
    expect(
        std::fabs(root.children.back()->computed.opacity - 0.25F) < 0.001F,
        "general-sibling selector result changed");

    root.children.erase(root.children.begin() + 100);
    panorama::panorama_notify_tree_structure_changed(root);
    expect(
        root.children[100]->previous_sibling_node == root.children[99].get(),
        "erase did not repair previous-sibling topology");

    panorama::PanoramaStyleSheet scoped_sheet;
    scoped_sheet.begin_source_batch();
    for (std::uint16_t scope = 2; scope < 34; ++scope)
    {
        scoped_sheet.add_source(".scoped { opacity: 0.75; }", scope);
    }
    scoped_sheet.end_source_batch();

    panorama::PanoramaNode deep_root;
    deep_root.tag = "root";
    deep_root.tag_lower = "root";
    panorama::PanoramaNode* cursor = &deep_root;
    for (std::uint16_t depth = 0; depth < 256; ++depth)
    {
        auto child = make_node(*cursor);
        child->style_scope_mark = static_cast<std::uint16_t>(2 + (depth % 32));
        child->classes.push_back("scoped");
        cursor->children.push_back(std::move(child));
        panorama::panorama_notify_tree_structure_changed(*cursor);
        cursor = cursor->children.back().get();
    }
    scoped_sheet.compute(deep_root);
    expect(
        cursor->style_source_membership.size() == 32,
        "deep scope cache did not retain one result per source");
    for (unsigned char member : cursor->style_source_membership)
    {
        expect(member != 0, "inherited deep scope membership was lost");
    }
}

void test_bounded_matched_style_cache()
{
    panorama::PanoramaStyleSheet sheet;
    sheet.add_source("Panel { opacity: 0.9; }");
    panorama::PanoramaNode root;
    root.tag = "root";
    root.tag_lower = "root";
    for (std::size_t index = 0; index < 4200; ++index)
    {
        auto child = make_node(root);
        child->inline_style = "width: " + std::to_string(index + 1) + "px;";
        root.children.push_back(std::move(child));
    }
    panorama::panorama_notify_tree_structure_changed(root);

    (void)panorama::panorama_matched_style_cache_stats_take();
    sheet.compute(root);
    const panorama::PanoramaMatchedStyleCacheStats stats =
        panorama::panorama_matched_style_cache_stats_take();
    expect(stats.lookups >= 4200, "matched-style cache lookup counter changed");
    expect(stats.inserts >= 4200, "matched-style cache insert counter changed");
    expect(stats.evictions > 0, "over-capacity cache did not evict deterministically");
    expect(stats.entries == stats.capacity, "matched-style cache exceeded or undershot its bound");
}
}

int main()
{
    test_source_templates_and_batched_finalization();
    test_structured_inline_declarations();
    test_sibling_topology_and_scoped_membership();
    test_bounded_matched_style_cache();
    std::cout << "style/source optimization regression tests passed\n";
    return 0;
}
