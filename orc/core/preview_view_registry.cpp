/*
 * File:        preview_view_registry.cpp
 * Module:      orc-core
 * Purpose:     Preview view registry implementation.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "preview_view_registry.h"

#include <orc/stage/preview/colour_preview_provider.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <utility>

#include "analysis/vectorscope/composite_vectorscope.h"
#include "analysis/vectorscope/vectorscope_analysis.h"
#include "orc_histogram.h"
#include "preview_renderer.h"

namespace orc {
namespace {

constexpr const char* kLineScopeViewId = "preview.linescope";
constexpr const char* kFrameTimingViewId = "preview.frame_timing";

bool is_generic_vfr_view_id(const std::string& view_id) {
  return view_id == kLineScopeViewId || view_id == kFrameTimingViewId;
}

const DAGNode* find_node(const DAG& dag, NodeID node_id) {
  const auto& nodes = dag.nodes();
  auto it = std::find_if(
      nodes.begin(), nodes.end(),
      [node_id](const DAGNode& node) { return node.node_id == node_id; });

  if (it == nodes.end()) {
    return nullptr;
  }

  return &(*it);
}

PreviewOutputType output_type_for_data_type(VideoDataType data_type) {
  switch (data_type) {
    case VideoDataType::CompositeNTSC:
    case VideoDataType::CompositePAL:
    case VideoDataType::YC_NTSC:
    case VideoDataType::YC_PAL:
      return PreviewOutputType::Frame_Field1;
    case VideoDataType::ColourNTSC:
    case VideoDataType::ColourPAL:
      return PreviewOutputType::Frame_Field1_First;
  }

  return PreviewOutputType::Frame_Field1;
}

class ImagePreviewView final : public IPreviewView {
 public:
  ImagePreviewView(NodeID node_id, PreviewRenderer* renderer)
      : node_id_(node_id), renderer_(renderer) {}

  std::vector<VideoDataType> supported_data_types() const override {
    return {
        VideoDataType::CompositeNTSC, VideoDataType::CompositePAL,
        VideoDataType::YC_NTSC,       VideoDataType::YC_PAL,
        VideoDataType::ColourNTSC,    VideoDataType::ColourPAL,
    };
  }

  PreviewViewDataResult request_data(
      VideoDataType data_type, const PreviewCoordinate& coordinate) override {
    PreviewViewDataResult result{};
    if (!renderer_) {
      result.error_message = "Preview renderer is not initialized";
      return result;
    }

    if (!coordinate.is_valid()) {
      result.error_message = "Preview coordinate is invalid";
      return result;
    }

    last_type_ = output_type_for_data_type(data_type);
    last_index_ = coordinate.field_index;

    auto rendered = renderer_->render_output(node_id_, last_type_, last_index_);
    if (!rendered.success || !rendered.image.is_valid()) {
      result.error_message = rendered.error_message.empty()
                                 ? "Failed to render preview image"
                                 : rendered.error_message;
      return result;
    }

    result.success = true;
    result.payload_kind = PreviewViewPayloadKind::Image;
    result.image = std::move(rendered.image);
    return result;
  }

  PreviewViewExportResult export_as(const std::string& format,
                                    const std::string& path) const override {
    PreviewViewExportResult result{};

    if (!renderer_) {
      result.error_message = "Preview renderer is not initialized";
      return result;
    }

    if (format != "png") {
      result.error_message = "Unsupported export format for image view";
      return result;
    }

    if (!renderer_->save_png(node_id_, last_type_, last_index_, path)) {
      result.error_message = "Failed to export image as PNG";
      return result;
    }

    result.success = true;
    return result;
  }

 private:
  NodeID node_id_;
  PreviewRenderer* renderer_{nullptr};
  PreviewOutputType last_type_{PreviewOutputType::Frame_Field1};
  uint64_t last_index_{0};
};

class VectorscopePreviewView final : public IPreviewView {
 public:
  VectorscopePreviewView(const DAG* dag, NodeID node_id,
                         PreviewRenderer* renderer)
      : dag_(dag), node_id_(node_id), renderer_(renderer) {}

  // The decoded (grading) acquisition needs a stage that produces colour
  // carriers; the composite (measurement) acquisition needs only a CVBS or
  // Y/C carrier, which every signal-domain stage has.  Both are offered on
  // every type so the dialog's mode selector is reachable wherever either can
  // be satisfied.
  std::vector<VideoDataType> supported_data_types() const override {
    return {
        VideoDataType::CompositeNTSC, VideoDataType::CompositePAL,
        VideoDataType::YC_NTSC,       VideoDataType::YC_PAL,
        VideoDataType::ColourNTSC,    VideoDataType::ColourPAL,
    };
  }

  PreviewViewDataResult request_data(
      VideoDataType data_type, const PreviewCoordinate& coordinate) override {
    PreviewViewDataResult result{};

    if (!dag_) {
      result.error_message = "DAG is not initialized";
      return result;
    }

    if (!coordinate.is_valid()) {
      result.error_message = "Preview coordinate is invalid";
      return result;
    }

    // The acquisition follows the data type rather than a caller's choice:
    // a colour-domain output has decoder planes to plot, a signal-domain one
    // has a carrier to demodulate, and neither can stand in for the other.
    if (data_type != VideoDataType::ColourNTSC &&
        data_type != VideoDataType::ColourPAL) {
      return request_composite_data(coordinate);
    }

    const DAGNode* node = find_node(*dag_, node_id_);
    if (!node || !node->stage) {
      result.error_message = "Target node not found";
      return result;
    }

    auto* provider =
        dynamic_cast<const IColourPreviewProvider*>(node->stage.get());
    if (!provider) {
      result.error_message = "Stage does not provide colour preview carriers";
      return result;
    }

    auto carrier_opt =
        provider->get_colour_preview_carrier(coordinate.field_index);
    if (!carrier_opt.has_value()) {
      result.error_message = "Failed to fetch colour preview carrier";
      return result;
    }

    const bool active_area_only = coordinate.vectorscope_active_area_only;
    const bool whole_active_picture = active_area_only &&
                                      coordinate.vectorscope_first_line == 0 &&
                                      coordinate.vectorscope_last_line == 0;

    // The stage already extracted the active picture on the way past, so the
    // default request is answered from that rather than walked again.  Any
    // narrower selection has to be taken from the planes.
    if (whole_active_picture && carrier_opt->vectorscope_data.has_value()) {
      last_vectorscope_ = carrier_opt->vectorscope_data;
      // The stage reports the extent it sampled but not where it started;
      // for the active picture that is the carrier's own active line range.
      last_vectorscope_->first_line = carrier_opt->active_y_start;
      last_vectorscope_->last_line =
          (carrier_opt->active_y_end > carrier_opt->active_y_start)
              ? (carrier_opt->active_y_end - 1)
              : carrier_opt->active_y_start;
    } else {
      const uint64_t field_number =
          carrier_opt->vectorscope_data.has_value()
              ? carrier_opt->vectorscope_data->field_number
              : coordinate.field_index;

      last_vectorscope_ =
          VectorscopeAnalysisTool::extractFromColourFrameCarrier(
              *carrier_opt, field_number, 4, active_area_only,
              coordinate.vectorscope_first_line,
              coordinate.vectorscope_last_line);
    }

    if (!last_vectorscope_.has_value() || last_vectorscope_->samples.empty()) {
      result.error_message =
          "Vectorscope data is not available for requested frame";
      return result;
    }

    last_vectorscope_->acquisition_mode =
        VectorscopeAcquisitionMode::DecodedComponent;
    last_vectorscope_->system = carrier_opt->system;
    last_vectorscope_->cvbs_white =
        static_cast<int32_t>(carrier_opt->cvbs_white);
    last_vectorscope_->cvbs_blanking =
        static_cast<int32_t>(carrier_opt->cvbs_blanking);

    result.success = true;
    result.payload_kind = PreviewViewPayloadKind::Vectorscope;
    result.vectorscope = last_vectorscope_;
    return result;
  }

  PreviewViewExportResult export_as(const std::string& format,
                                    const std::string& path) const override {
    PreviewViewExportResult result{};

    if (format != "csv") {
      result.error_message = "Unsupported export format for vectorscope view";
      return result;
    }

    if (!last_vectorscope_.has_value()) {
      result.error_message =
          "No vectorscope data has been requested for export";
      return result;
    }

    std::ofstream out(path);
    if (!out.is_open()) {
      result.error_message = "Failed to open export path";
      return result;
    }

    out << "u,v,field_id,line,sample_class,line_phase\n";
    for (const auto& sample : last_vectorscope_->samples) {
      out << sample.u << ',' << sample.v << ','
          << static_cast<int>(sample.field_id) << ',' << sample.line_number
          << ',' << static_cast<int>(sample.sample_class) << ','
          << static_cast<int>(sample.line_phase) << '\n';
    }

    out.flush();
    if (!out.good()) {
      result.error_message = "Failed while writing vectorscope CSV";
      return result;
    }

    result.success = true;
    return result;
  }

 private:
  // Composite (measurement) acquisition: demodulate the CVBS (or Y/C chroma)
  // carrier the stage produces, with no decoder in the path.  For a
  // signal-domain data type PreviewCoordinate::field_index addresses the frame
  // to acquire.
  PreviewViewDataResult request_composite_data(
      const PreviewCoordinate& coordinate) {
    PreviewViewDataResult result{};

    if (!renderer_) {
      result.error_message = "Preview renderer is not initialized";
      return result;
    }

    auto representation = renderer_->get_representation_at_node(node_id_);
    if (!representation) {
      result.error_message =
          "Stage does not provide a composite carrier to measure";
      return result;
    }

    const FrameID frame_id = static_cast<FrameID>(coordinate.field_index);
    if (!representation->has_frame(frame_id)) {
      result.error_message = "Requested frame is not available at this stage";
      return result;
    }

    CompositeVectorscopeOptions options;
    options.window = coordinate.vectorscope_window;
    options.active_lines_only = coordinate.vectorscope_active_area_only;
    options.first_line = coordinate.vectorscope_first_line;
    options.last_line = coordinate.vectorscope_last_line;

    auto data =
        extract_composite_vectorscope(*representation, frame_id, options);
    if (!data.has_value() || data->samples.empty()) {
      result.error_message =
          "Composite vectorscope data is not available for requested frame";
      return result;
    }

    last_vectorscope_ = std::move(data);
    result.success = true;
    result.payload_kind = PreviewViewPayloadKind::Vectorscope;
    result.vectorscope = last_vectorscope_;
    return result;
  }

  const DAG* dag_{nullptr};
  NodeID node_id_;
  PreviewRenderer* renderer_{nullptr};
  std::optional<VectorscopeData> last_vectorscope_;
};

// Accumulates a per-channel histogram from a ColourFrameCarrier.
// Channel normalisation follows the same convention as VectorscopePreviewView:
//   Y  — relative to the picture-black floor (cvbs_black); 0 % = black.
//   U/V — relative to full active-video swing, centred at zero.
//   I/Q — U/V rotated by 33° (SMPTE 170M-2004 §7.3), NTSC only.
static VideoHistogramData extractHistogramFromCarrier(
    const ColourFrameCarrier& carrier, uint64_t field_number) {
  VideoHistogramData data;
  data.field_number = field_number;
  data.system = carrier.system;
  data.cvbs_blanking = carrier.cvbs_blanking;
  data.cvbs_black = carrier.cvbs_black;
  data.cvbs_white = carrier.cvbs_white;
  data.width = carrier.width;
  data.height = carrier.height;

  if (!carrier.is_valid()) {
    return data;
  }

  const double range = std::max(1.0, carrier.cvbs_white - carrier.cvbs_black);
  const double uv_range =
      std::max(1.0, carrier.cvbs_white - carrier.cvbs_blanking);

  // Pre-compute NTSC IQ rotation constants.
  // SMPTE 170M-2004 §7.3: I is at 33° from V (R-Y), Q at 33° from U (B-Y).
  // In normalised (U,V) space: I = −U·sin33° + V·cos33°,
  //                             Q =  U·cos33° + V·sin33°.
  constexpr double kSin33 = 0.5446390350;
  constexpr double kCos33 = 0.8386705679;

  const bool is_ntsc = (carrier.system == VideoSystem::NTSC);

  const double bin_range =
      VideoHistogramData::kRangeMax - VideoHistogramData::kRangeMin;
  const double bins_per_percent =
      static_cast<double>(VideoHistogramData::kBinCount) / bin_range;

  const double chroma_bin_range =
      VideoHistogramData::kChromaRangeMax - VideoHistogramData::kChromaRangeMin;
  const double chroma_bins_per_percent =
      static_cast<double>(VideoHistogramData::kBinCount) / chroma_bin_range;

  auto to_bin = [&](double percent) -> std::optional<size_t> {
    const double offset = percent - VideoHistogramData::kRangeMin;
    const int bin = static_cast<int>(offset * bins_per_percent);
    if (bin < 0 || bin >= static_cast<int>(VideoHistogramData::kBinCount)) {
      return std::nullopt;
    }
    return static_cast<size_t>(bin);
  };

  auto to_chroma_bin = [&](double percent) -> std::optional<size_t> {
    const double offset = percent - VideoHistogramData::kChromaRangeMin;
    const int bin = static_cast<int>(offset * chroma_bins_per_percent);
    if (bin < 0 || bin >= static_cast<int>(VideoHistogramData::kBinCount)) {
      return std::nullopt;
    }
    return static_cast<size_t>(bin);
  };

  // The chroma sink always sets active_area_cropping_applied = true, which
  // causes decoders to remap active-picture data to 0-based indices within
  // the full-sized ComponentFrame buffer.  plane[0] is therefore the first
  // active pixel.  active_x/y_start hold the original pre-crop signal
  // coordinates and must NOT be used as absolute plane indices — only the
  // difference (active width / height) gives the correct iteration bounds.
  const uint32_t active_width =
      (carrier.active_x_end > carrier.active_x_start)
          ? (carrier.active_x_end - carrier.active_x_start)
          : carrier.width;
  const uint32_t active_height =
      (carrier.active_y_end > carrier.active_y_start)
          ? (carrier.active_y_end - carrier.active_y_start)
          : carrier.height;

  uint32_t pixel_count = 0;

  for (uint32_t row = 0; row < active_height; ++row) {
    for (uint32_t col = 0; col < active_width; ++col) {
      const size_t i = static_cast<size_t>(row) * carrier.width + col;

      // Y: normalised so that cvbs_black = 0 %, cvbs_white = 100 %.
      const double y_norm = (carrier.y_plane[i] - carrier.cvbs_black) / range;
      if (auto b = to_bin(y_norm * 100.0)) {
        data.y_bins[*b]++;
      }

      // U/V: bipolar, centred at 0 % (neutral chroma), binned over
      // [kChromaRangeMin, kChromaRangeMax] so the full swing is visible.
      const double u_norm = carrier.u_plane[i] / uv_range;
      const double v_norm = carrier.v_plane[i] / uv_range;

      if (auto b = to_chroma_bin(u_norm * 100.0)) {
        data.u_bins[*b]++;
      }
      if (auto b = to_chroma_bin(v_norm * 100.0)) {
        data.v_bins[*b]++;
      }

      // I/Q: SMPTE 170M rotation of (U, V), NTSC only.
      if (is_ntsc) {
        const double i_norm = (-u_norm * kSin33) + (v_norm * kCos33);
        const double q_norm = (u_norm * kCos33) + (v_norm * kSin33);
        if (auto b = to_chroma_bin(i_norm * 100.0)) {
          data.i_bins[*b]++;
        }
        if (auto b = to_chroma_bin(q_norm * 100.0)) {
          data.q_bins[*b]++;
        }
      }

      ++pixel_count;
    }
  }

  data.total_pixels = pixel_count;
  return data;
}

class HistogramPreviewView final : public IPreviewView {
 public:
  HistogramPreviewView(const DAG* dag, NodeID node_id)
      : dag_(dag), node_id_(node_id) {}

  std::vector<VideoDataType> supported_data_types() const override {
    return {
        VideoDataType::ColourNTSC,
        VideoDataType::ColourPAL,
    };
  }

  PreviewViewDataResult request_data(
      VideoDataType /*data_type*/,
      const PreviewCoordinate& coordinate) override {
    PreviewViewDataResult result{};

    if (!dag_) {
      result.error_message = "DAG is not initialized";
      return result;
    }

    if (!coordinate.is_valid()) {
      result.error_message = "Preview coordinate is invalid";
      return result;
    }

    const DAGNode* node = find_node(*dag_, node_id_);
    if (!node || !node->stage) {
      result.error_message = "Target node not found";
      return result;
    }

    auto* provider =
        dynamic_cast<const IColourPreviewProvider*>(node->stage.get());
    if (!provider) {
      result.error_message = "Stage does not provide colour preview carriers";
      return result;
    }

    auto carrier_opt =
        provider->get_colour_preview_carrier(coordinate.field_index);
    if (!carrier_opt.has_value()) {
      result.error_message = "Failed to fetch colour preview carrier";
      return result;
    }

    last_histogram_ =
        extractHistogramFromCarrier(*carrier_opt, coordinate.field_index);

    result.success = true;
    result.payload_kind = PreviewViewPayloadKind::Histogram;
    result.histogram = last_histogram_;
    return result;
  }

  PreviewViewExportResult export_as(const std::string& format,
                                    const std::string& path) const override {
    PreviewViewExportResult result{};

    if (format != "csv") {
      result.error_message = "Unsupported export format for histogram view";
      return result;
    }

    if (!last_histogram_.has_value()) {
      result.error_message = "No histogram data has been requested for export";
      return result;
    }

    std::ofstream out(path);
    if (!out.is_open()) {
      result.error_message = "Failed to open export path";
      return result;
    }

    out << "bin,y,u,v,i,q\n";
    for (size_t b = 0; b < VideoHistogramData::kBinCount; ++b) {
      out << b << ',' << last_histogram_->y_bins[b] << ','
          << last_histogram_->u_bins[b] << ',' << last_histogram_->v_bins[b]
          << ',' << last_histogram_->i_bins[b] << ','
          << last_histogram_->q_bins[b] << '\n';
    }

    out.flush();
    if (!out.good()) {
      result.error_message = "Failed while writing histogram CSV";
      return result;
    }

    result.success = true;
    return result;
  }

 private:
  const DAG* dag_{nullptr};
  NodeID node_id_;
  std::optional<VideoHistogramData> last_histogram_;
};

