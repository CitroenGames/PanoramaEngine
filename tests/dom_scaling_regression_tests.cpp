#include "ui/panorama/panorama_dom.hpp"
#include "ui/panorama/panorama_input.hpp"
#include "ui/panorama/panorama_layout.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{
bool expect(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::fprintf(
            stderr,
            "DOM scaling regression failed: %.*s\n",
            static_cast<int>(message.size()),
            message.data());
    }
    return condition;
}

panorama::PanoramaNode& append_node(
    panorama::PanoramaNode& parent,
    std::string id,
    float x,
    float y,
    float width,
    float height,
    bool hittable = false)
{
    using namespace panorama;
    auto child = std::make_unique<PanoramaNode>();
    child->tag = "Panel";
    child->tag_lower = "panel";
    child->id = std::move(id);
    child->parent = &parent;
    child->computed.has_position = true;
    child->computed.pos_x = x;
    child->computed.pos_y = y;
    child->computed.width = PanoramaLength{PanoramaLengthType::Pixels, width};
    child->computed.height = PanoramaLength{PanoramaLengthType::Pixels, height};
    if (hittable)
    {
        child->attributes["hittest"] = "true";
    }
    PanoramaNode* result = child.get();
    parent.children.push_back(std::move(child));
    return *result;
}

bool inside(const panorama::PanoramaLayoutBox& box, float x, float y)
{
    return x >= box.x && x < box.x + box.width && y >= box.y && y < box.y + box.height;
}

panorama::PanoramaNode* unpruned_hit_test(panorama::PanoramaNode& node, float x, float y, std::size_t& visits)
{
    using namespace panorama;
    ++visits;
    if (!node.computed.visible)
    {
        return nullptr;
    }
    for (auto child = node.children.rbegin(); child != node.children.rend(); ++child)
    {
        if (PanoramaNode* hit = unpruned_hit_test(**child, x, y, visits))
        {
            return hit;
        }
    }
    return inside(node.layout, x, y) && panorama_node_is_hittable(node) ? &node : nullptr;
}

bool test_hit_test_pruning_and_parity()
{
    using namespace panorama;
    PanoramaNode root;
    root.tag = "root";
    root.tag_lower = "root";

    constexpr int columns = 40;
    constexpr int rows = 25;
    for (int y = 0; y < rows; ++y)
    {
        for (int x = 0; x < columns; ++x)
        {
            append_node(
                root,
                "cell-" + std::to_string(y * columns + x),
                static_cast<float>(x * 20),
                static_cast<float>(y * 20),
                10.0F,
                10.0F,
                true);
        }
    }
    // Ordinary hit testing historically allows noclip-like descendants outside
    // the root box. The conservative union must retain this target.
    PanoramaNode& overflow = append_node(root, "overflow", 900.0F, 100.0F, 20.0F, 20.0F, true);
    layout_panorama_tree(root, 800.0F, 500.0F);

    for (int y = -5; y <= 505; y += 17)
    {
        for (int x = -5; x <= 925; x += 19)
        {
            std::size_t oracle_visits = 0;
            PanoramaNode* expected = unpruned_hit_test(root, static_cast<float>(x), static_cast<float>(y), oracle_visits);
            PanoramaNode* actual = panorama_hit_test(root, static_cast<float>(x), static_cast<float>(y));
            if (!expect(actual == expected, "pruned hit target diverged from the unpruned oracle"))
            {
                return false;
            }
        }
    }
    if (!expect(panorama_hit_test(root, 905.0F, 105.0F) == &overflow,
            "overflowing descendant was rejected by the root bounds"))
    {
        return false;
    }

    panorama_reset_dom_structural_counters();
    panorama_enable_dom_structural_counters(true);
    PanoramaNode* selected = panorama_hit_test(root, 5.0F, 5.0F);
    const PanoramaDomStructuralCounters counters = panorama_dom_structural_counters();
    panorama_enable_dom_structural_counters(false);
    if (!expect(selected != nullptr, "grid point did not select a cell") ||
        !expect(counters.hit_test_nodes_visited < 10, "large sibling hit test visited an unbounded node count") ||
        !expect(counters.hit_test_subtrees_pruned > 900, "large sibling hit test did not prune separated branches"))
    {
        return false;
    }

    // A transformed branch is deliberately marked uncertain. Pruning may remain
    // active for known-safe siblings, but the uncertain branch itself must be
    // visited and preserve oracle parity.
    PanoramaTransformOp translate;
    translate.type = PanoramaTransformOp::Type::Translate;
    translate.x = 50.0F;
    overflow.computed.transform.ops.push_back(translate);
    layout_panorama_tree(root, 800.0F, 500.0F);
    std::size_t oracle_visits = 0;
    return expect(
        panorama_hit_test(root, 905.0F, 105.0F) == unpruned_hit_test(root, 905.0F, 105.0F, oracle_visits),
        "transform-uncertain branch was falsely pruned");
}

