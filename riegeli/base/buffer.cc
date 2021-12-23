// Copyright 2017 Google LLC
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

#include "riegeli/base/buffer.h"

#include <functional>
#include <utility>

#include "absl/strings/cord.h"
#include "absl/strings/string_view.h"
#include "riegeli/base/base.h"

namespace riegeli {

absl::Cord Buffer::ToCord(absl::string_view substr) {
  RIEGELI_ASSERT(std::greater_equal<>()(substr.data(), data()))
      << "Failed precondition of Buffer::ToCord(): "
         "substring not contained in the buffer";
  RIEGELI_ASSERT(
      std::less_equal<>()(substr.data() + substr.size(), data() + capacity()))
      << "Failed precondition of Buffer::ToCord(): "
         "substring not contained in the buffer";
  if (substr.size() <= 15 /* `absl::Cord::InlineRep::kMaxInline` */ ||
      Wasteful(capacity(), substr.size())) {
    return MakeFlatCord(substr);
  }

  struct Releaser {
    void operator()() const {
      // Nothing to do: the destructor does the work.
    }
    Buffer buffer;
  };
  return absl::MakeCordFromExternal(substr, Releaser{std::move(*this)});
}

}  // namespace riegeli