class GenericVfrVisualizationPreviewView final : public IPreviewView {
 public:
  explicit GenericVfrVisualizationPreviewView(std::string visualization_name)
      : visualization_name_(std::move(visualization_name)) {}

  std::vector<VideoDataType> supported_data_types() const override {
    return {
        VideoDataType::CompositeNTSC, VideoDataType::CompositePAL,
        VideoDataType::YC_NTSC,       VideoDataType::YC_PAL,
        VideoDataType::ColourNTSC,    VideoDataType::ColourPAL,
    };
  }

  PreviewViewDataResult request_data(VideoDataType,
                                     const PreviewCoordinate&) override {
    PreviewViewDataResult result{};
    result.error_message =
        visualization_name_ +
        " is driven by dedicated requests and has no direct preview payload";
    return result;
  }

  PreviewViewExportResult export_as(const std::string&,
                                    const std::string&) const override {
    PreviewViewExportResult result{};
    result.error_message = visualization_name_ +
                           " does not support export via preview view registry";
    return result;
  }

 private:
  std::string visualization_name_;
};

}  // namespace

bool PreviewViewRegistry::register_view(PreviewViewDescriptor descriptor,
                                        ViewFactory factory) {
  if (descriptor.id.empty() || !factory) {
    return false;
  }

  const std::string view_id = descriptor.id;

  if (entries_.find(view_id) != entries_.end()) {
    return false;
  }

  entries_.emplace(view_id, Entry{std::move(descriptor), std::move(factory)});
  return true;
}

