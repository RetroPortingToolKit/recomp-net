/* rnet_lobby_json.c -- span-bounded JSON reader for the lobby client. */
#include "lobby/rnet_lobby_json.h"
#include "recomp_net/lobby_client.h"

#include <stdlib.h>
#include <string.h>

RNetJsonSpan rnet_json_span(const char *s)
{
    RNetJsonSpan sp;
    sp.p = s ? s : "";
    sp.n = s ? strlen(s) : 0;
    return sp;
}

RNetJsonSpan rnet_json_span_n(const char *s, size_t n)
{
    RNetJsonSpan sp;
    sp.p = s ? s : "";
    sp.n = s ? n : 0;
    return sp;
}

static int is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static size_t skip_ws(const char *p, size_t i, size_t n)
{
    while (i < n && is_ws(p[i]))
        ++i;
    return i;
}

/* p[i] == '"'. Returns the index just past the closing quote, or n+1 on an
 * unterminated string. */
static size_t skip_string(const char *p, size_t i, size_t n)
{
    ++i;
    while (i < n) {
        if (p[i] == '\\') {
            i += 2;
            continue;
        }
        if (p[i] == '"')
            return i + 1;
        ++i;
    }
    return n + 1;
}

/* Skips one value starting at p[i] (no leading whitespace). Returns the index
 * just past it, or n+1 when malformed / truncated. */
static size_t skip_value(const char *p, size_t i, size_t n)
{
    if (i >= n)
        return n + 1;
    if (p[i] == '"')
        return skip_string(p, i, n);
    if (p[i] == '{' || p[i] == '[') {
        int depth = 0;
        while (i < n) {
            char c = p[i];
            if (c == '"') {
                i = skip_string(p, i, n);
                if (i > n)
                    return n + 1;
                continue;
            }
            if (c == '{' || c == '[') {
                ++depth;
            } else if (c == '}' || c == ']') {
                --depth;
                if (depth == 0)
                    return i + 1;
            }
            ++i;
        }
        return n + 1;
    }
    /* Scalar: number, true, false, null. */
    {
        size_t start = i;
        while (i < n && p[i] != ',' && p[i] != '}' && p[i] != ']' &&
               !is_ws(p[i]))
            ++i;
        return i > start ? i : n + 1;
    }
}

int rnet_json_members_next(RNetJsonSpan *iter, RNetJsonSpan *key,
                           RNetJsonSpan *val, RNetJsonSpan *whole)
{
    const char *p;
    size_t n, i, ks, ke, vs, ve;
    if (!iter || !iter->p)
        return 0;
    p = iter->p;
    n = iter->n;
    i = skip_ws(p, 0, n);
    /* The first call sees the object's '{'; later calls see ",..." */
    if (i < n && p[i] == '{')
        i = skip_ws(p, i + 1, n);
    if (i < n && p[i] == ',')
        i = skip_ws(p, i + 1, n);
    if (i >= n || p[i] != '"') {
        iter->p += n;
        iter->n = 0;
        return 0;
    }
    ks = i + 1;
    i = skip_string(p, i, n);
    if (i > n) {
        iter->p += n;
        iter->n = 0;
        return 0;
    }
    ke = i - 1;
    i = skip_ws(p, i, n);
    if (i >= n || p[i] != ':') {
        iter->p += n;
        iter->n = 0;
        return 0;
    }
    i = skip_ws(p, i + 1, n);
    vs = i;
    ve = skip_value(p, i, n);
    if (ve > n) {
        iter->p += n;
        iter->n = 0;
        return 0;
    }
    if (key) {
        key->p = p + ks;
        key->n = ke - ks;
    }
    if (val) {
        val->p = p + vs;
        val->n = ve - vs;
    }
    if (whole) {
        whole->p = p + ks - 1;
        whole->n = ve - (ks - 1);
    }
    iter->p = p + ve;
    iter->n = n - ve;
    return 1;
}

int rnet_json_find(RNetJsonSpan obj, const char *key, RNetJsonSpan *val)
{
    RNetJsonSpan it = obj, k, v;
    size_t klen;
    if (!key)
        return 0;
    klen = strlen(key);
    while (rnet_json_members_next(&it, &k, &v, NULL)) {
        if (k.n == klen && memcmp(k.p, key, klen) == 0) {
            if (val)
                *val = v;
            return 1;
        }
    }
    return 0;
}

