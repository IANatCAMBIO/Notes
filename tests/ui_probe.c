/* ===========================================================================
 * ui_probe.c — drive an OnNoteView from a script and record what it did
 *
 *   build/ui-probe DB SCRIPT OUT.png [OUT.bnbf] [IN.bnbf]
 *
 * Builds a window around a note view (over the database DB — the sandbox
 * one, for the #tag choices), loads IN.bnbf if given, runs SCRIPT one line
 * at a time, then writes the rendered window to OUT.png and the note's
 * blob to OUT.bnbf.  Script lines:
 *
 *   text WORDS…      type the rest of the line (through the input method)
 *   key NAME [MODS]  a key press: NAME is a GDK key name (Return, Left,
 *                    BackSpace, a…), MODS any of shift ctrl alt meta primary
 *   inline bold|italic|underline|strike
 *   para h1|h2|body|bullet|number|check|code
 *   undo | redo | table | date | select-all
 *   find TEXT | step | step-back
 *   check BLOCK KIND TEXT   assert block BLOCK's kind and text (exit 1)
 *   caret BLOCK OFFSET      assert the caret
 *   print                   dump the blocks to stdout
 *   width W                 resize the window to W px wide
 *
 * GTK4 has no synthetic input, so the view offers its keyboard as calls
 * (on_note_view_feed_*): the same code the controllers run.
 * =========================================================================== */

#include "app.h"
#include "db.h"
#include "note_view.h"

#include <stdio.h>
#include <string.h>

static const gchar *KIND_NAMES[ON_N_BLOCK_KINDS] = {
    "para", "h1", "h2", "bullet", "number", "check", "code", "image", "table",
};

/* pump() — run the main loop until it is idle (layout, allocation).       */
static void
pump(void)
{
    for (gint i = 0; i < 50; i++)
        while (g_main_context_iteration(NULL, FALSE))
            ;
}

/* settle() — run the loop for `ms` of wall time (a window mapping).      */
static gboolean
settle_done(gpointer flag)
{
    *(gboolean *)flag = TRUE;
    return G_SOURCE_REMOVE;
}

static void
settle(guint ms)
{
    gboolean done = FALSE;
    g_timeout_add(ms, settle_done, &done);
    while (!done)
        g_main_context_iteration(NULL, TRUE);
}

/* dump() — every block on stdout.                                          */
static void
dump(OnDocument *d)
{
    for (guint i = 0; i < on_document_n_blocks(d); i++) {
        const OnBlock *b = on_document_block(d, i);
        if (b->text != NULL) {
            gchar *esc = g_strescape(b->text->text->str, NULL);
            printf("  %2u %-6s \"%s\"", i, KIND_NAMES[b->kind], esc);
            g_free(esc);
            for (guint k = 0; k < b->text->runs->len; k++)
                printf(" (%" G_GSIZE_FORMAT ",%x)",
                       g_array_index(b->text->runs, OnRun, k).len,
                       g_array_index(b->text->runs, OnRun, k).flags);
            if (b->kind == ON_BLOCK_CHECK)
                printf(" [%c]", b->checked ? 'x' : ' ');
            printf("\n");
        } else if (b->kind == ON_BLOCK_TABLE) {
            printf("  %2u table %dx%d", i, b->rows, b->cols);
            for (guint c = 0; c < b->cells->len; c++) {
                gchar *esc = g_strescape(
                    ((OnText *)g_ptr_array_index(b->cells, c))->text->str,
                    NULL);
                printf(" [%s]", esc);
                g_free(esc);
            }
            printf("\n");
        } else {
            printf("  %2u image\n", i);
        }
    }
}

static guint32
inline_flag(const gchar *name)
{
    if (strcmp(name, "bold") == 0)      return ON_FMT_BOLD;
    if (strcmp(name, "italic") == 0)    return ON_FMT_ITALIC;
    if (strcmp(name, "underline") == 0) return ON_FMT_UNDERLINE;
    return ON_FMT_STRIKE;
}

static guint32
para_flag(const gchar *name)
{
    if (strcmp(name, "h1") == 0)     return ON_FMT_H1;
    if (strcmp(name, "h2") == 0)     return ON_FMT_H2;
    if (strcmp(name, "bullet") == 0) return ON_FMT_LIST_BULLET;
    if (strcmp(name, "number") == 0) return ON_FMT_LIST_NUMBER;
    if (strcmp(name, "check") == 0)  return ON_FMT_LIST_CHECK;
    if (strcmp(name, "code") == 0)   return ON_FMT_CODEBLOCK;
    return 0;
}

