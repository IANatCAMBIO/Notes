/*
 * apple_notes.m — see apple_notes.h.
 *
 * Three layers, top to bottom: the sweep (a main_service timer that finds
 * the notes whose pushed state is stale), the push (one note into its
 * ICNote, through the converter), and the framework (NotesShared reached
 * through dlopen, every class and selector checked once at load).  The
 * binding map, a SQLite file of our own in the data directory, ties a
 * note id to the Apple identifier of its copy and remembers what was
 * pushed.
 */
#import "apple_notes.h"

#import <CoreData/CoreData.h>
#import <CoreGraphics/CoreGraphics.h>
#include <dlfcn.h>
#include <sqlite3.h>
#include <unistd.h>

#include "config.h"
#include "db.h"
#include "identity.h"
#include "look.h"
#include "main_service.h"
#include "serialize.h"
#import "editor_window.h"

/* ---------------------------------------------------------------------------
 * NotesShared, as the compiler needs to see it.  The framework ships no
 * headers: each selector the module sends is declared here with the
 * signature the runtime reports, and objects are messaged through these
 * protocols so the compiler emits the right call and ARC the right
 * ownership (a "new…" class method returns an owned object).
 * ------------------------------------------------------------------------- */
@protocol ic_note_context_class <NSObject>
+ (BOOL)hasSharedContext;
+ (void)startSharedContextWithOptions:(unsigned long long)options;
+ (id)sharedContext;
@end

@protocol ic_note_context <NSObject>
- (NSManagedObjectContext *)managedObjectContext;
- (BOOL)save;
- (id)crossProcessChangeCoordinator;
@end

@protocol ic_change_coordinator <NSObject>
- (void)postEditorExtensionDidSaveNotification;
@end

@protocol ic_account_class <NSObject>
+ (NSArray *)allActiveAccountsInContext:(NSManagedObjectContext *)moc;
@end

@protocol ic_account <NSObject>
- (BOOL)isPrimaryiCloudAccount;
- (id)visibleFolders;
- (id)trashFolder;
@end

@protocol ic_folder_class <NSObject>
+ (id)folderWithIdentifier:(NSString *)identifier context:(NSManagedObjectContext *)moc;
+ (id)newFolderInAccount:(id)account;
@end

@protocol ic_folder <NSObject>
- (void)setTitle:(NSString *)title;
- (void)updateChangeCountWithReason:(NSString *)reason;
@end

@protocol ic_note_class <NSObject>
+ (id)newEmptyNoteInFolder:(id)folder;
+ (id)noteWithIdentifier:(NSString *)identifier context:(NSManagedObjectContext *)moc;
@end

@protocol ic_note <NSObject>
- (id)mergeableString;
- (void)beginEditing;
- (void)endEditing;
- (void)didChangeNoteText;
- (void)saveNoteData;
- (BOOL)regenerateTitle:(BOOL)title snippet:(BOOL)snippet;
- (void)updateChangeCountWithReason:(NSString *)reason;
- (void)setIsPinned:(BOOL)pinned;
- (void)setFolder:(id)folder;
- (void)setShouldAddMediaAsynchronously:(BOOL)asynchronously;
- (id)addAttachmentWithFileURL:(NSURL *)url filename:(NSString *)filename;
- (id)addTableAttachmentWithText:(NSAttributedString *)text;
@end

@protocol ic_mergeable_string <NSObject>
- (NSAttributedString *)attributedString;
- (NSUInteger)length;
- (void)replaceCharactersInRange:(NSRange)range
            withAttributedString:(NSAttributedString *)text;
@end

@protocol ic_tt_attachment <NSObject>
- (void)setAttachmentIdentifier:(NSString *)identifier;
- (void)setAttachmentUTI:(NSString *)uti;
- (NSString *)attachmentIdentifier;
- (NSString *)attachmentUTI;
- (id)attachmentInContext:(NSManagedObjectContext *)moc;
- (id)inlineAttachmentInContext:(NSManagedObjectContext *)moc;
@end

@protocol ic_paragraph_style <NSObject>
- (void)setStyle:(unsigned int)style;
- (void)setUuid:(NSUUID *)uuid;
- (void)setTodo:(id)todo;
@end

@protocol ic_todo <NSObject>
- (instancetype)initWithIdentifier:(NSUUID *)identifier done:(BOOL)done;
@end

@protocol ic_inline_attachment_class <NSObject>
+ (id)newHashtagAttachmentWithIdentifier:(NSString *)identifier
                             hashtagText:(NSString *)text
              creatingHashtagIfNecessary:(BOOL)creating
                                    note:(id)note
                        parentAttachment:(id)parent;
@end

@protocol ic_attachment <NSObject>
- (void)setPreferredViewSize:(short)size;
@end

@protocol ic_syncing_object <NSObject>
- (void)markForDeletion;
@end

/* The classes, looked up once by on_apple_notes_load_framework.           */
static Class cls_context, cls_account, cls_folder, cls_note, cls_tt_attachment,
             cls_paragraph_style, cls_todo, cls_inline_attachment;

/* What the framework check found: nil while usable.                         */
static NSString *framework_problem;
static BOOL      framework_checked;

/* The NoteStore the framework opens: the container Full Disk Access guards. */
#define APPLE_NOTES_STORE "Library/Group Containers/group.com.apple.notes/NoteStore.sqlite"
#define NOTES_SHARED_PATH "/System/Library/PrivateFrameworks/NotesShared.framework/NotesShared"
#define NOTES_UI_PATH     "/System/Library/PrivateFrameworks/NotesUI.framework/NotesUI"

/* Notes' preferredViewSize values for an image attachment.                  */
#define APPLE_VIEW_LARGE 0
#define APPLE_VIEW_SMALL 1

/* The sweep: how often it looks for changes, and the pause between the
 * pushes of a run — one note per pause, so the library's progress line can
 * count them and the app stays responsive between two.                      */
static const double sweep_seconds = 3.0;
static const double drain_seconds = 0.25;

/* The binding map's file in the data directory.                             */
#define MAP_FILENAME "apple_notes.db"

/*
 * require_class — look a class up and record the problem when it is
 * missing or lacks a selector the module sends.
 *
 * Inputs:
 *   name      — the class
 *   selectors — the selectors it must answer, instance ones as "-sel",
 *               class ones as "+sel"
 *
 * Output: the class, or Nil (framework_problem then says what is missing).
 */
static Class
require_class(NSString *name, NSArray<NSString *> *selectors)
{
    Class cls = NSClassFromString(name);
    if (cls == Nil) {
        framework_problem = [NSString stringWithFormat:@"Apple Notes' framework has no %@ class on this macOS.", name];
        return Nil;
    }
    for (NSString *s in selectors) {
        SEL sel = NSSelectorFromString([s substringFromIndex:1]);
        BOOL ok = [s hasPrefix:@"+"] ? [cls respondsToSelector:sel] : [cls instancesRespondToSelector:sel];
        if (!ok) {
            framework_problem = [NSString stringWithFormat:@"Apple Notes' %@ has no %@ on this macOS.", name, s];
            return Nil;
        }
    }
    return cls;
}

/*
 * on_apple_notes_load_framework — see apple_notes.h.
 */
