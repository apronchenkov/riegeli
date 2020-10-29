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

#ifndef RIEGELI_BYTES_ZSTD_WRITER_H_
#define RIEGELI_BYTES_ZSTD_WRITER_H_

#include <stddef.h>

#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/optimization.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/types/optional.h"
#include "riegeli/base/base.h"
#include "riegeli/base/dependency.h"
#include "riegeli/base/recycling_pool.h"
#include "riegeli/base/resetter.h"
#include "riegeli/bytes/buffered_writer.h"
#include "riegeli/bytes/writer.h"
#include "zstd.h"

namespace riegeli {

// Template parameter independent part of `ZstdWriter`.
class ZstdWriterBase : public BufferedWriter {
 public:
  // Interpretation of dictionary data.
  enum class ContentType {
    // If dictionary data begin with `ZSTD_MAGIC_DICTIONARY`, then like
    // `kSerialized`, otherwise like `kRaw`.
    kAuto = 0,
    // Dictionary data should contain sequences that are commonly seen in the
    // data being compressed.
    kRaw = 1,
    // Prepared with the dictBuilder library.
    kSerialized = 2,
  };

  // Stores an optional Zstd dictionary for compression.
  //
  // An empty dictionary is equivalent to no dictionary.
  //
  // A `Dictionary` object can own the dictionary data, or can hold a pointer
  // to unowned dictionary data which must not be changed until the last
  // `ZstdWriter` using this dictionary is closed or no longer used.
  // A `Dictionary` object also holds prepared structures derived from
  // dictionary data. If the same dictionary is needed for multiple compression
  // sessions, the `Dictionary` object can be reused.
  //
  // The prepared dictionary depends on the compression level. At most one
  // prepared dictionary is cached, corresponding to the last compression level
  // used.
  //
  // Copying a `Dictionary` object is cheap, sharing the actual dictionary.
  class Dictionary {
   public:
    Dictionary() noexcept {}

    // Sets interpretation of dictionary data.
    //
    // Default: `ContentType::kAuto`
    Dictionary& set_content_type(ContentType content_type) & {
      content_type_ = content_type;
      cache_.Invalidate();
      return *this;
    }
    Dictionary&& set_content_type(ContentType content_type) && {
      return std::move(set_content_type(content_type));
    }
    ContentType content_type() const { return content_type_; }

    // Sets parameters to defaults.
    Dictionary& reset() & {
      content_type_ = ContentType::kAuto;
      owned_data_.reset();
      data_ = absl::string_view();
      cache_.Invalidate();
      return *this;
    }
    Dictionary&& reset() && { return std::move(reset()); }

    // Sets a dictionary.
    //
    // `std::string&&` is accepted with a template to avoid implicit conversions
    // to `std::string` which can be ambiguous against `absl::string_view`
    // (e.g. `const char*`).
    Dictionary& set_data(absl::string_view data) & {
      // TODO: When `absl::string_view` becomes C++17
      // `std::string_view`: std::make_shared<const std::string>(data).
      owned_data_ =
          std::make_shared<const std::string>(data.data(), data.size());
      data_ = *owned_data_;
      cache_.Invalidate();
      return *this;
    }
    template <typename Src,
              std::enable_if_t<std::is_same<Src, std::string>::value, int> = 0>
    Dictionary& set_data(Src&& data) & {
      // `std::move(data)` is correct and `std::forward<Src>(data)` is not
      // necessary: `Src` is always `std::string`, never an lvalue reference.
      owned_data_ = std::make_shared<const std::string>(std::move(data));
      data_ = *owned_data_;
      cache_.Invalidate();
      return *this;
    }
    Dictionary&& set_data(absl::string_view data) && {
      return std::move(set_data(data));
    }
    template <typename Src,
              std::enable_if_t<std::is_same<Src, std::string>::value, int> = 0>
    Dictionary&& set_data(Src&& data) && {
      // `std::move(data)` is correct and `std::forward<Src>(data)` is not
      // necessary: `Src` is always `std::string`, never an lvalue reference.
      return std::move(set_data(std::move(data)));
    }

