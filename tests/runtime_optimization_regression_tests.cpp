#include "ui/panorama/panorama_dom.hpp"
#include "ui/panorama/panorama_input.hpp"
#include "ui/panorama/panorama_log.hpp"
#include "ui/panorama/panorama_resource_provider.hpp"
#include "ui/panorama/panorama_runtime.hpp"

#include <cstdio>
#include <format>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace regression
{
int formatting_calls = 0;

struct CountingFormatArgument
{
    int value = 0;
};
}

template <>
struct std::formatter<regression::CountingFormatArgument, char> : std::formatter<int, char>
{
    template <class FormatContext>
    auto format(const regression::CountingFormatArgument& value, FormatContext& context) const
    {
        ++regression::formatting_calls;
        return std::formatter<int, char>::format(value.value, context);
    }
};

namespace
{
bool expect(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::fprintf(
            stderr,
            "runtime optimization regression failed: %.*s\n",
            static_cast<int>(message.size()),
            message.data());
    }
    return condition;
}

panorama::PanoramaNode& append_node(
    panorama::PanoramaNode& parent,
    std::string id,
    panorama::PanoramaLayoutBox layout)
{
    auto child = std::make_unique<panorama::PanoramaNode>();
    child->tag = "Panel";
    child->tag_lower = "panel";
    child->id = std::move(id);
    child->layout = layout;
    child->parent = &parent;
    panorama::PanoramaNode* result = child.get();
    parent.children.push_back(std::move(child));
    return *result;
}

void clear_style_dirt(panorama::PanoramaNode& node)
{
    node.style_dirty = false;
    node.descendant_style_dirty = false;
    for (const auto& child : node.children)
    {
        clear_style_dirt(*child);
    }
}

bool initialize_runtime(
    panorama::PanoramaRuntime& runtime,
    panorama::PanoramaNode& root,
    std::vector<std::string>& actions)
{
    runtime.set_host_action_handler(
        [&](const std::string& action, const std::string& argument) {
            if (action == "record")
            {
                actions.push_back(argument);
            }
        });
    panorama::PanoramaResourceManager resources;
    return runtime.initialize(root, resources, std::vector<std::string>{});
}

bool expect_actions(
    const std::vector<std::string>& actual,
    std::initializer_list<std::string_view> expected,
    std::string_view message)
{
    if (actual.size() != expected.size())
    {
        return expect(false, message);
    }
    std::size_t index = 0;
    for (std::string_view value : expected)
    {
        if (actual[index] != value)
        {
            return expect(false, message);
        }
        ++index;
    }
    return true;
}

bool test_hover_exclusive_suffix_and_transition_order()
{
    using namespace panorama;

    PanoramaNode root;
    root.tag = "Panel";
    root.tag_lower = "panel";
    root.id = "root";
    root.layout = {0.0F, 0.0F, 200.0F, 100.0F};

    PanoramaNode& common = append_node(root, "common", root.layout);
    PanoramaNode& left_branch = append_node(common, "left-branch", {0.0F, 0.0F, 100.0F, 100.0F});
    PanoramaNode& left_leaf = append_node(left_branch, "left-leaf", left_branch.layout);
    PanoramaNode& right_branch = append_node(common, "right-branch", {100.0F, 0.0F, 100.0F, 100.0F});
    PanoramaNode& right_leaf = append_node(right_branch, "right-leaf", right_branch.layout);

    left_leaf.attributes["hittest"] = "true";
    right_leaf.attributes["hittest"] = "true";
    left_leaf.attributes["onmouseout"] = "$.__host('record', 'left-leaf-out')";
    left_branch.attributes["onmouseout"] = "$.__host('record', 'left-branch-out')";
    right_branch.attributes["onmouseover"] = "$.__host('record', 'right-branch-over')";
    right_leaf.attributes["onmouseover"] = "$.__host('record', 'right-leaf-over')";

    std::vector<std::string> actions;
    PanoramaRuntime runtime;
    if (!expect(initialize_runtime(runtime, root, actions), "could not initialize hover test runtime"))
    {
        return false;
    }

    PanoramaInputController input;
    if (!expect(input.update_pointer(root, 25.0F, 50.0F, false, &runtime), "initial hover did not change state"))
    {
        return false;
    }
    actions.clear();
    clear_style_dirt(root);

    if (!expect(input.update_pointer(root, 175.0F, 50.0F, false, &runtime), "sibling hover did not change state") ||
        !expect(!left_leaf.hovered && !left_branch.hovered, "left exclusive hover suffix was not cleared") ||
        !expect(right_leaf.hovered && right_branch.hovered, "right exclusive hover suffix was not set") ||
        !expect(common.hovered && root.hovered, "shared hover ancestry was cleared") ||
        !expect(left_leaf.style_dirty && left_branch.style_dirty, "left exclusive suffix was not dirtied") ||
        !expect(right_leaf.style_dirty && right_branch.style_dirty, "right exclusive suffix was not dirtied") ||
        !expect(!common.style_dirty && !root.style_dirty, "shared hover ancestry was redundantly dirtied") ||
        !expect(common.descendant_style_dirty && root.descendant_style_dirty,
            "shared ancestry did not retain descendant-dirt routing") ||
        !expect_actions(
            actions,
            {"left-leaf-out", "left-branch-out", "right-branch-over", "right-leaf-over"},
            "pointer transition handler order changed"))
    {
        return false;
    }

    if (!expect(input.hover_node() == &right_leaf, "hover target did not become the right leaf"))
    {
        return false;
    }

    // Transition watches must null a not-yet-entered target if an earlier
    // mouseout handler deletes it.
    actions.clear();
    right_leaf.attributes["onmouseout"] =
        "$.__host('record', 'delete-entered'); $('#left-branch').RemoveAndDeleteChildren()";
    left_branch.attributes["onmouseover"] = "$.__host('record', 'left-branch-over')";
    left_leaf.attributes["onmouseover"] = "$.__host('record', 'deleted-leaf-over')";
    if (!expect(input.update_pointer(root, 25.0F, 50.0F, false, &runtime),
            "deleting transition did not report a state change") ||
        !expect(left_branch.children.empty(), "mouseout handler did not delete the entering leaf") ||
        !expect(input.hover_node() == nullptr, "destroyed entering target remained the hover pointer") ||
        !expect_actions(
            actions,
            {"delete-entered", "left-branch-over"},
            "pointer transition deletion watch did not skip the destroyed target"))
    {
        return false;
    }
    return true;
}

