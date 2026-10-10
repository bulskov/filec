/* filec — the in-place normaliser shared by path.c and native.c (see
 * names.h). */

#include "names.h"

/* Removes the last name appended after floor — "a/b" -> "a", "/C:/x" ->
 * "/C:" — and returns true; false when there is none, or (relative) when it
 * is a ".." that must stay. */
static bool drop_last_name(strbuf_t *out, size_t floor)
{
    string_t v = strbuf_view(out);
    if (v.len == floor)
    {
        return false;
    }
    size_t start = v.len; /* start of the last name */
    while (start > floor && v.ptr[start - 1] != '/')
    {
        start--;
    }
    string_t last = string_slice(v, start, v.len);
    if (string_equals(last, STRING_LIT("..")))
    {
        return false;
    }
    /* cut the name and the '/' before it — but never the floor itself */
    strbuf_truncate(out, start > floor ? start - 1 : floor);
    return true;
}

static bool is_sep(char c, bool backslash_sep)
{
    return c == '/' || (backslash_sep && c == '\\');
}

seqc_status_t filec_append_names(
    strbuf_t *out,
    string_t src,
    bool backslash_sep,
    size_t floor,
    bool absolute)
{
    size_t i = 0;
    while (i <= src.len)
    {
        size_t j = i;
        while (j < src.len && !is_sep(src.ptr[j], backslash_sep))
        {
            j++;
        }
        string_t name = string_slice(src, i, j);
        i = j + 1;

        if (name.len == 0 || string_equals(name, STRING_LIT(".")))
        {
            continue;
        }
        if (string_equals(name, STRING_LIT("..")))
        {
            if (drop_last_name(out, floor) || absolute)
            {
                continue; /* removed a name, or stopped at the root */
            }
        }
        if (absolute || strbuf_len(out) > floor)
        {
            if (strbuf_append_char(out, '/') != SEQC_OK)
            {
                return SEQC_OOM;
            }
        }
        if (strbuf_append(out, name) != SEQC_OK)
        {
            return SEQC_OOM;
        }
    }
    return SEQC_OK;
}

seqc_status_t filec_finish_names(strbuf_t *out, size_t floor, bool absolute)
{
    if (strbuf_len(out) > floor)
    {
        return SEQC_OK;
    }
    return strbuf_append_char(out, absolute ? '/' : '.');
}
