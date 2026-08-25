/*
 * File:        source_join_stage.h
 * Module:      orc-stage-plugin-source-join
 * Purpose:     Join several sources end to end into a single output sequence
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/plugin/orc_stage_preview.h>
#include <orc/plugin/orc_stage_runtime.h>
#include <orc/stage/frame_descriptor.h>
#include <orc/stage/frame_id.h>
#include <orc/stage/node_id.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/video_frame_representation.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace orc {

// ============================================================================
// JoinedVideoFrameRepresentation
// ============================================================================
// Presents several input representations end to end as one continuous frame
// sequence, without copying sample data.  Output frame IDs are renumbered from
// zero across the whole join; each one resolves to (input, source frame ID)
// through a lookup table built at stage execute() time.
//
// Every input must describe the same signal geometry — the stage checks that
// before constructing this — so the joined representation's video parameters,
// YC-ness and audio channel-pair layout are those of the first joined input.
//
// Thread safety: all const methods may be called concurrently; the object owns
// no mutable state.
class JoinedVideoFrameRepresentation : public VideoFrameRepresentationWrapper,
                                       public Artifact {
 public:
  // One output frame: which joined input it came from, and its frame ID there.
  struct Entry {
    uint32_t source_index;
    FrameID source_frame;
  };

  JoinedVideoFrameRepresentation(
      std::vector<std::shared_ptr<const VideoFrameRepresentation>> sources,
      std::vector<Entry> mapping, const std::string& tag);

  ~JoinedVideoFrameRepresentation() override = default;

  std::string type_name() const override {
    return "joined_video_frame_representation";
  }

  // Navigation
  FrameIDRange frame_range() const override;
  size_t frame_count() const override { return mapping_.size(); }
  bool has_frame(FrameID id) const override;
  std::optional<FrameDescriptor> get_frame_descriptor(
      FrameID id) const override;

  // Flat access — delegates to the joined input via the mapped frame ID
  const sample_type* get_frame(FrameID id) const override;
  const sample_type* get_line(FrameID id, size_t line) const override;
  std::vector<sample_type> get_frame_copy(FrameID id) const override;
  std::vector<sample_type> get_line_samples(FrameID id,
                                            size_t line) const override;

  // YC
  const sample_type* get_frame_luma(FrameID id) const override;
  const sample_type* get_frame_chroma(FrameID id) const override;
  const sample_type* get_line_luma(FrameID id, size_t line) const override;
  const sample_type* get_line_chroma(FrameID id, size_t line) const override;

  // Hints
  std::vector<DropoutRun> get_dropout_hints(FrameID id) const override;

  // Sample data is passed through untouched, so an output frame that keeps its
  // source frame's ID is byte-identical to it and the host can reuse the
  // upstream frame's stored analysis. Frames the join renumbered answer
  // nullptr: the hook is keyed on the frame ID, not on content.
  std::shared_ptr<const VideoFrameRepresentation> video_passthrough_source(
      FrameID id) const override;

  // Audio — pair count and descriptors come from the first joined input (the
  // wrapper base forwards them). Samples come from the input that supplies the
  // output frame; an input that carries fewer channel pairs contributes
  // silence for the pairs it lacks. Every output frame serves exactly
  // audio_pairs_in_frame() stereo pairs, so a join that lands a source frame
  // at an output index of a different NTSC/PAL-M five-frame sequence phase
  // (SMPTE 272M-1994 §14.3) truncates one trailing pair or appends one
  // trailing silence pair. PAL joins are always sample-exact.
  std::vector<int32_t> get_audio_samples(size_t pair,
                                         FrameID id) const override;

  // A deferred decode may be nested under any of the joined inputs, so the
  // priming hook reaches all of them rather than only the first.
  void prime_audio_decode(const AudioDecodeProgressFn& progress) const override;

  // EFM / AC3
  uint32_t get_efm_sample_count(FrameID id) const override;
  std::vector<uint8_t> get_efm_samples(FrameID id) const override;
  uint32_t get_ac3_symbol_count(FrameID id) const override;
  std::vector<uint8_t> get_ac3_symbols(FrameID id) const override;

 private:
  std::vector<std::shared_ptr<const VideoFrameRepresentation>> sources_;
  std::vector<Entry> mapping_;

  // Resolve an output frame ID to its entry; nullptr when out of range.
  const Entry* resolve(FrameID id) const;

  // The input an output frame comes from; nullptr when out of range.
  const VideoFrameRepresentation* source_for(FrameID id,
                                             FrameID& source_frame) const;
};

// ============================================================================
// SourceJoinStage
// ============================================================================
// Concatenates the connected inputs into a single output sequence, in a
// user-specified order.
//
// The order is given as a list of source node IDs — the numbers the node
// editor draws in the corner of each node — because that is the only handle
// the user has on an input: a stage's inputs arrive as an unnamed vector whose
// order follows how the connections happen to have been made. The host
// supplies the identity of those inputs through the reserved
// orc::kInputNodeIdsParameter parameter.
//
// Parameters:
//   input_order     — ordered, comma-separated source node IDs ("16,2,4").
//                     Empty = join in connection order.
//   input_node_ids  — host-owned; see orc::kInputNodeIdsParameter.
//
// Observations emitted:
//   source_join.inputs_joined  — number of inputs in the output
//   source_join.output_frames  — length of the joined sequence
//   source_join.inputs_skipped — connected inputs the order left out
class SourceJoinStage : public DAGStage,
                        public ParameterizedStage,
                        public IStagePreviewCapability {
 public:
  SourceJoinStage();

  std::string version() const override { return "1.0"; }
  ORC_STAGE_INSTRUCTIONS_MD

  NodeTypeInfo get_node_type_info() const override {
    return NodeTypeInfo{NodeType::TRANSFORM,
                        "source_join",
                        "Source Join",
                        "Join several sources end to end into one sequence, "
                        "in a specified order (1 input = passthrough)",
                        1,
                        16,
                        1,
                        UINT32_MAX,
                        VideoFormatCompatibility::ALL};
  }

  std::vector<ArtifactPtr> execute(
      const std::vector<ArtifactPtr>& inputs,
      const std::map<std::string, ParameterValue>& parameters,
      ObservationContext& observation_context) override;

  size_t required_input_count() const override { return 1; }
  size_t output_count() const override { return 1; }

  // IStagePreviewCapability
  StagePreviewCapability get_preview_capability() const override;

  // ParameterizedStage
  std::vector<ParameterDescriptor> get_parameter_descriptors(
      VideoSystem project_format, SourceType source_type) const override;
  using ParameterizedStage::get_parameter_descriptors;
  std::map<std::string, ParameterValue> get_parameters() const override;
  bool set_parameters(
      const std::map<std::string, ParameterValue>& params) override;

  // Parse a comma-separated node-ID list ("16, 2,4").  Returns nullopt when a
  // token is not a node ID or an ID is repeated; an empty spec parses to an
  // empty list.
  static std::optional<std::vector<NodeID::value_type>> parse_node_id_list(
      const std::string& spec);

  // Positions in |connected| of the node IDs in |order|, in the order given.
  // IDs that are not connected are skipped (they are reported by the caller).
  // An empty |order| yields the identity permutation, which is the
  // connection-order join.
  static std::vector<size_t> resolve_input_positions(
      const std::vector<NodeID::value_type>& order,
      const std::vector<NodeID::value_type>& connected,
      std::vector<NodeID::value_type>& unresolved_out,
      std::vector<NodeID::value_type>& skipped_out);

 private:
  // Everything one execute() needs, resolved from the parameter map it was
  // handed over the configuration the stage holds. Kept on the stack because
  // a DAG's stage instances are shared between worker threads.
  struct RunConfig {
    std::vector<NodeID::value_type> order;
    std::vector<NodeID::value_type> connected;
  };

  std::optional<RunConfig> config_for(
      const std::map<std::string, ParameterValue>& parameters) const;

  // True when |order| names every connected input exactly once — the state in
  // which the stage's configuration still describes the graph it is in.
  static bool order_matches_inputs(
      const std::vector<NodeID::value_type>& order,
      const std::vector<NodeID::value_type>& connected);

  void refresh_configuration_status();

  // Reject a join whose inputs do not share one signal geometry: the output is
  // a single sequence and downstream stages read its geometry once.
  static void require_uniform_geometry(
      const std::vector<std::shared_ptr<const VideoFrameRepresentation>>&
          sources);

  // Parameters
  std::string input_order_;
  std::string input_node_ids_;
  std::vector<NodeID::value_type> parsed_order_;
  std::vector<NodeID::value_type> parsed_connected_;

  // Whether the host has told this stage what its inputs are. Distinct from
  // parsed_connected_ being empty, which is the host saying "nothing is
  // connected" — an answer, and one a stored order cannot match.
  bool input_identity_known_ = false;

  // Cached output for preview rendering
  mutable std::shared_ptr<const VideoFrameRepresentation> cached_output_;
};

}  // namespace orc