bool PreviewViewRegistry::unregister_view(const std::string& view_id) {
  if (view_id.empty()) {
    return false;
  }

  size_t removed_entries = entries_.erase(view_id);
  if (removed_entries == 0) {
    return false;
  }

  for (auto it = view_cache_.begin(); it != view_cache_.end();) {
    if (it->first.view_id == view_id) {
      it = view_cache_.erase(it);
    } else {
      ++it;
    }
  }

  return true;
}

std::vector<PreviewViewDescriptor> PreviewViewRegistry::list_views() const {
  std::vector<PreviewViewDescriptor> result;
  result.reserve(entries_.size());

  for (const auto& entry : entries_) {
    result.push_back(entry.second.descriptor);
  }

  std::sort(result.begin(), result.end(),
            [](const PreviewViewDescriptor& a, const PreviewViewDescriptor& b) {
              return a.id < b.id;
            });

  return result;
}

std::vector<PreviewViewDescriptor> PreviewViewRegistry::get_applicable_views(
    const DAG& dag, NodeID node_id, VideoDataType data_type) const {
  std::vector<PreviewViewDescriptor> result;

  const DAGNode* node = find_node(dag, node_id);
  if (!node || !node->stage) {
    return result;
  }

  const bool supports_stage_data_type =
      node_supports_data_type(dag, node_id, data_type);

  for (const auto& entry : entries_) {
    if (is_generic_vfr_view_id(entry.second.descriptor.id)) {
      result.push_back(entry.second.descriptor);
      continue;
    }

    if (contains_data_type(entry.second.descriptor.supported_data_types,
                           data_type) &&
        supports_stage_data_type) {
      result.push_back(entry.second.descriptor);
    }
  }

  std::sort(result.begin(), result.end(),
            [](const PreviewViewDescriptor& a, const PreviewViewDescriptor& b) {
              return a.id < b.id;
            });

  return result;
}

