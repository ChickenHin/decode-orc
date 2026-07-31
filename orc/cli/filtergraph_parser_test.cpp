/*
 * File:        filtergraph_parser_test.cpp
 * Module:      orc-cli-tests
 * Purpose:     Unit tests for the ffmpeg-style filtergraph parser.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "filtergraph_parser.h"

#include <gtest/gtest.h>
#include <orc/stage/params/parameter_types.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "stage_details.h"

namespace {

using orc::presenters::FilterGraph;
using orc::presenters::makeStageFiltergraphSpec;
using orc::presenters::parse_filtergraph;

bool HasEdge(const FilterGraph& g, std::size_t from, std::size_t to) {
  for (const auto& e : g.edges) {
    if (e.first == from && e.second == to) {
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Happy-path parsing
// ---------------------------------------------------------------------------

TEST(FiltergraphParser, SimpleAutoChain) {
  auto r = parse_filtergraph(
      "NTSC_CVBS_Source=c_path=/a/x.c:y_path=/a/x.y, video_sink");
  ASSERT_TRUE(r.ok) << r.error;
  ASSERT_EQ(r.graph.stages.size(), 2u);
  EXPECT_EQ(r.graph.stages[0].stage_name, "NTSC_CVBS_Source");
  EXPECT_EQ(r.graph.stages[0].params.at("c_path"), "/a/x.c");
  EXPECT_EQ(r.graph.stages[0].params.at("y_path"), "/a/x.y");
  EXPECT_EQ(r.graph.stages[1].stage_name, "video_sink");
  ASSERT_EQ(r.graph.edges.size(), 1u);
  EXPECT_TRUE(HasEdge(r.graph, 0, 1));
}

TEST(FiltergraphParser, ExplicitLabelsAcrossChains) {
  auto r = parse_filtergraph(
      "tbc_source=c_path=/a.tbcc:y_path=/a.tbcy [v]; [v] video_sink");
  ASSERT_TRUE(r.ok) << r.error;
  ASSERT_EQ(r.graph.stages.size(), 2u);
  ASSERT_EQ(r.graph.stages[0].output_labels.size(), 1u);
  EXPECT_EQ(r.graph.stages[0].output_labels[0], "v");
  ASSERT_EQ(r.graph.stages[1].input_labels.size(), 1u);
  EXPECT_EQ(r.graph.stages[1].input_labels[0], "v");
  ASSERT_EQ(r.graph.edges.size(), 1u);
  EXPECT_TRUE(HasEdge(r.graph, 0, 1));
}

TEST(FiltergraphParser, MultiInputNode) {
  auto r = parse_filtergraph(
      "tbc_source=c_path=/a.tbcc[a]; tbc_source=c_path=/b.tbcc[b]; "
      "[a][b] stacker [s]; [s] video_sink");
  ASSERT_TRUE(r.ok) << r.error;
  ASSERT_EQ(r.graph.stages.size(), 4u);
  EXPECT_EQ(r.graph.stages[2].stage_name, "stacker");
  EXPECT_EQ(r.graph.stages[2].input_labels.size(), 2u);
  ASSERT_EQ(r.graph.edges.size(), 3u);
  EXPECT_TRUE(HasEdge(r.graph, 0, 2));
  EXPECT_TRUE(HasEdge(r.graph, 1, 2));
  EXPECT_TRUE(HasEdge(r.graph, 2, 3));
}

TEST(FiltergraphParser, QuotedValueWithSpecialChars) {
  auto r =
      parse_filtergraph("src=path='/media/My Videos/a:b,c.tbc':gain=1.5, sink");
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(r.graph.stages[0].params.at("path"), "/media/My Videos/a:b,c.tbc");
  EXPECT_EQ(r.graph.stages[0].params.at("gain"), "1.5");
}

TEST(FiltergraphParser, BackslashEscaping) {
  auto r = parse_filtergraph("src=path=/a\\:b\\,c:k=v, sink");
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(r.graph.stages[0].params.at("path"), "/a:b,c");
  EXPECT_EQ(r.graph.stages[0].params.at("k"), "v");
}

TEST(FiltergraphParser, ValueContainingEquals) {
  auto r = parse_filtergraph("src=query=a=b=c, sink");
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(r.graph.stages[0].params.at("query"), "a=b=c");
}

TEST(FiltergraphParser, StageWithoutArgs) {
  auto r = parse_filtergraph("dropout_correct, video_sink");
  ASSERT_TRUE(r.ok) << r.error;
  ASSERT_EQ(r.graph.stages.size(), 2u);
  EXPECT_TRUE(r.graph.stages[0].params.empty());
  ASSERT_EQ(r.graph.edges.size(), 1u);
  EXPECT_TRUE(HasEdge(r.graph, 0, 1));
}

TEST(FiltergraphParser, FanOutSharedLabel) {
  auto r = parse_filtergraph("src [x]; [x] sinkA; [x] sinkB");
  ASSERT_TRUE(r.ok) << r.error;
  ASSERT_EQ(r.graph.edges.size(), 2u);
  EXPECT_TRUE(HasEdge(r.graph, 0, 1));
  EXPECT_TRUE(HasEdge(r.graph, 0, 2));
}

TEST(FiltergraphParser, ThreeStageChain) {
  auto r = parse_filtergraph("a, b, c");
  ASSERT_TRUE(r.ok) << r.error;
  ASSERT_EQ(r.graph.edges.size(), 2u);
  EXPECT_TRUE(HasEdge(r.graph, 0, 1));
  EXPECT_TRUE(HasEdge(r.graph, 1, 2));
}

// ---------------------------------------------------------------------------
// Windows path handling
// ---------------------------------------------------------------------------
//
// Windows absolute paths combine two characters that also have syntactic
// meaning in this grammar: ':' after a drive letter, and '\' as a path
// separator (which would otherwise be interpreted as an escape character).
// These tests lock in that unquoted Windows paths survive intact.

TEST(FiltergraphParser, UnquotedWindowsPathWithDriveLetter) {
  auto r = parse_filtergraph(
      "tbc_source=input_path=C:\\Users\\superfire\\Documents\\file.tbc, sink");
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(r.graph.stages[0].params.at("input_path"),
            "C:\\Users\\superfire\\Documents\\file.tbc");
}

TEST(FiltergraphParser, TwoWindowsPathsInOneStage) {
  auto r = parse_filtergraph(
      "dropout_correct=input_path=C:\\in\\a.tbc:output_path=D:\\out\\b.rgb, "
      "sink");
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(r.graph.stages[0].params.at("input_path"), "C:\\in\\a.tbc");
  EXPECT_EQ(r.graph.stages[0].params.at("output_path"), "D:\\out\\b.rgb");
}

TEST(FiltergraphParser, RelativeWindowsStylePathNoDriveLetter) {
  // The common case: no drive letter, just a bare filename (no colon at all).
  auto r = parse_filtergraph(
      "tbc_source=input_path=Santana_smooth_CLV_NTSC.tbc,"
      "dropout_correct=output_path=Santana_smooth_CLV_NTSC.rgb");
  ASSERT_TRUE(r.ok) << r.error;
  ASSERT_EQ(r.graph.stages.size(), 2u);
  ASSERT_EQ(r.graph.edges.size(), 1u);
  EXPECT_TRUE(HasEdge(r.graph, 0, 1));
}

TEST(FiltergraphParser, QuotedWindowsPathStillWorks) {
  auto r = parse_filtergraph("src=path='C:\\Users\\a b\\c.tbc', sink");
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(r.graph.stages[0].params.at("path"), "C:\\Users\\a b\\c.tbc");
}

TEST(FiltergraphParser, DoubledBackslashStillCollapsesToOne) {
  auto r = parse_filtergraph("src=path=C:\\\\Users\\\\x.tbc, sink");
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(r.graph.stages[0].params.at("path"), "C:\\Users\\x.tbc");
}

TEST(FiltergraphParser, RealSeparatorColonStillSplitsNormally) {
  auto r = parse_filtergraph("src=a=1:b=2, sink");
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(r.graph.stages[0].params.at("a"), "1");
  EXPECT_EQ(r.graph.stages[0].params.at("b"), "2");
}

TEST(FiltergraphParser, EscapedLiteralColonStillSupported) {
  auto r = parse_filtergraph("src=label=ratio\\:16x9, sink");
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(r.graph.stages[0].params.at("label"), "ratio:16x9");
}

TEST(FiltergraphParser, UncPathNotBrokenByBackslashHandling) {
  auto r = parse_filtergraph("src=path=\\\\server\\share\\file.tbc, sink");
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(r.graph.stages[0].params.at("path"), "\\server\\share\\file.tbc");
}

TEST(FiltergraphParser, ForwardSlashDrivePathAlsoProtected) {
  auto r = parse_filtergraph("src=path=C:/Users/x/file.tbc, sink");
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(r.graph.stages[0].params.at("path"), "C:/Users/x/file.tbc");
}

// ---------------------------------------------------------------------------
// Double-quote support
// ---------------------------------------------------------------------------
//
// Single quotes and double quotes are fully interchangeable value-quoting
// styles: whichever one is used, the OTHER quote character may appear
// literally inside it without ending the quoted span.

TEST(FiltergraphParser, DoubleQuotedWindowsPath) {
  auto r = parse_filtergraph(
      "src=path=\"C:\\Users\\superfire\\Documents\\file.tbc\", sink");
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(r.graph.stages[0].params.at("path"),
            "C:\\Users\\superfire\\Documents\\file.tbc");
}

TEST(FiltergraphParser, DoubleQuotedValueWithSpecialChars) {
  auto r = parse_filtergraph(
      "src=path=\"/media/My Videos/a:b,c.tbc\":gain=1.5, sink");
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(r.graph.stages[0].params.at("path"), "/media/My Videos/a:b,c.tbc");
  EXPECT_EQ(r.graph.stages[0].params.at("gain"), "1.5");
}

TEST(FiltergraphParser, DoubleQuotedValueContainingSingleQuote) {
  auto r = parse_filtergraph("src=label=\"it's a test: ok\", sink");
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(r.graph.stages[0].params.at("label"), "it's a test: ok");
}

TEST(FiltergraphParser, SingleQuotedValueContainingDoubleQuote) {
  auto r = parse_filtergraph("src=label='say \"hi\": ok', sink");
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(r.graph.stages[0].params.at("label"), "say \"hi\": ok");
}

TEST(FiltergraphParser, EscapedLiteralDoubleQuoteOutsideQuoting) {
  auto r = parse_filtergraph("src=label=say \\\"hi\\\", sink");
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(r.graph.stages[0].params.at("label"), "say \"hi\"");
}

TEST(FiltergraphParser, RejectsUnterminatedDoubleQuote) {
  EXPECT_FALSE(parse_filtergraph("src=path=\"/unterminated, sink").ok);
}

TEST(FiltergraphParser, DoubleQuotedTwoWindowsPathsInOneStage) {
  auto r = parse_filtergraph(
      "dropout_correct=input_path=\"C:\\in\\a.tbc\":"
      "output_path=\"D:\\out\\b.rgb\", sink");
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_EQ(r.graph.stages[0].params.at("input_path"), "C:\\in\\a.tbc");
  EXPECT_EQ(r.graph.stages[0].params.at("output_path"), "D:\\out\\b.rgb");
}

// ---------------------------------------------------------------------------
// Round trip with `stages info --filtergraph`
// ---------------------------------------------------------------------------
//
// What `stages info --filtergraph` prints has to run unmodified through
// --source/--filters/--sink, so the emitter is pinned against the parser here
// rather than trusted to keep its own quoting rules straight.

orc::ParameterDescriptor MakeStringParam(const std::string& name,
                                         const std::string& default_value) {
  orc::ParameterDescriptor descriptor;
  descriptor.name = name;
  descriptor.type = orc::ParameterType::STRING;
  descriptor.constraints.default_value = default_value;
  return descriptor;
}

TEST(FiltergraphParser, ParsesAStageSpecEmittedByStagesInfo) {
  const std::vector<orc::ParameterDescriptor> descriptors = {
      MakeStringParam("input_path", "/media/my captures/a.tbc"),
      MakeStringParam("ranges", "0-9,20-29"),
      MakeStringParam("label", "he said \"go\""),
      MakeStringParam("empty", ""),
  };

  const std::string spec = makeStageFiltergraphSpec("frame_map", descriptors);
  auto r = parse_filtergraph(spec);

  ASSERT_TRUE(r.ok) << r.error << " for: " << spec;
  ASSERT_EQ(r.graph.stages.size(), 1u);
  EXPECT_EQ(r.graph.stages[0].stage_name, "frame_map");
  EXPECT_EQ(r.graph.stages[0].params.at("input_path"),
            "/media/my captures/a.tbc");
  EXPECT_EQ(r.graph.stages[0].params.at("ranges"), "0-9,20-29");
  EXPECT_EQ(r.graph.stages[0].params.at("label"), "he said \"go\"");
  EXPECT_EQ(r.graph.stages[0].params.at("empty"), "");
}

TEST(FiltergraphParser, ParsesAnEmittedSpecChainedWithAnother) {
  const std::string source = makeStageFiltergraphSpec(
      "tbc_source", {MakeStringParam("input_path", "/a/b c.tbc")});
  auto r = parse_filtergraph(source + ", video_sink");

  ASSERT_TRUE(r.ok) << r.error;
  ASSERT_EQ(r.graph.stages.size(), 2u);
  EXPECT_EQ(r.graph.stages[0].params.at("input_path"), "/a/b c.tbc");
  EXPECT_EQ(r.graph.stages[1].stage_name, "video_sink");
  EXPECT_TRUE(HasEdge(r.graph, 0, 1));
}

// ---------------------------------------------------------------------------
// Error handling
// ---------------------------------------------------------------------------

TEST(FiltergraphParser, RejectsEmptyGraph) {
  EXPECT_FALSE(parse_filtergraph("").ok);
  EXPECT_FALSE(parse_filtergraph("   ").ok);
}

TEST(FiltergraphParser, RejectsParameterWithoutValue) {
  EXPECT_FALSE(parse_filtergraph("src=badparam:k=v, sink").ok);
}

TEST(FiltergraphParser, RejectsEmptyFilter) {
  EXPECT_FALSE(parse_filtergraph("src, , sink").ok);
}

TEST(FiltergraphParser, RejectsUnterminatedQuote) {
  EXPECT_FALSE(parse_filtergraph("src=path='/unterminated, sink").ok);
}

TEST(FiltergraphParser, RejectsUnterminatedLabel) {
  EXPECT_FALSE(parse_filtergraph("[a] src [b").ok);
}

TEST(FiltergraphParser, RejectsTextAfterOutputLabel) {
  EXPECT_FALSE(parse_filtergraph("src=k=v [out] extra, sink").ok);
}

TEST(FiltergraphParser, RejectsLoneSeparators) {
  EXPECT_FALSE(parse_filtergraph(";").ok);
  EXPECT_FALSE(parse_filtergraph(",").ok);
}

}  // namespace