BOOL
on_apple_notes_load_framework(NSString **why)
{
    if (!framework_checked) {
        framework_checked = YES;
        if (dlopen(NOTES_SHARED_PATH, RTLD_NOW) == NULL || dlopen(NOTES_UI_PATH, RTLD_NOW) == NULL) {
            framework_problem = [NSString stringWithFormat:@"Apple Notes' framework could not be loaded: %s", dlerror()];
        } else {
            cls_context = require_class(@"ICNoteContext",
                @[@"+hasSharedContext", @"+startSharedContextWithOptions:", @"+sharedContext",
                  @"-managedObjectContext", @"-save", @"-crossProcessChangeCoordinator"]);
            cls_account = require_class(@"ICAccount",
                @[@"+allActiveAccountsInContext:", @"-isPrimaryiCloudAccount", @"-visibleFolders", @"-trashFolder"]);
            cls_folder = require_class(@"ICFolder",
                @[@"+folderWithIdentifier:context:", @"+newFolderInAccount:", @"-setTitle:",
                  @"-updateChangeCountWithReason:"]);
            cls_note = require_class(@"ICNote",
                @[@"+newEmptyNoteInFolder:", @"+noteWithIdentifier:context:", @"-mergeableString",
                  @"-beginEditing", @"-endEditing", @"-didChangeNoteText", @"-saveNoteData",
                  @"-regenerateTitle:snippet:", @"-updateChangeCountWithReason:", @"-setIsPinned:",
                  @"-setFolder:", @"-setShouldAddMediaAsynchronously:", @"-addAttachmentWithFileURL:filename:",
                  @"-addTableAttachmentWithText:"]);
            cls_tt_attachment = require_class(@"ICTTAttachment",
                @[@"-setAttachmentIdentifier:", @"-setAttachmentUTI:", @"-attachmentIdentifier", @"-attachmentUTI",
                  @"-attachmentInContext:", @"-inlineAttachmentInContext:"]);
            cls_paragraph_style = require_class(@"ICTTMutableParagraphStyle", @[@"-setStyle:", @"-setUuid:", @"-setTodo:"]);
            cls_todo = require_class(@"ICTTTodo", @[@"-initWithIdentifier:done:"]);
            cls_inline_attachment = require_class(@"ICInlineAttachment",
                @[@"+newHashtagAttachmentWithIdentifier:hashtagText:creatingHashtagIfNecessary:note:parentAttachment:"]);
        }
    }
    if (why != NULL) {
        *why = framework_problem;
    }
    return framework_problem == nil;
}

/* ---------------------------------------------------------------------------
 * THE CONVERTER
 * ------------------------------------------------------------------------- */

/*
 * NotesAppleWalk — the state of one text block's walk (on_text_walk): the
 * paragraph being appended to, where the action colour starts, and the
 * byte offset the walk stands at.
 */
@interface NotesAppleWalk : NSObject
@property (nonatomic, strong) NSMutableAttributedString *paragraph;
@property (nonatomic, strong) id<apple_notes_objects> objects;
@property (nonatomic, assign) gssize mark;       /* action mark, byte offset, or -1 */
@property (nonatomic, assign) gsize  bytes;      /* walked so far                   */
@end

@implementation NotesAppleWalk
@end

/*
 * action_color — the action colour as a CGColor, made once (the light
 * value: Apple Notes keeps one colour for both appearances).
 *
 * Output: the colour (owned by the module).
 */
static CGColorRef
action_color(void)
{
    static CGColorRef color;
    if (color == NULL) {
        shared_rgba_t c = on_look_action.light;
        color = CGColorCreateSRGB(c.r, c.g, c.b, c.a);
    }
    return color;
}

/*
 * run_attributes — the attributes of a run's text: the inline flags, and
 * the action colour when the run is past the mark.
 *
 * Inputs:
 *   flags   — the run's ON_FMT_* bits
 *   colored — whether the action colour applies
 *
 * Output: a new dictionary.
 */
static NSMutableDictionary *
run_attributes(guint32 flags, BOOL colored)
{
    NSMutableDictionary *attrs = [NSMutableDictionary dictionary];
    int hints = (flags & ON_FMT_BOLD ? ON_APPLE_HINT_BOLD : 0) | (flags & ON_FMT_ITALIC ? ON_APPLE_HINT_ITALIC : 0);
    if (hints != 0) {
        attrs[ON_APPLE_ATTR_HINTS] = @(hints);
    }
    if (flags & ON_FMT_UNDERLINE) {
        attrs[ON_APPLE_ATTR_UNDERLINE] = @1;
    }
    if (flags & ON_FMT_STRIKE) {
        attrs[ON_APPLE_ATTR_STRIKE] = @1;
    }
    if (colored) {
        attrs[ON_APPLE_ATTR_COLOR] = (__bridge id)action_color();
    }
    return attrs;
}

/*
 * append_bytes — append `n` bytes of UTF-8 to the paragraph under `attrs`.
 *
 * Inputs:
 *   w     — the walk
 *   s, n  — the bytes
 *   attrs — their attributes
 */
static void
append_bytes(NotesAppleWalk *w, const gchar *s, gsize n, NSDictionary *attrs)
{
    if (n == 0) {
        return;
    }
    NSString *text = [[NSString alloc] initWithBytes:s length:n encoding:NSUTF8StringEncoding];
    if (text != nil) {
        [w.paragraph appendAttributedString:[[NSAttributedString alloc] initWithString:text attributes:attrs]];
    }
}

/*
 * append_object — append the attachment character for an object, or
 * nothing when the source made none.
 *
 * Inputs:
 *   w       — the walk
 *   tt      — the ICTTAttachment, or nil
 *   colored — whether the action colour applies
 */
static void
append_object(NotesAppleWalk *w, id tt, BOOL colored)
{
    if (tt == nil) {
        return;
    }
    NSMutableDictionary *attrs = run_attributes(0, colored);
    attrs[ON_APPLE_ATTR_ATTACHMENT] = tt;
    [w.paragraph appendAttributedString:[[NSAttributedString alloc] initWithString:@"￼" attributes:attrs]];
}

/*
 * walk_text — on_text_walk's text callback: a stretch of a run, split
 * where the action mark falls inside it.
 */
static void
walk_text(const gchar *s, gsize n, guint32 flags, gpointer data)
{
    NotesAppleWalk *w = (__bridge NotesAppleWalk *)data;
    gsize before = n;
    if (w.mark >= 0 && (gsize)w.mark > w.bytes && (gsize)w.mark < w.bytes + n) {
        before = (gsize)w.mark - w.bytes;
    }
    BOOL colored = w.mark >= 0 && (gsize)w.mark <= w.bytes;
    append_bytes(w, s, before, run_attributes(flags, colored));
    if (before < n) {
        append_bytes(w, s + before, n - before, run_attributes(flags, YES));
    }
    w.bytes += n;
}

/*
 * walk_object — on_text_walk's object callback: an inline image or file.
 */
static void
walk_object(const OnInlineObject *obj, gpointer data)
{
    NotesAppleWalk *w = (__bridge NotesAppleWalk *)data;
    BOOL colored = w.mark >= 0 && (gsize)w.mark <= w.bytes;
    id tt = nil;
    if (obj->kind == ON_OBJECT_IMAGE) {
        gsize n = 0;
        const void *bytes = g_bytes_get_data(obj->png, &n);
        tt = [w.objects image_attachment:[NSData dataWithBytes:bytes length:n] small:obj->display_width == 0];
    } else {
        tt = [w.objects file_attachment:obj->file_id name:@(obj->file_name)];
    }
    append_object(w, tt, colored);
    w.bytes += ON_OBJ_CHAR_LEN;
}

/*
 * make_style — a paragraph style of its own identity.
 *
 * Inputs:
 *   style — the on_apple_style_t number
 *   todo  — whether it carries a to-do
 *   done  — the to-do's state
 *
 * Output: the ICTTMutableParagraphStyle.
 */
