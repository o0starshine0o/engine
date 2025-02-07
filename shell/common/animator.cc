// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/animator.h"

#include "flutter/fml/trace_event.h"
#include "third_party/dart/runtime/include/dart_tools_api.h"

namespace flutter {

namespace {

// Wait 51 milliseconds (which is 1 more milliseconds than 3 frames at 60hz)
// before notifying the engine that we are idle.  See comments in |BeginFrame|
// for further discussion on why this is necessary.
constexpr fml::TimeDelta kNotifyIdleTaskWaitTime =
    fml::TimeDelta::FromMilliseconds(51);

}  // namespace

Animator::Animator(Delegate& delegate,
                   TaskRunners task_runners,
                   std::unique_ptr<VsyncWaiter> waiter)
    : delegate_(delegate),
      task_runners_(std::move(task_runners)),
      waiter_(std::move(waiter)),
      last_frame_begin_time_(),
      last_vsync_start_time_(),
      last_frame_target_time_(),
      dart_frame_deadline_(0),
      layer_tree_holder_(std::make_shared<LayerTreeHolder>()),
      pending_frame_semaphore_(1),
      frame_number_(1),
      paused_(false),
      regenerate_layer_tree_(false),
      frame_scheduled_(false),
      notify_idle_task_id_(0),
      dimension_change_pending_(false),
      weak_factory_(this) {}

Animator::~Animator() = default;

void Animator::Stop() {
  paused_ = true;
}

void Animator::Start() {
  if (!paused_) {
    return;
  }

  paused_ = false;
  RequestFrame();
}

// Indicate that screen dimensions will be changing in order to force rendering
// of an updated frame even if the animator is currently paused.
void Animator::SetDimensionChangePending() {
  dimension_change_pending_ = true;
}

void Animator::EnqueueTraceFlowId(uint64_t trace_flow_id) {
  fml::TaskRunner::RunNowOrPostTask(
      task_runners_.GetUITaskRunner(),
      [self = weak_factory_.GetWeakPtr(), trace_flow_id] {
        if (!self) {
          return;
        }
        self->trace_flow_ids_.push_back(trace_flow_id);
        self->ScheduleMaybeClearTraceFlowIds();
      });
}

// This Parity is used by the timeline component to correctly align
// GPU Workloads events with their respective Framework Workload.
const char* Animator::FrameParity() {
  return (frame_number_ % 2) ? "even" : "odd";
}

static int64_t FxlToDartOrEarlier(fml::TimePoint time) {
  int64_t dart_now = Dart_TimelineGetMicros();
  fml::TimePoint fxl_now = fml::TimePoint::Now();
  return (time - fxl_now).ToMicroseconds() + dart_now;
}

void Animator::BeginFrame(fml::TimePoint vsync_start_time,
                          fml::TimePoint frame_target_time) {
  TRACE_EVENT_ASYNC_END0("flutter", "Frame Request Pending", frame_number_++);

  TRACE_EVENT0("flutter", "Animator::BeginFrame");
  while (!trace_flow_ids_.empty()) {
    uint64_t trace_flow_id = trace_flow_ids_.front();
    TRACE_FLOW_END("flutter", "PointerEvent", trace_flow_id);
    trace_flow_ids_.pop_front();
  }

  frame_scheduled_ = false;
  notify_idle_task_id_++;
  regenerate_layer_tree_ = false;
  pending_frame_semaphore_.Signal();

  last_frame_begin_time_ = fml::TimePoint::Now();
  last_vsync_start_time_ = vsync_start_time;
  fml::tracing::TraceEventAsyncComplete("flutter", "VsyncSchedulingOverhead",
                                        last_vsync_start_time_,
                                        last_frame_begin_time_);
  last_frame_target_time_ = frame_target_time;
  dart_frame_deadline_ = FxlToDartOrEarlier(frame_target_time);
  {
    TRACE_EVENT2("flutter", "Framework Workload", "mode", "basic", "frame",
                 FrameParity());
    delegate_.OnAnimatorBeginFrame(frame_target_time);
  }

  if (!frame_scheduled_) {
    // Under certain workloads (such as our parent view resizing us, which is
    // communicated to us by repeat viewport metrics events), we won't
    // actually have a frame scheduled yet, despite the fact that we *will* be
    // producing a frame next vsync (it will be scheduled once we receive the
    // viewport event).  Because of this, we hold off on calling
    // |OnAnimatorNotifyIdle| for a little bit, as that could cause garbage
    // collection to trigger at a highly undesirable time.
    task_runners_.GetUITaskRunner()->PostDelayedTask(
        [self = weak_factory_.GetWeakPtr(),
         notify_idle_task_id = notify_idle_task_id_]() {
          if (!self) {
            return;
          }
          // If our (this task's) task id is the same as the current one
          // (meaning there were no follow up frames to the |BeginFrame| call
          // that posted this task) and no frame is currently scheduled, then
          // assume that we are idle, and notify the engine of this.
          if (notify_idle_task_id == self->notify_idle_task_id_ &&
              !self->frame_scheduled_) {
            TRACE_EVENT0("flutter", "BeginFrame idle callback");
            self->delegate_.OnAnimatorNotifyIdle(Dart_TimelineGetMicros() +
                                                 100000);
          }
        },
        kNotifyIdleTaskWaitTime);
  }
}

void Animator::Render(std::unique_ptr<flutter::LayerTree> layer_tree) {
  if (dimension_change_pending_ &&
      layer_tree->frame_size() != last_layer_tree_size_) {
    dimension_change_pending_ = false;
  }
  last_layer_tree_size_ = layer_tree->frame_size();

  // Note the frame time for instrumentation.
  layer_tree->RecordBuildTime(last_vsync_start_time_, last_frame_begin_time_,
                              last_frame_target_time_);

  layer_tree_holder_->PushIfNewer(std::move(layer_tree));
  delegate_.OnAnimatorDraw(layer_tree_holder_, last_frame_target_time_);
}

bool Animator::CanReuseLastLayerTree() {
  return !regenerate_layer_tree_;
}

void Animator::DrawLastLayerTree() {
  pending_frame_semaphore_.Signal();
  delegate_.OnAnimatorDrawLastLayerTree();
}

void Animator::RequestFrame(bool regenerate_layer_tree) {
  if (regenerate_layer_tree) {
    regenerate_layer_tree_ = true;
  }
  if (paused_ && !dimension_change_pending_) {
    return;
  }

  if (!pending_frame_semaphore_.TryWait()) {
    // Multiple calls to Animator::RequestFrame will still result in a
    // single request to the VsyncWaiter.
    return;
  }

  // The AwaitVSync is going to call us back at the next VSync. However, we want
  // to be reasonably certain that the UI thread is not in the middle of a
  // particularly expensive callout. We post the AwaitVSync to run right after
  // an idle. This does NOT provide a guarantee that the UI thread has not
  // started an expensive operation right after posting this message however.
  // To support that, we need edge triggered wakes on VSync.

  task_runners_.GetUITaskRunner()->PostTask([self = weak_factory_.GetWeakPtr(),
                                             frame_number = frame_number_]() {
    if (!self) {
      return;
    }
    TRACE_EVENT_ASYNC_BEGIN0("flutter", "Frame Request Pending", frame_number);
    self->AwaitVSync();
  });
  frame_scheduled_ = true;
}

void Animator::AwaitVSync() {
  waiter_->AsyncWaitForVsync(
      [self = weak_factory_.GetWeakPtr()](fml::TimePoint vsync_start_time,
                                          fml::TimePoint frame_target_time) {
        if (self) {
          if (self->CanReuseLastLayerTree()) {
            self->DrawLastLayerTree();
          } else {
            self->BeginFrame(vsync_start_time, frame_target_time);
          }
        }
      });

  delegate_.OnAnimatorNotifyIdle(dart_frame_deadline_);
}

void Animator::ScheduleSecondaryVsyncCallback(uintptr_t id,
                                              const fml::closure& callback) {
  waiter_->ScheduleSecondaryCallback(id, callback);
}

void Animator::ScheduleMaybeClearTraceFlowIds() {
  waiter_->ScheduleSecondaryCallback(
      reinterpret_cast<uintptr_t>(this), [self = weak_factory_.GetWeakPtr()] {
        if (!self) {
          return;
        }
        if (!self->frame_scheduled_ && !self->trace_flow_ids_.empty()) {
          TRACE_EVENT0("flutter",
                       "Animator::ScheduleMaybeClearTraceFlowIds - callback");
          while (!self->trace_flow_ids_.empty()) {
            auto flow_id = self->trace_flow_ids_.front();
            TRACE_FLOW_END("flutter", "PointerEvent", flow_id);
            self->trace_flow_ids_.pop_front();
          }
        }
      });
}

}  // namespace flutter
