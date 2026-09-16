/* ===========================================================================
 * bnbf.c — the BNBF note format: reader, writer, tables and the two line
 * parsers (implementation).  See bnbf.h for the format specification.
 * =========================================================================== */

#include "bnbf.h"

#include <stdio.h>                   /* sscanf                              */
#include <string.h>

/* Magic bytes at the start of every BNBF blob.  (The pre-rename "ONBF"
 * magic was retired 2026-07 after an offline migration verified zero
 * such blobs remained in the database.)                                     */
static const guint8 BNBF_MAGIC[4] = { 'B', 'N', 'B', 'F' };

/* TABLE record flag bits (the tflags field).                                */
#define TABLE_FLAG_HEADER 1u         /* first row is a header row           */

/* ---------------------------------------------------------------------------
 * put_u32() — append a little-endian u32 to a byte array.
 *   buf — destination array.
 *   v   — value to append.
 * ------------------------------------------------------------------------- */
static void
put_u32(GByteArray *buf, guint32 v)
{
    guint8 b[4] = {
        (guint8)(v & 0xff),          (guint8)((v >> 8) & 0xff),
        (guint8)((v >> 16) & 0xff),  (guint8)((v >> 24) & 0xff),
    };
    g_byte_array_append(buf, b, 4);
}

/* ---------------------------------------------------------------------------
 * get_u32() — read a little-endian u32, advancing *pos.
 *   data — blob bytes.
 *   len  — blob length.
 *   pos  — in/out read cursor.
 *   out  — receives the value.
 * Returns FALSE if fewer than 4 bytes remain.
 * ------------------------------------------------------------------------- */
static gboolean
get_u32(const guint8 *data, gsize len, gsize *pos, guint32 *out)
{
    if (*pos + 4 > len)
        return FALSE;
    *out = (guint32)data[*pos]
         | ((guint32)data[*pos + 1] << 8)
         | ((guint32)data[*pos + 2] << 16)
         | ((guint32)data[*pos + 3] << 24);
    *pos += 4;
    return TRUE;
}

/* ===========================================================================
 * LINE PARSERS
 * ======================================================================== */

glong
on_list_prefix_chars(const gchar *head)
{
    if (g_str_has_prefix(head, ON_BULLET_PREFIX))
        return 2;                    /* bullet + one space                  */

    glong d = 0;                     /* leading digit characters            */
    while (g_ascii_isdigit(head[d]))
        d++;
    if (d > 0 && head[d] == '.' && head[d + 1] == ' ')
        return d + 2;                /* "12. "                              */
    return 0;
}

/* ---------------------------------------------------------------------------
 * parse_due_date() — parse one date string: ISO "YYYY-MM-DD" (the form
 * the app writes) or the shorthand "M/D/YY" / "M/D/YYYY".  On success
 * *out_ts receives local midnight of that day as a UNIX timestamp.
 * ------------------------------------------------------------------------- */
static gboolean
parse_due_date(const gchar *s, gint64 *out_ts)
{
    gint y = 0, m = 0, d = 0;        /* parsed components                   */
    gchar tail;                      /* catches trailing garbage            */
    if (sscanf(s, "%d-%d-%d%c", &y, &m, &d, &tail) != 3 &&
        sscanf(s, "%d/%d/%d%c", &m, &d, &y, &tail) != 3)
        return FALSE;
    if (y < 100)
        y += 2000;                   /* "26" means 2026                     */
    if (!g_date_valid_dmy((GDateDay)d, (GDateMonth)m, (GDateYear)y))
        return FALSE;

    GDateTime *dt = g_date_time_new_local(y, m, d, 0, 0, 0);
    if (dt == NULL)
        return FALSE;
    *out_ts = g_date_time_to_unix(dt);
    g_date_time_unref(dt);
    return TRUE;
}

