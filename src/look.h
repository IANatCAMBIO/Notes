/* ===========================================================================
 * look.h — the look of the app in one place: colours and sizes
 *
 * These are the numbers that were CSS strings and layout #defines scattered
 * through the GTK sources (doc_layout.c, library_window.c, image_viewer.c,
 * media_window.c, editor_window.c), plus the window metrics of the Files
 * app this port takes its look from.  A module that needs a colour or a
 * size reads it from here; it does not define its own.
 *
 * COLOURS.  The GTK build was light-only.  Every colour here has a light
 * and a dark value; the light values are the GTK build's, the dark values
 * are new.  Drawing code picks with on_look_pick(colour, dark), where
 * `dark` comes from the view's effective appearance (ui_util.h:
 * on_ui_is_dark).  Chrome that AppKit draws — window and sidebar
 * backgrounds, table rows, selection in tables, labels — uses the system's
 * semantic NSColors and has no entry here.
 *
 * SIZES are in points.
 *
 * This header is plain C so the editor's geometry (doc_layout.c) can use it.
 * =========================================================================== */

#ifndef ON_LOOK_H
#define ON_LOOK_H

#include <stdbool.h>

/* One colour, components 0..1.                                              */
typedef struct {
    double r, g, b, a;
} on_color_t;

/* A colour as the app defines it: one value per appearance.                 */
typedef struct {
    on_color_t light;
    on_color_t dark;
} on_look_color_t;

/*
 * on_look_pick — the value of a look colour for an appearance.
 *
 * Inputs:
 *   color — the look colour
 *   dark  — true under a dark appearance
 *
 * Output: the colour to draw with.
 */
static inline on_color_t
on_look_pick(on_look_color_t color, bool dark)
{
    return dark ? color.dark : color.light;
}

/* ── Type ─────────────────────────────────────────────────────────────────── */

/* THE size of the app's own text: every label, button, field, table cell
 * and tool panel (on_ui_use_ui_font applies it to a window).  Three things
 * keep their own: the status bars (on_look_status_font), the sidebar
 * (the system font, as Tasks' sidebar has it) and a note's text in the editor (on_look_font_size
 * and its headings).                                                      */
static const double on_look_ui_font          = 11.0;
static const double on_look_status_font      = 11.0;   /* the status bars, Settings' health plate:
                                                        * the Files and Tasks apps' status size  */
static const double on_look_status_hold      = 3.0;    /* a status message stays this long ...   */
static const double on_look_status_fade      = 1.0;    /* ... then fades out over this, as Tasks' */

/* ── The note editor ──────────────────────────────────────────────────────── */

static const on_look_color_t on_look_page         = { { 1.000, 1.000, 1.000, 1 }, { 0.118, 0.118, 0.118, 1 } };
static const on_look_color_t on_look_text         = { { 0.000, 0.000, 0.000, 1 }, { 0.875, 0.875, 0.875, 1 } };
static const on_look_color_t on_look_tag          = { { 0.764, 0.353, 0.000, 1 }, { 1.000, 0.624, 0.271, 1 } };  /* #tag text         */
static const on_look_color_t on_look_action       = { { 0.102, 0.373, 0.706, 1 }, { 0.384, 0.627, 0.918, 1 } };  /* '!' line          */
static const on_look_color_t on_look_code_fill    = { { 0.941, 0.941, 0.941, 1 }, { 0.165, 0.165, 0.165, 1 } };  /* code run shading  */
static const on_look_color_t on_look_code_number  = { { 0.533, 0.533, 0.533, 1 }, { 0.541, 0.541, 0.541, 1 } };  /* gutter numbers    */
static const on_look_color_t on_look_table_border = { { 0.733, 0.733, 0.733, 1 }, { 0.333, 0.333, 0.333, 1 } };
static const on_look_color_t on_look_table_header = { { 0.925, 0.925, 0.925, 1 }, { 0.200, 0.200, 0.200, 1 } };  /* header-row fill   */
static const on_look_color_t on_look_find_hit     = { { 1.000, 0.925, 0.545, 1 }, { 0.478, 0.392, 0.000, 1 } };
static const on_look_color_t on_look_selection    = { { 0.36, 0.56, 0.90, 0.35 }, { 0.36, 0.56, 0.90, 0.45 } };  /* view has focus    */
static const on_look_color_t on_look_selection_dim = { { 0.60, 0.60, 0.60, 0.30 }, { 0.50, 0.50, 0.50, 0.35 } }; /* view lacks focus  */
static const on_look_color_t on_look_checkbox     = { { 0.200, 0.470, 0.860, 1 }, { 0.208, 0.518, 0.894, 1 } };  /* ticked task box   */