PreviewViewDataResult PreviewViewRegistry::request_data(
    const DAG& dag, NodeID node_id, const std::string& view_id,
    VideoDataType data_type, const PreviewCoordinate& coordinate) {
  PreviewViewDataResult result{};

  const DAGNode* node = find_node(dag, node_id);
  if (!node || !node->stage) {
    result.error_message = "Target node not found";
    return result;
  }

  auto entry_it = entries_.find(view_id);
  if (entry_it == entries_.end()) {
    result.error_message = "Preview view is not registered";
    return result;
  }

  if (!is_generic_vfr_view_id(view_id)) {
    if (!contains_data_type(entry_it->second.descriptor.supported_data_types,
                            data_type)) {
      result.error_message =
          "Preview view does not support requested data type";
      return result;
    }

    if (!node_supports_data_type(dag, node_id, data_type)) {
      result.error_message =
          "Requested data type is not supported by the stage";
      return result;
    }
  }

  NodeViewKey key{node_id, view_id};
  auto cache_it = view_cache_.find(key);
  if (cache_it == view_cache_.end()) {
    auto instance = create_view(node_id, view_id);
    if (!instance) {
      result.error_message = "Failed to construct preview view";
      return result;
    }

    cache_it = view_cache_.emplace(std::move(key), std::move(instance)).first;
  }

  return cache_it->second->request_data(data_type, coordinate);
}