gboolean
on_action_split_due(const gchar *rest, gsize *due_start, gint64 *due)
{
    /* The LAST word-boundary "due" whose remainder parses as a date wins
     * ("send due diligence report due 2026-07-07" keeps its text).         */
    const gchar *limit = rest + strlen(rest);   /* scan window end          */
    while (limit > rest) {
        const gchar *p = g_strrstr_len(rest, limit - rest, "due");
        if (p == NULL)
            return FALSE;
        gboolean word = (p == rest ||
                         g_ascii_isspace((guchar)p[-1])) &&
                        g_ascii_isspace((guchar)p[3]);
        if (word) {
            gchar *date = g_strstrip(g_strdup(p + 3));
            gboolean ok = parse_due_date(date, due);
            g_free(date);
            if (ok) {
                *due_start = (gsize)(p - rest);
                return TRUE;
            }
        }
        limit = p;                   /* keep scanning leftward              */
    }
    return FALSE;
}

/* ===========================================================================
 * TABLES
 * ======================================================================== */

OnTable *
on_table_new(gint rows, gint cols)
{
    OnTable *t = g_new0(OnTable, 1);
    t->rows  = MAX(1, rows);
    t->cols  = MAX(1, cols);
    t->cells = g_ptr_array_new_with_free_func(g_free);
    for (gint i = 0; i < t->rows * t->cols; i++)
        g_ptr_array_add(t->cells, g_strdup(""));
    return t;
}

void
on_table_free(OnTable *table)
{
    if (table == NULL)
        return;
    g_ptr_array_free(table->cells, TRUE);
    g_free(table);
}

const gchar *
on_table_get(OnTable *table, gint r, gint c)
{
    if (r < 0 || r >= table->rows || c < 0 || c >= table->cols)
        return "";
    return g_ptr_array_index(table->cells, r * table->cols + c);
}

void
on_table_set(OnTable *table, gint r, gint c, const gchar *text)
{
    if (r < 0 || r >= table->rows || c < 0 || c >= table->cols)
        return;
    gint i = r * table->cols + c;    /* row-major cell index                */
    g_free(g_ptr_array_index(table->cells, i));
    g_ptr_array_index(table->cells, i) =
        g_strdup(text != NULL ? text : "");
}

/* ===========================================================================
 * READER
 * ======================================================================== */

gboolean
on_bnbf_open(OnBnbfReader *r, const guint8 *data, gsize len)
{
    r->data = data;
    r->len  = len;
    r->pos  = 4;                     /* past the magic                      */
    r->version = 0;
    r->saw_end = FALSE;
    r->error   = NULL;

    if (data == NULL || len < 8 || memcmp(data, BNBF_MAGIC, 4) != 0) {
        r->error = "bad or missing BNBF header";
        return FALSE;
    }
    if (!get_u32(data, len, &r->pos, &r->version) ||
        r->version < 1 || r->version > ON_BNBF_VERSION) {
        r->error = "unsupported BNBF version";
        return FALSE;
    }
    return TRUE;
}

