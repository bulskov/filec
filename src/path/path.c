/* filec/path — lexical manipulation of canonical paths.  No OS code and no
 * platform: the same file, and the same tests, everywhere. */

#include "filec/path.h"

#include "names.h"
#include "seqc/string.h"

static bool next_component(string_t *rest, string_t *part)
{
    while (string_split_next(rest, '/', part))
    {
        if (part->len > 0)
        {
            return true;
        }
    }
    return false;
}

bool path_is_absolute(string_t p)
{
    return p.ptr && p.len > 0 && p.ptr[0] == '/';
}

/* p without its last component.  The parent of "/" is "/"; of a single
 * relative component (and of ".") it is ".".  Purely lexical: the parent
 * of ".." is ".", even though the real parent is "../..". */
string_t path_parent(string_t p)
{
    if (p.len == 0)
    {
        return (string_t){0, 0};
    }
    if (p.len == 2 && p.ptr[0] == '.' && p.ptr[1] == '.')
    {
        return (string_t){p.ptr, 1};
    }
    if (p.len == 1 && (p.ptr[0] == '.' || p.ptr[0] == '/'))
    {
        return p;
    }
    size_t idx = string_rfind_char(p, '/');
    if (idx == STRING_NOT_FOUND)
    {

        return STRING_LIT(".");
    }
    if (idx == 0)
    {
        return (string_t){p.ptr, 1};
    }
    return (string_t){p.ptr, idx};
}

/* The last component; "" for "/" and ".". */
string_t path_file_name(string_t p)
{
    if (p.len == 1 && (p.ptr[0] == '/' || p.ptr[0] == '.'))
    {
        return (string_t){p.ptr, 0};
    }
    size_t idx = string_rfind_char(p, '/');
    if (idx != STRING_NOT_FOUND)
    {
        return (string_t){p.ptr + idx + 1, p.len - idx - 1};
    }
    return p;
}

/* file_name without its extension. */
string_t path_stem(string_t p)
{
    string_t fn = path_file_name(p);
    size_t idx = string_rfind_char(fn, '.');
    if (idx != STRING_NOT_FOUND && idx != 0 && idx != fn.len - 1)
    {
        return (string_t){fn.ptr, idx};
    }
    return fn;
}

/* What follows the last '.' of file_name, without the dot.  "" when there
 * is no dot, when the only dot is the first character (".bashrc"), or when
 * the name ends in a dot ("a."). */
string_t path_extension(string_t p)
{
    string_t fn = path_file_name(p);
    size_t idx = string_rfind_char(fn, '.');
    if (idx != STRING_NOT_FOUND && idx != 0 && idx != fn.len - 1)
    {
        return (string_t){fn.ptr + idx + 1, fn.len - idx - 1};
    }
    return (string_t){p.ptr, 0};
}

typedef struct
{
    string_t rest;     /* what is left to split          */
    bool root_pending; /* absolute: "/" not yielded yet  */
    const char *root;  /* points at the leading '/'      */
} components_state_t;

static void split_drop(iter_t *it)
{
    mem_free(it->allocator, it->state, sizeof(components_state_t));
}

static bool split_next(iter_t *it, void *tok)
{
    components_state_t *state = it->state;

    if (state->root_pending)
    {
        *(string_t *)tok = (string_t){state->root, 1};
        state->root_pending = false;
        return true;
    }

    while (string_split_next(&state->rest, '/', (string_t *)tok))
    {
        if (((string_t *)tok)->len > 0
            && !string_equals((*(string_t *)tok), STRING_LIT(".")))
        {
            return true;
        }
    }

    return false;
}

/* Yields string_t: "/" first if p is absolute, then each component.
 * "." yields nothing.  path_join_iter(path_components(p)) gives p back. */
iter_t path_components(string_t p, allocator_t a)
{
    components_state_t *state =
        mem_alloc(a, sizeof(components_state_t), _Alignof(components_state_t));
    if (!state)
    {
        return (iter_t){0};
    }
    state->root_pending = path_is_absolute(p);
    state->rest = state->root_pending ? (string_t){p.ptr + 1, p.len - 1} : p;
    state->root = state->root_pending ? p.ptr : NULL;

    return (iter_t){
        .state = state,
        .destroy = split_drop,
        .next = split_next,
        .allocator = a,
        .elem_size = sizeof(string_t)};
}

/* --- Building (append to out, normalised results) ----------------------- */
/* Every function saves mark = strbuf_len(out) on entry, appends, and on any
 * failure truncates back to mark.  The normalising is filec_append_names
 * (names.h), with mark as the floor. */

/* Undoes everything appended since mark. */
static path_err_t fail(strbuf_t *out, size_t mark)
{
    strbuf_truncate(out, mark);
    return PATH_OOM;
}

path_err_t path_normalize(string_t p, strbuf_t *out)
{
    if (!out)
    {
        return PATH_INVALID;
    }
    size_t mark = strbuf_len(out);
    bool absolute = path_is_absolute(p);
    if (filec_append_names(out, p, false, mark, absolute) != SEQC_OK
        || filec_finish_names(out, mark, absolute) != SEQC_OK)
    {
        return fail(out, mark);
    }
    return PATH_OK;
}

path_err_t path_join(string_t a, string_t b, strbuf_t *out)
{
    if (!out)
    {
        return PATH_INVALID;
    }
    if (path_is_absolute(b))
    {
        return path_normalize(b, out); /* an absolute b replaces a */
    }
    size_t mark = strbuf_len(out);
    bool absolute = path_is_absolute(a);
    if (filec_append_names(out, a, false, mark, absolute) != SEQC_OK
        || filec_append_names(out, b, false, mark, absolute) != SEQC_OK
        || filec_finish_names(out, mark, absolute) != SEQC_OK)
    {
        return fail(out, mark);
    }
    return PATH_OK;
}