PreviewViewExportResult PreviewViewRegistry::export_as(
    NodeID node_id, const std::string& view_id, const std::string& format,
    const std::string& path) const {
  PreviewViewExportResult result{};

  NodeViewKey key{node_id, view_id};
  auto it = view_cache_.find(key);
  if (it == view_cache_.end()) {
    result.error_message = "Preview view has no cached request data";
    return result;
  }

  return it->second->export_as(format, path);
}

void PreviewViewRegistry::clear_cache_for_node(NodeID node_id) {
  for (auto it = view_cache_.begin(); it != view_cache_.end();) {
    if (it->first.node_id == node_id) {
      it = view_cache_.erase(it);
    } else {
      ++it;
    }
  }
}

void PreviewViewRegistry::register_default_views(
    PreviewViewRegistry& registry, const std::shared_ptr<const DAG>& dag,
    PreviewRenderer* renderer) {
  registry.register_view(
      PreviewViewDescriptor{
          "preview.image",
          "Preview Image",
          {
              VideoDataType::CompositeNTSC,
              VideoDataType::CompositePAL,
              VideoDataType::YC_NTSC,
              VideoDataType::YC_PAL,
              VideoDataType::ColourNTSC,
              VideoDataType::ColourPAL,
          },
      },
      [renderer](NodeID node_id) {
        return std::make_unique<ImagePreviewView>(node_id, renderer);
      });

  registry.register_view(
      PreviewViewDescriptor{
          "preview.vectorscope",
          "Vectorscope",
          {
              VideoDataType::CompositeNTSC,
              VideoDataType::CompositePAL,
              VideoDataType::YC_NTSC,
              VideoDataType::YC_PAL,
              VideoDataType::ColourNTSC,
              VideoDataType::ColourPAL,
          },
      },
      [dag, renderer](NodeID node_id) {
        return std::make_unique<VectorscopePreviewView>(dag.get(), node_id,
                                                        renderer);
      });

  registry.register_view(
      PreviewViewDescriptor{
          "preview.histogram",
          "Video Histogram",
          {
              VideoDataType::ColourNTSC,
              VideoDataType::ColourPAL,
          },
      },
      [dag](NodeID node_id) {
        return std::make_unique<HistogramPreviewView>(dag.get(), node_id);
      });

  registry.register_view(
      PreviewViewDescriptor{
          kLineScopeViewId,
          "Line Scope",
          {
              VideoDataType::CompositeNTSC,
              VideoDataType::CompositePAL,
              VideoDataType::YC_NTSC,
              VideoDataType::YC_PAL,
              VideoDataType::ColourNTSC,
              VideoDataType::ColourPAL,
          },
      },
      [](NodeID) {
        return std::make_unique<GenericVfrVisualizationPreviewView>(
            "Line scope");
      });

  registry.register_view(
      PreviewViewDescriptor{
          kFrameTimingViewId,
          "Frame Timing",
          {
              VideoDataType::CompositeNTSC,
              VideoDataType::CompositePAL,
              VideoDataType::YC_NTSC,
              VideoDataType::YC_PAL,
              VideoDataType::ColourNTSC,
              VideoDataType::ColourPAL,
          },
      },
      [](NodeID) {
        return std::make_unique<GenericVfrVisualizationPreviewView>(
            "Frame timing");
      });
}