    // Like `set_data()`, but does not take ownership of `data`, which must not
    // be changed until the last `ZstdWriter` using this dictionary is closed or
    // no longer used.
    Dictionary& set_data_unowned(absl::string_view data) & {
      owned_data_.reset();
      data_ = data;
      cache_.Invalidate();
      return *this;
    }
    Dictionary&& set_data_unowned(absl::string_view data) && {
      return std::move(set_data_unowned(data));
    }

    // Returns `true` if no dictionary is present.
    bool empty() const { return data_.empty(); }

    // Returns the dictionary data.
    absl::string_view data() const { return data_; }

   private:
    // Caches dictionary in the prepared form, to avoid preparing the dictionary
    // from the same data multiple times.
    class Cache {
     public:
      Cache() noexcept {}

      Cache(const Cache& that);
      Cache& operator=(const Cache& that);

      Cache(Cache&& that) noexcept;
      Cache& operator=(Cache&& that) noexcept;

      // Clears the cache.
      void Invalidate();

      // Returns the dictionary in the prepared form, or `nullptr` if
      // `ZSTD_createCDict_advanced()` failed.
      std::shared_ptr<const ZSTD_CDict> PrepareDictionary(
          absl::string_view dictionary, ContentType content_type,
          int compression_level) const;

     private:
      struct Shared;

      std::shared_ptr<Shared> EnsureShared() const;

      mutable absl::Mutex mutex_;
      // If multiple `Dictionary` objects are known to use the same dictionary,
      // `shared_` is present and shared between them.
      //
      // `shared_` is guarded by `mutex_` for const access. It is not guarded
      // for non-const access which is assumed to be exclusive.
      mutable std::shared_ptr<Shared> shared_;
    };

    friend class ZstdWriterBase;

    // Returns the dictionary in the prepared form, or `nullptr` if
    // `ZSTD_createCDict_advanced()` failed.
    std::shared_ptr<const ZSTD_CDict> PrepareDictionary(
        int compression_level) const;

    ContentType content_type_ = ContentType::kAuto;
    std::shared_ptr<const std::string> owned_data_;
    absl::string_view data_;
    Cache cache_;
  };

  class Options {
   public:
    Options() noexcept {}

    // Tunes the tradeoff between compression density and compression speed
    // (higher = better density but slower).
    //
    // `compression_level` must be between `kMinCompressionLevel` (-131072) and
    // `kMaxCompressionLevel` (22). Level 0 is currently equivalent to 3.
    // Default: `kDefaultCompressionLevel` (3).
    static constexpr int kMinCompressionLevel =
        -(1 << 17);                                  // `ZSTD_minCLevel()`
    static constexpr int kMaxCompressionLevel = 22;  // `ZSTD_maxCLevel()`
    static constexpr int kDefaultCompressionLevel = 3;
    Options& set_compression_level(int compression_level) & {
      RIEGELI_ASSERT_GE(compression_level, kMinCompressionLevel)
          << "Failed precondition of "
             "ZstdWriterBase::Options::set_compression_level(): "
             "compression level out of range";
      RIEGELI_ASSERT_LE(compression_level, kMaxCompressionLevel)
          << "Failed precondition of "
             "ZstdWriterBase::Options::set_compression_level()"
             "compression level out of range";
      compression_level_ = compression_level;
      return *this;
    }
    Options&& set_compression_level(int compression_level) && {
      return std::move(set_compression_level(compression_level));
    }
    int compression_level() const { return compression_level_; }