path_err_t path_join_iter(iter_t parts, strbuf_t *out)
{
    if (!out)
    {
        iter_destroy(&parts);
        return PATH_INVALID;
    }
    size_t mark = strbuf_len(out);
    bool absolute = false;
    seqc_status_t st = SEQC_OK;
    string_t part;
    while (st == SEQC_OK && parts.next(&parts, &part))
    {
        if (path_is_absolute(part))
        {
            strbuf_truncate(out, mark); /* an absolute part replaces all */
            absolute = true;
        }
        st = filec_append_names(out, part, false, mark, absolute);
    }
    iter_destroy(&parts);
    if (st != SEQC_OK || filec_finish_names(out, mark, absolute) != SEQC_OK)
    {
        return fail(out, mark);
    }
    return PATH_OK;
}

path_err_t path_with_extension(string_t p, string_t ext, strbuf_t *out)
{
    string_t stem = path_stem(p);
    if (!out || stem.len == 0)
    {
        return PATH_INVALID; /* no file name to give an extension: "/", "." */
    }
    size_t mark = strbuf_len(out);
    size_t keep = (size_t)(stem.ptr - p.ptr) + stem.len;
    if (strbuf_append(out, (string_t){p.ptr, keep}) != SEQC_OK)
    {
        return fail(out, mark);
    }
    if (ext.len > 0)
    {
        if ((ext.ptr[0] != '.' && strbuf_append_char(out, '.') != SEQC_OK)
            || strbuf_append(out, ext) != SEQC_OK)
        {
            return fail(out, mark);
        }
    }
    return PATH_OK;
}

path_err_t path_relative(
    string_t base, string_t target, path_case_t cs, strbuf_t *out)
{
    if (!out || !path_is_absolute(base) || !path_is_absolute(target))
    {
        return PATH_INVALID;
    }

    string_t base_rest = base;
    string_t target_rest = target;
    string_t base_tok = {NULL, 0};
    string_t target_tok = {NULL, 0};
    bool more_base = false;
    bool more_target = false;
    for (;;)
    {
        more_base = next_component(&base_rest, &base_tok);
        more_target = next_component(&target_rest, &target_tok);
        if (!more_base || !more_target
            || !path_equals(base_tok, target_tok, cs))
        {
            break;
        }
    }

    size_t mark = strbuf_len(out);

    /* Up: one ".." for the first base component past the common part, and
     * one for each base component after it. */
    if (more_base)
    {
        do
        {
            if ((strbuf_len(out) > mark
                 && strbuf_append_char(out, '/') != SEQC_OK)
                || strbuf_append(out, STRING_LIT("..")) != SEQC_OK)
            {
                return fail(out, mark);
            }
        } while (next_component(&base_rest, &base_tok));
    }

    /* Down: the rest of target from its first component past the common
     * part — a single view, since target is normalised. */
    if (more_target)
    {
        string_t down = {
            target_tok.ptr, (size_t)(target.ptr + target.len - target_tok.ptr)};
        if ((strbuf_len(out) > mark && strbuf_append_char(out, '/') != SEQC_OK)
            || strbuf_append(out, down) != SEQC_OK)
        {
            return fail(out, mark);
        }
    }

    if (filec_finish_names(out, mark, false) != SEQC_OK)
    {
        return fail(out, mark);
    }
    return PATH_OK;
}

/* --- Comparing -------------------------------------------------------------
 */
/* Component by component, so "/foo" is not a prefix of "/foobar", and a
 * directory sorts directly before its contents: "/a" < "/a/b" < "/a-b". */

bool path_equals(string_t a, string_t b, path_case_t cs)
{
    return cs == PATH_CASE_SENSITIVE ? string_equals(a, b)
                                     : string_equals_case_insensitive(a, b);
}

int path_compare(string_t a, string_t b, path_case_t cs)
{
    string_t sa = a;
    string_t sb = b;
    string_t atok = (string_t){NULL, 0};
    string_t btok = (string_t){NULL, 0};

    for (;;)
    {
        bool more_a = string_split_next(&sa, '/', &atok);
        bool more_b = string_split_next(&sb, '/', &btok);

        if (!more_a || !more_b)
        {
            return more_a - more_b;
        }

        if (cs == PATH_CASE_SENSITIVE)
        {
            int cmp = string_compare(atok, btok);
            if (cmp != 0)
            {
                return cmp;
            }
        }
        else
        {
            int cmp = string_compare_case_insensitive(atok, btok);
            if (cmp != 0)
            {
                return cmp;
            }
        }
    }

    return 0;
}

bool path_starts_with(string_t p, string_t prefix, path_case_t cs)
{
    if (prefix.len == 0)
    {
        return true;
    }
    if (prefix.len > p.len)
    {
        return false;
    }
    string_t head = {p.ptr, prefix.len}; /* the start of p, as long as prefix */
    if (!path_equals(head, prefix, cs))
    {
        return false;
    }
    /* "/ab" starts with the bytes "/a", but not with the path "/a": the
     * match must end where a component ends. */
    return p.len == prefix.len                  /* the same path            */
           || prefix.ptr[prefix.len - 1] == '/' /* prefix is the root "/"   */
           || p.ptr[prefix.len] == '/';         /* a component starts next */
}

/* Consistent with path_equals for the same cs: equal paths hash equal. */
size_t path_hash(string_t p, path_case_t cs)
{
    if (cs == PATH_CASE_SENSITIVE)
    {
        return string_hash((void *)&p, 0);
    }
    return string_hash_case_insensitive(&p, 0);
}