bool test_deep_tree_parity()
{
    using namespace panorama;
    PanoramaNode root;
    root.tag = "root";
    root.tag_lower = "root";
    PanoramaNode* cursor = &root;
    constexpr int depth = 256;
    for (int index = 0; index < depth; ++index)
    {
        cursor = &append_node(*cursor, "depth-" + std::to_string(index), 0.0F, 0.0F, 100.0F, 100.0F, index + 1 == depth);
    }
    layout_panorama_tree(root, 100.0F, 100.0F);

    panorama_reset_dom_structural_counters();
    panorama_enable_dom_structural_counters(true);
    PanoramaNode* actual = panorama_hit_test(root, 50.0F, 50.0F);
    const PanoramaDomStructuralCounters counters = panorama_dom_structural_counters();
    panorama_enable_dom_structural_counters(false);
    std::size_t oracle_visits = 0;
    PanoramaNode* expected = unpruned_hit_test(root, 50.0F, 50.0F, oracle_visits);
    return expect(actual == expected, "deep-tree target diverged from the oracle") &&
        expect(counters.hit_test_nodes_visited == static_cast<std::size_t>(depth + 1),
            "deep-tree visit counter was not deterministic");
}

bool test_document_id_index_mutations()
{
    using namespace panorama;
    PanoramaNode root;
    root.tag = "root";
    root.tag_lower = "root";
    PanoramaNode& left = append_node(root, "branch-left", 0, 0, 0, 0);
    PanoramaNode& first = append_node(left, "duplicate", 0, 0, 0, 0);
    PanoramaNode& right = append_node(root, "branch-right", 0, 0, 0, 0);
    PanoramaNode& second = append_node(right, "duplicate", 0, 0, 0, 0);
    for (int index = 0; index < 2000; ++index)
    {
        append_node(root, "unrelated-" + std::to_string(index), 0, 0, 0, 0);
    }

    panorama_reset_dom_structural_counters();
    panorama_enable_dom_structural_counters(true);
    if (!expect(root.find_by_id("duplicate") == &first, "duplicate ID did not return first document-order node") ||
        !expect(right.find_by_id("duplicate") == &second, "context lookup escaped its subtree"))
    {
        panorama_enable_dom_structural_counters(false);
        return false;
    }
    panorama_reset_dom_structural_counters();
    if (!expect(root.find_by_id("duplicate") == &first, "indexed duplicate lookup changed target"))
    {
        panorama_enable_dom_structural_counters(false);
        return false;
    }
    PanoramaDomStructuralCounters counters = panorama_dom_structural_counters();
    if (!expect(counters.id_index_rebuild_nodes == 0 && counters.id_index_candidate_checks == 1,
            "steady ID lookup scaled with unrelated nodes"))
    {
        panorama_enable_dom_structural_counters(false);
        return false;
    }

    std::unique_ptr<PanoramaNode> moved = std::move(left.children.front());
    left.children.erase(left.children.begin());
    panorama_notify_tree_structure_changed(left);
    moved->parent = &right;
    right.children.push_back(std::move(moved));
    panorama_notify_tree_structure_changed(right);
    if (!expect(root.find_by_id("duplicate") == &second, "reparent did not update duplicate document order"))
    {
        panorama_enable_dom_structural_counters(false);
        return false;
    }

    panorama_notify_tree_structure_changed(right);
    right.children.erase(right.children.begin()); // remove `second`
    PanoramaNode* remaining = root.find_by_id("duplicate");
    if (!expect(remaining == &first, "removal did not reveal the remaining duplicate"))
    {
        panorama_enable_dom_structural_counters(false);
        return false;
    }
    first.set_id("renamed");
    const bool valid =
        expect(root.find_by_id("duplicate") == nullptr, "ID mutation left a stale old-ID candidate") &&
        expect(root.find_by_id("renamed") == &first, "ID mutation did not publish the new ID");
    panorama_enable_dom_structural_counters(false);
    return valid;
}

struct LifetimeCount
{
    std::size_t callbacks = 0;
};

void count_lifetime(void* context, panorama::PanoramaNode&)
{
    ++static_cast<LifetimeCount*>(context)->callbacks;
}

bool test_node_attached_teardown_scaling()
{
    using namespace panorama;
    PanoramaNode root;
    root.tag = "root";
    root.tag_lower = "root";
    constexpr std::size_t node_count = 1500;
    std::vector<PanoramaNode*> nodes;
    nodes.reserve(node_count);
    for (std::size_t index = 0; index < node_count; ++index)
    {
        nodes.push_back(&append_node(root, "lifetime-" + std::to_string(index), 0, 0, 0, 0));
    }

    LifetimeCount count;
    std::vector<PanoramaNodeLifetimeRegistration> registrations;
    registrations.reserve(node_count);
    for (PanoramaNode* node : nodes)
    {
        registrations.emplace_back(*node, &count_lifetime, &count);
    }

    panorama_reset_dom_structural_counters();
    panorama_enable_dom_structural_counters(true);
    panorama_notify_tree_structure_changed(root);
    root.children.clear();
    const PanoramaDomStructuralCounters counters = panorama_dom_structural_counters();
    panorama_enable_dom_structural_counters(false);
    return expect(count.callbacks == node_count, "attached lifetime callbacks were not delivered exactly once") &&
        expect(counters.lifetime_attached_callbacks == node_count,
            "teardown work was not proportional to attached registrations") &&
        expect(counters.lifetime_global_callbacks == 0,
            "node-attached teardown unexpectedly scanned global observers");
}
}

int main()
{
    if (!test_hit_test_pruning_and_parity() ||
        !test_deep_tree_parity() ||
        !test_document_id_index_mutations() ||
        !test_node_attached_teardown_scaling())
    {
        return 1;
    }
    std::puts("Panorama DOM scaling regressions passed");
    return 0;
}
