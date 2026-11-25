// Copyright 2026 The Draco Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
#include "draco/compression/mesh/mesh_decode_session.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#include <ucontext.h>
#define DRACO_SESSION_HAS_UCONTEXT 1
#endif

#include "draco/attributes/geometry_attribute.h"
#include "draco/compression/config/compression_shared.h"
#include "draco/compression/config/decoder_options.h"
#include "draco/compression/mesh/decode_work_meter.h"
#include "draco/compression/mesh/mesh_edgebreaker_decoder.h"
#include "draco/core/decoder_buffer.h"
#include "draco/core/encoder_buffer.h"
#include "draco/mesh/mesh.h"

namespace draco {

namespace {

constexpr std::size_t kFixedHeaderSize = 11;

bool ParseFixedHeader(const void* data, std::size_t size,
                      DracoHeader* header) {
  if (data == nullptr || header == nullptr || size < kFixedHeaderSize) {
    return false;
  }
  std::memcpy(header->draco_string, data, 5);
  if (std::memcmp(header->draco_string, "DRACO", 5) != 0) {
    return false;
  }
  const uint8_t* p = static_cast<const uint8_t*>(data) + 5;
  header->version_major = p[0];
  header->version_minor = p[1];
  header->encoder_type = p[2];
  header->encoder_method = p[3];
  std::memcpy(&header->flags, p + 4, sizeof(uint16_t));
  return true;
}

bool FixedHeaderIsSupportedProfile(const DracoHeader& h) {
  return h.version_major == 2 && h.version_minor == 2 &&
         h.encoder_type == TRIANGULAR_MESH &&
         h.encoder_method == MESH_EDGEBREAKER_ENCODING &&
         (h.flags & METADATA_FLAG_MASK) == 0;
}

MeshDecodeSession::Progress TerminalProgress(
    MeshDecodeSession::State state, MeshDecodeSession::Phase phase,
    std::string message) {
  MeshDecodeSession::Progress p;
  p.state = state;
  p.phase = phase;
  p.error_message = std::move(message);
  return p;
}

}  // namespace

class MeshDecodeSession::Impl {
 public:
  Impl(const void* data, std::size_t size, Options options,
       const DracoHeader& header)
      : options_(options), header_(header) {
    input_.resize(size);
    if (size != 0) std::memcpy(input_.data(), data, size);
    decoder_options_.SetAttributeBool(GeometryAttribute::POSITION,
                                      "skip_attribute_transform",
                                      options_.skip_position_transform);
    decoder_options_.SetAttributeBool(GeometryAttribute::TEX_COORD,
                                      "skip_attribute_transform",
                                      options_.skip_tex_coord_transform);
    meter_.progress.state = State::kWorking;
    meter_.progress.phase = Phase::kHeader;
  }

  ~Impl() {
    // A canceled/destroyed fiber is never resumed.  The cooperative cancel
    // path unwinds decoder C++ frames before the stack allocation is released.
    DecodeWorkMeter::active = nullptr;
    current_impl_ = nullptr;
  }

  State state() const { return state_; }
  const std::string& error() const { return error_; }
  const Progress& progress() {
    meter_.SyncAliases();
    meter_.progress.state = state_;
    if (state_ == State::kDone) meter_.progress.phase = Phase::kDone;
    if (state_ == State::kError) meter_.progress.phase = Phase::kError;
    if (state_ == State::kCancelled)
      meter_.progress.phase = Phase::kCancelled;
    return meter_.progress;
  }
  const Mesh* result() const {
    return state_ == State::kDone ? mesh_.get() : nullptr;
  }
  std::unique_ptr<Mesh> release_result() {
    if (state_ != State::kDone) return nullptr;
    return std::move(mesh_);
  }

  void SetSkipTransform(int attribute_type) {
    decoder_options_.SetAttributeBool(
        static_cast<GeometryAttribute::Type>(attribute_type),
        "skip_attribute_transform", true);
    if (attribute_type == GeometryAttribute::POSITION)
      options_.skip_position_transform = true;
    if (attribute_type == GeometryAttribute::TEX_COORD)
      options_.skip_tex_coord_transform = true;
  }

  void CopyOptions(const DecoderOptions& options) {
    decoder_options_ = options;
  }