    // Logarithm of the LZ77 sliding window size. This tunes the tradeoff
    // between compression density and memory usage (higher = better density but
    // more memory).
    //
    // Special value `absl::nullopt` means to derive `window_log` from
    // `compression_level` and `size_hint`.
    //
    // `window_log` must be `absl::nullopt` or between `kMinWindowLog` (10) and
    // `kMaxWindowLog` (30 in 32-bit build, 31 in 64-bit build). Default:
    // `absl::nullopt`.
    static constexpr int kMinWindowLog = 10;  // `ZSTD_WINDOWLOG_MIN`
    static constexpr int kMaxWindowLog =
        sizeof(size_t) == 4 ? 30 : 31;  // `ZSTD_WINDOWLOG_MAX`
    Options& set_window_log(absl::optional<int> window_log) & {
      if (window_log != absl::nullopt) {
        RIEGELI_ASSERT_GE(*window_log, kMinWindowLog)
            << "Failed precondition of "
               "ZstdWriterBase::Options::set_window_log(): "
               "window log out of range";
        RIEGELI_ASSERT_LE(*window_log, kMaxWindowLog)
            << "Failed precondition of "
               "ZstdWriterBase::Options::set_window_log(): "
               "window log out of range";
      }
      window_log_ = window_log;
      return *this;
    }
    Options&& set_window_log(absl::optional<int> window_log) && {
      return std::move(set_window_log(window_log));
    }
    absl::optional<int> window_log() const { return window_log_; }

    // Zstd dictionary. The same dictionary must be used for decompression.
    //
    // Default: `Dictionary()`
    Options& set_dictionary(const Dictionary& dictionary) & {
      dictionary_ = dictionary;
      return *this;
    }
    Options& set_dictionary(Dictionary&& dictionary) & {
      dictionary_ = std::move(dictionary);
      return *this;
    }
    Options&& set_dictionary(const Dictionary& dictionary) && {
      return std::move(set_dictionary(dictionary));
    }
    Options&& set_dictionary(Dictionary&& dictionary) && {
      return std::move(set_dictionary(std::move(dictionary)));
    }
    Dictionary& dictionary() & { return dictionary_; }
    const Dictionary& dictionary() const& { return dictionary_; }
    Dictionary&& dictionary() && { return std::move(dictionary_); }
    const Dictionary&& dictionary() const&& { return std::move(dictionary_); }

    // Exact uncompressed size. This may improve compression density and
    // performance, and causes the size to be stored in the compressed stream
    // header.
    //
    // If the final size turns out to not match reality, compression fails.
    Options& set_final_size(absl::optional<Position> final_size) & {
      final_size_ = final_size;
      return *this;
    }
    Options&& set_final_size(absl::optional<Position> final_size) && {
      return std::move(set_final_size(final_size));
    }
    absl::optional<Position> final_size() const { return final_size_; }

    // Expected uncompressed size, or 0 if unknown. This may improve compression
    // density and performance.
    //
    // If the size hint turns out to not match reality, nothing breaks.
    //
    // `set_final_size()` overrides `set_size_hint()`.
    Options& set_size_hint(Position size_hint) & {
      size_hint_ = size_hint;
      return *this;
    }
    Options&& set_size_hint(Position size_hint) && {
      return std::move(set_size_hint(size_hint));
    }
    Position size_hint() const { return size_hint_; }

    // If `true`, computes checksum of uncompressed data and stores it in the
    // compressed stream. This lets decompression verify the checksum.
    //
    // Default: `false`
    Options& set_store_checksum(bool store_checksum) & {
      store_checksum_ = store_checksum;
      return *this;
    }
    Options&& set_store_checksum(bool store_checksum) && {
      return std::move(set_store_checksum(store_checksum));
    }
    bool store_checksum() const { return store_checksum_; }

    // Tunes how much data is buffered before calling the compression engine.
    //
    // Default: `ZSTD_CStreamInSize()`
    static size_t DefaultBufferSize() { return ZSTD_CStreamInSize(); }
    Options& set_buffer_size(size_t buffer_size) & {
      RIEGELI_ASSERT_GT(buffer_size, 0u)
          << "Failed precondition of "
             "ZstdWriterBase::Options::set_buffer_size(): "
             "zero buffer size";
      buffer_size_ = buffer_size;
      return *this;
    }
    Options&& set_buffer_size(size_t buffer_size) && {
      return std::move(set_buffer_size(buffer_size));
    }
    size_t buffer_size() const { return buffer_size_; }