static GdkModifierType
mods_of(gchar **words, gint from)
{
    GdkModifierType m = 0;
    if ((guint)from >= g_strv_length(words))
        return 0;
    for (gint i = from; words[i] != NULL; i++) {
        if (strcmp(words[i], "shift") == 0) m |= GDK_SHIFT_MASK;
        if (strcmp(words[i], "ctrl") == 0)  m |= GDK_CONTROL_MASK;
        if (strcmp(words[i], "alt") == 0)   m |= GDK_ALT_MASK;
        if (strcmp(words[i], "meta") == 0)  m |= GDK_META_MASK;
#ifdef __APPLE__
        if (strcmp(words[i], "primary") == 0) m |= GDK_META_MASK;
#else
        if (strcmp(words[i], "primary") == 0) m |= GDK_CONTROL_MASK;
#endif
    }
    return m;
}

/* render() — the window's pixels to a PNG.                                 */
static void
render(GtkWidget *window, const gchar *path)
{
    settle(100);
    GtkSnapshot *snap = gtk_snapshot_new();
    GdkPaintable *pt = gtk_widget_paintable_new(window);
    gdk_paintable_snapshot(pt, snap, gtk_widget_get_width(window),
                           gtk_widget_get_height(window));
    g_object_unref(pt);
    GskRenderNode *node = gtk_snapshot_free_to_node(snap);
    if (node == NULL) {
        fprintf(stderr, "nothing to render\n");
        return;
    }
    GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(window));
    graphene_rect_t bounds = GRAPHENE_RECT_INIT(
        0, 0, gtk_widget_get_width(window), gtk_widget_get_height(window));
    GdkTexture *tex = gsk_renderer_render_texture(renderer, node, &bounds);
    gsk_render_node_unref(node);
    gdk_texture_save_to_png(tex, path);
    g_object_unref(tex);
}