  StepResult RunStep(std::uint64_t budget) {
    StepResult result;
    result.budget = budget;
    if (state_ != State::kWorking) {
      FillResultFromState(&result);
      result.work_done = result.actual_work = result.work = 0;
      result.work_units_done = 0;
      result.work_units = result.units = result.consumed = 0;
      result.budget_used = 0;
      return result;
    }
    if (budget == 0) {
      meter_.SyncAliases();
      FillResultFromState(&result);
      result.work_done = 0;
      return result;
    }

    meter_.step_budget = budget;
    meter_.step_work = 0;
    meter_.cancel_requested = false;
    meter_.unsupported_requested = false;
    ResumeFiber();

    if (state_ == State::kWorking && meter_.unsupported_requested) {
      Fail("unsupported stream feature in MeshDecodeSession profile");
    }
    meter_.SyncAliases();
    FillResultFromState(&result);
    const std::uint64_t used = meter_.step_work;
    result.work_done = used;
    result.work_units_done = used;
    result.actual_work = used;
    result.work = used;
    result.work_units = used;
    result.units = used;
    result.consumed = used;
    result.budget_used = used;
    return result;
  }

  void Cancel() {
    if (state_ != State::kWorking) {
      state_ = State::kCancelled;
      mesh_.reset();
      return;
    }
    meter_.cancel_requested = true;
    if (fiber_started_ && !fiber_finished_) {
      // Resume once at the next charge checkpoint; Charge throws immediately
      // after context resumption, so no further decode element is processed.
      meter_.step_budget = 1;
      meter_.step_work = 0;
      ResumeFiber();
    }
    state_ = State::kCancelled;
    mesh_.reset();
  }

 private:
  static thread_local Impl* current_impl_;

#ifdef DRACO_SESSION_HAS_UCONTEXT
  ucontext_t caller_context_{};
  ucontext_t fiber_context_{};
  std::unique_ptr<char[]> fiber_stack_;
  bool fiber_started_ = false;
  bool fiber_finished_ = false;
#endif

  Options options_;
  DracoHeader header_{};
  std::vector<uint8_t> input_;
  DecoderOptions decoder_options_;
  std::unique_ptr<Mesh> mesh_;
  DecodeWorkMeter meter_;
  State state_ = State::kWorking;
  std::string error_;

  void Fail(std::string message) {
    state_ = State::kError;
    error_ = std::move(message);
    meter_.progress.error_message = error_;
    mesh_.reset();
  }

  void FillResultFromState(StepResult* result) {
    result->state = state_;
    result->status = state_;
    result->phase = meter_.progress.phase;
    result->ok = state_ != State::kError;
    result->error_message = error_;
  }

  static void YieldToCallerTrampoline(void* user_data) {
    static_cast<Impl*>(user_data)->YieldToCaller();
  }

  void YieldToCaller() {
#ifdef DRACO_SESSION_HAS_UCONTEXT
    swapcontext(&fiber_context_, &caller_context_);
#endif
  }

  static void FiberMain() {
    Impl* self = current_impl_;
    if (self != nullptr) self->DecodeOnFiber();
  }

  void ResumeFiber() {
#ifdef DRACO_SESSION_HAS_UCONTEXT
    current_impl_ = this;
    DecodeWorkMeter::active = &meter_;
    meter_.yield_callback = &YieldToCallerTrampoline;
    meter_.yield_user_data = this;

    if (!fiber_started_) {
      const std::size_t stack_size =
          std::max<std::size_t>(64u * 1024u, options_.fiber_stack_size);
      fiber_stack_ = std::unique_ptr<char[]>(new char[stack_size]);
      if (getcontext(&fiber_context_) != 0) {
        Fail("failed to initialize cooperative decoder context");
        DecodeWorkMeter::active = nullptr;
        current_impl_ = nullptr;
        return;
      }
      fiber_context_.uc_stack.ss_sp = fiber_stack_.get();
      fiber_context_.uc_stack.ss_size = stack_size;
      fiber_context_.uc_link = nullptr;
      makecontext(&fiber_context_, reinterpret_cast<void (*)()>(&FiberMain), 0);
      fiber_started_ = true;
      swapcontext(&caller_context_, &fiber_context_);
    } else {
      swapcontext(&caller_context_, &fiber_context_);
    }

    DecodeWorkMeter::active = nullptr;
    current_impl_ = nullptr;
#else
    // Standard C++17 has no resumable first-class stack.  Platforms without
    // ucontext are not used by the offline task build; keep a safe failure
    // rather than pretending to pause.
    if (!fiber_started_) {
      fiber_started_ = true;
      DecodeOnFiber();
    }
#endif
  }

