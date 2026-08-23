#pragma once

#include "psx/pipeline/pipeline.hpp"
#include "psx/result.hpp"

#include <string_view>

namespace psx::pipeline {

// Parses a `pipe` command spec into a linear pipeline. A spec is a series of
// stages joined by top-level `|` (a `|` inside a single-quoted command does not
// split). Each stage is a single-quoted command (which may contain spaces) or a
// single bare token, with an optional `@placement` suffix naming an inventory
// host/group. Stages are named s0, s1, … and each feeds the next.
//
// Examples:
//   'grep ERROR'@web | 'sort -u'@db
//   ps@host1 | wc
//
// Returns psx::ErrorClass::InvalidArgument (with a specific message) on a
// malformed spec: empty spec/stage, unterminated quote, junk after a quoted
// command, an unquoted command containing spaces, or an empty placement.
psx::Result<Pipeline> parsePipeSpec(std::string_view spec);

} // namespace psx::pipeline