bool test_timer_order_cancel_and_reschedule()
{
    using namespace panorama;

    PanoramaNode root;
    root.tag = "Panel";
    root.tag_lower = "panel";
    std::vector<std::string> actions;
    PanoramaRuntime runtime;
    if (!expect(initialize_runtime(runtime, root, actions), "could not initialize timer test runtime") ||
        !expect(runtime.work_budget().scheduled_callbacks_per_update == 0 &&
                runtime.work_budget().pending_jobs_per_pump == 0,
            "runtime compatibility budget was not drain-to-empty"))
    {
        return false;
    }

    runtime.run_source_in_context(
        R"(
            const cancelled = $.Schedule(0.1, () => $.__host('record', 'cancelled'));
            $.Schedule(0.2, () => $.__host('record', 'late'));
            $.Schedule(0.1, () => $.__host('record', 'same-1'));
            $.Schedule(0.1, () => $.__host('record', 'same-2'));
            $.CancelScheduled(cancelled);
            $.Schedule(-1.0, () => $.__host('record', 'negative'));
            $.Schedule(0.0, () => $.__host('record', 'zero'));
        )",
        "timer-order-test",
        root);
    runtime.update(0.1);
    if (!expect_actions(
            actions,
            {"negative", "zero", "same-1", "same-2"},
            "deadline/insertion ordering or cancellation changed"))
    {
        return false;
    }
    runtime.update(0.1);
    if (!expect_actions(
            actions,
            {"negative", "zero", "same-1", "same-2", "late"},
            "later deadline did not fire on time"))
    {
        return false;
    }

    actions.clear();
    runtime.run_source_in_context(
        R"(
            $.Schedule(0.0, () => {
                $.__host('record', 'parent');
                $.Schedule(0.0, () => $.__host('record', 'child'));
            });
        )",
        "timer-reschedule-test",
        root);
    runtime.update(0.0);
    if (!expect_actions(actions, {"parent"}, "same-pump reschedule did not retain snapshot semantics"))
    {
        return false;
    }
    runtime.update(0.0);
    return expect_actions(actions, {"parent", "child"}, "zero-delay reschedule did not run on the next update");
}

bool test_timer_budget_fairness_and_compatibility()
{
    using namespace panorama;

    PanoramaNode root;
    root.tag = "Panel";
    root.tag_lower = "panel";
    std::vector<std::string> actions;
    PanoramaRuntime runtime;
    if (!expect(initialize_runtime(runtime, root, actions), "could not initialize timer budget runtime"))
    {
        return false;
    }

    runtime.set_work_budget(PanoramaRuntimeWorkBudget{
        .scheduled_callbacks_per_update = 2,
        .pending_jobs_per_pump = 0,
    });
    runtime.run_source_in_context(
        R"(
            for (let i = 0; i != 5; ++i) {
                $.Schedule(0.0, () => $.__host('record', 'bounded-' + i));
            }
        )",
        "timer-budget-test",
        root);
    runtime.update(0.0);
    if (!expect_actions(actions, {"bounded-0", "bounded-1"}, "first timer budget slice was not stable"))
    {
        return false;
    }
    runtime.update(0.0);
    if (!expect_actions(
            actions,
            {"bounded-0", "bounded-1", "bounded-2", "bounded-3"},
            "second timer budget slice was not fair"))
    {
        return false;
    }
    runtime.update(0.0);
    if (!expect_actions(
            actions,
            {"bounded-0", "bounded-1", "bounded-2", "bounded-3", "bounded-4"},
            "timer budget did not retain the final callback"))
    {
        return false;
    }

    actions.clear();
    runtime.set_work_budget({});
    runtime.run_source_in_context(
        R"(
            for (let i = 0; i != 4; ++i) {
                $.Schedule(0.0, () => $.__host('record', 'compat-' + i));
            }
        )",
        "timer-compatibility-test",
        root);
    runtime.update(0.0);
    return expect_actions(
        actions,
        {"compat-0", "compat-1", "compat-2", "compat-3"},
        "compatibility timer mode did not drain all ready callbacks");
}

