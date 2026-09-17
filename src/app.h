/* ===========================================================================
 * app.h — shared application context for Notes
 *
 * A single OnApp instance is created in main() and passed to every window.
 * It owns the database handle, tracks open editor windows, carries the
 * user's preferences, and loads button icons from the app-local icons/
 * folder (see on_app_icon_image_sized).
 * =========================================================================== */

#ifndef BLUE_APP_H
#define BLUE_APP_H

#include <gtk/gtk.h>
#include "db.h"

/* Semantic version, baked in by the Makefile (-DON_VERSION="x.y.z") so
 * the About dialog and the package filenames come from ONE definition
 * (the VERSION variable at the top of the Makefile).                        */
#ifndef ON_VERSION
#define ON_VERSION "dev"
#endif

/* ---------------------------------------------------------------------------
 * OnApp — global application state.
 *
 * Fields:
 *   gtk_app        — the GtkApplication driving the main loop.
 *   db             — open notes database (owned; closed at shutdown).
 *   editors        — map of open editor windows, keyed by note id
 *                    (gint64* keys, GtkWindow* values).  An entry exists
 *                    exactly while that note's editor window is open.
 *   library_window — the (single) library window, or NULL before startup.
 *   notify_notes_changed — hook installed by the library window: the
 *                    FULL refresh (sidebar counts/tags + notes pane).
 *                    For structural changes — notes created/moved/
 *                    deleted, database switched/restored, tag set
 *                    edited.  May be NULL.
 *   notify_note_saved — lighter hook, also installed by the library
 *                    window: updates the ONE row of the notes pane that
 *                    the saved note owns (title, modified time, preview),
 *                    leaving the rest of the model, the selection and the
 *                    scroll position alone.  Editor saves use this unless
 *                    the note's tag set or action set changed — a save can
 *                    change neither which notes are listed nor the folder
 *                    counts, so neither the sidebar nor a full repopulate
 *                    is warranted.  Takes the note's id so the row can be
 *                    found without rebuilding the model.  May be NULL.
 *   notify_status  — hook installed by the library window: shows an event
 *                    message ("DB saved", …) on the right side of its
 *                    status bar.  Post through on_app_status(), which
 *                    handles the hook being NULL.
 *   icons_dir      — absolute path of the local icons/ folder the button
 *                    icons (elementary SVGs) are loaded from (owned
 *                    string).
 *   code_copy_buttons — whether code blocks show their floating copy
 *                    button (File → Settings…); persisted as the
 *                    "code_copy_button" setting.
 *   sidebar_counts — whether the library sidebar shows note counts next
 *                    to folders and tags; persisted as the
 *                    "sidebar_counts" setting (default off).
 *   sidebar_fit_content — whether the sidebar sizes itself to the rows
 *                    currently on show: expanding a folder widens it so
 *                    the revealed rows are not ellipsized, collapsing one
 *                    gives the width back.  While it is on the divider
 *                    belongs to the app, so a width the user dragged does
 *                    not survive the next expand or collapse; turn it off
 *                    to keep a hand-set width.  Persisted as
 *                    "sidebar_fit_content" (default ON).
 *   first_line_title — whether line 0 of a note is treated as its title:
 *                    the editor centers that line and auto-formats it as
 *                    Heading 1 while a brand-new (or emptied) note's title
 *                    is still unwritten.  Off = a plain left-aligned body
 *                    line; line 0 still SUPPLIES the note title either
 *                    way.  Persisted as "first_line_title" (default on).
 *   compact_editor_toolbar — whether the editor toolbar collapses the
 *                    paragraph-style buttons (H1/H2/¶) into a "Styles"
 *                    menu button and the list buttons into a "Lists"
 *                    one; persisted as the "compact_editor_toolbar"
 *                    setting (default off).
 *   comfortable_list — whether the library list view shows tall rows
 *                    with a bold title and a small grey body-text
 *                    preview (Comfortable density); persisted as
 *                    "list_density_comfortable" (default off = Compact).
 *   db_dir         — custom directory holding the db (owned string), or
 *                    NULL for the default location.  Persisted in the
 *                    config FILE (notes.ini next to the binary), not
 *                    the database — the database's own location cannot
 *                    live inside it.
 *   statusbar_db_path — whether the library/editor status bars prefix
 *                    the folder path with the database file's path;
 *                    persisted as the "statusbar_db_path" setting
 *                    (default on).
 *   statusbar_note_id — whether each editor's status bar shows the
 *                    note's database id at the right edge; persisted as
 *                    the "statusbar_note_id" setting (default off).
 *   show_done_actions — whether the library's Action Items view lists
 *                    completed (struck-through) items too; persisted as
 *                    the "show_done_actions" setting (default on).
 *   ai_enabled     — master switch for AI features (Summary toolbar button
 *                    visible only when TRUE); persisted as "ai_enabled".
 *   ai_command     — shell command invoked to run the AI model (owned
 *                    string); set via File → Settings → AI Features.
 *   ai_custom_prompt — prompt text used for folders in Custom AI mode;
 *                    set via File → Settings → AI Features (owned string).
 *   notify_ai_changed — callback that shows or hides the AI toolbar button
 *                    when ai_enabled changes.
 *   notify_db_health — hook installed by the Settings window while it is
 *                    open: the async health pass landed its verdict, so
 *                    the Database plate repaints.  NULL otherwise.
 *   db_transient   — TRUE when the database that is currently open was
 *                    chosen interactively at launch (not the configured
 *                    default); used to decide whether to persist the path.
 *   touch_css      — GtkCssProvider that hides the touch drag handles and
 *                    magnifier (applied when touch assistance is disabled);
 *                    NULL when touch assistance is shown.
 *   backup_running — TRUE while a rotating-backup pass is on its worker
 *                    thread (see backup.h), so a timer tick cannot start a
 *                    second one over a manual "Back Up Now".
 *   backup_timer   — GLib source id of the periodic backup timer, or 0 when
 *                    backups are switched off or set to manual only.
 * ------------------------------------------------------------------------- */

