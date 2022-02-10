// Copyright 2018 Google LLC
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

#ifndef RIEGELI_RECORDS_CHUNK_WRITER_DEPENDENCY_H_
#define RIEGELI_RECORDS_CHUNK_WRITER_DEPENDENCY_H_

#include <tuple>
#include <type_traits>
#include <utility>

#include "riegeli/base/dependency.h"
#include "riegeli/base/object.h"
#include "riegeli/bytes/writer.h"
#include "riegeli/records/chunk_writer.h"

namespace riegeli {

// Specialization of `DependencyImpl<ChunkWriter*, M>` adapted from
// `DependencyImpl<Writer*, M>` by wrapping `M` in `DefaultChunkWriter<M>`.
template <typename M>
class DependencyImpl<ChunkWriter*, M,
                     std::enable_if_t<IsValidDependency<Writer*, M>::value>> {
 public:
  DependencyImpl() noexcept : chunk_writer_(kClosed) {}

  explicit DependencyImpl(const M& manager) : chunk_writer_(manager) {}
  explicit DependencyImpl(M&& manager) : chunk_writer_(std::move(manager)) {}

  template <typename... MArgs>
  explicit DependencyImpl(std::tuple<MArgs...> manager_args)
      : chunk_writer_(std::move(manager_args)) {}

  DependencyImpl(DependencyImpl&& that) noexcept
      : chunk_writer_(std::move(that.chunk_writer_)) {}
  DependencyImpl& operator=(DependencyImpl&& that) noexcept {
    chunk_writer_ = std::move(that.chunk_writer_);
    return *this;
  }

  void Reset() { chunk_writer_.Reset(kClosed); }

  void Reset(const M& manager) { chunk_writer_.Reset(manager); }
  void Reset(M&& manager) { chunk_writer_.Reset(std::move(manager)); }

  template <typename... MArgs>
  void Reset(std::tuple<MArgs...> manager_args) {
    chunk_writer_.Reset(std::move(manager_args));
  }

  M& manager() { return chunk_writer_.dest(); }
  const M& manager() const { return chunk_writer_.dest(); }

  DefaultChunkWriter<M>* get() { return &chunk_writer_; }
  const DefaultChunkWriter<M>* get() const { return &chunk_writer_; }
  DefaultChunkWriter<M>* Release() { return nullptr; }

  bool is_owning() const { return true; }
  static constexpr bool kIsStable = false;

 private:
  DefaultChunkWriter<M> chunk_writer_;
};

}  // namespace riegeli

#endif  // RIEGELI_RECORDS_CHUNK_WRITER_DEPENDENCY_H_