gboolean
on_bnbf_next(OnBnbfReader *r, OnBnbfRecord *rec)
{
    if (r->error != NULL || r->pos >= r->len)
        return FALSE;

    memset(rec, 0, sizeof *rec);
    rec->type = r->data[r->pos++];

    switch (rec->type) {
    case ON_REC_END:
        r->saw_end = TRUE;
        return FALSE;

    case ON_REC_TEXT:
        if (!get_u32(r->data, r->len, &r->pos, &rec->flags) ||
            !get_u32(r->data, r->len, &r->pos, &rec->n_text) ||
            r->pos + rec->n_text > r->len) {
            r->error = "truncated TEXT record";
            return FALSE;
        }
        rec->text = (const gchar *)r->data + r->pos;
        r->pos += rec->n_text;
        return TRUE;

    case ON_REC_IMAGE:
        if (r->version >= 2 &&
            !get_u32(r->data, r->len, &r->pos, &rec->display_width)) {
            r->error = "truncated IMAGE record";
            return FALSE;
        }
        if (!get_u32(r->data, r->len, &r->pos, &rec->n_png) ||
            r->pos + rec->n_png > r->len) {
            r->error = "truncated IMAGE record";
            return FALSE;
        }
        rec->png = r->data + r->pos;
        r->pos += rec->n_png;
        return TRUE;

    case ON_REC_CHECK:
        if (r->pos >= r->len) {
            r->error = "truncated CHECK record";
            return FALSE;
        }
        rec->checked = r->data[r->pos++] != 0;
        return TRUE;

    case ON_REC_TABLE: {
        guint32 tflags = 0, rows, cols;
        if (r->version >= 4 &&
            !get_u32(r->data, r->len, &r->pos, &tflags)) {
            r->error = "truncated TABLE record";
            return FALSE;
        }
        if (!get_u32(r->data, r->len, &r->pos, &rows) ||
            !get_u32(r->data, r->len, &r->pos, &cols) ||
            rows == 0 || cols == 0 || rows > 1024 || cols > 1024) {
            r->error = "bad TABLE record";
            return FALSE;
        }
        OnTable *t = on_table_new((gint)rows, (gint)cols);
        t->header = (tflags & TABLE_FLAG_HEADER) != 0;
        for (guint32 i = 0; i < rows * cols; i++) {
            guint32 n;               /* cell byte length                    */
            if (!get_u32(r->data, r->len, &r->pos, &n) ||
                r->pos + n > r->len) {
                on_table_free(t);
                r->error = "truncated TABLE cell";
                return FALSE;
            }
            gchar *cell = g_strndup((const gchar *)r->data + r->pos, n);
            on_table_set(t, (gint)(i / cols), (gint)(i % cols), cell);
            g_free(cell);
            r->pos += n;
        }
        rec->table = t;              /* caller owns it                      */
        return TRUE;
    }

    default:
        r->error = "unknown record type";
        return FALSE;
    }
}

/* ===========================================================================
 * WRITER
 * ======================================================================== */

void
on_bnbf_writer_init(OnBnbfWriter *w)
{
    w->out = g_byte_array_new();
    g_byte_array_append(w->out, BNBF_MAGIC, 4);
    put_u32(w->out, ON_BNBF_VERSION);
}

/* put_rec() — append a record type byte.                                    */
static void
put_rec(OnBnbfWriter *w, guint8 type)
{
    g_byte_array_append(w->out, &type, 1);
}

void
on_bnbf_write_text(OnBnbfWriter *w, guint32 flags, const gchar *text,
                   gsize n)
{
    put_rec(w, ON_REC_TEXT);
    put_u32(w->out, flags);
    put_u32(w->out, (guint32)n);
    g_byte_array_append(w->out, (const guint8 *)text, (guint)n);
}

void
on_bnbf_write_image(OnBnbfWriter *w, guint32 display_width,
                    const guint8 *png, gsize n_png)
{
    put_rec(w, ON_REC_IMAGE);
    put_u32(w->out, display_width);
    put_u32(w->out, (guint32)n_png);
    g_byte_array_append(w->out, png, (guint)n_png);
}

void
on_bnbf_write_check(OnBnbfWriter *w, gboolean checked)
{
    put_rec(w, ON_REC_CHECK);
    guint8 state = checked ? 1 : 0;
    g_byte_array_append(w->out, &state, 1);
}

void
on_bnbf_write_table_begin(OnBnbfWriter *w, gboolean header, gint rows,
                          gint cols)
{
    put_rec(w, ON_REC_TABLE);
    put_u32(w->out, header ? TABLE_FLAG_HEADER : 0);
    put_u32(w->out, (guint32)rows);
    put_u32(w->out, (guint32)cols);
}

void
on_bnbf_write_table_cell(OnBnbfWriter *w, const gchar *text, gsize n)
{
    put_u32(w->out, (guint32)n);
    g_byte_array_append(w->out, (const guint8 *)text, (guint)n);
}

guint8 *
on_bnbf_writer_finish(OnBnbfWriter *w, gsize *out_len)
{
    put_rec(w, ON_REC_END);
    *out_len = w->out->len;
    guint8 *bytes = g_byte_array_free(w->out, FALSE);
    w->out = NULL;
    return bytes;
}