static const double on_look_font_size        = 11.0;   /* body text                              */
static const double on_look_h1_scale         = 1.6;    /* heading 1, and the derived title line  */
static const double on_look_h2_scale         = 1.3;
static const double on_look_margin_left      = 16.0;
static const double on_look_margin_right     = 16.0;
static const double on_look_margin_top       = 12.0;
static const double on_look_margin_bottom    = 20.0;
static const double on_look_block_gap        = 2.0;    /* below every block                      */
static const double on_look_list_indent      = 32.0;   /* list text starts here; prefix in gutter */
static const double on_look_code_inset       = 24.0;   /* code text inside its shading           */
static const double on_look_code_inset_numbered = 36.0; /* … with room for line numbers          */
static const double on_look_code_pad         = 3.0;    /* shading above/below a code run         */
static const double on_look_table_pad        = 6.0;    /* cell padding                           */
static const double on_look_table_col_min    = 40.0;
static const double on_look_table_col_max    = 320.0;
static const double on_look_checkbox_size    = 14.0;
static const double on_look_image_thumb_w    = 200.0;  /* box a freshly inserted image fits      */
static const double on_look_image_thumb_h    = 125.0;
static const double on_look_tag_popup_row_h  = 20.0;   /* the #tag suggestion list               */
static const double on_look_tag_popup_min_w  = 120.0;
static const double on_look_tag_popup_pad    = 10.0;   /* row text inset, each side              */
static const int    on_look_tag_popup_max    = 8;      /* rows at most (GTK TAG_POPUP_MAX)       */

/* ── Windows ──────────────────────────────────────────────────────────────── */

static const double on_look_library_w        = 900.0;  /* initial content size                   */
static const double on_look_library_h        = 620.0;
static const double on_look_library_min_w    = 700.0;
static const double on_look_library_min_h    = 450.0;
static const double on_look_editor_w         = 544.0;  /* unless editor_win_w/h say otherwise    */
static const double on_look_editor_h         = 433.0;
static const double on_look_editor_min_w     = 480.0;  /* an editor window's least size          */
static const double on_look_editor_min_h     = 360.0;
static const double on_look_screen_margin    = 12.0;   /* a window placed in the screen's
                                                        * lower-right corner (new editors),
                                                        * this clear of the work area's edges;
                                                        * Tasks places Quick Task the same    */
static const double on_look_search_win_w     = 400.0;  /* the search window: its minimum, and */
static const double on_look_search_win_h     = 260.0;  /* the size it always opens at          */
static const double on_look_media_w          = 780.0;
static const double on_look_media_h          = 560.0;
static const double on_look_settings_max_h   = 600.0;

/* ── Tool panels (tool_panel.m) — the Files and Tasks apps' dialog ─────────── */

static const on_look_color_t on_look_hero         = { { 0.000, 0.447, 0.776, 1 }, { 0.000, 0.447, 0.776, 1 } };  /* hero strip's fill (white text) */
static const on_look_color_t on_look_hero_sub     = { { 0.880, 0.880, 0.880, 1 }, { 0.880, 0.880, 0.880, 1 } };  /* its second line                */
static const on_look_color_t on_look_hero_problem = { { 1.000, 0.920, 0.550, 1 }, { 1.000, 0.920, 0.550, 1 } };  /* ... when it reports a problem  */

static const double on_look_tool_min_w      = 340.0;
static const double on_look_hero_h          = 43.0;   /* the blue hero strip                   */
static const double on_look_hero_pad        = 16.0;   /* horizontal inset in the hero          */
static const double on_look_hero_gap        = 3.0;    /* hero title to subtitle                */
static const double on_look_hero_icon       = 22.0;   /* hero symbol                           */
static const double on_look_hero_icon_gap   = 10.0;   /* symbol to the text                    */
static const double on_look_tool_pad        = 16.0;   /* around the caller's controls          */
static const double on_look_tool_bar_h      = 40.0;   /* the button bar                        */
static const double on_look_tool_btn_h      = 25.0;
static const double on_look_tool_btn_w      = 83.0;   /* narrowest button                      */
static const double on_look_tool_btn_gap    = 8.0;
static const double on_look_tool_btn_margin = 12.0;
static const double on_look_tool_btn_pad    = 24.0;   /* label to button edge, long titles     */
static const double on_look_tool_msg_w      = 388.0;  /* wrapped message text                  */
static const double on_look_tool_msg_max_h  = 240.0;  /* taller messages scroll                */
static const double on_look_tool_emoji_w    = 44.0;   /* the Edit Tag panel's emoji field      */

/* ── Library ──────────────────────────────────────────────────────────────── */

