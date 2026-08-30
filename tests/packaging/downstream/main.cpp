#include "psx/pipeline/planner.hpp"

#include <string>
#include <vector>

int main() {
    psx::pipeline::Pipeline pipeline{
        .stages = {psx::pipeline::Stage{.id = "consumer", .argv = {"true"}, .placement = ""}},
        .edges = {},
    };
    auto planned = psx::pipeline::Planner::plan(pipeline);
    return planned.ok() && planned.value().order == std::vector<std::string>{"consumer"} ? 0 : 1;
}