char rnet_json_kind(RNetJsonSpan v)
{
    size_t i = skip_ws(v.p, 0, v.n);
    return i < v.n ? v.p[i] : '\0';
}

static unsigned hex4(const char *p, int *ok)
{
    unsigned v = 0;
    int i;
    for (i = 0; i < 4; ++i) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else { *ok = 0; return 0; }
    }
    return v;
}

/* Appends one UTF-8 sequence. Returns 0 when it did not fit (nothing
 * written: a code point is never split). */
static int put_utf8(char *out, size_t cap, size_t *o, unsigned cp)
{
    char buf[4];
    size_t len, k;
    if (cp < 0x80) { buf[0] = (char)cp; len = 1; }
    else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        len = 2;
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        len = 3;
    } else {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        len = 4;
    }
    if (*o + len + 1 > cap)
        return 0;
    for (k = 0; k < len; ++k)
        out[(*o)++] = buf[k];
    return 1;
}

int rnet_json_unescape(RNetJsonSpan v, char *out, size_t cap)
{
    size_t i, n = v.n, o = 0;
    const char *p = v.p;
    if (out && cap)
        out[0] = '\0';
    if (!out || cap == 0)
        return 0;
    i = skip_ws(p, 0, n);
    if (i >= n || p[i] != '"')
        return 0;
    ++i;
    while (i < n && p[i] != '"') {
        unsigned cp;
        if (p[i] == '\\' && i + 1 < n) {
            char e = p[i + 1];
            i += 2;
            switch (e) {
            case 'n': cp = '\n'; break;
            case 'r': cp = '\r'; break;
            case 't': cp = '\t'; break;
            case 'b': cp = '\b'; break;
            case 'f': cp = '\f'; break;
            case 'u': {
                int ok = 1;
                if (i + 4 > n) { out[o] = '\0'; return 1; }
                cp = hex4(p + i, &ok);
                if (!ok) cp = '?';
                i += 4;
                if (ok && cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= n &&
                    p[i] == '\\' && p[i + 1] == 'u') {
                    int ok2 = 1;
                    unsigned lo = hex4(p + i + 2, &ok2);
                    if (ok2 && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                }
                if (cp >= 0xD800 && cp <= 0xDFFF)
                    cp = 0xFFFD; /* lone surrogate */
                break;
            }
            default: cp = (unsigned char)e; break; /* " \\ / */
            }
            if (!put_utf8(out, cap, &o, cp)) {
                out[o] = '\0';
                return -1;
            }
            continue;
        }
        if (o + 2 > cap) {
            out[o] = '\0';
            return -1;
        }
        out[o++] = p[i++];
    }
    out[o] = '\0';
    return 1;
}

int rnet_json_str(RNetJsonSpan obj, const char *key, char *out, size_t cap)
{
    RNetJsonSpan v;
    if (out && cap)
        out[0] = '\0';
    if (!rnet_json_find(obj, key, &v))
        return 0;
    return rnet_json_unescape(v, out, cap);
}

long long rnet_json_i64(RNetJsonSpan obj, const char *key, long long def)
{
    RNetJsonSpan v;
    char buf[32];
    char *end = NULL;
    long long r;
    size_t len;
    if (!rnet_json_find(obj, key, &v))
        return def;
    if (v.n == 0)
        return def;
    if (!((v.p[0] >= '0' && v.p[0] <= '9') || v.p[0] == '-'))
        return def;
    len = v.n < sizeof(buf) - 1 ? v.n : sizeof(buf) - 1;
    memcpy(buf, v.p, len);
    buf[len] = '\0';
    r = strtoll(buf, &end, 10);
    if (end == buf)
        return def;
    return r;
}

int rnet_json_int(RNetJsonSpan obj, const char *key, int def)
{
    long long r = rnet_json_i64(obj, key, (long long)def);
    if (r > 2147483647LL) return 2147483647;
    if (r < -2147483647LL - 1) return -2147483647 - 1;
    return (int)r;
}

int rnet_json_bool(RNetJsonSpan obj, const char *key, int def)
{
    RNetJsonSpan v;
    if (!rnet_json_find(obj, key, &v))
        return def;
    if (v.n >= 4 && memcmp(v.p, "true", 4) == 0)
        return 1;
    if (v.n >= 5 && memcmp(v.p, "false", 5) == 0)
        return 0;
    if (v.n > 0 && ((v.p[0] >= '0' && v.p[0] <= '9') || v.p[0] == '-'))
        return rnet_json_int(obj, key, 0) != 0;
    return def;
}

int rnet_json_obj(RNetJsonSpan obj, const char *key, RNetJsonSpan *out)
{
    RNetJsonSpan v;
    if (!rnet_json_find(obj, key, &v) || rnet_json_kind(v) != '{')
        return 0;
    if (out)
        *out = v;
    return 1;
}

int rnet_json_arr(RNetJsonSpan obj, const char *key, RNetJsonSpan *out)
{
    RNetJsonSpan v;
    if (!rnet_json_find(obj, key, &v) || rnet_json_kind(v) != '[')
        return 0;
    if (out)
        *out = v;
    return 1;
}

int rnet_json_arr_next(RNetJsonSpan *iter, RNetJsonSpan *elem)
{
    const char *p;
    size_t n, i, e;
    if (!iter || !iter->p)
        return 0;
    p = iter->p;
    n = iter->n;
    i = skip_ws(p, 0, n);
    if (i < n && p[i] == '[')
        i = skip_ws(p, i + 1, n);
    if (i < n && p[i] == ',')
        i = skip_ws(p, i + 1, n);
    if (i >= n || p[i] == ']') {
        iter->p += n;
        iter->n = 0;
        return 0;
    }
    e = skip_value(p, i, n);
    if (e > n) {
        iter->p += n;
        iter->n = 0;
        return 0;
    }
    if (elem) {
        elem->p = p + i;
        elem->n = e - i;
    }
    iter->p = p + e;
    iter->n = n - e;
    return 1;
}

int rnet_json_copy(RNetJsonSpan v, char *out, size_t cap)
{
    if (!out || cap == 0)
        return 0;
    if (v.n + 1 > cap) {
        out[0] = '\0';
        return 0;
    }
    memcpy(out, v.p, v.n);
    out[v.n] = '\0';
    return 1;
}

size_t rnet_json_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    if (!out || cap == 0)
        return 0;
    if (!in) {
        out[0] = '\0';
        return 0;
    }
    while (*in && o + 1 < cap) {
        unsigned char c = (unsigned char)*in;
        char esc = 0;
        if (c >= 0xC0) {
            /* A multi-byte sequence goes in whole or not at all: a split one
             * is invalid UTF-8, which ends a WebSocket connection. */
            size_t need = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
            size_t k;
            for (k = 1; k < need; ++k)
                if (((unsigned char)in[k] & 0xC0) != 0x80)
                    break;
            if (k == need) {
                if (o + need + 1 > cap)
                    break;
                memcpy(out + o, in, need);
                o += need;
                in += need;
                continue;
            }
        }
        ++in;
        if (c == '"' || c == '\\') esc = (char)c;
        else if (c == '\n') esc = 'n';
        else if (c == '\r') esc = 'r';
        else if (c == '\t') esc = 't';
        else if (c < 0x20) continue;
        if (esc) {
            if (o + 3 > cap)
                break;
            out[o++] = '\\';
            out[o++] = esc;
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    return o;
}

/* ── public wrappers ─────────────────────────────────────────────────────── */

int rnet_lobby_json_get_str(const char *json, const char *key, char *out,
                            size_t cap)
{
    return rnet_json_str(rnet_json_span(json), key, out, cap);
}

int rnet_lobby_json_get_int(const char *json, const char *key, int def)
{
    return rnet_json_int(rnet_json_span(json), key, def);
}

int rnet_lobby_json_get_bool(const char *json, const char *key, int def)
{
    return rnet_json_bool(rnet_json_span(json), key, def);
}

int rnet_lobby_json_get_raw(const char *json, const char *key, char *out,
                            size_t cap)
{
    RNetJsonSpan v;
    if (out && cap)
        out[0] = '\0';
    if (!rnet_json_find(rnet_json_span(json), key, &v))
        return 0;
    return rnet_json_copy(v, out, cap);
}

size_t rnet_lobby_json_escape(const char *in, char *out, size_t cap)
{
    return rnet_json_escape(in, out, cap);
}
