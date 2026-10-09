/* filec/path — lexical manipulation of canonical paths.  No OS code and no
 * platform: the same file, and the same tests, everywhere.
 *
 */

#include "filec/path.h"
#include "seqc/string.h"
#include "seqc/vec.h"
#include <bits/types/stack_t.h>

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
        .drop = split_drop,
        .next = split_next,
        .allocator = a,
        .elem_size = sizeof(string_t)};
}

/* --- Building (allocating, normalised results) --------------------------- */

/* a, then b.  An absolute b replaces a.  Accepts unnormalised input:
 *   join("/a", "b") = "/a/b"     join("/a/b", "../c") = "/a/c"
 *   join("/a", "/b") = "/b"      join("/a", "") = "/a" */
string_t path_join(string_t a, string_t b, allocator_t alloc)
{
    strbuf_t *buf = strbuf_create(alloc);
    if (buf == NULL)
    {
        return (string_t){NULL, 0};
    }

    if (path_is_absolute(b))
    {
        if (strbuf_append(buf, b) != SEQC_OK)
        {
            strbuf_free(buf);
            return (string_t){NULL, 0};
        }
    }
    else
    {
        if (strbuf_append(buf, a) != SEQC_OK)
        {
            strbuf_free(buf);
            return (string_t){NULL, 0};
        }
        if (strbuf_len(buf) > 0)
        {
            if (strbuf_append(buf, STRING_LIT("/")) != SEQC_OK)
            {
                strbuf_free(buf);
                return (string_t){NULL, 0};
            }
        }
        if (strbuf_append(buf, b) != SEQC_OK)
        {
            strbuf_free(buf);
            return (string_t){NULL, 0};
        }
    }

    string_t result = path_normalize(strbuf_finish(buf), alloc);
    strbuf_free(buf);
    return result;
}
/* Joins every string_t yielded by parts, left to right, as path_join.
 * Consumes the iterator.  No parts gives ".". */
string_t path_join_iter(iter_t parts, allocator_t alloc)
{
    strbuf_t *buf = strbuf_create(alloc);
    if (buf == NULL)
    {
        iter_drop(&parts);
        return (string_t){NULL, 0};
    }

    string_t part = {NULL, 0};

    while (parts.next(&parts, &part))
    {
        if (path_is_absolute(part))
        {
            strbuf_clear(buf);
            if (strbuf_append(buf, part) != SEQC_OK)
            {
                strbuf_free(buf);
                iter_drop(&parts);
                return (string_t){NULL, 0};
            }
            continue;
        }
        if (strbuf_len(buf) > 0)
        {
            if (strbuf_append(buf, STRING_LIT("/")) != SEQC_OK)
            {
                strbuf_free(buf);
                iter_drop(&parts);
                return (string_t){NULL, 0};
            }
        }
        if (strbuf_append(buf, part) != SEQC_OK)
        {
            strbuf_free(buf);
            iter_drop(&parts);
            return (string_t){NULL, 0};
        }
    }
    iter_drop(&parts);

    string_t result = path_normalize(strbuf_finish(buf), alloc);
    strbuf_free(buf);
    return result;
}

/* The normalised form of any canonical path: empty and "." components
 * dropped, ".." resolved lexically.  ".." never climbs above "/"; a
 * relative path keeps its leading ".."s.  "" -> "."
 *   "/a//./b/../c/" -> "/a/c"      "a/../.." -> ".."      "/.." -> "/" */
string_t path_normalize(string_t p, allocator_t alloc)
{
    vec_t *components = vec_create(sizeof(string_t), alloc);
    if (components == 0)
    {
        return (string_t){NULL, 0};
    }

    string_t rest = p;
    string_t tok = {NULL, 0};
    bool absolute = path_is_absolute(p);
    while (string_split_next(&rest, '/', &tok))
    {
        if (tok.len == 0)
        {
            continue;
        }
        if (string_equals(tok, STRING_LIT(".")))
        {
            continue;
        }
        if (string_equals(tok, STRING_LIT("..")))
        {
            size_t n = vec_len(components);
            bool top_is_dotdot =
                n > 0
                && string_equals(
                    *(string_t *)vec_get(components, n - 1), STRING_LIT(".."));
            if (n > 0 && !top_is_dotdot)
            {
                if (vec_pop(components, 0) != SEQC_OK)
                {
                    vec_free(components);
                    return (string_t){NULL, 0};
                }
            }
            else if (!absolute)
            {
                if (vec_push(components, &tok) != SEQC_OK)
                {
                    vec_free(components);
                    return (string_t){NULL, 0};
                }
            }
            continue;
        }
        if (vec_push(components, &tok) != SEQC_OK)
        {
            vec_free(components);
            return (string_t){NULL, 0};
        }
    }

    strbuf_t *buf = strbuf_create(alloc);
    if (buf == NULL)
    {
        vec_free(components);
        return (string_t){NULL, 0};
    }

    if (absolute)
    {
        strbuf_append_char(buf, '/');
    }

    for (size_t i = 0; i < vec_len(components); i++)
    {
        string_t comp;
        if (vec_get_copy(components, i, &comp) != SEQC_OK)
        {
            vec_free(components);
            return (string_t){NULL, 0};
        }
        if (i > 0)
        {
            strbuf_append_char(buf, '/');
        }
        strbuf_append(buf, comp);
    }
    vec_free(components);
    if (strbuf_len(buf) == 0)
    {
        strbuf_append(buf, STRING_LIT("."));
    }

    string_t result = string_copy(strbuf_finish(buf), alloc);
    strbuf_free(buf);
    return result;
}