int
main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: ui-probe DB SCRIPT OUT.png [OUT.bnbf] [IN.bnbf]\n");
        return 2;
    }
    gtk_init();
    on_app_config_init(argv[0]);
    OnApp app = { 0 };
    app.db = on_db_open(argv[1]);
    if (app.db == NULL) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    app.first_line_title   = TRUE;
    app.code_line_numbers  = TRUE;
    app.code_copy_buttons  = TRUE;

    GtkWidget *window = gtk_window_new();
    gtk_window_set_default_size(GTK_WINDOW(window), 640, 480);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *view = on_note_view_new(&app);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_window_set_child(GTK_WINDOW(window), scroll);
    gtk_window_present(GTK_WINDOW(window));
    settle(300);
    gtk_widget_grab_focus(view);
    pump();

    OnNoteView *v = ON_NOTE_VIEW(view);
    if (argc >= 6) {
        gchar *blob;
        gsize n;
        if (!g_file_get_contents(argv[5], &blob, &n, NULL)) {
            fprintf(stderr, "cannot read %s\n", argv[5]);
            return 2;
        }
        gint64 t0 = g_get_monotonic_time();
        on_note_view_load(v, (const guint8 *)blob, n);
        g_free(blob);
        gint64 t1 = g_get_monotonic_time();
        gint min_h, nat_h;           /* a measure lays every block out      */
        gtk_widget_measure(view, GTK_ORIENTATION_VERTICAL, 640, &min_h,
                           &nat_h, NULL, NULL);
        gint64 t2 = g_get_monotonic_time();
        pump();
        fprintf(stderr, "load %.1f ms, layout %.1f ms (%u blocks, %d px)\n",
                (t1 - t0) / 1e3, (t2 - t1) / 1e3,
                on_document_n_blocks(on_note_view_document(v)), nat_h);
    }

    gchar *script;
    if (!g_file_get_contents(argv[2], &script, NULL, NULL)) {
        fprintf(stderr, "cannot read %s\n", argv[2]);
        return 2;
    }
    gint rc = 0;
    gchar **lines = g_strsplit(script, "\n", -1);
    for (gint ln = 0; lines[ln] != NULL; ln++) {
        gchar *line = g_strstrip(lines[ln]);
        if (*line == '\0' || *line == '#')
            continue;
        gchar **w = g_strsplit(line, " ", -1);
        const gchar *cmd = w[0];
        if (strcmp(cmd, "text") == 0) {
            on_note_view_feed_text(v, line + 5);
        } else if (strcmp(cmd, "key") == 0 && w[1] != NULL) {
            guint keyval = gdk_keyval_from_name(w[1]);
            if (keyval == GDK_KEY_VoidSymbol) {
                fprintf(stderr, "line %d: unknown key %s\n", ln + 1, w[1]);
                rc = 1;
            } else {
                on_note_view_feed_key(v, keyval, mods_of(w, 2));
            }
        } else if (strcmp(cmd, "inline") == 0 && w[1] != NULL) {
            on_note_view_toggle_inline(v, inline_flag(w[1]));
        } else if (strcmp(cmd, "para") == 0 && w[1] != NULL) {
            on_note_view_toggle_paragraph(v, para_flag(w[1]));
        } else if (strcmp(cmd, "undo") == 0) {
            on_note_view_undo(v);
        } else if (strcmp(cmd, "redo") == 0) {
            on_note_view_redo(v);
        } else if (strcmp(cmd, "table") == 0) {
            on_note_view_insert_table(v);
        } else if (strcmp(cmd, "date") == 0) {
            on_note_view_insert_date(v);
        } else if (strcmp(cmd, "select-all") == 0) {
            on_note_view_feed_key(v, GDK_KEY_a, mods_of((gchar *[]){ "", "primary", NULL }, 1));
        } else if (strcmp(cmd, "find") == 0) {
            on_note_view_find(v, line + 5);
        } else if (strcmp(cmd, "step") == 0) {
            on_note_view_find_step(v, w[1] != NULL ? line + 5 : "", TRUE);
        } else if (strcmp(cmd, "step-back") == 0) {
            on_note_view_find_step(v, w[1] != NULL ? line + 10 : "", FALSE);
        } else if (strcmp(cmd, "width") == 0 && w[1] != NULL) {
            gtk_window_set_default_size(GTK_WINDOW(window), atoi(w[1]), 480);
        } else if (strcmp(cmd, "image") == 0 && w[1] != NULL) {
            GdkPixbuf *pb = gdk_pixbuf_new_from_file(w[1], NULL);
            if (pb == NULL) {
                fprintf(stderr, "line %d: cannot load %s\n", ln + 1, w[1]);
                rc = 1;
            } else {
                on_note_view_insert_image(v, pb);
                g_object_unref(pb);
            }
        } else if (strcmp(cmd, "snapshot") == 0 && w[1] != NULL) {
            render(window, w[1]);
        } else if (strcmp(cmd, "settle") == 0) {
            settle(w[1] != NULL ? (guint)atoi(w[1]) : 200);
        } else if (strcmp(cmd, "click") == 0 && w[1] != NULL && w[2] != NULL) {
            /* Widget coordinates of the VIEW (below the window's origin). */
            gint n = (w[3] != NULL) ? atoi(w[3]) : 1;
            on_note_view_feed_click(v, atof(w[1]), atof(w[2]), n,
                                    mods_of(w, 4), GDK_BUTTON_PRIMARY);
        } else if (strcmp(cmd, "where") == 0) {
            OnPos c = on_note_view_caret(v);
            printf("line %d: caret %u:%d:%" G_GSIZE_FORMAT "\n", ln + 1,
                   c.block, c.cell, c.offset);
        } else if (strcmp(cmd, "print") == 0) {
            printf("line %d:\n", ln + 1);
            dump(on_note_view_document(v));
        } else if (strcmp(cmd, "check") == 0 && w[1] != NULL && w[2] != NULL) {
            guint i = (guint)atoi(w[1]);
            const OnBlock *b = on_document_block(on_note_view_document(v), i);
            const gchar *want_text = line + strlen(w[0]) + strlen(w[1]) +
                                     strlen(w[2]) + 3;
            gchar *have = (b != NULL && b->text != NULL)
                          ? g_strcompress(b->text->text->str) : g_strdup("");
            if (b == NULL || strcmp(KIND_NAMES[b->kind], w[2]) != 0 ||
                strcmp(have, want_text) != 0) {
                fprintf(stderr, "line %d: block %u is %s \"%s\", wanted %s "
                        "\"%s\"\n", ln + 1, i,
                        b != NULL ? KIND_NAMES[b->kind] : "missing", have,
                        w[2], want_text);
                rc = 1;
            }
            g_free(have);
        } else if (strcmp(cmd, "caret") == 0 && w[1] != NULL && w[2] != NULL) {
            OnPos c = on_note_view_caret(v);
            if (c.block != (guint)atoi(w[1]) || c.offset != (gsize)atoi(w[2])) {
                fprintf(stderr, "line %d: caret at %u:%" G_GSIZE_FORMAT
                        ", wanted %s:%s\n", ln + 1, c.block, c.offset, w[1],
                        w[2]);
                rc = 1;
            }
        } else {
            fprintf(stderr, "line %d: unknown command: %s\n", ln + 1, line);
            rc = 1;
        }
        g_strfreev(w);
        pump();
    }
    g_strfreev(lines);
    g_free(script);

    render(window, argv[3]);
    if (argc >= 5) {
        gsize n;
        guint8 *blob = on_note_view_serialize(v, &n);
        g_file_set_contents(argv[4], (const gchar *)blob, (gssize)n, NULL);
        g_free(blob);
    }
    on_db_close(app.db);
    return rc;
}
