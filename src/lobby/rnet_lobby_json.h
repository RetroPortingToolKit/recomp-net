/* rnet_lobby_json.h -- the lobby client's JSON reader (private).
 *
 * The server speaks serde_json; the client needs to read a handful of known
 * keys out of each message. The three engine copies did that with strstr on
 * `"key"`, which answers for a key at ANY depth -- a "country" inside a
 * nested "opponent" object answered for the frame's own -- and copied each row
 * into a fixed chunk first, silently clipping long rows (a seat row carrying a
 * full mod offer parsed as "has nothing"). This walks the real structure,
 * bounded by a span, so neither can happen.
 */
#ifndef RNET_LOBBY_JSON_H
#define RNET_LOBBY_JSON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RNetJsonSpan {
    const char *p;
    size_t n;
} RNetJsonSpan;

RNetJsonSpan rnet_json_span(const char *s);
RNetJsonSpan rnet_json_span_n(const char *s, size_t n);

/* Top-level member `key` of an object span ("{...}") or a bare member list.
 * *val receives the value's exact text. 1 found, 0 not. */
int rnet_json_find(RNetJsonSpan obj, const char *key, RNetJsonSpan *val);

/* First significant character of a value ('{', '[', '"', 't', digit...). */
char rnet_json_kind(RNetJsonSpan v);

/* 1 copied whole, 0 absent / not a string (out ""), -1 truncated. */
int rnet_json_str(RNetJsonSpan obj, const char *key, char *out, size_t cap);
/* Unescape a string VALUE span ("..."). Same returns. */
int rnet_json_unescape(RNetJsonSpan v, char *out, size_t cap);
int rnet_json_int(RNetJsonSpan obj, const char *key, int def);
long long rnet_json_i64(RNetJsonSpan obj, const char *key, long long def);
int rnet_json_bool(RNetJsonSpan obj, const char *key, int def);
/* Object-valued member. 1 found and is an object. */
int rnet_json_obj(RNetJsonSpan obj, const char *key, RNetJsonSpan *out);
/* Array-valued member. 1 found and is an array. */
int rnet_json_arr(RNetJsonSpan obj, const char *key, RNetJsonSpan *out);
/* Iterate an array span: *iter starts as the array value ("[...]"); each
 * call yields the next element. 1 yielded, 0 end / malformed. */
int rnet_json_arr_next(RNetJsonSpan *iter, RNetJsonSpan *elem);
/* Verbatim copy. 1 fits, 0 does not (out "" then). */
int rnet_json_copy(RNetJsonSpan v, char *out, size_t cap);

/* Iterate the members of an object span: *iter starts as the object; yields
 * each member's key span (without quotes, still escaped), value span, and the
 * whole "key":value text. 1 yielded, 0 end. */
int rnet_json_members_next(RNetJsonSpan *iter, RNetJsonSpan *key,
                           RNetJsonSpan *val, RNetJsonSpan *whole);

/* The escaper, and the buffer its worst case needs. */
size_t rnet_json_escape(const char *in, char *out, size_t cap);
#define RNET_JSON_ESC_CAP(n) ((n) * 2 + 8)

#ifdef __cplusplus
}
#endif

#endif /* RNET_LOBBY_JSON_H */