  void DecodeOnFiber() {
    try {
      mesh_ = std::unique_ptr<Mesh>(new Mesh());
      DecoderBuffer buffer;
      buffer.Init(reinterpret_cast<const char*>(input_.data()), input_.size());

      // This is the same production MeshEdgebreakerDecoder::Decode path used
      // by the synchronous Decoder.  Work-meter checkpoints suspend this whole
      // call with its loop locals and active stacks intact.
      MeshEdgebreakerDecoder decoder;
      const Status status =
          decoder.Decode(decoder_options_, &buffer, mesh_.get());
      if (meter_.cancel_requested) throw SessionCanceledException();
      if (meter_.unsupported_requested) {
        Fail("unsupported stream feature in MeshDecodeSession profile");
      } else if (!status.ok()) {
        Fail(status.error_msg_string().empty() ? "Draco mesh decode failed"
                                               : status.error_msg_string());
      } else if (!ValidateResultProfile()) {
        if (error_.empty())
          Fail("decoded stream is outside MeshDecodeSession profile");
      } else {
        state_ = State::kDone;
        meter_.progress.phase = Phase::kDone;
      }
    } catch (const SessionCanceledException&) {
      state_ = State::kCancelled;
      mesh_.reset();
    } catch (const std::exception& e) {
      Fail(e.what());
    } catch (...) {
      Fail("unknown MeshDecodeSession failure");
    }

    meter_.SyncAliases();
    meter_.progress.state = state_;
    fiber_finished_ = true;
    // Return to RunStep/Cancel.  A terminal session is never resumed.
    YieldToCaller();
  }