/* The due tints are Tasks' (look.m there), light and dark alike. */
static const on_look_color_t on_look_due_overdue = { { 0.753, 0.110, 0.157, 1 }, { 0.965, 0.380, 0.318, 1 } };
static const on_look_color_t on_look_due_today   = { { 0.820, 0.604, 0.000, 1 }, { 0.973, 0.894, 0.361, 1 } };
static const on_look_color_t on_look_due_ahead   = { { 0.149, 0.635, 0.412, 1 }, { 0.341, 0.890, 0.537, 1 } };
static const on_look_color_t on_look_card        = { { 1.000, 1.000, 1.000, 1 }, { 0.165, 0.165, 0.165, 1 } };  /* grid thumbnail    */
static const on_look_color_t on_look_card_border = { { 0.800, 0.800, 0.800, 1 }, { 0.333, 0.333, 0.333, 1 } };
static const on_look_color_t on_look_card_text   = { { 0.267, 0.267, 0.267, 1 }, { 0.733, 0.733, 0.733, 1 } };

static const double on_look_toolbar_search_w = 220.0;  /* the library's "Search all notes" field */
static const double on_look_about_icon       = 128.0;
static const double on_look_format_bar_h     = 30.0;   /* editor's format bar, above a separator */
static const double on_look_format_btn_h     = 22.0;
static const double on_look_format_gap       = 2.0;    /* between buttons of one group           */
static const double on_look_format_group_gap = 10.0;   /* between groups                         */
static const double on_look_format_icon      = 16.0;   /* a format button's icon                 */
static const double on_look_find_w           = 180.0;  /* the editor's "Find in note" field      */
static const double on_look_status_h         = 20.0;   /* status bar, under a 1 pt separator     */
static const double on_look_favorites_bar_h  = 28.0;   /* the library's Favorites bar, over a rule */
static const double on_look_favorite_btn_h   = 24.0;   /* a favorite tag's button in it          */
static const double on_look_favorite_btn_min_w = 60.0;
static const double on_look_favorite_gap     = 4.0;    /* between buttons                        */
static const double on_look_status_inset     = 8.0;
static const double on_look_sidebar_w        = 180.0;  /* initial                                */
static const double on_look_sidebar_min_w    = 160.0;  /* also the fit-to-content floor          */
static const double on_look_sidebar_row_h    = 26.0;   /* a sidebar row, as in Tasks             */
static const double on_look_sidebar_text_pad = 4.0;    /* a sidebar row's text, from the left    */
static const double on_look_sidebar_count_pad = 6.0;   /* its count, from the right              */
static const double on_look_sidebar_max_fraction = 0.5; /* fit-to-content ceiling, of the split  */
static const double on_look_pane_min_w       = 400.0;  /* the notes pane                         */
static const double on_look_row_h            = 20.0;   /* compact list row                       */
static const double on_look_row_comfortable_h = 40.0;  /* title + preview                        */
static const double on_look_cell_pad_h       = 4.0;    /* a list cell's text, from either edge   */
static const double on_look_preview_alpha    = 0.65;   /* the dimmed preview line                */
static const double on_look_thumb_size       = 140.0;  /* grid card thumbnail, square            */
static const double on_look_thumb_image_w    = 128.0;  /* a note's first image inside the card   */
static const double on_look_thumb_image_h    = 72.0;
static const double on_look_thumb_text_font  = 8.0;
static const double on_look_media_thumb      = 144.0;  /* media browser cell, square             */

/* ── Image viewer overlay ─────────────────────────────────────────────────── */

static const on_look_color_t on_look_viewer_backdrop = { { 0.063, 0.063, 0.063, 0.88 }, { 0.063, 0.063, 0.063, 0.88 } };
static const on_look_color_t on_look_viewer_text     = { { 0.949, 0.949, 0.949, 1.0 },  { 0.949, 0.949, 0.949, 1.0 } };  /* on the backdrop, either appearance */
static const on_look_color_t on_look_viewer_link     = { { 0.541, 0.706, 0.973, 1.0 },  { 0.541, 0.706, 0.973, 1.0 } };  /* a clickable word                   */
static const double on_look_viewer_dim_alpha = 0.40;   /* a word that leads nowhere            */
static const double on_look_viewer_gap       = 18.0;   /* caption to action link               */
static const double on_look_viewer_row_gap   = 4.0;    /* picture to caption row, row to nav   */
static const double on_look_viewer_nav_gap   = 12.0;   /* Previous, |, Next                    */
static const double on_look_viewer_inset     = 20.0;   /* image clear of the overlay's edge      */
static const double on_look_viewer_caption_h = 30.0;
static const double on_look_viewer_nav_h     = 24.0;
static const double on_look_viewer_min_box   = 32.0;

#endif /* ON_LOOK_H */
