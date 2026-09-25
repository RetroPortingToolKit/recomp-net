/* lobby_json_test -- the lobby client's JSON reader.
 *
 * The engine copies read keys with strstr("\"key\""), which answers for a key
 * at ANY depth and inside string values, and clipped rows into fixed chunks.
 * These cases pin the replacement: top-level only, span-bounded, escapes
 * decoded, truncation reported, malformed input survived. */
#include "recomp_net/lobby_client.h"
#include "lobby/rnet_lobby_json.h"

#include <stdio.h>
#include <string.h>

static int g_fail;

static void ck(int cond, const char *what)
{
    if (!cond) {
        printf("FAIL: %s\n", what);
        g_fail++;
    }
}

static void case_top_level_only(void)
{
    const char *j =
        "{\"op\":\"automatch_found\",\"opponent\":{\"handle\":\"Marisa\","
        "\"country\":\"JP\"},\"note\":\"\\\"country\\\":\\\"XX\\\"\","
        "\"country\":\"DE\"}";
    char out[64];
    ck(rnet_lobby_json_get_str(j, "country", out, sizeof(out)) == 1 &&
           !strcmp(out, "DE"),
       "a nested object's key does not answer for the frame's own");
    ck(rnet_lobby_json_get_str(j, "handle", out, sizeof(out)) == 0 && !out[0],
       "a key that exists only nested is absent at top level");
    ck(rnet_lobby_json_get_raw(j, "opponent", out, sizeof(out)) == 1 &&
           out[0] == '{',
       "the nested object is reachable as a raw value");
    {
        char handle[16];
        ck(rnet_lobby_json_get_str(out, "handle", handle, sizeof(handle)) == 1 &&
               !strcmp(handle, "Marisa"),
           "and readable once extracted");
    }
}

static void case_member_list(void)
{
    /* RNetLobbyMatchCaps.game_json has no braces. */
    const char *g = "\"widescreen\":true,\"ws_extra\":12,\"pad\":[0,1],"
                    "\"lang\":\"en\"";
    char out[16];
    ck(rnet_lobby_json_get_bool(g, "widescreen", 0) == 1, "bool in a member list");
    ck(rnet_lobby_json_get_int(g, "ws_extra", -1) == 12, "int in a member list");
    ck(rnet_lobby_json_get_str(g, "lang", out, sizeof(out)) == 1 &&
           !strcmp(out, "en"),
       "string after an array member");
    ck(rnet_lobby_json_get_int(g, "missing", 77) == 77, "absent -> default");
    ck(rnet_lobby_json_get_int(g, "lang", 5) == 5, "a string is not an int");
    ck(rnet_lobby_json_get_bool(g, "ws_extra", 0) == 1, "a nonzero number is true");
}

static void case_escapes(void)
{
    const char *j = "{\"a\":\"line\\nquote\\\"slash\\/\\\\\","
                    "\"u\":\"caf\\u00e9 \\u65e5\","
                    "\"s\":\"\\ud83d\\ude00\",\"lone\":\"x\\ud800y\"}";
    char out[64];
    ck(rnet_lobby_json_get_str(j, "a", out, sizeof(out)) == 1 &&
           !strcmp(out, "line\nquote\"slash/\\"),
       "simple escapes decode");
    ck(rnet_lobby_json_get_str(j, "u", out, sizeof(out)) == 1 &&
           !strcmp(out, "caf\xc3\xa9 \xe6\x97\xa5"),
       "\\u escapes become UTF-8 (the engine copies emitted a literal 'u')");
    ck(rnet_lobby_json_get_str(j, "s", out, sizeof(out)) == 1 &&
           !strcmp(out, "\xf0\x9f\x98\x80"),
       "a surrogate pair becomes one 4-byte code point");
    ck(rnet_lobby_json_get_str(j, "lone", out, sizeof(out)) == 1 &&
           !strcmp(out, "x\xef\xbf\xbdy"),
       "a lone surrogate becomes U+FFFD");
}

