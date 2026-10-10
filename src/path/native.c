/* filec/native — conversion between native and canonical paths.  No OS
 * code: every style builds and is tested on every platform.
 *
 * Both directions append to the caller's strbuf and allocate nothing else.
 * path_from_native normalises while it appends: ".." cuts out back to the
 * previous '/', never below the root ("the floor").  Any failure truncates
 * out back to the length it had on entry. */

#include "filec/native.h"

#include "names.h"
#include "seqc/status.h"
#include "seqc/string.h"

/* Windows separators: both '\\' and '/'. */
static bool is_sep(char ch)
{
    return ch == '\\' || ch == '/';
}

/* The name at the start of *s (up to the next separator or the end), and
 * *s advanced past it.  *had_sep tells whether a separator followed. */
static string_t take_name(string_t *s, bool *had_sep)
{
    size_t n = 0;
    while (n < s->len && !is_sep(s->ptr[n]))
    {
        n++;
    }
    string_t name = {s->ptr, n};
    *had_sep = n < s->len;
    size_t skip = *had_sep ? n + 1 : n;  /* past the separator, if any */
    *s = string_slice(*s, skip, s->len); /* {NULL, 0} stays {NULL, 0} */
    return name;
}

static bool is_drive_letter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static char upper_drive(char c)
{
    return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
}

typedef struct
{
    enum
    {
        ROOT_NONE,
        ROOT_DRIVE,
        ROOT_UNC,
    } kind;
    char drive; /* 'C' — upper case */
    string_t server;
    string_t share;
} win_root_t;

/* Appends the canonical root: "/C:" or "/UNC/server/share"; nothing for
 * ROOT_NONE. */
static seqc_status_t append_win_root(strbuf_t *out, const win_root_t *root)
{
    seqc_status_t st = SEQC_OK;
    if (root->kind == ROOT_DRIVE)
    {
        char drive[] = {'/', root->drive, ':'};
        st = strbuf_append(out, (string_t){drive, sizeof drive});
    }
    else if (root->kind == ROOT_UNC)
    {
        st = strbuf_append(out, STRING_LIT("/UNC/"));
        if (st == SEQC_OK)
        {
            st = strbuf_append(out, root->server);
        }
        if (st == SEQC_OK)
        {
            st = strbuf_append_char(out, '/');
        }
        if (st == SEQC_OK)
        {
            st = strbuf_append(out, root->share);
        }
    }
    return st;
}

static path_native_err_t take_unc(string_t *s, win_root_t *root)
{
    root->kind = ROOT_UNC;
    bool sep = false;
    root->server = take_name(s, &sep);
    if (root->server.len == 0 || !sep)
    {
        return PATH_NATIVE_INVALID;
    }
    root->share = take_name(s, &sep);
    if (root->share.len == 0)
    {
        return PATH_NATIVE_INVALID;
    }
    return PATH_NATIVE_OK;
}

/* Reads the root at the start of *s and advances *s to the rest. */
static path_native_err_t take_root(string_t *s, win_root_t *root)
{
    root->kind = ROOT_NONE;
    root->drive = '\0';
    root->server = (string_t){NULL, 0};
    root->share = (string_t){NULL, 0};

    // 1. \\?\UNC\  -> strip, then UNC (server, share)
    // 2. \\?\      -> strip, then ONLY a drive is allowed
    if (string_starts_with(*s, STRING_LIT("\\\\?\\")))
    {
        *s = string_slice(*s, 4, s->len); /* skip \\?\ */
        if (string_starts_with(*s, STRING_LIT("UNC\\")))
        {
            *s = string_slice(*s, 4, s->len); /* skip UNC\ */
            return take_unc(s, root);
        }
        if (s->len > 1 && is_drive_letter(s->ptr[0]) && s->ptr[1] == ':')
        {
            root->kind = ROOT_DRIVE;
            root->drive = upper_drive(s->ptr[0]);
            *s = string_slice(*s, 2, s->len);

            // skip the separator after the drive letter
            if (s->len > 0 && is_sep(s->ptr[0]))
            {
                *s = string_slice(*s, 1, s->len);
            }
            return PATH_NATIVE_OK;
        }
        return PATH_NATIVE_INVALID; /* \\?\ followed by something other than
                                       UNC\ is invalid */
    }

    // 3. \\.\      -> INVALID
    if (string_starts_with(*s, STRING_LIT("\\\\.\\")))
    {
        return PATH_NATIVE_INVALID; /* \\.\ is always invalid */
    }

    // 4. \\        -> UNC (server, share)
    if (s->len > 1 && is_sep(s->ptr[0]) && is_sep(s->ptr[1]))
    {
        *s = string_slice(*s, 2, s->len); /* skip \\ */
        return take_unc(s, root);
    }

    // 5. X:        -> X:\ or X:/ = drive; otherwise DRIVE_RELATIVE
    if (s->len > 1 && is_drive_letter(s->ptr[0]) && s->ptr[1] == ':')
    {
        root->drive = upper_drive(s->ptr[0]);
        *s = string_slice(*s, 2, s->len);
        if (s->len > 0 && is_sep(s->ptr[0]))
        {
            root->kind = ROOT_DRIVE;
            *s = string_slice(*s, 1, s->len);
            return PATH_NATIVE_OK;
        }
        return PATH_NATIVE_DRIVE_RELATIVE;
    }

    // 6. \ or /    -> ROOT_RELATIVE
    if (s->len > 0 && is_sep(s->ptr[0]))
    {
        return PATH_NATIVE_ROOT_RELATIVE;
    }

    // 7. else      -> ROOT_NONE, *s unchanged
    root->kind = ROOT_NONE;
    return PATH_NATIVE_OK;
}

