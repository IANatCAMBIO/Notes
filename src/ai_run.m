/*
 * ai_run.m — running an AI command, and the prompt it is given
 * (see ai_run.h)
 *
 * Carried from the GTK build's library_window.c run_ai_summary by way of
 * library_ai.m, which was this code's first home: the editor's last-meeting
 * summary needed the same launcher and the same notes body, so both live
 * here and neither feature has a copy.
 */

#import "ai_run.h"

#import "shared_ui.h"

#include "app_core.h"
#include "serialize.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>

/* Where a user's command-line tools live, ahead of whatever PATH a GUI
 * app was launched with (see on_ai_run).  A "~" is the password
 * database's home directory, as everywhere in this app (config.h). */
static NSString * const tool_dirs[] = {
    @"~/.local/bin", @"~/.claude/local", @"/opt/homebrew/bin", @"/usr/local/bin", @"/opt/local/bin",
};

/* The plain-text rule, part of every prompt (GTK run_ai_summary's). */
static NSString * const prompt_plain = @"Respond in plain text only. Do not use markdown formatting such as "
                                        "headers, bold, italics, or bullet symbols.\n\n";

/*
 * home_dir — this user's home directory, from the password database and
 * not $HOME: a shell or a test harness that exported a different one must
 * not move where the app looks for a command (config.h's rule).
 *
 * Output: the path, or nil when the user has none.
 */
static NSString *
home_dir(void)
{
    const struct passwd *pw = getpwuid(getuid());
    return pw != NULL && pw->pw_dir != NULL ? @(pw->pw_dir) : nil;
}

/*
 * child_env — the environment an AI command runs in: this process's, with
 * the tool directories prepended to PATH.
 *
 * Output: the environment.
 */
static NSDictionary<NSString *, NSString *> *
child_env(void)
{
    NSString *home = home_dir();
    NSMutableArray<NSString *> *dirs = [NSMutableArray array];
    for (size_t i = 0; i < sizeof tool_dirs / sizeof *tool_dirs; i++) {
        NSString *dir = tool_dirs[i];
        if (![dir hasPrefix:@"~/"]) {
            [dirs addObject:dir];
        } else if (home != nil) {
            [dirs addObject:[home stringByAppendingPathComponent:[dir substringFromIndex:2]]];
        }
    }
    NSMutableDictionary<NSString *, NSString *> *env =
        [NSMutableDictionary dictionaryWithDictionary:NSProcessInfo.processInfo.environment];
    NSString *inherited = env[@"PATH"];
    if (inherited.length > 0) [dirs addObject:inherited];
    env[@"PATH"] = [dirs componentsJoinedByString:@":"];
    return env;
}

/*
 * stream_text — a stream's bytes as text, without the blank lines a
 * command tends to leave at either end.
 *
 * Inputs:
 *   bytes — what the stream held
 *
 * Output: the text, or nil when the stream was empty or all whitespace.
 */