  bool ValidateResultProfile() {
    if (!mesh_) return false;
    int position_count = 0;
    int texcoord_count = 0;
    for (int i = 0; i < mesh_->num_attributes(); ++i) {
      const PointAttribute* att = mesh_->attribute(i);
      if (att == nullptr) return false;
      if (att->attribute_type() == GeometryAttribute::POSITION) {
        ++position_count;
        if (att->num_components() != 3) {
          Fail("only three-component POSITION is supported");
          return false;
        }
        // When transforms are not skipped, output must be FLOAT32.  A skipped
        // quantization transform intentionally leaves INT32 portable values.
        if (!options_.skip_position_transform &&
            att->data_type() != DT_FLOAT32) {
          Fail("POSITION must be FLOAT32 after inverse transform");
          return false;
        }
      } else if (att->attribute_type() == GeometryAttribute::TEX_COORD) {
        ++texcoord_count;
        if (att->num_components() != 2) {
          Fail("only two-component TEX_COORD is supported");
          return false;
        }
        if (!options_.skip_tex_coord_transform &&
            att->data_type() != DT_FLOAT32) {
          Fail("TEX_COORD must be FLOAT32 after inverse transform");
          return false;
        }
      } else {
        Fail("only POSITION and TEX_COORD attributes are supported");
        return false;
      }
    }
    if (position_count != 1 || texcoord_count > 1) {
      Fail("expected one POSITION and at most one TEX_COORD");
      return false;
    }
    return true;
  }
};

thread_local MeshDecodeSession::Impl* MeshDecodeSession::Impl::current_impl_ =
    nullptr;

MeshDecodeSession::MeshDecodeSession() = default;

MeshDecodeSession::MeshDecodeSession(const Options& options)
    : default_options_(options) {}

MeshDecodeSession::~MeshDecodeSession() { reset(); }

MeshDecodeSession::BeginResult MeshDecodeSession::begin(
    const void* data, std::size_t size) {
  return begin(data, size, default_options_);
}

MeshDecodeSession::BeginResult MeshDecodeSession::begin(
    const void* data, std::size_t size, const Options& options) {
  if (impl_ && impl_->state() == State::kWorking) {
    return BeginResult(BeginResult::kBusy, State::kWorking,
                       "MeshDecodeSession is busy");
  }

  DracoHeader header{};
  if (!ParseFixedHeader(data, size, &header)) {
    // Invalid fixed headers preserve the previous terminal/idle state.
    return BeginResult(BeginResult::kInvalidHeader, progress_.state,
                       "invalid Draco fixed header");
  }

  if (!FixedHeaderIsSupportedProfile(header)) {
    const char* message =
        header.version_major != 2 || header.version_minor != 2
            ? "only Draco bitstream version 2.2 is supported"
            : header.encoder_type != TRIANGULAR_MESH
                  ? "only triangular meshes are supported"
                  : header.encoder_method != MESH_EDGEBREAKER_ENCODING
                        ? "only Edgebreaker meshes are supported"
                        : "metadata is not supported by MeshDecodeSession";
    progress_ = TerminalProgress(State::kError, Phase::kHeader, message);
    impl_.reset();
    return BeginResult(BeginResult::kUnsupported, State::kError, message);
  }

  default_options_ = options;
  impl_.reset(new Impl(data, size, options, header));
  progress_ = impl_->progress();
  return BeginResult(BeginResult::kOk, State::kWorking, "");
}

MeshDecodeSession::BeginResult MeshDecodeSession::begin(
    const DecoderBuffer& buffer) {
  return begin(buffer.data_head(), static_cast<std::size_t>(
                                      std::max<int64_t>(0, buffer.remaining_size())));
}

MeshDecodeSession::BeginResult MeshDecodeSession::begin(
    const DecoderBuffer* buffer) {
  if (buffer == nullptr) {
    return BeginResult(BeginResult::kInvalidArgument, progress_.state,
                       "DecoderBuffer is null");
  }
  return begin(*buffer);
}

MeshDecodeSession::BeginResult MeshDecodeSession::begin(
    const EncoderBuffer& buffer) {
  return begin(buffer.data(), buffer.size());
}

MeshDecodeSession::BeginResult MeshDecodeSession::begin(
    const EncoderBuffer* buffer) {
  if (buffer == nullptr) {
    return BeginResult(BeginResult::kInvalidArgument, progress_.state,
                       "EncoderBuffer is null");
  }
  return begin(*buffer);
}

MeshDecodeSession::StepResult MeshDecodeSession::step(std::uint64_t budget) {
  if (!impl_) {
    StepResult result;
    result.state = progress_.state;
    result.phase = progress_.phase;
    result.budget = budget;
    result.ok = progress_.state != State::kError;
    result.error_message = progress_.error_message;
    return result;
  }
  StepResult result = impl_->RunStep(budget);
  progress_ = impl_->progress();
  return result;
}

const MeshDecodeSession::Progress& MeshDecodeSession::progress() const {
  if (impl_) {
    // progress() synchronizes aliases and state; const_cast keeps the public
    // getter naturally const while caching alias fields.
    return const_cast<Impl*>(impl_.get())->progress();
  }
  return progress_;
}

const Mesh* MeshDecodeSession::result() const {
  return impl_ ? impl_->result() : nullptr;
}

std::unique_ptr<Mesh> MeshDecodeSession::release_result() {
  if (!impl_) return nullptr;
  auto mesh = impl_->release_result();
  if (mesh) {
    progress_.state = State::kDone;
    // Leave impl progress done but result released.
  }
  return mesh;
}

void MeshDecodeSession::cancel() {
  if (impl_) impl_->Cancel();
  progress_.state = State::kCancelled;
  progress_.phase = Phase::kCancelled;
  impl_.reset();
}

void MeshDecodeSession::reset() {
  if (impl_) impl_->Cancel();
  impl_.reset();
  progress_ = Progress();
}

void MeshDecodeSession::SetSkipAttributeTransform(int attribute_type) {
  if (impl_) impl_->SetSkipTransform(attribute_type);
  if (attribute_type == GeometryAttribute::POSITION)
    default_options_.skip_position_transform = true;
  if (attribute_type == GeometryAttribute::TEX_COORD)
    default_options_.skip_tex_coord_transform = true;
}

void MeshDecodeSession::CopyDecoderOptions(
    const DecoderOptions& decoder_options) {
  if (impl_) impl_->CopyOptions(decoder_options);
}

}  // namespace draco