/* Undoes everything appended since mark and returns err. */
static path_native_err_t fail(strbuf_t *out, size_t mark, path_native_err_t err)
{
    strbuf_truncate(out, mark);
    return err;
}

path_native_err_t path_from_native(
    string_t native, path_style_t style, strbuf_t *out)
{
    if (!out || string_find_char(native, '\0') != STRING_NOT_FOUND)
    {
        return PATH_NATIVE_INVALID;
    }
    size_t mark = strbuf_len(out);

    bool absolute = false;
    if (style == PATH_STYLE_WINDOWS)
    {
        win_root_t root;
        path_native_err_t err = take_root(&native, &root);
        if (err != PATH_NATIVE_OK)
        {
            return err;
        }
        if (append_win_root(out, &root) != SEQC_OK)
        {
            return fail(out, mark, PATH_NATIVE_OOM);
        }
        absolute = root.kind != ROOT_NONE;
    }
    else if (native.len > 0 && native.ptr[0] == '/')
    {
        absolute = true; /* the root "/" is written with the first name */
    }

    size_t floor = strbuf_len(out);
    bool windows = style == PATH_STYLE_WINDOWS;
    if (filec_append_names(out, native, windows, floor, absolute) != SEQC_OK)
    {
        return fail(out, mark, PATH_NATIVE_OOM);
    }

    /* no names left: "/" or "." — but a Windows root stands alone */
    if (!(windows && absolute)
        && filec_finish_names(out, floor, absolute) != SEQC_OK)
    {
        return fail(out, mark, PATH_NATIVE_OOM);
    }
    return PATH_NATIVE_OK;
}

/* Appends s with every '/' written as sep. */
static seqc_status_t append_with_sep(strbuf_t *out, string_t s, char sep)
{
    for (size_t i = 0; i < s.len; i++)
    {
        if (strbuf_append_char(out, s.ptr[i] == '/' ? sep : s.ptr[i])
            != SEQC_OK)
        {
            return SEQC_OOM;
        }
    }
    return SEQC_OK;
}

/* Canonical "/C:/rest" or "/UNC/srv/share/rest" -> its Windows root
 * ("C:\" or "\\srv\share") and *rest; any other absolute path has no
 * Windows form. */
static path_native_err_t append_native_root(strbuf_t *out, string_t *rest)
{
    bool sep = false;
    string_t s = string_slice(*rest, 1, rest->len); /* past the '/' */
    string_t first = take_name(&s, &sep);

    if (first.len == 2 && is_drive_letter(first.ptr[0]) && first.ptr[1] == ':')
    {
        char drive[] = {upper_drive(first.ptr[0]), ':', '\\'};
        *rest = s;
        return strbuf_append(out, (string_t){drive, sizeof drive}) == SEQC_OK
                   ? PATH_NATIVE_OK
                   : PATH_NATIVE_OOM;
    }
    if (string_equals(first, STRING_LIT("UNC")))
    {
        string_t server = take_name(&s, &sep);
        string_t share =
            server.len > 0 && sep ? take_name(&s, &sep) : (string_t){NULL, 0};
        if (share.len == 0)
        {
            return PATH_NATIVE_NO_NATIVE_FORM; /* "/UNC", "/UNC/srv" */
        }
        *rest = s;
        bool ok = strbuf_append(out, STRING_LIT("\\\\")) == SEQC_OK
                  && strbuf_append(out, server) == SEQC_OK
                  && strbuf_append_char(out, '\\') == SEQC_OK
                  && strbuf_append(out, share) == SEQC_OK
                  && (s.len == 0 || strbuf_append_char(out, '\\') == SEQC_OK);
        return ok ? PATH_NATIVE_OK : PATH_NATIVE_OOM;
    }
    return PATH_NATIVE_NO_NATIVE_FORM; /* "/", "/home/me" */
}

path_native_err_t path_to_native(
    string_t canonical, path_style_t style, strbuf_t *out)
{
    if (!out || canonical.len == 0)
    {
        return PATH_NATIVE_INVALID;
    }
    size_t mark = strbuf_len(out);

    if (style == PATH_STYLE_POSIX)
    {
        return strbuf_append(out, canonical) == SEQC_OK
                   ? PATH_NATIVE_OK
                   : fail(out, mark, PATH_NATIVE_OOM);
    }

    string_t rest = canonical;
    if (canonical.ptr[0] == '/')
    {
        path_native_err_t err = append_native_root(out, &rest);
        if (err != PATH_NATIVE_OK)
        {
            return fail(out, mark, err);
        }
    }
    if (append_with_sep(out, rest, '\\') != SEQC_OK)
    {
        return fail(out, mark, PATH_NATIVE_OOM);
    }
    return PATH_NATIVE_OK;
}
