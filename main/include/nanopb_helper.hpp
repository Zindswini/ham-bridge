#pragma once

#include <pb_encode.h>

#include <array>
#include <cstddef>
#include <cstring>

#include "pb.h"

namespace npb {

// String Helpers
inline bool enc_str(pb_ostream_t *s, const pb_field_t *f, void *const *arg) {
  const char *str = static_cast<const char *>(*arg);
  return pb_encode_tag_for_field(s, f) && pb_encode_string(s, reinterpret_cast<const pb_byte_t *>(str), strlen(str));
}

constexpr pb_callback_t str(const char *s) {
  pb_callback_t callback{};
  callback.funcs.encode = enc_str;
  // nanopb has arg as opaque void*. Callback immediately readds const
  callback.arg = const_cast<char *>(s);  // NOLINT(cppcoreguidelines-pro-type-const-cast)
  return callback;
}

// List Helpers
template <typename T, size_t N>
struct ListArg {
  const pb_msgdesc_t *fields;
  const std::array<T, N> *items;
};

template <typename T, size_t N>
inline bool enc_list(pb_ostream_t *s, const pb_field_t *f, void *const *arg) {
  const auto *L = static_cast<const ListArg<T, N> *>(*arg);
  for (const T &item : *L->items) {
    if (!pb_encode_tag_for_field(s, f) || !pb_encode_submessage(s, L->fields, &item)) {
      return false;
    }
  }
  return true;
}

template <typename T, size_t N>
constexpr pb_callback_t list(const ListArg<T, N> *a) {
  pb_callback_t callback{};
  callback.funcs.encode = enc_list<T, N>;
  callback.arg = const_cast<ListArg<T, N> *>(a);  // NOLINT(cppcoreguidelines-pro-type-const-cast)
  return callback;
}

// Repeated enum/varint Helpers
template <typename T, size_t N>
inline bool enc_enum_list(pb_ostream_t *s, const pb_field_t *f, void *const *arg) {
  const auto *items = static_cast<const std::array<T, N> *>(*arg);
  for (const T item : *items) {
    // Sign-extend through int64_t: negative enum values must be encoded as
    // 10-byte two's-complement varints.
    if (!pb_encode_tag_for_field(s, f) || !pb_encode_varint(s, static_cast<uint64_t>(static_cast<int64_t>(item)))) {
      return false;
    }
  }
  return true;
}

template <typename T, size_t N>
constexpr pb_callback_t enum_list(const std::array<T, N> *items) {
  pb_callback_t callback{};
  callback.funcs.encode = enc_enum_list<T, N>;
  callback.arg = const_cast<std::array<T, N> *>(items);  // NOLINT(cppcoreguidelines-pro-type-const-cast)
  return callback;
}

}  // namespace npb