   private:
    int compression_level_ = kDefaultCompressionLevel;
    absl::optional<int> window_log_;
    Dictionary dictionary_;
    absl::optional<Position> final_size_;
    Position size_hint_ = 0;
    bool store_checksum_ = false;
    size_t buffer_size_ = DefaultBufferSize();
  };

  // Returns the compressed `Writer`. Unchanged by `Close()`.
  virtual Writer* dest_writer() = 0;
  virtual const Writer* dest_writer() const = 0;

  // `ZstdWriterBase` overrides `Writer::Fail()` to annotate the status with
  // the current position, clarifying that this is the uncompressed position.
  // A status propagated from `*dest_writer()` might carry annotation with the
  // compressed position.
  using BufferedWriter::Fail;
  ABSL_ATTRIBUTE_COLD bool Fail(absl::Status status) override;

  bool Flush(FlushType flush_type) override;

 protected:
  ZstdWriterBase() noexcept {}

  explicit ZstdWriterBase(Dictionary&& dictionary, size_t buffer_size,
                          Position size_hint);

  ZstdWriterBase(ZstdWriterBase&& that) noexcept;
  ZstdWriterBase& operator=(ZstdWriterBase&& that) noexcept;

  void Reset();
  void Reset(Dictionary&& dictionary, size_t buffer_size, Position size_hint);
  void Initialize(Writer* dest, int compression_level,
                  absl::optional<int> window_log,
                  absl::optional<Position> final_size, Position size_hint,
                  bool store_checksum);

  void Done() override;
  bool WriteInternal(absl::string_view src) override;

 private:
  struct ZSTD_CCtxDeleter {
    void operator()(ZSTD_CCtx* ptr) const { ZSTD_freeCCtx(ptr); }
  };

  bool WriteInternal(absl::string_view src, Writer& dest,
                     ZSTD_EndDirective end_op);

  Dictionary dictionary_;
  std::shared_ptr<const ZSTD_CDict> prepared_dictionary_;
  RecyclingPool<ZSTD_CCtx, ZSTD_CCtxDeleter>::Handle compressor_;
};

// A `Writer` which compresses data with Zstd before passing it to another
// `Writer`.
//
// The `Dest` template parameter specifies the type of the object providing and
// possibly owning the compressed `Writer`. `Dest` must support
// `Dependency<Writer*, Dest>`, e.g. `Writer*` (not owned, default),
// `std::unique_ptr<Writer>` (owned), `ChainWriter<>` (owned).
//
// The compressed `Writer` must not be accessed until the `ZstdWriter` is closed
// or no longer used, except that it is allowed to read the destination of the
// compressed `Writer` immediately after `Flush()`.
template <typename Dest = Writer*>
class ZstdWriter : public ZstdWriterBase {
 public:
  // Creates a closed `ZstdWriter`.
  ZstdWriter() noexcept {}

  // Will write to the compressed `Writer` provided by `dest`.
  explicit ZstdWriter(const Dest& dest, Options options = Options());
  explicit ZstdWriter(Dest&& dest, Options options = Options());

  // Will write to the compressed `Writer` provided by a `Dest` constructed from
  // elements of `dest_args`. This avoids constructing a temporary `Dest` and
  // moving from it.
  template <typename... DestArgs>
  explicit ZstdWriter(std::tuple<DestArgs...> dest_args,
                      Options options = Options());

  ZstdWriter(ZstdWriter&& that) noexcept;
  ZstdWriter& operator=(ZstdWriter&& that) noexcept;

