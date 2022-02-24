// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/a11y/lib/screen_reader/screen_reader_action.h"

#include <lib/fpromise/bridge.h>
#include <lib/syslog/cpp/macros.h>

#include <string>

#include "fuchsia/accessibility/semantics/cpp/fidl.h"
#include "src/ui/a11y/lib/screen_reader/util/util.h"

namespace a11y {
namespace {

using fuchsia::accessibility::tts::Utterance;

}  // namespace

ScreenReaderAction::ScreenReaderAction(ActionContext* context,
                                       ScreenReaderContext* screen_reader_context)
    : action_context_(context), screen_reader_context_(screen_reader_context) {
  FX_DCHECK(action_context_);
  FX_DCHECK(screen_reader_context_);
}

ScreenReaderAction::~ScreenReaderAction() = default;

void ScreenReaderAction::ExecuteHitTesting(
    ActionContext* context, GestureContext gesture_context,
    fuchsia::accessibility::semantics::SemanticListener::HitTestCallback callback) {
  FX_DCHECK(context);
  FX_DCHECK(context->semantics_source);
  context->semantics_source->ExecuteHitTesting(
      gesture_context.view_ref_koid, gesture_context.CurrentCentroid(true /* local coordinates */),
      std::move(callback));
}

fpromise::promise<> ScreenReaderAction::ExecuteAccessibilityActionPromise(
    zx_koid_t view_ref_koid, uint32_t node_id, fuchsia::accessibility::semantics::Action action) {
  fpromise::bridge<> bridge;
  action_context_->semantics_source->PerformAccessibilityAction(
      view_ref_koid, node_id, action,
      [completer = std::move(bridge.completer)](bool handled) mutable {
        if (!handled) {
          return completer.complete_error();
        }
        completer.complete_ok();
      });
  return bridge.consumer.promise_or(fpromise::error());
}

fpromise::promise<> ScreenReaderAction::SetA11yFocusPromise(const uint32_t node_id,
                                                            zx_koid_t view_koid) {
  fpromise::bridge<> bridge;
  auto* a11y_focus_manager = screen_reader_context_->GetA11yFocusManager();
  a11y_focus_manager->SetA11yFocus(
      view_koid, node_id, [this, completer = std::move(bridge.completer)](bool success) mutable {
        if (!success) {
          return completer.complete_error();
        }

        // Update the navigation context to reflect
        // the new focus.
        UpdateNavigationContext();
        completer.complete_ok();
      });
  return bridge.consumer.promise_or(fpromise::error());
}

fpromise::promise<> ScreenReaderAction::BuildSpeechTaskFromNodePromise(zx_koid_t view_koid,
                                                                       uint32_t node_id) {
  return fpromise::make_promise([this, node_id, view_koid]() mutable -> fpromise::promise<> {
    const auto* node = action_context_->semantics_source->GetSemanticNode(view_koid, node_id);
    if (!node) {
      FX_LOGS(INFO) << "ScreenReaderAction: No node found for node id:" << node_id;
      return fpromise::make_error_promise();
    }

    auto* speaker = screen_reader_context_->speaker();
    FX_DCHECK(speaker);
    if (screen_reader_context_->IsVirtualKeyboardFocused()) {
      // Read the key in the virtual keyboard.
      return speaker->SpeakNodeCanonicalizedLabelPromise(node, {.interrupt = true});
    }

    // When not focusing a virtual keyboard node, just describe the node.
    return speaker->SpeakNodePromise(node, {.interrupt = true}, GetMessageContext());
  });
}

fpromise::promise<> ScreenReaderAction::BuildSpeechTaskForRangeValuePromise(zx_koid_t view_koid,
                                                                            uint32_t node_id) {
  return fpromise::make_promise([this, node_id, view_koid]() mutable -> fpromise::promise<> {
    const auto* node = action_context_->semantics_source->GetSemanticNode(view_koid, node_id);
    if (!node) {
      FX_LOGS(INFO) << "ScreenReaderAction: No node found for node id:" << node_id;
      return fpromise::make_error_promise();
    }

    std::string slider_value = GetSliderValue(*node);
    if (slider_value.empty()) {
      FX_LOGS(INFO) << "ScreenReaderAction: Slider node is missing |range_value| and |value|. "
                       "Nothing to send to TTS.";
      return fpromise::make_error_promise();
    }

    auto* speaker = screen_reader_context_->speaker();
    FX_DCHECK(speaker);

    Utterance utterance;
    utterance.set_message(slider_value);
    return speaker->SpeakMessagePromise(std::move(utterance), {.interrupt = true});
  });
}

void ScreenReaderAction::UpdateNavigationContext() {
  auto* a11y_focus_manager = screen_reader_context_->GetA11yFocusManager();
  FX_DCHECK(a11y_focus_manager);

  auto a11y_focus = a11y_focus_manager->GetA11yFocus();
  auto view_koid = a11y_focus->view_ref_koid;
  auto node_id = a11y_focus->node_id;

  const auto& previous_navigation_context = screen_reader_context_->previous_navigation_context();

  // If we've entered a new view, then the previous navigation context is no
  // longer relevant, so we should clear it. Otherwise, we should set the
  // previous navigation context before we update the current.
  if (previous_navigation_context.view_ref_koid &&
      view_koid != *previous_navigation_context.view_ref_koid) {
    screen_reader_context_->set_previous_navigation_context({});
  } else {
    // Set the previous navigation context before we update the current.
    screen_reader_context_->set_previous_navigation_context(
        screen_reader_context_->current_navigation_context());
  }

  auto* new_container = GetContainerNode(view_koid, node_id, action_context_->semantics_source);

  ScreenReaderContext::NavigationContext new_navigation_context;
  new_navigation_context.view_ref_koid = view_koid;

  if (new_container) {
    new_navigation_context.current_container.emplace(new_container->node_id());

    if (new_container->has_role() &&
        new_container->role() == fuchsia::accessibility::semantics::Role::TABLE &&
        new_container->has_attributes() && new_container->attributes().has_table_attributes()) {
      const auto& table_attributes = new_container->attributes().table_attributes();

      new_navigation_context.table_context.emplace();
      auto& new_table_context = new_navigation_context.table_context;

      if (table_attributes.has_row_header_ids()) {
        for (const auto row_header_node_id : table_attributes.row_header_ids()) {
          std::string row_header;

          const auto* row_header_node =
              action_context_->semantics_source->GetSemanticNode(view_koid, row_header_node_id);

          if (row_header_node && row_header_node->has_attributes() &&
              row_header_node->attributes().has_label()) {
            row_header = row_header_node->attributes().label();
          }

          new_table_context->row_headers.push_back(row_header);
        }
      }

      if (table_attributes.has_column_header_ids()) {
        for (const auto column_header_node_id : table_attributes.column_header_ids()) {
          std::string column_header;

          const auto* column_header_node =
              action_context_->semantics_source->GetSemanticNode(view_koid, column_header_node_id);

          if (column_header_node && column_header_node->has_attributes() &&
              column_header_node->attributes().has_label()) {
            column_header = column_header_node->attributes().label();
          }

          new_table_context->column_headers.push_back(column_header);
        }
      }

      const auto* node = action_context_->semantics_source->GetSemanticNode(view_koid, node_id);

      if (node && node->has_attributes() && node->attributes().has_table_cell_attributes()) {
        const auto& table_cell_attributes = node->attributes().table_cell_attributes();
        new_table_context->row_index = table_cell_attributes.row_index();
        new_table_context->column_index = table_cell_attributes.column_index();
      }
    }
  }

  screen_reader_context_->set_current_navigation_context(new_navigation_context);
}

ScreenReaderMessageGenerator::ScreenReaderMessageContext ScreenReaderAction::GetMessageContext() {
  ScreenReaderMessageGenerator::ScreenReaderMessageContext message_context;
  ScreenReaderMessageGenerator::TableCellContext table_cell_context;

  auto old_navigation_context = screen_reader_context_->previous_navigation_context();
  auto new_navigation_context = screen_reader_context_->current_navigation_context();

  auto* a11y_focus_manager = screen_reader_context_->GetA11yFocusManager();
  if (!a11y_focus_manager) {
    return {};
  }

  auto a11y_focus = a11y_focus_manager->GetA11yFocus();
  if (!a11y_focus) {
    return {};
  }

  // Set the current and previous container nodes, if any.
  if (new_navigation_context.current_container) {
    message_context.current_container = action_context_->semantics_source->GetSemanticNode(
        a11y_focus->view_ref_koid, *new_navigation_context.current_container);
  }
  if (old_navigation_context.current_container) {
    message_context.previous_container = action_context_->semantics_source->GetSemanticNode(
        a11y_focus->view_ref_koid, *old_navigation_context.current_container);
  }

  // Set the exited_nested_container bit.
  message_context.exited_nested_container =
      message_context.current_container && message_context.previous_container &&
      GetContainerNode(a11y_focus->view_ref_koid, message_context.previous_container->node_id(),
                       action_context_->semantics_source) == message_context.current_container;

  // If we've entered a new container or we've changed row/column index within
  // the same container, we need to fill the message context.
  if (new_navigation_context.current_container && new_navigation_context.table_context) {
    if (old_navigation_context.current_container != new_navigation_context.current_container ||
        new_navigation_context.table_context->row_index !=
            old_navigation_context.table_context->row_index) {
      // Some tables may not have row headers, or they may not populate the row headers
      // field. In that case, we should not try to read the header.
      if (new_navigation_context.table_context->row_index <
          new_navigation_context.table_context->row_headers.size()) {
        table_cell_context.row_header =
            new_navigation_context.table_context
                ->row_headers[new_navigation_context.table_context->row_index];
      }
    }

    if (old_navigation_context.current_container != new_navigation_context.current_container ||
        new_navigation_context.table_context->column_index !=
            old_navigation_context.table_context->column_index) {
      // Some tables may not have column headers, or they may not populate the column headers
      // field. In that case, we should not try to read the header.
      if (new_navigation_context.table_context->column_index <
          new_navigation_context.table_context->column_headers.size()) {
        table_cell_context.column_header =
            new_navigation_context.table_context
                ->column_headers[new_navigation_context.table_context->column_index];
      }
    }

    if (!table_cell_context.row_header.empty() || !table_cell_context.column_header.empty()) {
      message_context.table_cell_context.emplace(table_cell_context);
    }
  }

  return message_context;
}

}  // namespace a11y