typedef struct OnApp {
    GtkApplication  *gtk_app;
    OnDatabase      *db;
    GHashTable      *editors;
    GtkWidget       *library_window;
    void           (*notify_notes_changed)(struct OnApp *app);
    void           (*notify_note_saved)(struct OnApp *app, gint64 note_id);
    void           (*notify_status)(struct OnApp *app, const gchar *message);
    gchar           *icons_dir;
    gboolean         code_copy_buttons;
    gboolean         code_line_numbers;
    gboolean         sidebar_counts;
    gboolean         sidebar_fit_content;
    gboolean         first_line_title;
    gboolean         compact_editor_toolbar;
    gboolean         comfortable_list;
    gchar           *db_dir;
    gboolean         statusbar_db_path;
    gboolean         statusbar_note_id;
    gboolean         show_done_actions;
    gboolean         bold_list_titles;   /* bold titles in comfortable list mode */
    gboolean         ai_enabled;       /* AI features master kill switch       */
    gchar           *ai_command;       /* command path to invoke AI            */
    gchar           *ai_custom_prompt; /* prompt text for Custom folder mode   */
    void           (*notify_ai_changed)(struct OnApp *app); /* show/hide btn   */
    void           (*notify_db_health)(struct OnApp *app);  /* verdict landed  */
    gpointer         settings_db_section; /* the open Settings window's
                                           * Database plate (settings_window.c),
                                           * NULL while none is open       */
    gboolean         db_transient;     /* TRUE when the current DB was opened
                                        * for this session only (not default) */
    GtkCssProvider  *touch_css;        /* screen CSS hiding the touch aids
                                        * (selection handles + magnifier);
                                        * NULL = touch assistance shown       */
    gboolean         backup_running;   /* a rotating-backup pass is in flight
                                        * (see backup.h); the guard that keeps
                                        * a timer tick off a manual press     */
    guint            backup_timer;     /* the periodic backup timer's source
                                        * id, or 0 when backups are off       */
} OnApp;

