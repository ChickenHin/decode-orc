/*
 * File:        store_backed_observation_context.h
 * Module:      orc-core
 * Purpose:     Observation context that loads stored records per field on
 *              first access instead of pre-loading a whole recording
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

// Host-internal (wraps ObservationStore). Only orc-core and orc-presenters may
// include this header; GUI/CLI code must go through presenters.
#if defined(ORC_GUI_BUILD)
#error \
    "GUI code cannot include core/include/store_backed_observation_context.h. Use a presenter instead."
#endif
#if defined(ORC_CLI_BUILD)
#error \
    "CLI code cannot include core/include/store_backed_observation_context.h. Use a presenter instead."
#endif

#include <orc/stage/frame_id.h>
#include <orc/stage/observation/observation_context_interface.h>
#include <orc/stage/observation/observation_service_interface.h>

#include <map>
#include <string>
#include <vector>

#include "observation_store.h"

namespace orc {

/**
 * @brief Read-through IObservationContext over an ObservationStore.
 *
 * A triggered sink walks its input range once, reading each frame's stored
 * observations and (for a well-behaved sink) clearing them as it goes. Loading
 * every stored record of the recording into the context before the sink
 * starts — as the host previously did — costs memory proportional to the
 * recording: several GB for a 600k-frame source with a dense observation
 * namespace. This context instead materialises a field's records on the first
 * read or write that touches the field, so peak memory follows the sink's
 * working set, and the store's own LRU budget with sidecar read-through
 * governs residency of everything else.
 *
 * Loading rules mirror the eager pre-load they replace:
 *  - Stateless observers load per field.
 *  - Stateful observers load only when the store covers every frame of the
 *    range (all-or-nothing, so a per-frame skip can never break a cross-frame
 *    stream's continuity). Coverage is checked once per observer, with
 *    presence-only probes that leave the store's LRU untouched.
 *  - A cleared field is never re-loaded: cleared data is gone, exactly as it
 *    was when the whole recording was pre-loaded up front.
 *
 * Writes, schema registration and everything else delegate to the wrapped
 * inner context, so a caller that persists the inner context afterwards sees
 * what a pre-loaded run would have left there.
 *
 * Thread safety: none; confine an instance to the trigger thread (the
 * wrapped ObservationStore is itself thread-safe).
 */
class StoreBackedObservationContext : public IObservationContext {
 public:
  /**
   * @param inner       Context that actually holds the observations; must
   *                    outlive this object.
   * @param store       Store the records are loaded from; must outlive this
   *                    object.
   * @param fingerprint Provenance of the node whose output the records key on.
   * @param observers   Observers whose records are candidates for loading.
   * @param frame_range Frames of the triggered input; fields outside the
   *                    derived range are delegated without loading.
   */
  StoreBackedObservationContext(IObservationContext& inner,
                                ObservationStore& store,
                                NodeFingerprint fingerprint,
                                std::vector<ObserverInfo> observers,
                                FrameIDRange frame_range);

  void set(FieldID field_id, const std::string& namespace_,
           const std::string& key, const ObservationValue& value) override;
  std::optional<ObservationValue> get(FieldID field_id,
                                      const std::string& namespace_,
                                      const std::string& key) const override;
  bool has(FieldID field_id, const std::string& namespace_,
           const std::string& key) const override;
  std::vector<std::string> get_keys(
      FieldID field_id, const std::string& namespace_) const override;
  std::vector<std::string> get_namespaces(FieldID field_id) const override;
  std::map<std::string, std::map<std::string, ObservationValue>>
  get_all_observations(FieldID field_id) const override;
  void clear() override;
  void clear_field(FieldID field_id) override;
  void register_schema(const std::vector<ObservationKey>& keys) override;
  void clear_schema() override;

 private:
  // Index of @p field_id in loaded_, or SIZE_MAX when the field lies outside
  // the derived range and is never loaded.
  size_t slot_of(FieldID field_id) const;

  // Load every eligible observer's stored record for @p field_id into the
  // inner context, once per field.
  void ensure_loaded(FieldID field_id) const;

  // Whether @p observer (stateful) has a stored record for every field of the
  // range. Computed once per observer id, with presence-only probes.
  bool stateful_fully_covered(const ObserverInfo& observer) const;

  // Held as pointers (never null; the constructor takes references) so the
  // class stays movable and can travel in a std::optional.
  IObservationContext* inner_;
  ObservationStore* store_;
  NodeFingerprint fingerprint_;
  std::vector<ObserverInfo> observers_;
  FrameIDRange frame_range_;

  // Mutable: reads materialise state (documented single-thread confinement).
  mutable std::vector<bool> loaded_;
  mutable std::map<std::string, bool> stateful_covered_;
  // Set once clear() wipes the context: nothing loads afterwards, matching
  // the pre-load-then-clear behaviour this class replaces.
  mutable bool cleared_ = false;
};

}  // namespace orc