static id
make_style(unsigned int style, BOOL todo, BOOL done)
{
    id<ic_paragraph_style> s = [[cls_paragraph_style alloc] init];
    [s setStyle:style];
    [s setUuid:[NSUUID UUID]];
    if (todo) {
        [s setTodo:[(id<ic_todo>)[cls_todo alloc] initWithIdentifier:[NSUUID UUID] done:done]];
    }
    return s;
}

/*
 * block_style — the paragraph style number of a block kind.
 *
 * Inputs:
 *   kind  — the block kind
 *   title — whether this block is the title line
 *
 * Output: the number.
 */
static unsigned int
block_style(OnBlockKind kind, BOOL title)
{
    if (title) {
        return on_apple_style_title;
    }
    switch (kind) {
    case ON_BLOCK_H1:     return on_apple_style_heading;
    case ON_BLOCK_H2:     return on_apple_style_subheading;
    case ON_BLOCK_BULLET: return on_apple_style_bullet;
    case ON_BLOCK_NUMBER: return on_apple_style_numbered;
    case ON_BLOCK_CHECK:  return on_apple_style_checklist;
    case ON_BLOCK_CODE:   return on_apple_style_monospaced;
    default:              return on_apple_style_body;
    }
}

/*
 * cell_text — a table cell's text on one line (Notes' table builder reads
 * tab-separated rows).
 *
 * Inputs:
 *   cell — the cell, or NULL
 *
 * Output: the text.
 */
static NSString *
cell_text(const OnText *cell)
{
    if (cell == NULL || cell->text == NULL) {
        return @"";
    }
    NSString *s = @(cell->text->str);
    s = [s stringByReplacingOccurrencesOfString:@"\n" withString:@" "];
    return [s stringByReplacingOccurrencesOfString:@"\t" withString:@" "];
}

/*
 * block_paragraph — one block as one paragraph, styled.
 *
 * Inputs:
 *   b       — the block
 *   title   — whether it is the title line
 *   objects — the object source
 *
 * Output: the paragraph (its newline is added by the caller).
 */