/* p with its extension replaced (or added, or with ext = "" removed). */
string_t path_with_extension(string_t p, string_t ext, allocator_t alloc)
{
    if (p.len == 0)
    {
        return (string_t){NULL, 0};
    }

    string_t stem = path_stem(p);
    if (stem.len == 0)
    {
        return (string_t){0, 0};
    }
    size_t keep = (size_t)(stem.ptr - p.ptr) + stem.len;
    if (ext.len > 0)
    {
        strbuf_t *buf = strbuf_create(alloc);
        if (buf == NULL)
        {
            return (string_t){NULL, 0};
        }
        if (strbuf_append(buf, (string_t){p.ptr, keep}) != SEQC_OK)
        {
            strbuf_free(buf);
            return (string_t){NULL, 0};
        }
        if (ext.ptr[0] != '.')
        {
            if (strbuf_append(buf, STRING_LIT(".")) != SEQC_OK)
            {
                strbuf_free(buf);
                return (string_t){NULL, 0};
            }
        }
        if (strbuf_append(buf, ext) != SEQC_OK)
        {
            strbuf_free(buf);
            return (string_t){NULL, 0};
        }
        string_t result = string_copy(strbuf_finish(buf), alloc);
        strbuf_free(buf);
        return result;
    }
    return string_copy((string_t){p.ptr, keep}, alloc);
}

/* The relative path that leads from base to target, both absolute:
 *   ("/a/b", "/a/c/d") -> "../c/d"     ("/a", "/a") -> "."
 * Every absolute path shares the root "/", so there is always an answer —
 * on Windows it may lead through the virtual root ("../../D:/x"), which has
 * no native form.  Returns {NULL, 0} if base or target is relative. */
string_t path_relative(
    string_t base, string_t target, path_case_t cs, allocator_t alloc)
{
    if (!path_is_absolute(base) || !path_is_absolute(target))
    {
        return (string_t){NULL, 0};
    }

    string_t base_copy = base;
    string_t target_copy = target;
    string_t base_tok = {NULL, 0};
    string_t target_tok = {NULL, 0};
    bool more_base = false;
    bool more_target = false;

    for (;;)
    {
        more_base = next_component(&base_copy, &base_tok);
        more_target = next_component(&target_copy, &target_tok);
        if (!more_base || !more_target
            || !path_equals(base_tok, target_tok, cs))
        {
            break;
        }
    }

    strbuf_t *buf = strbuf_create(alloc);
    if (buf == NULL)
    {
        return (string_t){NULL, 0};
    }

    /* Up: one ".." for the first base component past the common part, and
     * one for each base component after it. */
    if (more_base)
    {
        do
        {
            if (strbuf_len(buf) > 0 && strbuf_append_char(buf, '/') != SEQC_OK)
            {
                goto oom;
            }
            if (strbuf_append(buf, STRING_LIT("..")) != SEQC_OK)
            {
                goto oom;
            }
        } while (next_component(&base_copy, &base_tok));
    }

    /* Down: the rest of target from its first component past the common
     * part — a single view, since target is normalised. */
    if (more_target)
    {
        string_t down = {
            target_tok.ptr, (size_t)(target.ptr + target.len - target_tok.ptr)};
        if (strbuf_len(buf) > 0 && strbuf_append_char(buf, '/') != SEQC_OK)
        {
            goto oom;
        }
        if (strbuf_append(buf, down) != SEQC_OK)
        {
            goto oom;
        }
    }

    if (strbuf_len(buf) == 0 && strbuf_append_char(buf, '.') != SEQC_OK)
    {
        goto oom;
    }

    string_t result = string_copy(strbuf_finish(buf), alloc);
    strbuf_free(buf);
    return result;

oom:
    strbuf_free(buf);
    return (string_t){NULL, 0};
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