/* ---------------------------------------------------------------------------
 * on_app_status() — post a one-line event message to the library window's
 * status bar (printf-style).  Safe to call from anywhere: a no-op until
 * the library window has installed app->notify_status.
 *   app — the application context.
 *   fmt — printf-style format for the message.
 * ------------------------------------------------------------------------- */
void on_app_status(OnApp *app, const gchar *fmt, ...) G_GNUC_PRINTF(2, 3);

/* ---------------------------------------------------------------------------
 * on_app_location_text() — format a folder path for a status bar's left
 * label: when the statusbar_db_path setting is on, the database file's
 * path is prepended as one continuous path ("<db path><location>", a
 * "/" inserted for the non-path views); otherwise the location is
 * returned as-is.
 *   app      — the application context.
 *   location — the folder-path part (e.g. "/Work/Projects", "#tag").
 * Returns a newly allocated string; free with g_free().
 * ------------------------------------------------------------------------- */
gchar *on_app_location_text(OnApp *app, const gchar *location);

/* ---------------------------------------------------------------------------
 * Emoji padding.  Apple Color Emoji, drawn through cairo's CoreText path,
 * inks WIDER than the advance Pango reserves for it (measured at 13 pt:
 * ink 22 px over a 17 px advance, all of the excess to the RIGHT), so the
 * character after an emoji lands on its right edge.  The remedy —
 * everywhere emoji are rendered, so the editor and the library's cells
 * cannot disagree — is letter spacing on the emoji, and ONLY the emoji:
 * Pango splits a run's spacing half-per-side, so the emoji's own run gets
 * half a gap each side.  Tagging the FOLLOWING character as well (the
 * rule until 2026-09-15) does not help — its left half lands in the same
 * gap, its right half opens the word after it ("W orld") — and the gap it
 * made (5 px) was exactly the overhang, i.e. no visible clearance at all.
 * The amount is MEASURED, not assumed: on_emoji_pad() reads the overhang
 * off a sample emoji in the caller's font, so a font that fits its
 * advance (Linux Noto, the fontconfig backend) gets no padding at all.
 * ------------------------------------------------------------------------- */
#define ON_EMOJI_GAP 2                   /* px of clear air past the ink      */

/* ---------------------------------------------------------------------------
 * on_is_emoji_char() — rough emoji detection: the blocks that render via
 * the color emoji font and overlap neighbouring text on macOS.
 *   c — the character.
 * Returns TRUE when it is one.
 * ------------------------------------------------------------------------- */
gboolean on_is_emoji_char(gunichar c);

/* ---------------------------------------------------------------------------
 * on_is_emoji_joiner() — a character that CONTINUES an emoji sequence and
 * must stay in the same padded run as the emoji before it: the variation
 * selectors (U+FE0E/FE0F — "\u2764\uFE0F" is the red heart), the zero
 * width joiner (families, professions), the keycap combiner and the tag
 * characters (subdivision flags).  A letter-spacing boundary is an
 * itemization boundary: padding only the base would shape the selector
 * on its own, as a visible hex box.
 *   c — the character.
 * Returns TRUE when it is one.
 * ------------------------------------------------------------------------- */
gboolean on_is_emoji_joiner(gunichar c);

/* ---------------------------------------------------------------------------
 * on_emoji_pad() — the letter spacing an emoji needs in a font, measured:
 * a sample emoji is laid out in the context's default font and its ink
 * overhang past the logical advance read off the extents.
 *   ctx — the widget's PangoContext (gtk_widget_get_pango_context).
 * Returns the spacing in Pango units — twice (overhang + ON_EMOJI_GAP),
 * since Pango puts half of it on each side — or 0 when the font's emoji
 * fit their advance, in which case callers must not pad at all.
 * ------------------------------------------------------------------------- */
