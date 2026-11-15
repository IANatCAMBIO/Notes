/*
 * apple_notes_test.m — the Apple Notes converter (apple_notes.h
 * on_apple_notes_text), headless: NotesShared is loaded for its text
 * classes, but no store is opened and nothing of Apple Notes is read or
 * written.  A fake object source stands in for the note's objects.
 *
 * What it proves: every block kind lands as the paragraph style the
 * mapping names; the tag line follows the title, led by 🏷️, one hashtag
 * per tag; a check item's tick is its to-do state; bold is a hint, strike
 * and underline their attributes; an action line is coloured from its mark
 * to its end and the words before a mid-line mark are not; inline objects
 * keep their place; and no character is cut — each paragraph's text is
 * byte for byte the block's UTF-8, with emoji, a variation selector, a
 * combining mark and a four-byte character in the middle of it.
 */
#import <Cocoa/Cocoa.h>

#include "check.h"
#import "apple_notes.h"
#include "document.h"

/* The object source of the test: makes nothing, hands back an
 * ICTTAttachment with a made-up identifier, and counts what was asked. */
@interface FakeObjects : NSObject <apple_notes_objects>
@property (nonatomic, assign) int hashtags, images, small_images, files, tables;
@property (nonatomic, strong) NSMutableArray<NSString *> *hashtag_names;
@end

@implementation FakeObjects

/*
 * tt — an ICTTAttachment carrying `uti` and a fresh identifier.
 */
static id
tt(NSString *uti)
{
    id object = [[NSClassFromString(@"ICTTAttachment") alloc] init];
    [object setValue:NSUUID.UUID.UUIDString forKey:@"attachmentIdentifier"];
    [object setValue:uti forKey:@"attachmentUTI"];
    return object;
}

- (id)hashtag_attachment:(NSString *)name
{
    self.hashtags++;
    [self.hashtag_names addObject:name];
    return tt(ON_APPLE_UTI_HASHTAG);
}

- (id)image_attachment:(NSData *)png small:(BOOL)small
{
    self.images++;
    if (small) self.small_images++;
    return tt(@"public.png");
}

- (id)file_attachment:(int64_t)file_id name:(NSString *)name
{
    self.files++;
    return tt(@"public.data");
}

- (id)table_attachment:(NSArray<NSArray<NSString *> *> *)rows
{
    self.tables++;
    return tt(ON_APPLE_UTI_TABLE);
}

@end

/*
 * add_text — append a text block of `kind` holding `text` to the document.
 *
 * Output: the block.
 */
static OnBlock *
add_text(OnDocument *doc, OnBlockKind kind, const char *text)
{
    OnBlock *b = on_block_new_text(kind);
    on_text_append(b->text, text, strlen(text), 0);
    on_document_insert_block(doc, on_document_n_blocks(doc), b);
    return b;
}

/*
 * style_of — the paragraph style number at `index` of the text.
 */
static int
style_of(NSAttributedString *text, NSUInteger index)
{
    id style = [text attribute:ON_APPLE_ATTR_STYLE atIndex:index effectiveRange:NULL];
    return style != nil ? [[style valueForKey:@"style"] intValue] : -1;
}

/*
 * todo_done — the to-do state at `index`: 1 done, 0 not, -1 no to-do.
 */
static int
todo_done(NSAttributedString *text, NSUInteger index)
{
    id style = [text attribute:ON_APPLE_ATTR_STYLE atIndex:index effectiveRange:NULL];
    id todo  = [style valueForKey:@"todo"];
    return todo != nil ? [[todo valueForKey:@"done"] intValue] : -1;
}

/*
 * same_bytes — whether an NSString is exactly the UTF-8 `bytes`.
 */
static bool
same_bytes(NSString *s, const char *bytes)
{
    return strcmp(s.UTF8String, bytes) == 0;
}

