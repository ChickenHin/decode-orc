/*
 * File:        stage_details_test.cpp
 * Module:      orc-tests/core/unit
 * Purpose:     Unit tests for the shared stage and parameter description
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "stage_details.h"

#include <gtest/gtest.h>
#include <stage_ux_strings.h>

#include <algorithm>
#include <string>
#include <variant>
#include <vector>

#include "../../../../orc/core/include/project.h"
#include "filtergraph_parser.h"

namespace orc::presenters {
namespace {

StageInfo makeStage(const std::string& name, orc::NodeType type) {
  StageInfo stage;
  stage.name = name;
  stage.display_name = "Display " + name;
  stage.description = "Describes " + name;
  stage.node_type = type;
  stage.is_source = (type == orc::NodeType::SOURCE);
  stage.is_sink =
      (type == orc::NodeType::SINK || type == orc::NodeType::ANALYSIS_SINK);
  stage.is_core_plugin = true;
  stage.owning_plugin_id = "com.decode-orc." + name;
  return stage;
}

orc::ParameterDescriptor makeStringParameter(const std::string& name,
                                             const std::string& default_value) {
  orc::ParameterDescriptor descriptor;
  descriptor.name = name;
  descriptor.display_name = "Display " + name;
  descriptor.description = "Describes " + name;
  descriptor.type = orc::ParameterType::STRING;
  descriptor.constraints.default_value = default_value;
  return descriptor;
}

std::string valueOf(const std::vector<StageDetailField>& fields,
                    const std::string& label) {
  const auto it = std::find_if(
      fields.begin(), fields.end(),
      [&label](const StageDetailField& f) { return f.label == label; });
  return it == fields.end() ? std::string() : it->value;
}

bool hasLabel(const std::vector<StageDetailField>& fields,
              const std::string& label) {
  return std::any_of(
      fields.begin(), fields.end(),
      [&label](const StageDetailField& f) { return f.label == label; });
}

// --- Kinds ------------------------------------------------------------------

TEST(StageDetailsTest, KindLabelsMatchTheStageMenuCategories) {
  EXPECT_STREQ(stageKindLabel(makeStage("src", orc::NodeType::SOURCE)),
               "Source");
  EXPECT_STREQ(stageKindLabel(makeStage("xform", orc::NodeType::TRANSFORM)),
               "Transform");
  EXPECT_STREQ(stageKindLabel(makeStage("merge", orc::NodeType::MERGER)),
               "Transform");
  EXPECT_STREQ(stageKindLabel(makeStage("an", orc::NodeType::ANALYSIS_SINK)),
               "Analysis");
  EXPECT_STREQ(stageKindLabel(makeStage("sink", orc::NodeType::SINK)), "Sink");
}

TEST(StageDetailsTest, KindArgumentAcceptsEveryPrintedLabelLowercased) {
  const StageInfo stages[] = {
      makeStage("src", orc::NodeType::SOURCE),
      makeStage("xform", orc::NodeType::TRANSFORM),
      makeStage("an", orc::NodeType::ANALYSIS_SINK),
      makeStage("sink", orc::NodeType::SINK),
  };
  for (const auto& stage : stages) {
    orc::StageCategory kind{};
    ASSERT_TRUE(parseStageKind(stageKindId(stage), &kind)) << stage.name;
    EXPECT_TRUE(stageMatchesKind(stage, kind)) << stage.name;
  }
}

TEST(StageDetailsTest, KindArgumentAcceptsTheTriadsWordForTransforms) {
  orc::StageCategory kind{};
  ASSERT_TRUE(parseStageKind("filters", &kind));
  EXPECT_EQ(kind, orc::StageCategory::TRANSFORM);
  ASSERT_TRUE(parseStageKind("FILTER", &kind));
  EXPECT_EQ(kind, orc::StageCategory::TRANSFORM);
}

TEST(StageDetailsTest, UnknownKindIsRejected) {
  orc::StageCategory kind{};
  EXPECT_FALSE(parseStageKind("decoder", &kind));
  EXPECT_FALSE(parseStageKind("", &kind));
}

// --- Stage fields -----------------------------------------------------------

TEST(StageDetailsTest, StageNameLeadsAndDisplayNameIsASeparateField) {
  const auto stage = makeStage("tbc_source", orc::NodeType::SOURCE);
  const auto fields = makeStageDetails(stage);

  ASSERT_FALSE(fields.empty());
  EXPECT_EQ(fields.front().label, stage_ux::kFieldName);
  EXPECT_EQ(fields.front().value, "tbc_source");
  EXPECT_EQ(valueOf(fields, stage_ux::kFieldDisplayName), "Display tbc_source");
  EXPECT_EQ(valueOf(fields, stage_ux::kFieldKind), "Source");
  EXPECT_EQ(valueOf(fields, stage_ux::kFieldPlugin),
            "com.decode-orc.tbc_source");
}

TEST(StageDetailsTest, StageWithNoOwningPluginPrintsNoPluginField) {
  auto stage = makeStage("host_stage", orc::NodeType::TRANSFORM);
  stage.owning_plugin_id.clear();

  // A plugin field would have to hold a plugin selector; there is none, and a
  // placeholder is not a selector.
  EXPECT_FALSE(hasLabel(makeStageDetails(stage), stage_ux::kFieldPlugin));
}

// --- Parameter fields -------------------------------------------------------

TEST(StageDetailsTest, ParameterFieldsCarryTheDescriptorVerbatim) {
  orc::ParameterDescriptor descriptor;
  descriptor.name = "overcorrect_extension";
  descriptor.display_name = "Overcorrect Extension";
  descriptor.description = "How far to extend a correction";
  descriptor.type = orc::ParameterType::INT32;
  descriptor.constraints.required = true;
  descriptor.constraints.default_value = static_cast<int32_t>(4);
  descriptor.constraints.min_value = static_cast<int32_t>(0);
  descriptor.constraints.max_value = static_cast<int32_t>(16);

  const auto fields = makeStageParameterDetails("dropout_correct", descriptor);

  EXPECT_EQ(valueOf(fields, stage_ux::kParamFieldDisplayName),
            "Overcorrect Extension");
  EXPECT_EQ(valueOf(fields, stage_ux::kParamFieldDescription),
            "How far to extend a correction");
  EXPECT_EQ(valueOf(fields, stage_ux::kParamFieldType), "int32");
  EXPECT_EQ(valueOf(fields, stage_ux::kParamFieldRequired), "yes");
  EXPECT_EQ(valueOf(fields, stage_ux::kParamFieldDefault), "4");
  EXPECT_EQ(valueOf(fields, stage_ux::kParamFieldMinimum), "0");
  EXPECT_EQ(valueOf(fields, stage_ux::kParamFieldMaximum), "16");
}

TEST(StageDetailsTest, AllowedValuesLeadWithTheStoredValueNotTheLabel) {
  auto descriptor = makeStringParameter("output_mode", "raw");
  descriptor.constraints.allowed_strings = {
      "raw", std::string("ffmpeg") + stage_ux::kComboValueLabelSeparator +
                 "FFmpeg encoder"};

  const auto fields = makeStageParameterDetails("video_sink", descriptor);

  // The value is what has to be typed into a filtergraph; the label is only
  // what the GUI combo box shows.
  EXPECT_EQ(valueOf(fields, stage_ux::kParamFieldAllowed),
            "raw, ffmpeg (FFmpeg encoder)");
}

TEST(StageDetailsTest, DependencyNamesTheParameterAndItsEnablingValues) {
  auto descriptor = makeStringParameter("ffmpeg_format", "mp4");
  orc::ParameterDependency dependency;
  dependency.parameter_name = "output_mode";
  dependency.required_values = {"ffmpeg"};
  descriptor.constraints.depends_on = dependency;

  EXPECT_EQ(valueOf(makeStageParameterDetails("video_sink", descriptor),
                    stage_ux::kParamFieldDependsOn),
            "output_mode = ffmpeg");
}

TEST(StageDetailsTest, IndexedSpecDefaultIsShownOneBasedLikeTheGuiDialog) {
  // frame_map's "ranges" is stored 0-based and presented 1-based; the CLI must
  // not print a raw 0-based value where the GUI dialog prints a 1-based one.
  const auto descriptor = makeStringParameter("ranges", "0-9,20-29");
  const auto fields = makeStageParameterDetails("frame_map", descriptor);

  EXPECT_EQ(valueOf(fields, stage_ux::kParamFieldDefault), "1-10,21-30");
  EXPECT_TRUE(stageHasIndexedSpecParameter("frame_map", {descriptor}));
}

TEST(StageDetailsTest, PasteReadyFormsKeepTheStoredZeroBasedValue) {
  // The 1-based presentation is for people; a project file and a filtergraph
  // are read back by the loader and the parser, which expect stored values.
  const auto descriptor = makeStringParameter("ranges", "0-9");

  EXPECT_NE(makeStageParameterYaml({descriptor}).find("\"0-9\""),
            std::string::npos);
  EXPECT_NE(makeStageFiltergraphSpec("frame_map", {descriptor}).find("0-9"),
            std::string::npos);
}

TEST(StageDetailsTest, ParameterWithNoIndexedSpecNeedsNoNote) {
  EXPECT_FALSE(stageHasIndexedSpecParameter(
      "dropout_correct", {makeStringParameter("mode", "auto")}));
}

// --- Paste-ready forms ------------------------------------------------------

TEST(StageDetailsTest, YamlBlockIsShapedLikeAProjectFilesParameters) {
  orc::ParameterDescriptor number;
  number.name = "threshold";
  number.type = orc::ParameterType::DOUBLE;
  number.constraints.default_value = 1.5;

  orc::ParameterDescriptor flag;
  flag.name = "enabled";
  flag.type = orc::ParameterType::BOOL;
  flag.constraints.default_value = true;

  orc::ParameterDescriptor path;
  path.name = "input_path";
  path.type = orc::ParameterType::FILE_PATH;

  const std::string yaml = makeStageParameterYaml({number, flag, path});

  // Indented to the depth a node's parameters sit at in a written .orcprj,
  // so the block pastes under a node with no re-indenting.
  EXPECT_EQ(yaml,
            "      parameters:\n"
            "        threshold:\n"
            "          type: double\n"
            "          value: 1.500000\n"
            "        enabled:\n"
            "          type: bool\n"
            "          value: true\n"
            "        input_path:\n"
            // A project file has no file-path type; a path is stored as a
            // string, so that is what a paste-ready block must say.
            "          type: string\n"
            "          value: \"\"\n");
}

// The acceptance test for the paste claim: the emitted block, dropped verbatim
// under a node in a project document, loads through the real project reader
// with every default intact.
TEST(StageDetailsTest, YamlBlockLoadsUnmodifiedUnderAProjectNode) {
  orc::ParameterDescriptor number;
  number.name = "threshold";
  number.type = orc::ParameterType::DOUBLE;
  number.constraints.default_value = 1.5;

  orc::ParameterDescriptor flag;
  flag.name = "enabled";
  flag.type = orc::ParameterType::BOOL;
  flag.constraints.default_value = true;

  auto ranges = makeStringParameter("ranges", "0-9");

  const std::string yaml_block = makeStageParameterYaml({number, flag, ranges});

  const std::string document =
      "project:\n"
      "  name: paste-test\n"
      "  version: \"2.0\"\n"
      "  video_format: PAL\n"
      "  source_format: Composite\n"
      "  amplitude_unit: IRE\n"
      "dag:\n"
      "  nodes:\n"
      "    - id: 1\n"
      "      stage: frame_map\n"
      "      node_type: TRANSFORM\n"
      "      x: 0\n"
      "      y: 0\n" +
      yaml_block + "  edges: []\n";

  const auto project = orc::project_io::load_project_from_yaml(
      document, "/tmp/paste-test.orcprj");
  const auto& nodes = project.get_nodes();
  ASSERT_EQ(nodes.size(), 1u);
  const auto& parameters = nodes[0].parameters;
  ASSERT_EQ(parameters.size(), 3u);
  EXPECT_DOUBLE_EQ(std::get<double>(parameters.at("threshold")), 1.5);
  EXPECT_TRUE(std::get<bool>(parameters.at("enabled")));
  // Stored 0-based, exactly as emitted — the loader must see the stored form.
  EXPECT_EQ(std::get<std::string>(parameters.at("ranges")), "0-9");
}

TEST(StageDetailsTest, FiltergraphSpecParsesBackToTheSameParameters) {
  auto path = makeStringParameter("input_path", "/media/my captures/a.tbc");
  path.type = orc::ParameterType::FILE_PATH;
  auto ranges = makeStringParameter("ranges", "0-9,20-29");
  orc::ParameterDescriptor count;
  count.name = "count";
  count.type = orc::ParameterType::UINT32;
  count.constraints.default_value = static_cast<uint32_t>(3);

  const std::string spec =
      makeStageFiltergraphSpec("frame_map", {path, ranges, count});
  const auto parsed = parse_filtergraph(spec);

  ASSERT_TRUE(parsed.ok) << parsed.error;
  ASSERT_EQ(parsed.graph.stages.size(), 1u);
  EXPECT_EQ(parsed.graph.stages[0].stage_name, "frame_map");
  EXPECT_EQ(parsed.graph.stages[0].params.at("input_path"),
            "/media/my captures/a.tbc");
  EXPECT_EQ(parsed.graph.stages[0].params.at("ranges"), "0-9,20-29");
  EXPECT_EQ(parsed.graph.stages[0].params.at("count"), "3");
}

TEST(StageDetailsTest, StageWithNoParametersEmitsABareFiltergraphName) {
  EXPECT_EQ(makeStageFiltergraphSpec("dropout_correct", {}), "dropout_correct");
}

TEST(StageDetailsTest, StageWithNoParametersEmitsAnExplicitEmptyMapping) {
  // A bare `parameters:` key would paste into a project file as null.
  EXPECT_EQ(makeStageParameterYaml({}), "      parameters: {}\n");
}

// --- Near matches -----------------------------------------------------------

TEST(StageDetailsTest, NearMatchesRankContainmentAboveDistance) {
  const std::vector<StageInfo> stages = {
      makeStage("tbc_source", orc::NodeType::SOURCE),
      makeStage("cvbs_source", orc::NodeType::SOURCE),
      makeStage("video_sink", orc::NodeType::SINK),
  };

  const auto near = findNearbyStageNames("source", stages, 5);
  ASSERT_GE(near.size(), 2u);
  EXPECT_TRUE(std::find(near.begin(), near.end(), "tbc_source") != near.end());
  EXPECT_TRUE(std::find(near.begin(), near.end(), "cvbs_source") != near.end());
}

TEST(StageDetailsTest, NearMatchesCatchATypoAndRespectTheResultLimit) {
  const std::vector<StageInfo> stages = {
      makeStage("tbc_source", orc::NodeType::SOURCE),
      makeStage("cvbs_source", orc::NodeType::SOURCE),
      makeStage("video_sink", orc::NodeType::SINK),
  };

  const auto typo = findNearbyStageNames("tbc_sourc", stages, 5);
  ASSERT_FALSE(typo.empty());
  EXPECT_EQ(typo.front(), "tbc_source");

  EXPECT_LE(findNearbyStageNames("source", stages, 1).size(), 1u);
}

TEST(StageDetailsTest, NothingIsSuggestedForAnUnrelatedName) {
  const std::vector<StageInfo> stages = {
      makeStage("tbc_source", orc::NodeType::SOURCE),
      makeStage("video_sink", orc::NodeType::SINK),
  };

  EXPECT_TRUE(findNearbyStageNames("zzzzzzzzzzzz", stages, 5).empty());
}

}  // namespace
}  // namespace orc::presenters