  // Makes `*this` equivalent to a newly constructed `ZstdWriter`. This avoids
  // constructing a temporary `ZstdWriter` and moving from it.
  void Reset();
  void Reset(const Dest& dest, Options options = Options());
  void Reset(Dest&& dest, Options options = Options());
  template <typename... DestArgs>
  void Reset(std::tuple<DestArgs...> dest_args, Options options = Options());

  // Returns the object providing and possibly owning the compressed `Writer`.
  // Unchanged by `Close()`.
  Dest& dest() { return dest_.manager(); }
  const Dest& dest() const { return dest_.manager(); }
  Writer* dest_writer() override { return dest_.get(); }
  const Writer* dest_writer() const override { return dest_.get(); }

 protected:
  void Done() override;

 private:
  // The object providing and possibly owning the compressed `Writer`.
  Dependency<Writer*, Dest> dest_;
};

// Support CTAD.
#if __cpp_deduction_guides
template <typename Dest>
ZstdWriter(Dest&& dest,
           ZstdWriterBase::Options options = ZstdWriterBase::Options())
    -> ZstdWriter<std::decay_t<Dest>>;
template <typename... DestArgs>
ZstdWriter(std::tuple<DestArgs...> dest_args,
           ZstdWriterBase::Options options = ZstdWriterBase::Options())
    -> ZstdWriter<void>;  // Delete.
#endif

// Implementation details follow.

inline ZstdWriterBase::Dictionary::Cache::Cache(const Cache& that)
    : shared_(that.EnsureShared()) {}

inline ZstdWriterBase::Dictionary::Cache&
ZstdWriterBase::Dictionary::Cache::operator=(const Cache& that) {
  shared_ = that.EnsureShared();
  return *this;
}

inline ZstdWriterBase::Dictionary::Cache::Cache(Cache&& that) noexcept
    : shared_(std::move(that.shared_)) {}

inline ZstdWriterBase::Dictionary::Cache&
ZstdWriterBase::Dictionary::Cache::operator=(Cache&& that) noexcept {
  shared_ = std::move(that.shared_);
  return *this;
}

inline void ZstdWriterBase::Dictionary::Cache::Invalidate() { shared_.reset(); }

inline ZstdWriterBase::ZstdWriterBase(Dictionary&& dictionary,
                                      size_t buffer_size, Position size_hint)
    : BufferedWriter(buffer_size, size_hint),
      dictionary_(std::move(dictionary)) {}

inline ZstdWriterBase::ZstdWriterBase(ZstdWriterBase&& that) noexcept
    : BufferedWriter(std::move(that)),
      // Using `that` after it was moved is correct because only the base class
      // part was moved.
      dictionary_(std::move(that.dictionary_)),
      prepared_dictionary_(std::move(that.prepared_dictionary_)),
      compressor_(std::move(that.compressor_)) {}

inline ZstdWriterBase& ZstdWriterBase::operator=(
    ZstdWriterBase&& that) noexcept {
  BufferedWriter::operator=(std::move(that));
  // Using `that` after it was moved is correct because only the base class part
  // was moved.
  dictionary_ = std::move(that.dictionary_);
  prepared_dictionary_ = std::move(that.prepared_dictionary_);
  compressor_ = std::move(that.compressor_);
  return *this;
}

inline void ZstdWriterBase::Reset() {
  BufferedWriter::Reset();
  dictionary_.reset();
  prepared_dictionary_.reset();
  compressor_.reset();
}

inline void ZstdWriterBase::Reset(Dictionary&& dictionary, size_t buffer_size,
                                  Position size_hint) {
  BufferedWriter::Reset(buffer_size, size_hint);
  dictionary_ = std::move(dictionary);
  prepared_dictionary_.reset();
  compressor_.reset();
}

template <typename Dest>
inline ZstdWriter<Dest>::ZstdWriter(const Dest& dest, Options options)
    : ZstdWriterBase(std::move(options.dictionary()), options.buffer_size(),
                     options.final_size().value_or(options.size_hint())),
      dest_(dest) {
  Initialize(dest_.get(), options.compression_level(), options.window_log(),
             options.final_size(),
             options.final_size().value_or(options.size_hint()),
             options.store_checksum());
}

template <typename Dest>
inline ZstdWriter<Dest>::ZstdWriter(Dest&& dest, Options options)
    : ZstdWriterBase(std::move(options.dictionary()), options.buffer_size(),
                     options.final_size().value_or(options.size_hint())),
      dest_(std::move(dest)) {
  Initialize(dest_.get(), options.compression_level(), options.window_log(),
             options.final_size(),
             options.final_size().value_or(options.size_hint()),
             options.store_checksum());
}

template <typename Dest>
template <typename... DestArgs>
inline ZstdWriter<Dest>::ZstdWriter(std::tuple<DestArgs...> dest_args,
                                    Options options)
    : ZstdWriterBase(std::move(options.dictionary()), options.buffer_size(),
                     options.final_size().value_or(options.size_hint())),
      dest_(std::move(dest_args)) {
  Initialize(dest_.get(), options.compression_level(), options.window_log(),
             options.final_size(),
             options.final_size().value_or(options.size_hint()),
             options.store_checksum());
}

template <typename Dest>
inline ZstdWriter<Dest>::ZstdWriter(ZstdWriter&& that) noexcept
    : ZstdWriterBase(std::move(that)),
      // Using `that` after it was moved is correct because only the base class
      // part was moved.
      dest_(std::move(that.dest_)) {}

template <typename Dest>
inline ZstdWriter<Dest>& ZstdWriter<Dest>::operator=(
    ZstdWriter&& that) noexcept {
  ZstdWriterBase::operator=(std::move(that));
  // Using `that` after it was moved is correct because only the base class part
  // was moved.
  dest_ = std::move(that.dest_);
  return *this;
}

template <typename Dest>
inline void ZstdWriter<Dest>::Reset() {
  ZstdWriterBase::Reset();
  dest_.Reset();
}

template <typename Dest>
inline void ZstdWriter<Dest>::Reset(const Dest& dest, Options options) {
  ZstdWriterBase::Reset(std::move(options.dictionary()), options.buffer_size(),
                        options.final_size().value_or(options.size_hint()));
  dest_.Reset(dest);
  Initialize(dest_.get(), options.compression_level(), options.window_log(),
             options.final_size(),
             options.final_size().value_or(options.size_hint()),
             options.store_checksum());
}

template <typename Dest>
inline void ZstdWriter<Dest>::Reset(Dest&& dest, Options options) {
  ZstdWriterBase::Reset(std::move(options.dictionary()), options.buffer_size(),
                        options.final_size().value_or(options.size_hint()));
  dest_.Reset(std::move(dest));
  Initialize(dest_.get(), options.compression_level(), options.window_log(),
             options.final_size(),
             options.final_size().value_or(options.size_hint()),
             options.store_checksum());
}

template <typename Dest>
template <typename... DestArgs>
inline void ZstdWriter<Dest>::Reset(std::tuple<DestArgs...> dest_args,
                                    Options options) {
  ZstdWriterBase::Reset(std::move(options.dictionary()), options.buffer_size(),
                        options.final_size().value_or(options.size_hint()));
  dest_.Reset(std::move(dest_args));
  Initialize(dest_.get(), options.compression_level(), options.window_log(),
             options.final_size(),
             options.final_size().value_or(options.size_hint()),
             options.store_checksum());
}

template <typename Dest>
void ZstdWriter<Dest>::Done() {
  ZstdWriterBase::Done();
  if (dest_.is_owning()) {
    if (ABSL_PREDICT_FALSE(!dest_->Close())) Fail(*dest_);
  }
}

template <typename Dest>
struct Resetter<ZstdWriter<Dest>> : ResetterByReset<ZstdWriter<Dest>> {};

}  // namespace riegeli

#endif  // RIEGELI_BYTES_ZSTD_WRITER_H_