static NSMutableAttributedString *
block_paragraph(const OnBlock *b, BOOL title, id<apple_notes_objects> objects)
{
    NotesAppleWalk *w = [NotesAppleWalk new];
    w.paragraph = [NSMutableAttributedString new];
    w.objects   = objects;
    w.mark      = -1;
    if (b->kind == ON_BLOCK_IMAGE) {
        gsize n = 0;
        const void *bytes = g_bytes_get_data(b->png, &n);
        append_object(w, [objects image_attachment:[NSData dataWithBytes:bytes length:n] small:b->display_width == 0], NO);
    } else if (b->kind == ON_BLOCK_TABLE) {
        NSMutableArray *rows = [NSMutableArray arrayWithCapacity:(NSUInteger)b->rows];
        for (gint r = 0; r < b->rows; r++) {
            NSMutableArray *row = [NSMutableArray arrayWithCapacity:(NSUInteger)b->cols];
            for (gint c = 0; c < b->cols; c++) {
                [row addObject:cell_text(on_block_cell(b, r, c))];
            }
            [rows addObject:row];
        }
        id tt = [objects table_attachment:rows];
        if (tt != nil) {
            append_object(w, tt, NO);
        } else {
            for (NSArray *row in rows) {
                if (w.paragraph.length > 0) {
                    [w.paragraph appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n"]];
                }
                [w.paragraph appendAttributedString:
                    [[NSAttributedString alloc] initWithString:[row componentsJoinedByString:@" | "]]];
            }
        }
    } else if (b->text != NULL) {
        w.mark = on_block_action_mark(b);
        static const OnTextWalk walk = { walk_text, walk_object };
        on_text_walk(b->text, &walk, (__bridge gpointer)w);
    }
    id style = make_style(block_style(b->kind, title), b->kind == ON_BLOCK_CHECK, b->checked);
    [w.paragraph addAttribute:ON_APPLE_ATTR_STYLE value:style range:NSMakeRange(0, w.paragraph.length)];
    return w.paragraph;
}

/*
 * tag_names — the tags of the tag line: its "#word" tokens, without '#'.
 *
 * Inputs:
 *   doc — the document
 *
 * Output: the names, in order; empty without a tag line.
 */
static NSArray<NSString *> *
tag_names(const OnDocument *doc)
{
    NSMutableArray *names = [NSMutableArray array];
    if (on_document_n_blocks(doc) == 0) {
        return names;
    }
    const OnBlock *b = on_document_block(doc, 0);
    if (b->kind != ON_BLOCK_TAGS || b->text == NULL) {
        return names;
    }
    for (NSString *token in [@(b->text->text->str) componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceCharacterSet]) {
        if (token.length > 1 && [token hasPrefix:@"#"]) {
            [names addObject:[token substringFromIndex:1]];
        }
    }
    return names;
}

/*
 * tag_paragraph — the tag line as Notes shows it: 🏷️, then a hashtag per
 * tag, spaced.
 *
 * Inputs:
 *   names   — the tags
 *   objects — the object source
 *
 * Output: the paragraph, body-styled.
 */
static NSMutableAttributedString *
tag_paragraph(NSArray<NSString *> *names, id<apple_notes_objects> objects)
{
    NSMutableAttributedString *p = [[NSMutableAttributedString alloc] initWithString:ON_APPLE_TAG_LINE_LEAD];
    for (NSUInteger i = 0; i < names.count; i++) {
        if (i > 0) {
            [p appendAttributedString:[[NSAttributedString alloc] initWithString:@" "]];
        }
        id tt = [objects hashtag_attachment:names[i]];
        if (tt != nil) {
            [p appendAttributedString:[[NSAttributedString alloc] initWithString:@"￼"
                                                                      attributes:@{ ON_APPLE_ATTR_ATTACHMENT: tt }]];
        } else {
            [p appendAttributedString:[[NSAttributedString alloc] initWithString:[@"#" stringByAppendingString:names[i]]]];
        }
    }
    [p addAttribute:ON_APPLE_ATTR_STYLE value:make_style(on_apple_style_body, NO, NO) range:NSMakeRange(0, p.length)];
    return p;
}

/*
 * on_apple_notes_text — see apple_notes.h.
 */
NSAttributedString *
on_apple_notes_text(const OnDocument *doc, id<apple_notes_objects> objects)
{
    NSMutableArray<NSAttributedString *> *paragraphs = [NSMutableArray array];
    guint title = on_document_title_block(doc);
    NSArray<NSString *> *tags = tag_names(doc);
    for (guint i = title; i < on_document_n_blocks(doc); i++) {
        [paragraphs addObject:block_paragraph(on_document_block(doc, i), i == title, objects)];
        if (i == title && tags.count > 0) {
            [paragraphs addObject:tag_paragraph(tags, objects)];
        }
    }
    NSMutableAttributedString *out = [NSMutableAttributedString new];
    for (NSUInteger i = 0; i < paragraphs.count; i++) {
        [out appendAttributedString:paragraphs[i]];
        if (i + 1 < paragraphs.count) {
            id style = paragraphs[i].length > 0
                     ? [paragraphs[i] attribute:ON_APPLE_ATTR_STYLE atIndex:0 effectiveRange:NULL]
                     : make_style(on_apple_style_body, NO, NO);
            [out appendAttributedString:[[NSAttributedString alloc] initWithString:@"\n"
                                                                       attributes:@{ ON_APPLE_ATTR_STYLE: style }]];
        }
    }
    return out;
}

/* ---------------------------------------------------------------------------
 * THE LIVE OBJECT SOURCE — makes a note's objects in Notes' store
 * ------------------------------------------------------------------------- */

/*
 * NotesAppleObjects — the object source bound to the ICNote being written
 * and the database the files come from.  Media goes through a temporary
 * file, the way Notes imports it (addAttachmentWithFileURL:filename: makes
 * the media row, the thumbnails and the file-based attributes) — once: an
 * image or file the last push imported is used again when the note still
 * has it (`existing`, by Apple identifier) and the binding map remembers
 * it under the same key (`reuse`, key → identifier).  `used` collects the
 * keys this push referred to, for the map.
 */
@interface NotesAppleObjects : NSObject <apple_notes_objects>
@property (nonatomic, strong) id note;
@property (nonatomic, assign) OnDatabase *db;
@property (nonatomic, strong) NSDictionary<NSString *, id> *existing;
@property (nonatomic, strong) NSDictionary<NSString *, NSString *> *reuse;
@property (nonatomic, strong) NSMutableDictionary<NSString *, NSString *> *used;
@end

@implementation NotesAppleObjects

/*
 * fnv64 — the FNV-1a hash of some bytes, the key an image is remembered by.
 *
 * Inputs:
 *   bytes — the bytes
 *   n     — how many
 *
 * Output: the hash.
 */
static uint64_t
fnv64(const uint8_t *bytes, NSUInteger n)
{
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (NSUInteger i = 0; i < n; i++) {
        hash ^= bytes[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

/*
 * tt_for — the text-side attachment for an attachment object of Notes.
 *
 * Inputs:
 *   identifier — the object's identifier
 *   uti        — its type
 *
 * Output: the ICTTAttachment.
 */
static id
tt_for(NSString *identifier, NSString *uti)
{
    id<ic_tt_attachment> tt = [[cls_tt_attachment alloc] init];
    [tt setAttachmentIdentifier:identifier];
    [tt setAttachmentUTI:uti];
    return tt;
}

/*
 * temp_path — a fresh path in the app's temporary folder.
 *
 * Inputs:
 *   extension — the file's extension, without the dot
 *
 * Output: the path, its folder made.
 */
static NSString *
temp_path(NSString *extension)
{
    NSString *dir = [NSTemporaryDirectory() stringByAppendingPathComponent:@ON_BUNDLE_ID];
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:NULL];
    NSString *name = [NSUUID.UUID.UUIDString stringByAppendingPathExtension:extension];
    return [dir stringByAppendingPathComponent:name];
}

/*
 * attach_file — import a file at `path` into the note as an attachment.
 *
 * Inputs:
 *   path  — the file (removed afterwards)
 *   name  — the name Notes shows
 *   small — for an image: show it small
 *
 * Output: the ICTTAttachment, or nil when Notes refused.
 */
- (id)
attach_file:(NSString *)path name:(NSString *)name small:(BOOL)small key:(NSString *)key
{
    id<ic_note> note = self.note;
    [note setShouldAddMediaAsynchronously:NO];
    id attachment = [note addAttachmentWithFileURL:[NSURL fileURLWithPath:path] filename:name];
    [NSFileManager.defaultManager removeItemAtPath:path error:NULL];
    if (attachment == nil) {
        return nil;
    }
    if ([attachment respondsToSelector:@selector(setPreferredViewSize:)]) {
        [(id<ic_attachment>)attachment setPreferredViewSize:small ? APPLE_VIEW_SMALL : APPLE_VIEW_LARGE];
    }
    NSString *identifier = [attachment valueForKey:@"identifier"];
    NSString *uti        = [attachment valueForKey:@"typeUTI"];
    if (identifier == nil || uti == nil) {
        return nil;
    }
    self.used[key] = identifier;
    return tt_for(identifier, uti);
}

/*
 * reused:small: — the attachment the last push made for this key, when the
 * note still has it: its view size is set again (the image may have been
 * resized here) and it is used as it is.
 *
 * Inputs:
 *   key   — the image's or file's key
 *   small — for an image: show it small
 *
 * Output: the ICTTAttachment, or nil when there is nothing to reuse.
 */
- (id)
reused:(NSString *)key small:(BOOL)small
{
    NSString *identifier = self.reuse[key];
    id attachment = identifier != nil ? self.existing[identifier] : nil;
    if (attachment == nil) {
        return nil;
    }
    NSString *uti = [attachment valueForKey:@"typeUTI"];
    if (uti == nil) {
        return nil;
    }
    if ([attachment respondsToSelector:@selector(setPreferredViewSize:)]) {
        [(id<ic_attachment>)attachment setPreferredViewSize:small ? APPLE_VIEW_SMALL : APPLE_VIEW_LARGE];
    }
    self.used[key] = identifier;
    return tt_for(identifier, uti);
}

- (id)
hashtag_attachment:(NSString *)name
{
    NSString *identifier = NSUUID.UUID.UUIDString;
    id made = [(Class<ic_inline_attachment_class>)cls_inline_attachment
               newHashtagAttachmentWithIdentifier:identifier hashtagText:name
                       creatingHashtagIfNecessary:YES note:self.note parentAttachment:nil];
    return made != nil ? tt_for(identifier, ON_APPLE_UTI_HASHTAG) : nil;
}

- (id)
image_attachment:(NSData *)png small:(BOOL)small
{
    NSString *key = [NSString stringWithFormat:@"image:%016llx", (unsigned long long)fnv64(png.bytes, png.length)];
    id reused = [self reused:key small:small];
    if (reused != nil) {
        return reused;
    }
    NSString *path = temp_path(@"png");
    if (![png writeToFile:path atomically:YES]) {
        return nil;
    }
    return [self attach_file:path name:@"image.png" small:small key:key];
}

- (id)
file_attachment:(int64_t)file_id name:(NSString *)name
{
    NSString *key = [NSString stringWithFormat:@"file:%lld", (long long)file_id];
    id reused = [self reused:key small:NO];
    if (reused != nil) {
        return reused;
    }
    NSString *path = temp_path(name.pathExtension.length > 0 ? name.pathExtension : @"bin");
    if (!on_db_note_file_export(self.db, file_id, path.fileSystemRepresentation)) {
        return nil;
    }
    return [self attach_file:path name:name small:NO key:key];
}

- (id)
table_attachment:(NSArray<NSArray<NSString *> *> *)rows
{
    NSMutableArray *lines = [NSMutableArray arrayWithCapacity:rows.count];
    for (NSArray *row in rows) {
        [lines addObject:[row componentsJoinedByString:@"\t"]];
    }
    NSAttributedString *tsv = [[NSAttributedString alloc] initWithString:[lines componentsJoinedByString:@"\n"]];
    id attachment = [(id<ic_note>)self.note addTableAttachmentWithText:tsv];
    NSString *identifier = [attachment valueForKey:@"identifier"];
    return identifier != nil ? tt_for(identifier, ON_APPLE_UTI_TABLE) : nil;
}

@end

/* ---------------------------------------------------------------------------
 * THE BINDING MAP — our note id ↔ its copy, and what was pushed
 * ------------------------------------------------------------------------- */

/* What a push records, and what the sweep compares.                        */
typedef struct {
    OnNoteStamp stamp;
    gboolean    pinned;
    gboolean    trashed;
    gint64      created_at;
    NSString   *tags;                /* the tag names joined by ' '         */
} pushed_state_t;

static sqlite3      *map;
static sqlite3_stmt *map_get_stmt;    /* note_map by note id, prepared once */

/*
 * map_open — open (and create) the binding map in the data directory.
 *
 * Output: YES when it is open.
 */
static BOOL
map_open(void)
{
    if (map != NULL) {
        return YES;
    }
    char *path = on_paths_data_file(MAP_FILENAME);
    int rc = sqlite3_open(path, &map);
    free(path);
    if (rc != SQLITE_OK) {
        sqlite3_close(map);
        map = NULL;
        return NO;
    }
    const char *schema =
        "CREATE TABLE IF NOT EXISTS state (key TEXT PRIMARY KEY, value TEXT);"
        "CREATE TABLE IF NOT EXISTS note_map ("
        "  note_id    INTEGER PRIMARY KEY,"
        "  apple_id   TEXT    NOT NULL,"
        "  updated_at INTEGER NOT NULL,"
        "  size       INTEGER NOT NULL,"
        "  hash       INTEGER NOT NULL,"
        "  pinned     INTEGER NOT NULL,"
        "  trashed    INTEGER NOT NULL,"
        "  created_at INTEGER NOT NULL,"
        "  tags       TEXT    NOT NULL);"
        "CREATE TABLE IF NOT EXISTS attachment_map ("
        "  note_id  INTEGER NOT NULL,"
        "  key      TEXT    NOT NULL,"
        "  apple_id TEXT    NOT NULL,"
        "  PRIMARY KEY (note_id, key));";
    if (sqlite3_exec(map, schema, NULL, NULL, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(map, "SELECT apple_id, updated_at, size, hash, pinned, trashed, created_at, tags"
                                " FROM note_map WHERE note_id = ?", -1, &map_get_stmt, NULL) != SQLITE_OK) {
        sqlite3_finalize(map_get_stmt);
        map_get_stmt = NULL;
        sqlite3_close(map);
        map = NULL;
        return NO;
    }
    return YES;
}

/*
 * map_close — close the binding map.
 */
static void
map_close(void)
{
    sqlite3_finalize(map_get_stmt);
    map_get_stmt = NULL;
    sqlite3_close(map);
    map = NULL;
}

/*
 * map_state_get — a value of the state table.
 *
 * Inputs:
 *   key — the key
 *
 * Output: the value, or nil.
 */
static NSString *
map_state_get(const char *key)
{
    sqlite3_stmt *st = NULL;
    NSString *value = nil;
    if (sqlite3_prepare_v2(map, "SELECT value FROM state WHERE key = ?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0) != NULL) {
            value = @((const char *)sqlite3_column_text(st, 0));
        }
    }
    sqlite3_finalize(st);
    return value;
}

/*
 * map_state_set — write a value of the state table.
 *
 * Inputs:
 *   key   — the key
 *   value — the value
 */
static void
map_state_set(const char *key, NSString *value)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(map, "INSERT OR REPLACE INTO state (key, value) VALUES (?, ?)", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, value.UTF8String, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
}

/*
 * map_get — the binding of a note.
 *
 * Inputs:
 *   note_id  — the note
 *   apple_id — out, optional: the copy's identifier
 *   pushed   — out, optional: what was pushed
 *
 * Output: YES when the note has a binding.
 */
static BOOL
map_get(gint64 note_id, NSString **apple_id, pushed_state_t *pushed)
{
    sqlite3_stmt *st = map_get_stmt;
    BOOL found = NO;
    sqlite3_bind_int64(st, 1, note_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        found = YES;
        if (apple_id != NULL) {
            *apple_id = @((const char *)sqlite3_column_text(st, 0));
        }
        if (pushed != NULL) {
            pushed->stamp.updated_at = sqlite3_column_int64(st, 1);
            pushed->stamp.size       = sqlite3_column_int64(st, 2);
            pushed->stamp.hash       = (guint64)sqlite3_column_int64(st, 3);
            pushed->pinned           = sqlite3_column_int(st, 4) != 0;
            pushed->trashed          = sqlite3_column_int(st, 5) != 0;
            pushed->created_at       = sqlite3_column_int64(st, 6);
            pushed->tags             = @((const char *)sqlite3_column_text(st, 7));
        }
    }
    sqlite3_reset(st);
    sqlite3_clear_bindings(st);
    return found;
}

/*
 * map_put — record a push.
 *
 * Inputs:
 *   note_id  — the note
 *   apple_id — its copy
 *   pushed   — what was pushed
 */
static void
map_put(gint64 note_id, NSString *apple_id, const pushed_state_t *pushed)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(map, "INSERT OR REPLACE INTO note_map"
                                " (note_id, apple_id, updated_at, size, hash, pinned, trashed, created_at, tags)"
                                " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, note_id);
        sqlite3_bind_text(st, 2, apple_id.UTF8String, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, pushed->stamp.updated_at);
        sqlite3_bind_int64(st, 4, pushed->stamp.size);
        sqlite3_bind_int64(st, 5, (sqlite3_int64)pushed->stamp.hash);
        sqlite3_bind_int(st, 6, pushed->pinned);
        sqlite3_bind_int(st, 7, pushed->trashed);
        sqlite3_bind_int64(st, 8, pushed->created_at);
        sqlite3_bind_text(st, 9, pushed->tags.UTF8String, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
}

/*
 * map_delete — forget a note's binding.
 *
 * Inputs:
 *   note_id — the note
 */
static void
map_delete(gint64 note_id)
{
    const char *sql[] = { "DELETE FROM note_map WHERE note_id = ?", "DELETE FROM attachment_map WHERE note_id = ?" };
    for (size_t i = 0; i < sizeof sql / sizeof sql[0]; i++) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(map, sql[i], -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, note_id);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }
}

/*
 * map_attachments_get — the attachments the last push of a note made, by
 * key (NotesAppleObjects).
 *
 * Inputs:
 *   note_id — the note
 *
 * Output: key → Apple identifier.
 */
static NSDictionary<NSString *, NSString *> *
map_attachments_get(gint64 note_id)
{
    NSMutableDictionary *found = [NSMutableDictionary dictionary];
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(map, "SELECT key, apple_id FROM attachment_map WHERE note_id = ?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, note_id);
        while (sqlite3_step(st) == SQLITE_ROW) {
            found[@((const char *)sqlite3_column_text(st, 0))] = @((const char *)sqlite3_column_text(st, 1));
        }
    }
    sqlite3_finalize(st);
    return found;
}

/*
 * map_attachments_put — record the attachments a push used, replacing the
 * note's earlier rows.
 *
 * Inputs:
 *   note_id — the note
 *   used    — key → Apple identifier
 */
static void
map_attachments_put(gint64 note_id, NSDictionary<NSString *, NSString *> *used)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(map, "DELETE FROM attachment_map WHERE note_id = ?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, note_id);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    st = NULL;
    if (sqlite3_prepare_v2(map, "INSERT INTO attachment_map (note_id, key, apple_id) VALUES (?, ?, ?)",
                           -1, &st, NULL) == SQLITE_OK) {
        for (NSString *key in used) {
            sqlite3_bind_int64(st, 1, note_id);
            sqlite3_bind_text(st, 2, key.UTF8String, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 3, used[key].UTF8String, -1, SQLITE_TRANSIENT);
            sqlite3_step(st);
            sqlite3_reset(st);
            sqlite3_clear_bindings(st);
        }
    }
    sqlite3_finalize(st);
}

/*
 * map_note_ids — every bound note id.
 *
 * Output: the ids.
 */
static NSArray<NSNumber *> *
map_note_ids(void)
{
    NSMutableArray *ids = [NSMutableArray array];
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(map, "SELECT note_id FROM note_map", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            [ids addObject:@(sqlite3_column_int64(st, 0))];
        }
    }
    sqlite3_finalize(st);
    return ids;
}

/*
 * map_forget_pushes — keep every binding but forget what was pushed, so
 * the next sweep pushes every note again.
 */
static void
map_forget_pushes(void)
{
    sqlite3_exec(map, "UPDATE note_map SET updated_at = -1, size = -1, hash = 0", NULL, NULL, NULL);
}

/* ---------------------------------------------------------------------------
 * THE STORE — Notes' context, our folder, and one note's push
 * ------------------------------------------------------------------------- */

static OnApp                  *the_app;
static id<ic_note_context>     context;
static NSManagedObjectContext *moc;
static id                      our_folder;
static id                      trash_folder;
static NSString               *status;
static void                  (^status_observer)(void);
static void                  (^progress_observer)(void);
static on_timer_id_t           sweep_timer;
static on_timer_id_t           drain_timer;    /* the run's next push; 0 = none */
static BOOL                    sweep_wanted;   /* something structural changed */
static NSMutableSet<NSNumber *> *dirty;        /* notes saved since the last tick */
static NSMutableArray<NSNumber *> *queue;      /* notes to push, in order    */
static int                     pushed_count;   /* since the setting went on  */
static int                     failed_count;
static int                     run_total;      /* notes the sweep queued; 0 = idle */
static int                     run_done;       /* of them, pushed so far     */

/*
 * set_status — change the status line and tell the observer.
 *
 * Inputs:
 *   line — the new line
 */
static void
set_status(NSString *line)
{
    status = line;
    if (status_observer != nil) {
        status_observer();
    }
}

/*
 * set_progress — a run of pushes moved on (or ended): tell the progress
 * observer.
 *
 * Inputs:
 *   total — the run's size, 0 when it ended
 *   done  — the pushes finished so far
 */
static void
set_progress(int total, int done)
{
    run_total = total;
    run_done  = done;
    if (progress_observer != nil) {
        progress_observer();
    }
}

/*
 * store_readable — whether the process may read Notes' store at all (Full
 * Disk Access), checked before the framework is asked to open it.
 *
 * Output: YES when it can.
 */
static BOOL
store_readable(void)
{
    NSString *path = [NSHomeDirectory() stringByAppendingPathComponent:@APPLE_NOTES_STORE];
    return access(path.fileSystemRepresentation, R_OK) == 0;
}

/*
 * save_and_notify — save the context and tell a running Notes.app that
 * another process saved, as Notes' own editor extension does; without
 * the notice the app may overwrite the change with its open copy.
 *
 * Output: YES when the save went through.
 */
static BOOL
save_and_notify(void)
{
    __block BOOL ok = YES;
    [moc performBlockAndWait:^{
        ok = [context save];
        if (moc.hasChanges) {
            NSError *error = nil;
            ok = [moc save:&error] && ok;
            if (error != nil) {
                NSLog(@"[apple-notes] save failed: %@", error);
            }
        }
        id coordinator = [context crossProcessChangeCoordinator];
        if ([coordinator respondsToSelector:@selector(postEditorExtensionDidSaveNotification)]) {
            [(id<ic_change_coordinator>)coordinator postEditorExtensionDidSaveNotification];
        }
    }];
    return ok;
}

/*
 * folder_titled — the folder of `account` whose title is `title`.
 *
 * Inputs:
 *   account — the account
 *   title   — the title
 *
 * Output: the folder, or nil.
 */
static id
folder_titled(id<ic_account> account, NSString *title)
{
    id folders = [account visibleFolders];
    NSArray *list = [folders isKindOfClass:NSArray.class] ? folders
                  : [folders respondsToSelector:@selector(allObjects)] ? [folders allObjects]
                  : [folders respondsToSelector:@selector(array)] ? [folders array] : @[];
    for (id folder in list) {
        if ([[folder valueForKey:@"title"] isEqualToString:title]) {
            return folder;
        }
    }
    return nil;
}

/*
 * open_store — start Notes' shared context and find or make our folder.
 * Sets the status on failure.
 *
 * Output: YES when the store is open and the folder known.
 */
static BOOL
open_store(void)
{
    if (moc != nil && our_folder != nil) {
        return YES;
    }
    NSString *why = nil;
    if (!on_apple_notes_load_framework(&why)) {
        set_status(why);
        return NO;
    }
    if (!store_readable()) {
        set_status(@"Needs Full Disk Access: System Settings ▸ Privacy & Security ▸ Full Disk Access, "
                   @"then turn " @ON_APP_NAME @" on there and open it again.");
        return NO;
    }
    Class<ic_note_context_class> ctx_class = cls_context;
    if (![ctx_class hasSharedContext]) {
        [ctx_class startSharedContextWithOptions:0];
    }
    context = [ctx_class sharedContext];
    moc     = [context managedObjectContext];
    if (moc == nil || moc.persistentStoreCoordinator.persistentStores.count == 0) {
        context = nil;
        moc     = nil;
        set_status(@"Apple Notes' store could not be opened. Is Full Disk Access granted?");
        return NO;
    }
    __block BOOL ok = NO;
    [moc performBlockAndWait:^{
        NSArray *accounts = [(Class<ic_account_class>)cls_account allActiveAccountsInContext:moc];
        id<ic_account> account = nil;
        for (id<ic_account> a in accounts) {
            if ([a isPrimaryiCloudAccount]) {
                account = a;
            }
        }
        if (account == nil) {
            account = accounts.firstObject;
        }
        if (account == nil) {
            set_status(@"Apple Notes has no account to write into.");
            return;
        }
        NSString *folder_id = map_state_get("folder_id");
        if (folder_id != nil) {
            our_folder = [(Class<ic_folder_class>)cls_folder folderWithIdentifier:folder_id context:moc];
        }
        if (our_folder == nil) {
            our_folder = folder_titled(account, @ON_APPLE_NOTES_FOLDER);
        }
        if (our_folder == nil) {
            id<ic_folder> made = [(Class<ic_folder_class>)cls_folder newFolderInAccount:account];
            if (made == nil) {
                set_status(@"Apple Notes would not make the folder “" @ON_APPLE_NOTES_FOLDER @"”.");
                return;
            }
            [made setTitle:@ON_APPLE_NOTES_FOLDER];
            [made updateChangeCountWithReason:@ON_APP_NAME @" folder"];
            our_folder = made;
            if (!save_and_notify()) {
                our_folder = nil;
                set_status(@"Apple Notes would not save the folder “" @ON_APPLE_NOTES_FOLDER @"”.");
                return;
            }
        }
        NSString *made_id = [our_folder valueForKey:@"identifier"];
        if (made_id != nil) {
            map_state_set("folder_id", made_id);
        }
        trash_folder = [account trashFolder];
        ok = YES;
    }];
    return ok;
}

/*
 * attachment_objects — the Notes objects behind the attachment characters
 * of a text, by identifier.
 *
 * Inputs:
 *   text — the text
 *
 * Output: identifier → object (objects Notes no longer has are left out).
 */
static NSDictionary<NSString *, id> *
attachment_objects(NSAttributedString *text)
{
    NSMutableDictionary *objects = [NSMutableDictionary dictionary];
    [text enumerateAttribute:ON_APPLE_ATTR_ATTACHMENT inRange:NSMakeRange(0, text.length) options:0
                  usingBlock:^(id value, NSRange range, BOOL *stop) {
        if (value == nil) {
            return;
        }
        id<ic_tt_attachment> tt = value;
        NSString *identifier = [tt attachmentIdentifier];
        BOOL inline_object = [[tt attachmentUTI] hasPrefix:@"com.apple.notes.inlinetextattachment."];
        id object = inline_object ? [tt inlineAttachmentInContext:moc] : [tt attachmentInContext:moc];
        if (identifier != nil && object != nil) {
            objects[identifier] = object;
        }
    }];
    return objects;
}

/*
 * current_state — what the database holds for a note now.
 *
 * Inputs:
 *   db      — the database
 *   note_id — the note
 *   state   — out
 *
 * Output: YES when the note exists.
 */
static BOOL
current_state(OnDatabase *db, gint64 note_id, pushed_state_t *state)
{
    OnNoteMeta *meta = on_db_note_get(db, note_id);
    if (meta == NULL || !on_db_note_stamp(db, note_id, &state->stamp)) {
        on_db_note_meta_free(meta);
        return NO;
    }
    state->pinned     = meta->pinned;
    state->trashed    = on_db_note_is_trashed(db, note_id);
    state->created_at = meta->created_at;
    on_db_note_meta_free(meta);
    NSMutableArray *names = [NSMutableArray array];
    GList *tags = on_db_note_tags(db, note_id);
    for (GList *l = tags; l != NULL; l = l->next) {
        [names addObject:@(((OnTag *)l->data)->name)];
    }
    on_db_tag_list_free(tags);
    state->tags = [names componentsJoinedByString:@" "];
    return YES;
}

/*
 * state_same — whether two pushed states are equal.
 */
static BOOL
state_same(const pushed_state_t *a, const pushed_state_t *b)
{
    return on_db_note_stamp_same(&a->stamp, &b->stamp) && a->pinned == b->pinned &&
           a->trashed == b->trashed && a->created_at == b->created_at && [a->tags isEqualToString:b->tags];
}

/*
 * push_note — copy one note into its ICNote, making the ICNote when the
 * note has none yet, and record the push.
 *
 * Inputs:
 *   note_id — the note
 *
 * Output: YES when the copy was saved.
 */
static BOOL
push_note(gint64 note_id)
{
    OnDatabase *db = the_app->db;
    pushed_state_t state;
    if (!current_state(db, note_id, &state)) {
        return NO;
    }
    OnDocument *doc = on_note_document_load(db, note_id);
    if (doc == NULL) {
        return NO;
    }
    NSString *apple_id = nil;
    map_get(note_id, &apple_id, NULL);
    __block BOOL ok = NO;
    __block NSString *saved_id = apple_id;
    __block NSDictionary<NSString *, NSString *> *used = @{};
    [moc performBlockAndWait:^{
        id<ic_note> note = nil;
        if (apple_id != nil) {
            note = [(Class<ic_note_class>)cls_note noteWithIdentifier:apple_id context:moc];
        }
        if (note == nil) {
            note = [(Class<ic_note_class>)cls_note newEmptyNoteInFolder:our_folder];
            saved_id = [(id)note valueForKey:@"identifier"];
        }
        id<ic_mergeable_string> text = [note mergeableString];
        if (note == nil || text == nil || saved_id == nil) {
            return;
        }
        NSDictionary *before = attachment_objects([text attributedString]);
        NotesAppleObjects *objects = [NotesAppleObjects new];
        objects.note     = note;
        objects.db       = db;
        objects.existing = before;
        objects.reuse    = map_attachments_get(note_id);
        objects.used     = [NSMutableDictionary dictionary];
        [note beginEditing];
        NSAttributedString *converted = on_apple_notes_text(doc, objects);
        [text replaceCharactersInRange:NSMakeRange(0, [text length]) withAttributedString:converted];
        NSDictionary *after = attachment_objects(converted);
        for (NSString *identifier in before) {
            if (after[identifier] == nil && [before[identifier] respondsToSelector:@selector(markForDeletion)]) {
                [(id<ic_syncing_object>)before[identifier] markForDeletion];
            }
        }
        [note endEditing];
        [note didChangeNoteText];
        [note saveNoteData];
        [note regenerateTitle:YES snippet:YES];
        if (state.created_at > 0) {
            [(id)note setValue:[NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)state.created_at] forKey:@"creationDate"];
        }
        [(id)note setValue:[NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)state.stamp.updated_at] forKey:@"modificationDate"];
        [note setIsPinned:state.pinned];
        id wanted = state.trashed && trash_folder != nil ? trash_folder : our_folder;
        if ([(id)note valueForKey:@"folder"] != wanted) {
            [note setFolder:wanted];
        }
        [note updateChangeCountWithReason:@ON_APP_NAME @" push"];
        used = objects.used;
        ok   = YES;
    }];
    on_document_free(doc);
    if (ok) {
        ok = save_and_notify();
    }
    if (ok) {
        map_put(note_id, saved_id, &state);
        map_attachments_put(note_id, used);
    }
    return ok;
}

/*
 * retire_note — a note left the database: move its copy to Recently
 * Deleted and forget the binding.
 *
 * Inputs:
 *   note_id — the note
 */
static void
retire_note(gint64 note_id)
{
    NSString *apple_id = nil;
    if (map_get(note_id, &apple_id, NULL) && trash_folder != nil) {
        [moc performBlockAndWait:^{
            id<ic_note> note = [(Class<ic_note_class>)cls_note noteWithIdentifier:apple_id context:moc];
            if (note != nil) {
                [note setFolder:trash_folder];
                [note updateChangeCountWithReason:@ON_APP_NAME @" delete"];
            }
        }];
        save_and_notify();
    }
    map_delete(note_id);
}

/* ---------------------------------------------------------------------------
 * THE SWEEP
 * ------------------------------------------------------------------------- */

/*
 * enqueue — put a note (or a retirement, negative) on the queue once.
 *
 * Inputs:
 *   id — the note id, negated for a retirement
 */
static void
enqueue(gint64 id)
{
    if (![queue containsObject:@(id)]) {
        [queue addObject:@(id)];
    }
}

/*
 * sweep — compare every note with what was pushed and queue the stale
 * ones; queue the bindings whose note is gone as retirements (negative
 * ids).  Two list queries give every note's modification time, pin, trash
 * state and creation date; only a note one of those moved for has its
 * content hashed and its tags read, so a sweep over a large library costs
 * little when nothing changed.
 */
static void
sweep(void)
{
    OnDatabase *db = the_app->db;
    NSMutableSet *seen    = [NSMutableSet set];
    NSMutableSet *visible = [NSMutableSet set];
    GList *shown = on_db_note_list_all(db, FALSE);
    for (GList *l = shown; l != NULL; l = l->next) {
        [visible addObject:@(((OnNoteMeta *)l->data)->id)];
    }
    on_db_note_list_free(shown);
    GList *notes = on_db_note_list_all(db, TRUE);
    for (GList *l = notes; l != NULL; l = l->next) {
        OnNoteMeta *meta = l->data;
        [seen addObject:@(meta->id)];
        pushed_state_t pushed;
        if (!map_get(meta->id, NULL, &pushed)) {
            enqueue(meta->id);
            continue;
        }
        BOOL trashed = ![visible containsObject:@(meta->id)];
        if (meta->updated_at == pushed.stamp.updated_at && meta->pinned == pushed.pinned &&
            trashed == pushed.trashed && meta->created_at == pushed.created_at) {
            continue;
        }
        pushed_state_t now;
        if (current_state(db, meta->id, &now) && !state_same(&now, &pushed)) {
            enqueue(meta->id);
        }
    }
    on_db_note_list_free(notes);
    for (NSNumber *bound in map_note_ids()) {
        if (![seen containsObject:bound]) {
            enqueue(-bound.longLongValue);
        }
    }
}

/*
 * pushing_allowed — whether a push may run now: never while a note is
 * being edited, that is while an editor is the key window of the active
 * app.  The library in front, or the app in the background, lets the
 * queue drain.
 *
 * Output: YES when it may.
 */
static BOOL
pushing_allowed(void)
{
    return !NSApp.active || !on_editor_window_is_key();
}

/*
 * next_pushable — the first queued id that may be pushed now: a
 * retirement, or a note no editor has open (an open note waits for its
 * editor to close, whose final save queues it again).
 *
 * Output: the index in the queue, or NSNotFound.
 */
static NSUInteger
next_pushable(void)
{
    for (NSUInteger i = 0; i < queue.count; i++) {
        gint64 id = queue[i].longLongValue;
        if (id < 0 || !on_editor_window_is_open(id)) {
            return i;
        }
    }
    return NSNotFound;
}

/*
 * status_line — the status after some pushes.
 *
 * Output: the line.
 */
static NSString *
status_line(void)
{
    NSString *line = [NSString stringWithFormat:@"On. %d note%s copied into “" @ON_APPLE_NOTES_FOLDER @"” since the setting went on",
                      pushed_count, pushed_count == 1 ? "" : "s"];
    if (failed_count > 0) {
        line = [line stringByAppendingFormat:@"; %d could not be copied (see the log)", failed_count];
    }
    if (queue.count > 0) {
        line = [line stringByAppendingFormat:@"; %lu waiting%s", (unsigned long)queue.count,
                pushing_allowed() && next_pushable() != NSNotFound ? "" : " until the note is no longer being edited"];
    }
    return [line stringByAppendingString:@"."];
}

/*
 * stop_drain — cancel the run's pending push, if any, and show the run as
 * ended.
 */
static void
stop_drain(void)
{
    if (drain_timer != 0) {
        on_timer_remove(drain_timer);
        drain_timer = 0;
    }
    if (run_total != 0) {
        set_progress(0, 0);
    }
}

/*
 * push_next — push the first queued note that may be pushed, then arrange
 * the next push after drain_seconds; the run ends when the queue is empty,
 * when what is left is being edited, or when editing began meanwhile (the
 * next tick resumes it).  The progress is told before the push (the note
 * in hand) and when the run ends.
 *
 * Inputs:
 *   data — unused (the drain timer's payload)
 */
static void
push_next(void *data)
{
    drain_timer = 0;
    NSUInteger at = pushing_allowed() ? next_pushable() : NSNotFound;
    if (at == NSNotFound) {
        set_progress(0, 0);
        set_status(status_line());
        return;
    }
    set_progress(run_total, run_done);
    gint64 id = queue[at].longLongValue;
    [queue removeObjectAtIndex:at];
    if (id < 0) {
        retire_note(-id);
    } else if (push_note(id)) {
        pushed_count++;
    } else {
        failed_count++;
        NSLog(@"[apple-notes] note %lld could not be copied", (long long)id);
    }
    run_done++;
    if (queue.count > 0) {
        drain_timer = on_timer_add(drain_seconds, false, push_next, NULL, NULL);
    } else {
        set_progress(0, 0);
    }
    set_status(status_line());
}

/*
 * tick — the timer: open the store if it is not, sweep when something
 * structural changed, queue the notes saved since the last tick, and start
 * a run of pushes when one may run.  Nothing while a run is under way:
 * push_next paces itself.
 */
static void
tick(void *data)
{
    if (drain_timer != 0 || !open_store()) {
        return;
    }
    if (sweep_wanted) {
        sweep_wanted = NO;
        sweep();
    }
    for (NSNumber *id in dirty) {
        enqueue(id.longLongValue);
    }
    [dirty removeAllObjects];
    if (queue.count == 0 || !pushing_allowed() || next_pushable() == NSNotFound) {
        set_status(status_line());
        return;
    }
    run_total = (int)queue.count;
    run_done  = 0;
    push_next(NULL);
}

/*
 * activate — the setting is on: open the map, start the timer, sweep
 * everything on the first tick.
 */
static void
activate(void)
{
    if (sweep_timer != 0) {
        return;
    }
    if (!map_open()) {
        set_status(@"The binding map (" @MAP_FILENAME @" in the data folder) could not be opened.");
        return;
    }
    queue        = [NSMutableArray array];
    dirty        = [NSMutableSet set];
    sweep_wanted = YES;
    pushed_count = 0;
    failed_count = 0;
    set_status(@"On. Looking for notes to copy…");
    sweep_timer = on_timer_add(sweep_seconds, true, tick, NULL, NULL);
    tick(NULL);
}

/*
 * deactivate — the setting is off: stop the timer, close the map.  The
 * framework and Notes' context stay loaded (they cannot be unloaded).
 */
static void
deactivate(void)
{
    if (sweep_timer != 0) {
        on_timer_remove(sweep_timer);
        sweep_timer = 0;
    }
    stop_drain();
    queue = nil;
    dirty = nil;
    map_close();
    set_status(@"Off.");
}

/*
 * on_apple_notes_start — see apple_notes.h.
 */
void
on_apple_notes_start(OnApp *app)
{
    the_app = app;
    status  = @"Off.";
    if (app->apple_notes) {
        activate();
    }
}

/*
 * on_apple_notes_apply — see apple_notes.h.
 */
void
on_apple_notes_apply(OnApp *app)
{
    the_app = app;
    if (app->apple_notes) {
        activate();
    } else {
        deactivate();
    }
}

/*
 * on_apple_notes_push_all — see apple_notes.h.
 */
void
on_apple_notes_push_all(OnApp *app)
{
    the_app = app;
    if (sweep_timer == 0 || map == NULL) {
        return;
    }
    stop_drain();
    map_forget_pushes();
    sweep_wanted = YES;
    [queue removeAllObjects];
    tick(NULL);
}

/*
 * on_apple_notes_stop — see apple_notes.h.
 */
void
on_apple_notes_stop(void)
{
    if (sweep_timer != 0) {
        on_timer_remove(sweep_timer);
        sweep_timer = 0;
    }
    stop_drain();
    queue = nil;
    dirty = nil;
    map_close();
}

/*
 * on_apple_notes_note_saved — see apple_notes.h.
 */
void
on_apple_notes_note_saved(gint64 note_id)
{
    if (dirty != nil) {
        [dirty addObject:@(note_id)];
    }
}

/*
 * on_apple_notes_notes_changed — see apple_notes.h.
 */
void
on_apple_notes_notes_changed(void)
{
    sweep_wanted = YES;
}

/*
 * on_apple_notes_status — see apple_notes.h.
 */
NSString *
on_apple_notes_status(void)
{
    return status != nil ? status : @"Off.";
}

/*
 * on_apple_notes_observe — see apple_notes.h.
 */
void
on_apple_notes_observe(void (^changed)(void))
{
    status_observer = changed;
}

/*
 * on_apple_notes_progress — see apple_notes.h.
 */
void
on_apple_notes_progress(int *current, int *total)
{
    *total   = run_total;
    *current = run_total != 0 ? MIN(run_done + 1, run_total) : 0;
}

/*
 * on_apple_notes_observe_progress — see apple_notes.h.
 */
void
on_apple_notes_observe_progress(void (^changed)(void))
{
    progress_observer = changed;
}