gint on_emoji_pad(PangoContext *ctx);

/* ---------------------------------------------------------------------------
 * on_markup_escape_emoji() — escape text for Pango markup and wrap every
 * run of emoji in a letter-spacing span of `pad` (the padding rule above,
 * for a GtkCellRendererText or GtkLabel).
 *   text — plain UTF-8, may be NULL (treated as "").
 *   pad  — from on_emoji_pad(); 0 = a plain escape.
 * Returns newly allocated markup; free with g_free().
 * ------------------------------------------------------------------------- */
gchar *on_markup_escape_emoji(const gchar *text, gint pad);

/* ---------------------------------------------------------------------------
 * on_app_db_health_start() — THE integrity check of a freshly opened
 * database: launch (main.c) and File → Open Database File… both run it,
 * every time, with no switch — a check that can be skipped can only ever
 * report a silence that means "not looked".  It runs on a worker thread
 * (on_db_health_check_async) so the window is up while PRAGMA
 * integrity_check walks the file; when the verdict lands it posts a
 * status line, shows the warning dialog if anything is wrong, and calls
 * app->notify_db_health for the Settings plate.
 *   app — the application context, its database open.
 * ------------------------------------------------------------------------- */
void on_app_db_health_start(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_app_notice() — show a modal OK message (a GtkAlertDialog) over
 * `parent` and return at once.  Fire-and-forget: GTK4 has no blocking
 * dialogs, and no caller ever needed the dismissal.
 *   parent — transient parent window, or NULL.
 *   title  — the message's heading, or NULL for the plain message only.
 *   fmt    — printf-style message.
 * ------------------------------------------------------------------------- */
void on_app_notice(GtkWindow *parent, const gchar *title,
                   const gchar *fmt, ...) G_GNUC_PRINTF(3, 4);

/* What on_app_pick_path() asks for.                                         */
typedef enum {
    ON_PICK_OPEN,                    /* an existing file                    */
    ON_PICK_SAVE,                    /* a file name to write                */
    ON_PICK_FOLDER,                  /* an existing directory               */
} OnPickKind;

/* on_app_pick_path()'s completion: `path` is the chosen filesystem path
 * (OWNED by the callback: g_free it) or NULL when the chooser was
 * cancelled.                                                                */
typedef void (*OnPickFunc)(gchar *path, gpointer user_data);

/* ---------------------------------------------------------------------------
 * on_app_pick_path() — run a modal file chooser (a GtkFileDialog) and
 * hand the selection to `done`.  ASYNCHRONOUS: returns as soon as the
 * dialog is up; the rest of the caller's work lives in `done`.
 *   parent         — transient parent window, or NULL.
 *   title          — dialog title.
 *   kind           — what to pick (see OnPickKind).
 *   accept_label   — accept-button label (e.g. "_Open").
 *   filter_name    — display name of a single file filter, or NULL for
 *                    no filter (filter_pattern is ignored when NULL).
 *   filter_pattern — glob the filter matches (e.g. "*.db"), or NULL with
 *                    a filter_name for "every image format GDK loads".
 *   start_dir      — folder the chooser opens in, or NULL for GTK's
 *                    choice (last used).  A re-pick of a persisted
 *                    location passes that location, so the chooser
 *                    starts where the setting points.
 *   done           — completion callback (always called, once).
 *   user_data      — passed to `done`.
 * ------------------------------------------------------------------------- */
void on_app_pick_path(GtkWindow *parent, const gchar *title,
                      OnPickKind kind, const gchar *accept_label,
                      const gchar *filter_name, const gchar *filter_pattern,
                      const gchar *start_dir,
                      OnPickFunc done, gpointer user_data);

/* ---------------------------------------------------------------------------
 * on_app_init_icons_dir() — locate the icons/ folder next to the
 * executable (falling back to ./icons) and remember it in app->icons_dir.
 *   app   — the application context.
 *   argv0 — argv[0] from main(), used to find the executable's directory.
 * ------------------------------------------------------------------------- */
void on_app_init_icons_dir(OnApp *app, const gchar *argv0);

/* ---------------------------------------------------------------------------
 * on_app_texture_for_pixbuf() — THE pixbuf → GdkTexture edge.  A pixbuf
 * stays the in-memory image type inside serialize.c (the "on-png" bytes
 * cache, capped decodes); a widget wants a texture.  This wraps the
 * pixbuf's pixels in a GdkMemoryTexture — NOT the deprecated
 * gdk_texture_new_for_pixbuf, and not gdk_texture_new_from_bytes over the
 * cached PNG either: that decodes pixels the pixbuf already holds.  The
 * bytes reference keeps the pixbuf alive for the texture's lifetime.
 *   pixbuf — the source; not consumed.
 * Returns a new texture (g_object_unref it).
 * ------------------------------------------------------------------------- */
GdkTexture *on_app_texture_for_pixbuf(GdkPixbuf *pixbuf);

/* ---------------------------------------------------------------------------
 * on_app_icon_image_sized() — a GtkImage showing icon `name`, at a LOGICAL
 * pixel size.  The icons are the PNGs in the app-local icons/ folder, which
 * main() adds to the icon theme's search path: GTK picks them up by
 * basename as unthemed icons, loads them at the display's scale factor
 * (sharp on HiDPI, quirk #5), caches them, and draws at the logical size.
 * Swapping a PNG in icons/ still re-themes a button (restart to see it).
 *   app  — the application context (unused: the theme knows the folder).
 *   name — icon file basename without extension (e.g. "new-folder").
 *   size — logical pixel size to draw at.
 * Returns a new GtkImage, or NULL if the theme has no such icon — callers
 * fall back to a text label in that case.
 * ------------------------------------------------------------------------- */
GtkWidget *on_app_icon_image_sized(OnApp *app, const gchar *name,
                                   gint size);

/* ---------------------------------------------------------------------------
 * on_app_icon_paintable() — the same icon as a paintable, for uses that
 * need one rather than a widget (gtk_drag_source_set_icon).  Same lookup;
 * rendered at the display's scale factor.
 *   app  — the application context (unused).
 *   name — icon file basename without extension (e.g. "folder").
 *   size — logical pixel size.
 * Returns a new paintable (g_object_unref it), or NULL if the theme has no
 * such icon.
 * ------------------------------------------------------------------------- */
GdkPaintable *on_app_icon_paintable(OnApp *app, const gchar *name,
                                    gint size);

/* ---------------------------------------------------------------------------
 * on_app_tool_item_new() — create an icon toolbar button: a flat
 * GtkButton (or GtkToggleButton) whose child is the icon widget.  GTK4
 * has no GtkToolbar; a toolbar is a GtkBox with the "toolbar" style class
 * holding these.
 *   app             — the application context.
 *   toggle          — TRUE for a GtkToggleButton, FALSE for a GtkButton.
 *   icon_name       — local icon file to use (see on_app_icon_image), or
 *                     NULL for none.
 *   fallback_markup — Pango markup rendered as the "icon" when the icon
 *                     file is missing (e.g. "<b>H1</b>"); NULL to fall
 *                     back to the plain label.
 *   label           — the button's accessible text label (its accessible
 *                     name); also the icon stand-in when both the icon
 *                     file and fallback_markup are absent.
 *   tooltip         — hover help text.
 * Returns the new button.
 * ------------------------------------------------------------------------- */
GtkWidget *on_app_tool_item_new(OnApp *app, gboolean toggle,
                                const gchar *icon_name,
                                const gchar *fallback_markup,
                                const gchar *label,
                                const gchar *tooltip);

/* ---------------------------------------------------------------------------
 * on_app_tool_item_set_icon() — re-point an existing toolbar button at a
 * different icon, for a button whose image names the ACTION it offers
 * rather than a fixed command (the library's List/Grid toggle).  Follows
 * the same icon-file-else-fallback-markup rule as on_app_tool_item_new,
 * and keeps the button's accessible label and tooltip untouched.
 *   app             — the application context.
 *   button          — the button to re-point (from on_app_tool_item_new).
 *   icon_name       — local icon file basename, or NULL.
 *   fallback_markup — markup shown when the file is missing.
 * ------------------------------------------------------------------------- */
void on_app_tool_item_set_icon(OnApp *app, GtkWidget *button,
                               const gchar *icon_name,
                               const gchar *fallback_markup);

/* ---------------------------------------------------------------------------
 * on_app_set_tooltip() — THE way to give a widget a tooltip (NULL removes
 * it).  A plain gtk_widget_set_tooltip_text is broken on macOS: GTK keeps
 * ONE tooltip popup surface and re-presents it for every tooltip, and when
 * the next tooltip needs a different size the surface is resized while
 * hidden — which the macOS backend's layer does not follow for about a
 * second (D32: the content stays tiled at the previous size and the box
 * comes out cut off mid-text).  It shows on a hover that follows another
 * tooltip closely — sweeping along a toolbar.  So the helper shows the
 * text through a custom label and REFUSES a tooltip asked for within
 * TOOLTIP_MIN_GAP_MS of the previous one hiding, asking again when the
 * time has passed: tooltips keep their natural size, and consecutive ones
 * come a beat slower than GTK's browse mode would show them.  And NO
 * tooltip shows in a window that is not the active one (D35): the macOS
 * backend hands the pointer to the window behind an editor's title bar,
 * and a tooltip popup shown there brings that window to the front.
 * ------------------------------------------------------------------------- */
void on_app_set_tooltip(GtkWidget *widget, const gchar *text);

/* ---------------------------------------------------------------------------
 * on_app_menu_popup() — pop up a one-shot context menu built from a menu
 * model, at a point in a widget.  THE transient-popup scaffold for every
 * right-click menu in the app: a GtkPopoverMenu parented to the WINDOW's
 * child box (not to `attach` — see the implementation for why a tree view
 * or text view cannot take one), pointing at (x, y) translated into that
 * box, that unparents and drops itself once closed.  Actions resolve
 * through the window either way.
 *   attach — the widget the press landed in (any widget in a window).
 *   model  — the items; OWNERSHIP IS TAKEN (the popover keeps its own ref).
 *   x, y   — the press position in `attach`'s coordinates (what a
 *            GtkGestureClick "pressed" handler receives).
 * ------------------------------------------------------------------------- */
void on_app_menu_popup(GtkWidget *attach, GMenuModel *model,
                       gdouble x, gdouble y);

/* ---------------------------------------------------------------------------
 * on_app_double_click_watch() — call `cb` when `widget` is double-clicked
 * with the primary button, counted by THIS app from the time and distance
 * between two presses (the gtk-double-click-time / -distance settings),
 * not by GtkGestureClick's n_press.  Measured on the macOS backend (D34):
 * GDK's fill_motion_event takes a motion event's BUTTON STATE from
 * [NSEvent pressedMouseButtons] when the event is TRANSLATED, not from the
 * event, so a drag motion of the first click that GTK gets to after the
 * button is already up carries no BUTTON1 — and GtkGestureSingle resets
 * an active gesture on a motion with no button (gtkgesturesingle.c,
 * `button == 0`).  Every GtkGestureClick in the window is reset that way,
 * GTK's own row gesture included, so its count restarts and the second
 * press arrives as a first one; a double-click then took a third or
 * fourth click, depending on whether the pointer moved a pixel under the
 * finger and on whether the main loop was busy at that moment.  The count
 * kept here lives OUTSIDE the gesture, so a reset cannot touch it.  A
 * capture-phase gesture on `widget`; on the second press it CLAIMS the
 * sequence, so GTK's own activation cannot fire a second time when its
 * count did survive.
 *   widget — the widget to watch (a list row or cell).
 *   cb     — called as cb(widget, data) on the double-click.
 *   data   — its user data.
 * ------------------------------------------------------------------------- */
typedef void (*OnDoubleClickFunc)(GtkWidget *widget, gpointer data);
void on_app_double_click_watch(GtkWidget *widget, OnDoubleClickFunc cb,
                               gpointer data);

/* ---------------------------------------------------------------------------
 * on_app_select_on_press() — make a list row select on the PRESS, not on
 * the release (D37).  GTK4's row widget selects from its click gesture's
 * "released" (gtklistfactorywidget.c), so a row looked unselected for
 * the length of the click.  A capture-phase primary-button gesture on
 * `widget`: the press runs the view's own "list.select-item" action
 * with the modifiers GTK would read (Shift extends, Control — Command on
 * macOS — toggles), except an unmodified press on a row that is ALREADY
 * selected, which leaves the selection alone so a drag can carry a
 * multi-selection; the release CLAIMS the sequence (cancelling the row's
 * own gesture, so it cannot select a second time and undo a toggle) and
 * collapses to the pressed row when the press left it for a drag that
 * never came.  Anything in `widget` that has a gesture of its own (a
 * check button, an expander's arrow) must NOT be inside it — the claim
 * cancels every gesture below — so the sidebar installs this on the
 * expander's LABEL and the Action Items view leaves its check cell out.
 *   widget — the cell or row child to watch.
 *   item   — its GtkListItem (the position is read at press time).
 * ------------------------------------------------------------------------- */
void on_app_select_on_press(GtkWidget *widget, GtkListItem *item);

/* ---------------------------------------------------------------------------
 * on_app_install_accels() — THE keyboard-shortcut table, bound once at
 * startup with gtk_application_set_accels_for_action.  Every shortcut is a
 * "win." action, so the same key can mean different things in different
 * windows (Primary+M: media browser in the library, code block in an
 * editor) — GTK activates whichever of an accel's actions the focused
 * window has and has enabled.  "app." actions carry no accelerators except
 * quit and preferences, which nothing else uses.  <Primary> is Command on
 * macOS and Control elsewhere.
 *   gtk_app — the application.
 * ------------------------------------------------------------------------- */
void on_app_install_accels(GtkApplication *gtk_app);

/* ---------------------------------------------------------------------------
 * on_app_config_init() — resolve the application config file once
 * ("notes.ini" in the same directory as the binary, from `argv0`)
 * and load it into memory.  All later reads are served from memory; the
 * file is only written when a setting changes.  Must run before any
 * other config call; safe to call repeatedly.
 * ------------------------------------------------------------------------- */
void on_app_config_init(const gchar *argv0);

/* ---------------------------------------------------------------------------
 * on_app_config_get() — read one setting from the in-memory config.
 * Returns a new string (g_free() it), or NULL when unset/empty.
 * ------------------------------------------------------------------------- */
gchar *on_app_config_get(const gchar *key);

/* ---------------------------------------------------------------------------
 * on_app_config_get_bool() — read a 0/1 setting; `def` when unset.  The
 * app only ever writes "0"/"1", so any other stored value reads as "1".
 * ------------------------------------------------------------------------- */
gboolean on_app_config_get_bool(const gchar *key, gboolean def);

/* ---------------------------------------------------------------------------
 * on_app_apply_touch_assist() — honor the "touch_assist" setting (`1|0`,
 * default 0 = DISABLED): unless enabled, install a screen-wide CSS
 * provider that hides GTK's touch aids — the teardrop drag handles under
 * selections/the cursor ("cursor-handle" nodes, collapsed to nothing)
 * and the selection magnifier (its popover, rendered transparent).  Some
 * Linux input stacks pop these up for plain mouse selections; GTK has no
 * API to turn them off, so CSS is the lever.  Removes the provider again
 * when assistance is re-enabled.  Safe to call any time after GTK is
 * initialized; applies live.  The node names are verified against GTK
 * 4.22.4 (see the function); the GTK3 build also set
 * GDK_CORE_DEVICE_EVENTS=1 to suppress the tap cut/copy/paste bubble, which
 * has no GTK4 equivalent.
 *   app — the application context (owns the provider).
 * ------------------------------------------------------------------------- */
void on_app_apply_touch_assist(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_app_install_css() — the app-wide DISPLAY-level stylesheet, installed
 * once per process (later calls are no-ops): the rules for the "notes-"
 * classes MORE THAN ONE window uses, so each look has one definition.
 *   notes-status-label — a status-bar label (library and editor): 85%.
 *   notes-dot-label    — a one-glyph indicator label (the editor's
 *                        save-state dot, the Settings health LED): 70%.
 * Per-window rules live in each module's own <module>_install_css().
 * Call any time after GTK is initialized (there must be a display).
 * ------------------------------------------------------------------------- */
void on_app_install_css(void);

/* ---------------------------------------------------------------------------
 * on_app_config_set() — change one setting: updates the in-memory config
 * AND writes the ini file through.  NULL removes the key.
 * ------------------------------------------------------------------------- */
void on_app_config_set(const gchar *key, const gchar *value);

/* ---------------------------------------------------------------------------
 * on_app_config_get_size() — read a persisted window size from two ini keys.
 * *w and *h are left ALONE unless both keys are present and both parse to a
 * positive number, so the caller just pre-loads its defaults.  The editor and
 * the search window each used to spell this out.
 *   key_w / key_h — the two ini keys (e.g. "search_win_w"/"search_win_h").
 *   w / h         — in/out: the caller's defaults, replaced on success.
 * ------------------------------------------------------------------------- */
void on_app_config_get_size(const gchar *key_w, const gchar *key_h,
                            gint *w, gint *h);

/* ---------------------------------------------------------------------------
 * on_app_read_stream() — read a whole stdio stream into a new string.
 * Used for the CLI's "-" (read stdin) arguments and for collecting a
 * delegated command's captured output.
 *   f      — the stream to drain.
 *   rewind_first — TRUE to rewind() before reading (a temp capture file);
 *                  FALSE to read from the current position (stdin).
 * Returns a newly allocated, NUL-terminated string; g_free() it.
 * ------------------------------------------------------------------------- */
gchar *on_app_read_stream(FILE *f, gboolean rewind_first);

/* ---------------------------------------------------------------------------
 * on_app_config_load_db_dir() — read the custom database directory from
 * the config file. Returns a new string (g_free() it), or NULL when the
 * default location is in use.
 * ------------------------------------------------------------------------- */
gchar *on_app_config_load_db_dir(void);

/* ---------------------------------------------------------------------------
 * on_app_close_all_editors() — destroy every open editor window (each
 * flushes its final autosave on destroy). Used before switching or
 * restoring the database.
 * ------------------------------------------------------------------------- */
void on_app_close_all_editors(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_app_actions_backfill() — one-time population of the action_items
 * table from pre-existing note content (gated by PRAGMA user_version, so
 * repeat calls cost a single PRAGMA read).  Run after EVERY on_db_open
 * that yields a long-lived handle: GUI startup, the headless CLI path,
 * and database switch/restore (an adopted or restored file may predate
 * the feature).  NULL-safe.
 * ------------------------------------------------------------------------- */
void on_app_actions_backfill(OnDatabase *db);

/* ---------------------------------------------------------------------------
 * on_app_action_uids_backfill() — one-time assignment of stable uids to
 * action_items rows written before the uid column existed (gated by
 * PRAGMA user_version, like on_app_actions_backfill, which it must run
 * AFTER: that one indexes the rows, this one identifies them).  Cheap —
 * it touches the table only, never the note blobs.  NULL-safe.
 * ------------------------------------------------------------------------- */
void on_app_action_uids_backfill(OnDatabase *db);

#endif /* BLUE_APP_H */