bool test_quickjs_job_budget_and_compatibility()
{
    using namespace panorama;

    PanoramaNode bounded_root;
    bounded_root.tag = "Panel";
    bounded_root.tag_lower = "panel";
    std::vector<std::string> bounded_actions;
    PanoramaRuntime bounded;
    bounded.set_work_budget(PanoramaRuntimeWorkBudget{
        .scheduled_callbacks_per_update = 0,
        .pending_jobs_per_pump = 1,
    });
    if (!expect(initialize_runtime(bounded, bounded_root, bounded_actions), "could not initialize bounded job runtime"))
    {
        return false;
    }
    bounded.run_source_in_context(
        R"(
            Promise.resolve()
                .then(() => $.__host('record', 'job-1'))
                .then(() => $.__host('record', 'job-2'))
                .then(() => $.__host('record', 'job-3'));
        )",
        "job-budget-test",
        bounded_root);
    if (!expect_actions(bounded_actions, {"job-1"}, "bounded source pump executed too many jobs"))
    {
        return false;
    }
    bounded.update(0.0);
    if (!expect_actions(bounded_actions, {"job-1", "job-2"}, "bounded update did not continue the job queue fairly"))
    {
        return false;
    }
    bounded.update(0.0);
    if (!expect_actions(
            bounded_actions,
            {"job-1", "job-2", "job-3"},
            "bounded job queue did not retain the final continuation"))
    {
        return false;
    }

    PanoramaNode compatibility_root;
    compatibility_root.tag = "Panel";
    compatibility_root.tag_lower = "panel";
    std::vector<std::string> compatibility_actions;
    PanoramaRuntime compatibility;
    if (!expect(
            initialize_runtime(compatibility, compatibility_root, compatibility_actions),
            "could not initialize compatibility job runtime"))
    {
        return false;
    }
    compatibility.run_source_in_context(
        R"(
            Promise.resolve()
                .then(() => $.__host('record', 'compat-job-1'))
                .then(() => $.__host('record', 'compat-job-2'))
                .then(() => $.__host('record', 'compat-job-3'));
        )",
        "job-compatibility-test",
        compatibility_root);
    return expect_actions(
        compatibility_actions,
        {"compat-job-1", "compat-job-2", "compat-job-3"},
        "compatibility job mode did not drain to empty");
}

bool test_disabled_log_formatting_and_dispatch()
{
    using namespace panorama;

    std::vector<std::pair<PanoramaLogLevel, std::string>> messages;
    set_panorama_log_sink(
        [&](PanoramaLogLevel level, std::string_view message) {
            messages.emplace_back(level, message);
        });

    regression::formatting_calls = 0;
    set_panorama_log_level(PanoramaLogLevel::Warning);
    pano_log_info("disabled {}", regression::CountingFormatArgument{7});
    panorama_log_emit(PanoramaLogLevel::Info, "already formatted");
    bool passed =
        expect(!panorama_log_enabled(PanoramaLogLevel::Info), "info level was not disabled") &&
        expect(panorama_log_enabled(PanoramaLogLevel::Warning), "warning level was unexpectedly disabled") &&
        expect(regression::formatting_calls == 0, "disabled info log formatted its argument") &&
        expect(messages.empty(), "disabled info log reached the sink");

    pano_log_warning("warning {}", regression::CountingFormatArgument{9});
    passed =
        expect(regression::formatting_calls == 1, "warning formatting behavior changed") &&
        expect(messages.size() == 1 && messages[0].first == PanoramaLogLevel::Warning &&
                messages[0].second == "warning 9",
            "warning dispatch behavior changed") &&
        passed;

    set_panorama_log_level(PanoramaLogLevel::Info);
    pano_log_info("enabled {}", regression::CountingFormatArgument{11});
    passed =
        expect(regression::formatting_calls == 2, "enabled info log was not formatted") &&
        expect(messages.size() == 2 && messages[1].first == PanoramaLogLevel::Info &&
                messages[1].second == "enabled 11",
            "enabled info log did not reach the sink") &&
        passed;

    set_panorama_log_sink({});
    return passed;
}
}

int main()
{
    if (!test_hover_exclusive_suffix_and_transition_order() ||
        !test_timer_order_cancel_and_reschedule() ||
        !test_timer_budget_fairness_and_compatibility() ||
        !test_quickjs_job_budget_and_compatibility() ||
        !test_disabled_log_formatting_and_dispatch())
    {
        return 1;
    }

    std::puts("Panorama runtime optimization regressions passed");
    return 0;
}