size_t PreviewViewRegistry::NodeViewKeyHash::operator()(
    const NodeViewKey& key) const {
  const size_t node_hash = std::hash<NodeID>{}(key.node_id);
  const size_t id_hash = std::hash<std::string>{}(key.view_id);
  return node_hash ^ (id_hash << 1);
}

bool PreviewViewRegistry::node_supports_data_type(const DAG& dag,
                                                  NodeID node_id,
                                                  VideoDataType data_type) {
  const DAGNode* node = find_node(dag, node_id);
  if (!node || !node->stage) {
    return false;
  }

  auto* capability_stage =
      dynamic_cast<const IStagePreviewCapability*>(node->stage.get());
  if (!capability_stage) {
    return false;
  }

  const StagePreviewCapability capability =
      capability_stage->get_preview_capability();
  if (!capability.is_valid()) {
    return false;
  }

  return contains_data_type(capability.supported_data_types, data_type);
}

bool PreviewViewRegistry::contains_data_type(
    const std::vector<VideoDataType>& list, VideoDataType data_type) {
  return std::find(list.begin(), list.end(), data_type) != list.end();
}

std::unique_ptr<IPreviewView> PreviewViewRegistry::create_view(
    NodeID node_id, const std::string& view_id) const {
  auto it = entries_.find(view_id);
  if (it == entries_.end() || !it->second.factory) {
    return nullptr;
  }

  return it->second.factory(node_id);
}

}  // namespace orc