static NSString *
stream_text(NSData *bytes)
{
    if (bytes.length == 0) return nil;
    NSString *text = [[NSString alloc] initWithData:bytes encoding:NSUTF8StringEncoding];
    text = [text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    return text.length ? text : nil;
}

/*
 * read_until_exit — a command's output, the two streams kept apart:
 * everything written to either until both have ended, or, once the command
 * has exited, whatever is still buffered (a child it left in the
 * background may hold a pipe open, and the end would never come).  Both
 * are drained by the one poll loop: a command that fills one pipe while
 * the other was being read would block for good.
 *
 * Inputs:
 *   out_fd  — the read end of the command's stdout pipe
 *   err_fd  — the read end of its stderr pipe
 *   task    — the command
 *   err_out — receives what it wrote to stderr
 *
 * Output: what it wrote to stdout.
 */
static NSData *
read_until_exit(int out_fd, int err_fd, NSTask *task, NSData **err_out)
{
    NSMutableData *got[2] = { [NSMutableData data], [NSMutableData data] };
    int            fd[2]  = { out_fd, err_fd };
    char           buf[16384];
    for (;;) {
        if (fd[0] < 0 && fd[1] < 0) break;          /* both streams have ended */
        struct pollfd p[2] = { { fd[0], POLLIN, 0 }, { fd[1], POLLIN, 0 } };
        int ready = poll(p, 2, 200);                /* poll ignores an fd of -1 */
        if (ready > 0) {
            BOOL moved = NO;
            for (int i = 0; i < 2; i++) {
                if (fd[i] < 0 || p[i].revents == 0) continue;
                ssize_t n = read(fd[i], buf, sizeof buf);
                if (n > 0) {
                    [got[i] appendBytes:buf length:(NSUInteger)n];
                    moved = YES;
                } else if (n < 0 && errno == EINTR) {
                    moved = YES;
                } else {
                    fd[i] = -1;        /* the end of that stream, or an error */
                }
            }
            if (moved) continue;
        }
        if (ready < 0 && errno != EINTR) break;
        if (ready == 0 && !task.isRunning) break;   /* exited, nothing left */
    }
    *err_out = got[1];
    return got[0];
}

/*
 * on_ai_run — see ai_run.h.
 */
NSTask *
on_ai_run(NSString *command, NSString *prompt, NSError **error,
          void (^done)(NSTask *task, NSString *output))
{
    NSTask *task = [[NSTask alloc] init];
    task.executableURL  = [NSURL fileURLWithPath:@"/bin/sh"];
    task.arguments      = @[@"-c", command];
    task.environment    = child_env();
    NSPipe *in = [NSPipe pipe], *out = [NSPipe pipe], *err = [NSPipe pipe];
    task.standardInput  = in;
    task.standardOutput = out;
    task.standardError  = err;
    if (![task launchAndReturnError:error]) return nil;

    fcntl(in.fileHandleForWriting.fileDescriptor, F_SETNOSIGPIPE, 1);   /* a command that ignores stdin */
    NSData *bytes = [prompt dataUsingEncoding:NSUTF8StringEncoding];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool {
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                @autoreleasepool {
                    [in.fileHandleForWriting writeData:bytes error:NULL];   /* while stdout drains */
                    [in.fileHandleForWriting closeFile];
                }
            });
            NSData *complaint = nil;
            NSData *result    = read_until_exit(out.fileHandleForReading.fileDescriptor,
                                                err.fileHandleForReading.fileDescriptor, task, &complaint);
            [task waitUntilExit];
            /* An answer is what the command wrote to stdout; stderr is heard
             * only when there is no answer, so the warnings and progress a
             * tool prints alongside one stay out of the summary. */
            NSString *text = stream_text(result) ?: stream_text(complaint);
            dispatch_async(dispatch_get_main_queue(), ^{ done(task, text); });
        }
    });
    return task;
}

/*
 * on_ai_prompt — see ai_run.h.
 */
NSString *
on_ai_prompt(NSString *request, OnDatabase *db, NSArray<NSNumber *> *ids)
{
    NSMutableString *p = [NSMutableString stringWithString:request];
    [p appendString:prompt_plain];
    [p appendString:@"=== Notes ===\n\n"];
    NSUInteger found = 0;
    for (NSNumber *n in ids) {
        OnNoteMeta *m = on_db_note_get(db, n.longLongValue);
        if (m == NULL) continue;
        found++;
        [p appendFormat:@"--- %@ (modified %@) ---\n", m->title ? @(m->title) : @"(untitled)",
                        shared_ui_format_date(m->updated_at, shared_ui_date_iso)];
        on_db_note_meta_free(m);
        gchar *body = on_note_text_cached(db, n.longLongValue);   /* fills an empty cache */
        [p appendString:body && *body ? [@(body) stringByAppendingString:@"\n"] : @"(empty note)\n"];
        g_free(body);
        GList *items = on_db_action_list_for_note(db, n.longLongValue);
        if (items) [p appendString:@"\nAction items:\n"];
        for (GList *l = items; l != NULL; l = l->next) {
            const OnActionItem *it = l->data;
            [p appendFormat:@"  %@ %@", it->done ? @"[done]" : @"[    ]", @(it->text ? it->text : "")];
            if (it->due) [p appendFormat:@" (due %@)", shared_ui_format_date(it->due, shared_ui_date_iso)];
            [p appendString:@"\n"];
        }
        on_db_action_list_free(items);
        [p appendString:@"\n"];
    }
    return found ? p : nil;
}
