#import <Cocoa/Cocoa.h>

#include "db.h"

/*
 * ai_run — THE launcher of the app's AI commands and THE builder of their
 * prompts.
 *
 * Two features run an external command the user configures: the library's
 * AI Summary pane (library_ai.h) and the editor's summary of the last
 * meeting (meeting_summary.h).  Both come through here, so there is one
 * launcher, one reader of a command's output and one prompt body.
 *
 * The command line is run by /bin/sh, so it is quoted as in a shell (the
 * GTK build split it with the same rules).  The prompt goes on the
 * command's stdin and the ANSWER comes back as one string: what it wrote
 * to stdout, or, when it wrote nothing there, what it wrote to stderr —
 * so a command that only complains is still heard, while the warnings and
 * progress an agent prints beside a real answer are left out of it.  It
 * runs off the main thread and hands back on the main thread.
 */

/*
 * on_ai_run — run an AI command with a prompt on its stdin.
 *
 * A GUI app inherits a bare PATH (/usr/bin:/bin:/usr/sbin:/sbin), so the
 * child's PATH is extended with the directories a user's command-line
 * tools live in — ~/.local/bin, ~/.claude/local, /opt/homebrew/bin,
 * /usr/local/bin and /opt/local/bin — and a bare "claude -p" is enough.
 *
 * Inputs:
 *   command — the command line, as the user typed it (run by /bin/sh -c)
 *   prompt  — the text written to its stdin, then closed
 *   error   — optional; on a command that would not start, receives why
 *   done    — run on the main thread when the command has exited, with the
 *             task it was handed back for and its answer: its stdout, or
 *             its stderr when stdout was empty, trimmed (nil when it wrote
 *             nothing either way).  Not run when the command never
 *             started.
 *
 * Output: the running task, so the caller can tell its own result from a
 *         stale one and terminate it; nil when it would not start (*error
 *         says why and `done` never runs).
 */
NSTask *on_ai_run(NSString *command, NSString *prompt, NSError **error,
                  void (^done)(NSTask *task, NSString *output));

/*
 * on_ai_prompt — the text an AI command is given: the caller's request,
 * the plain-text rule (both features show the answer in a plain text
 * view), then each note with its title, date, body and action items.
 *
 * Inputs:
 *   request — what the command is asked to do, e.g. "Summarize these
 *             notes."; ends where the caller ends it
 *   db      — the database
 *   ids     — the notes, in the order they are to appear
 *
 * Output: the prompt, or nil when none of the notes could be read.
 */
NSString *on_ai_prompt(NSString *request, OnDatabase *db, NSArray<NSNumber *> *ids);