static void case_truncation(void)
{
    char out[6];
    ck(rnet_lobby_json_get_str("{\"k\":\"abcdefgh\"}", "k", out, sizeof(out)) == -1 &&
           !strcmp(out, "abcde"),
       "a value that does not fit reports -1 and stays terminated");
    ck(rnet_lobby_json_get_str("{\"k\":\"ab\\u00e9\\u00e9\"}", "k", out, 5) == -1 &&
           !strcmp(out, "ab\xc3\xa9"),
       "a multi-byte code point is never split");
    ck(rnet_lobby_json_get_raw("{\"k\":[1,2,3,4,5]}", "k", out, sizeof(out)) == 0 &&
           !out[0],
       "a raw value that does not fit is refused whole");
}

static void case_malformed(void)
{
    char out[16];
    const char *bad[] = {
        "", "{", "{\"a\"", "{\"a\":", "{\"a\":\"unterminated", "[1,2]",
        "{\"a\":{\"b\":1}", "{\"a\" 1}", "{\"a\":1,,\"b\":2}", "not json"
    };
    size_t i;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        out[0] = 'x';
        (void)rnet_lobby_json_get_str(bad[i], "a", out, sizeof(out));
        (void)rnet_lobby_json_get_int(bad[i], "b", 0);
    }
    ck(1, "malformed inputs do not crash");
    ck(rnet_lobby_json_get_int("{\"a\":1,\"b\":{\"c\":\"}\"},\"d\":4}", "d", 0) == 4,
       "a brace inside a nested string does not end the object");
}

static void case_arrays(void)
{
    const char *j = "{\"rows\":[{\"id\":\"a\"}, {\"id\":\"b,]\"} ,\"s\",3,[1,[2]],"
                    "{\"id\":\"c\"}]}";
    RNetJsonSpan arr, e;
    int n = 0, objs = 0;
    char id[8];
    ck(rnet_json_arr(rnet_json_span(j), "rows", &arr) == 1, "array member found");
    while (rnet_json_arr_next(&arr, &e)) {
        ++n;
        if (rnet_json_kind(e) == '{') {
            ++objs;
            rnet_json_str(e, "id", id, sizeof(id));
        }
    }
    ck(n == 6, "every element is visited, nested arrays whole");
    ck(objs == 3, "objects among them");
    ck(!strcmp(id, "c"), "the last object parsed");
    ck(rnet_json_arr(rnet_json_span("{\"rows\":\"[]\"}"), "rows", &arr) == 0,
       "a string that looks like an array is not one");
}

static void case_members_iter(void)
{
    RNetJsonSpan it = rnet_json_span("{\"a\":1, \"b\" : {\"x\":[1]} ,\"c\":\"z\"}");
    RNetJsonSpan k, v, w;
    char keys[16];
    size_t o = 0;
    while (rnet_json_members_next(&it, &k, &v, &w) && o + k.n < sizeof(keys)) {
        memcpy(keys + o, k.p, k.n);
        o += k.n;
    }
    keys[o] = '\0';
    ck(!strcmp(keys, "abc"), "member iteration visits each top-level key once");
}

static void case_escape(void)
{
    char out[64];
    size_t n = rnet_lobby_json_escape("a\"b\\c\nd\te\x01" "f", out, sizeof(out));
    ck(!strcmp(out, "a\\\"b\\\\c\\nd\\te" "f"), "escaper output");
    ck(n == strlen(out), "escaper returns the length");
    n = rnet_lobby_json_escape("ab\xe6\x97\xa5", out, 5);
    ck(!strcmp(out, "ab"), "the escaper never splits a UTF-8 sequence at the cap");
    n = rnet_lobby_json_escape("\"\"\"\"", out, 6);
    ck(!strcmp(out, "\\\"\\\""), "an escape pair is never split at the cap");
    (void)n;
}

int main(void)
{
    case_top_level_only();
    case_member_list();
    case_escapes();
    case_truncation();
    case_malformed();
    case_arrays();
    case_members_iter();
    case_escape();
    printf(g_fail ? "%d failure(s)\n" : "lobby_json_test: all passed\n", g_fail);
    return g_fail != 0;
}