int
main(void)
{
    check_data_dir();
    NSString *why = nil;
    if (!on_apple_notes_load_framework(&why)) {
        check(false, why.UTF8String);
        return check_result();
    }

    /* A note with one of everything.  The title holds a four-byte emoji,
     * the tag-line glyph with its variation selector, an accented letter
     * as a base plus a combining mark, and a two-byte character. */
    const char *title   = "Caf\xC3\xA9 \xF0\x9F\x8E\x89 \xF0\x9F\x8F\xB7\xEF\xB8\x8F e\xCC\x81 \xC3\xB1";
    const char *bullet  = "First bold item";
    const char *action  = "! Call Ann due 2026-11-20";
    const char *midline = "asked Ann !send the quote";
    const char *inline_line = "Before \xEF\xBF\xBC after";
    OnDocument *doc = on_document_new();
    on_text_append(on_document_block(doc, 0)->text, "#Salt #Stride", 13, 0);
    on_text_append(on_document_block(doc, 1)->text, title, strlen(title), 0);
    add_text(doc, ON_BLOCK_H1, "Agenda");
    add_text(doc, ON_BLOCK_H2, "Items");
    OnBlock *b = add_text(doc, ON_BLOCK_BULLET, bullet);
    on_text_set_flags(b->text, 6, 4, ON_FMT_BOLD, TRUE);            /* "bold" */
    add_text(doc, ON_BLOCK_NUMBER, "Second");
    b = add_text(doc, ON_BLOCK_CHECK, "Done thing");
    b->checked = TRUE;
    on_text_set_flags(b->text, 0, 10, ON_FMT_STRIKE, TRUE);
    add_text(doc, ON_BLOCK_CHECK, "Open thing");
    add_text(doc, ON_BLOCK_CODE, "int x;");
    add_text(doc, ON_BLOCK_CODE, "return x;");
    b = add_text(doc, ON_BLOCK_PARA, inline_line);
    GBytes *png = g_bytes_new_static("png", 3);
    on_text_add_image(b->text, 7, png, 0);                          /* small */
    OnBlock *image = on_block_new_image(png, 320);                  /* large */
    on_document_insert_block(doc, on_document_n_blocks(doc), image);
    add_text(doc, ON_BLOCK_PARA, action);
    add_text(doc, ON_BLOCK_PARA, midline);
    OnBlock *table = on_block_new_table(2, 2);
    table->header = TRUE;
    on_text_append(on_block_cell(table, 0, 0), "A", 1, 0);
    on_text_append(on_block_cell(table, 1, 1), "D", 1, 0);
    on_document_insert_block(doc, on_document_n_blocks(doc), table);
    b = add_text(doc, ON_BLOCK_PARA, "File: \xEF\xBF\xBC");
    on_text_add_file(b->text, 6, 7, "plan.pdf");

    FakeObjects *objects = [FakeObjects new];
    objects.hashtag_names = [NSMutableArray array];
    NSAttributedString *text = on_apple_notes_text(doc, objects);
    NSArray<NSString *> *lines = [text.string componentsSeparatedByString:@"\n"];

    check(lines.count == 16, "sixteen paragraphs: title, tag line, fourteen blocks");
    if (lines.count < 16) {
        return check_result();
    }
    /* The title, byte for byte, as Title. */
    check(same_bytes(lines[0], title), "the title's text survives emoji, selector, combining mark");
    check(style_of(text, 0) == on_apple_style_title, "the title line is Title");
    /* The tag line. */
    NSString *tag_line = lines[1];
    check([tag_line hasPrefix:ON_APPLE_TAG_LINE_LEAD], "the tag line starts with the tag glyph and a space");
    check([tag_line isEqualToString:[ON_APPLE_TAG_LINE_LEAD stringByAppendingString:@"￼ ￼"]],
          "the tag line is the glyph then one hashtag per tag");
    check(objects.hashtags == 2 && [objects.hashtag_names isEqualToArray:@[@"Salt", @"Stride"]],
          "the hashtags are the tag line's names without '#'");
    NSUInteger at = lines[0].length + 1;
    check(style_of(text, at) == on_apple_style_body, "the tag line is Body");
    at += tag_line.length + 1;
    /* The headings and lists. */
    struct { const char *text; int style; int done; } expect[] = {
        { "Agenda",     on_apple_style_heading,    -1 },
        { "Items",      on_apple_style_subheading, -1 },
        { bullet,       on_apple_style_bullet,     -1 },
        { "Second",     on_apple_style_numbered,   -1 },
        { "Done thing", on_apple_style_checklist,   1 },
        { "Open thing", on_apple_style_checklist,   0 },
        { "int x;",     on_apple_style_monospaced, -1 },
        { "return x;",  on_apple_style_monospaced, -1 },
    };
    for (size_t i = 0; i < sizeof expect / sizeof *expect; i++) {
        NSString *line = lines[2 + i];
        char what[128];
        snprintf(what, sizeof what, "paragraph '%s' keeps its text", expect[i].text);
        check(same_bytes(line, expect[i].text), what);
        snprintf(what, sizeof what, "paragraph '%s' has style %d", expect[i].text, expect[i].style);
        check(style_of(text, at) == expect[i].style, what);
        snprintf(what, sizeof what, "paragraph '%s' to-do state %d", expect[i].text, expect[i].done);
        check(todo_done(text, at) == expect[i].done, what);
        if (strcmp(expect[i].text, bullet) == 0) {
            NSNumber *hints = [text attribute:ON_APPLE_ATTR_HINTS atIndex:at + 6 effectiveRange:NULL];
            check(hints.intValue == ON_APPLE_HINT_BOLD, "'bold' carries the bold hint");
            check([text attribute:ON_APPLE_ATTR_HINTS atIndex:at effectiveRange:NULL] == nil, "'First' carries none");
        }
        if (strcmp(expect[i].text, "Done thing") == 0) {
            check([[text attribute:ON_APPLE_ATTR_STRIKE atIndex:at effectiveRange:NULL] intValue] == 1,
                  "the done item is struck through");
        }
        at += line.length + 1;
    }
    /* The inline image keeps its place; the image block is one character. */
    check(same_bytes(lines[10], inline_line), "the inline image's line keeps the placeholder where it was");
    check([text attribute:ON_APPLE_ATTR_ATTACHMENT atIndex:at + 7 effectiveRange:NULL] != nil,
          "the placeholder carries the attachment");
    at += lines[10].length + 1;
    check([lines[11] isEqualToString:@"￼"] && style_of(text, at) == on_apple_style_body,
          "the image block is one attachment character in a Body paragraph");
    check(objects.images == 2 && objects.small_images == 1, "two images, the default-sized one Small");
    at += lines[11].length + 1;
    /* The action lines. */
    check(same_bytes(lines[12], action), "the action line keeps its text");
    check([text attribute:ON_APPLE_ATTR_COLOR atIndex:at effectiveRange:NULL] != nil, "an action line is coloured from its start");
    check([text attribute:ON_APPLE_ATTR_COLOR atIndex:at + lines[12].length - 1 effectiveRange:NULL] != nil,
          "… to its end");
    at += lines[12].length + 1;
    check(same_bytes(lines[13], midline), "the mid-line action keeps its text");
    check([text attribute:ON_APPLE_ATTR_COLOR atIndex:at effectiveRange:NULL] == nil, "the words before the mark are not coloured");
    check([text attribute:ON_APPLE_ATTR_COLOR atIndex:at + 9 effectiveRange:NULL] == nil, "… up to the space before it");
    check([text attribute:ON_APPLE_ATTR_COLOR atIndex:at + 10 effectiveRange:NULL] != nil, "the mark is coloured");
    check([text attribute:ON_APPLE_ATTR_COLOR atIndex:at + lines[13].length - 1 effectiveRange:NULL] != nil,
          "… and so is the item's last character");
    at += lines[13].length + 1;
    /* The table and the file. */
    check([lines[14] isEqualToString:@"￼"] && objects.tables == 1, "the table is one attachment character");
    at += lines[14].length + 1;
    check(same_bytes(lines[15], "File: \xEF\xBF\xBC") && objects.files == 1, "the file tile is an attachment where it stood");
    /* The whole: every character of the blocks is in the text, UTF-8 to
     * UTF-16 and back without loss. */
    NSMutableString *expected = [NSMutableString string];
    [expected appendFormat:@"%@\n%@\n", @(title), tag_line];
    for (size_t i = 0; i < sizeof expect / sizeof *expect; i++) {
        [expected appendFormat:@"%@\n", @(expect[i].text)];
    }
    [expected appendFormat:@"%@\n￼\n%@\n%@\n￼\nFile: ￼", @(inline_line), @(action), @(midline)];
    check([text.string isEqualToString:expected], "the plain text of the whole note is exactly the blocks' text");
    if (![text.string isEqualToString:expected]) {
        printf("     got      %s\n     expected %s\n", text.string.UTF8String, expected.UTF8String);
    }
    check(strcmp(text.string.UTF8String, expected.UTF8String) == 0, "… byte for byte in UTF-8");

    /* A note with no tags has no tag line. */
    OnDocument *plain = on_document_new();
    on_text_append(on_document_block(plain, 1)->text, "Alone", 5, 0);
    add_text(plain, ON_BLOCK_PARA, "Body");
    NSAttributedString *plain_text = on_apple_notes_text(plain, objects);
    check([plain_text.string isEqualToString:@"Alone\nBody"], "no tags, no tag line");
    on_document_free(plain);

    g_bytes_unref(png);
    on_document_free(doc);
    return check_result();
}
