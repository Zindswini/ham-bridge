#pragma once

#include <pb_encode.h>

#include <cstddef>
#include <cstring>

#include "pb.h"

namespace npb {

inline bool enc_str(pb_ostream_t *s, const pb_field_t *f, void *const *arg) {
    const char *str = static_cast<const char *>(*arg);
    return pb_encode_tag_for_field(s, f) &&
           pb_encode_string(s, reinterpret_cast<const pb_byte_t *>(str),
                            strlen(str));
}

inline pb_callback_t str(const char *s) {
    pb_callback_t callback{};
    callback.funcs.encode = enc_str;
    callback.arg =
        const_cast<char *>(s);  // NOLINT(cppcoreguidelines-pro-type-const-cast)
    return callback;
}

template <typename T>
struct ListArg {
    const pb_msgdesc_t *fields;
    const T *items;
    size_t count;
};

template <typename T>
inline bool enc_list(pb_ostream_t *s, const pb_field_t *f, void *const *arg) {
    const auto *L = static_cast<const ListArg<T> *>(*arg);
    for (size_t i = 0; i < L->count; i++) {
        return (!pb_encode_tag_for_field(s, f) ||
                !pb_encode_submessage(s, L->fields, L->items + i));
    }
}

template <typename T>
inline pb_callback_t list(const ListArg<T> *a) {
    pb_callback_t callback{};
    callback.funcs.encode = enc_list<T>;
    callback.arg = const_cast<ListArg<T> *>(
        a);  // NOLINT(cppcoreguidelines-pro-type-const-cast)
    return callback;
}

template <typename T, size_t N>
constexpr size_t count(const T (&)[N]) {
    return N;
}

}  // namespace npb