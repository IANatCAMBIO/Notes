/* ===========================================================================
 * ipc.h — the socket between the command-line tool and the running app
 *
 * WHY.  One process at a time may write the database.  While the app is
 * running, a command-line invocation therefore does not open the database
 * itself: it hands the command to the app over a Unix-domain socket, the
 * app runs it against its own connection — saving its open editors first
 * and reloading any whose note the command rewrote, so neither overwrites
 * the other — and ships the output back.  Only when no app is listening does the command line open
 * the database directly.  The socket both DETECTS a running app (connect
 * succeeds) and CARRIES the command to it.
 *
 * WHERE.  <data dir>/notes.sock (config.h, identity.h): inside this
 * build's own data directory, under a name the GTK build never uses (that
 * build's sockets are ~/.cache/notes-<hash>.sock).  The two builds can
 * therefore never serve each other's commands, and NOTES_DATA_DIR
 * redirects the socket together with the ini and the database — a
 * development run and an installed app never meet either.  One socket per
 * data directory: the command line always finds the app that owns the same
 * notes.ini, whatever database that app has open.
 *
 * WHETHER AN APP IS RUNNING is decided by a lock, not by the socket: the
 * app holds an exclusive flock on <data dir>/notes.lock for its
 * whole life (the kernel releases it if the app dies).  A client that
 * cannot connect runs headless only when that lock is free.  If the lock
 * is held — the app is starting, is busy, or could not create its socket —
 * or the question cannot be answered, the client REFUSES: a wrong guess
 * would put a second writer on the database behind a live app's back.
 *
 * WIRE FORMAT (the GTK build's, plus the working directory in RUN).
 *   Protocol A, one line each way:
 *     request   quicknote | open <path>
 *     response  OK <message>\n | ERR <message>\n
 *     <path> is a numeric note id or "Folder/Sub/Title" (exact title).
 *   Protocol B, a delegated data command:
 *     request   RUN\n <nargs>\n then per argument <len>\n<bytes>,
 *               then stdin as <len>\n<bytes>, then the client's working
 *               directory as <len>\n<bytes>
 *     response  <exit code>\n <len>\n<stdout bytes> <len>\n<stderr bytes>
 *   Limits: 256 arguments, 64 KiB per line, 64 MiB per blob; 10 s per
 *   socket operation, except the client's wait for a RUN response, which
 *   is unbounded (an export takes as long as it takes).
 *
 * The server side needs the main run loop (its listening socket is a
 * dispatch source on the main queue) and the UI's hooks in OnApp; the
 * client side needs neither.
 * =========================================================================== */

#ifndef ON_IPC_H
#define ON_IPC_H

#include "app_core.h"

/* Outcome of trying to reach a running app (client side).                   */
typedef enum {
    ON_IPC_NO_SERVER = 0,            /* nothing listening                    */
    ON_IPC_OK,                       /* the app handled the command          */
    ON_IPC_ERROR,                    /* it reported a failure, or could not
                                        be reached                           */
} OnIpcResult;

/* Outcome of taking the app lock.                                           */
typedef enum {
    on_ipc_lock_held,                /* this process holds it                */
    on_ipc_lock_busy,                /* a live app holds it                  */
    on_ipc_lock_failed,              /* it could not be tried (stderr)       */
} on_ipc_lock_result_t;

/* Outcome of starting the server (app side).                                */
typedef enum {
    on_ipc_server_listening,         /* this process now serves the socket   */
    on_ipc_server_busy,              /* another live app already serves this
                                        data directory                       */
    on_ipc_server_failed,            /* the socket could not be created      */
} on_ipc_server_result_t;

/* ---------------------------------------------------------------------------
 * on_ipc_try_remote() — CLIENT: send one protocol-A command line to a
 * running app and wait for its one-line reply.
 *
 * Inputs:
 *   command   — "quicknote" or "open PATH" (no trailing newline)
 *   reply_out — receives the app's message with the OK/ERR prefix and the
 *               newline stripped (g_free it); may be NULL to ignore
 *
 * Output: ON_IPC_NO_SERVER when no app is listening (*reply_out NULL),
 *         otherwise ON_IPC_OK or ON_IPC_ERROR.
 * ------------------------------------------------------------------------- */
OnIpcResult on_ipc_try_remote(const gchar *command, gchar **reply_out);

/* ---------------------------------------------------------------------------
 * on_ipc_try_remote_run() — CLIENT: ask a running app to execute a data
 * command (protocol B) and copy its captured stdout/stderr to ours.
 *
 * Relative paths in the arguments mean this process's working directory,
 * which is sent along.
 *
 * Inputs:
 *   argc, argv — the full program arguments (argv[1] is the command)
 *   stdin_data — the command's "-" content, already read; NULL for none
 *   ran        — receives TRUE when the caller must NOT run the command
 *                itself: the app ran it, or the socket could not be tried
 *                (an error has been printed)
 *
 * Output: the command's exit code when *ran is TRUE (2, with a message,
 *         when the response could not be read whole, or the socket could
 *         not be tried); 0 with *ran FALSE when no app is listening, so the
 *         caller runs headless — this process then HOLDS the app lock, so
 *         no app can start on the database until on_ipc_lock_release (or
 *         the process ends).
 * ------------------------------------------------------------------------- */
int on_ipc_try_remote_run(int argc, char **argv, const gchar *stdin_data,
                          gboolean *ran);

/* ---------------------------------------------------------------------------
 * on_ipc_run_command() — run one data command inside the running app, as
 * the server does for a remote CLI: a command that writes the database
 * runs between app->save_editors (open editors save first; if one cannot,
 * the command is refused) and app->reload_editors (editors catch up with
 * what it wrote), then the windows refresh.  Main thread only.
 *
 * Inputs:
 *   app        — the application context, its database open
 *   argc, argv — the full program arguments (argv[1] is the command)
 *   stdin_data — the text a "-" argument reads; NULL for none
 *   cwd        — the directory relative paths mean (the CLI's), the app's
 *                own restored afterwards; NULL to stay where the app is
 *   out_text   — receives the captured stdout (g_free it; may be NULL)
 *   err_text   — receives the captured stderr (g_free it; may be NULL)
 *
 * Output: the command's exit code; 2 when it was refused or not run.
 * ------------------------------------------------------------------------- */
int on_ipc_run_command(OnApp *app, int argc, char **argv,
                       const gchar *stdin_data, const gchar *cwd,
                       gchar **out_text, gchar **err_text);

/* ---------------------------------------------------------------------------
 * on_ipc_set_pending_quicknote() / on_ipc_set_pending_open() — record a GUI
 * command that found no running app, for the app about to start to perform.
 *
 * Inputs:
 *   path — the note path to open (copied)
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_ipc_set_pending_quicknote(void);
void on_ipc_set_pending_open(const gchar *path);

/* ---------------------------------------------------------------------------
 * on_ipc_has_pending() — is a pending action recorded?
 *
 * Output: TRUE when one is.
 * ------------------------------------------------------------------------- */
gboolean on_ipc_has_pending(void);

/* ---------------------------------------------------------------------------
 * on_ipc_run_pending() — perform and clear the pending action, through the
 * UI's hooks; a failure is reported on stderr.  A no-op when none is
 * recorded.
 *
 * Inputs:
 *   app — the application context, hooks installed, library on screen
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_ipc_run_pending(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_ipc_lock_take() — make this process THE one using the data directory:
 * take the app lock (retrying briefly, as a client may be probing it).
 * The app takes it first thing at launch, before it opens the database or
 * shows a window, so a second copy never has the database open.
 *
 * Output: on_ipc_lock_held (also when already held), on_ipc_lock_busy when
 *         a live app holds it, on_ipc_lock_failed when it could not be
 *         tried (reported on stderr).
 * ------------------------------------------------------------------------- */
on_ipc_lock_result_t on_ipc_lock_take(void);

/* ---------------------------------------------------------------------------
 * on_ipc_lock_release() — let go of the app lock (on_ipc_server_stop does;
 * a headless command-line run does once its database is closed).  A no-op
 * when not held.
 * ------------------------------------------------------------------------- */
void on_ipc_lock_release(void);

/* ---------------------------------------------------------------------------
 * on_ipc_server_start() — SERVER: take the app lock if this process does
 * not hold it yet, and begin listening for command-line invocations.  A
 * socket file left by a crashed instance is cleared; one a live instance
 * still serves is left alone.  Main thread, after the hooks are installed.
 *
 * Inputs:
 *   app — the application context commands will run against
 *
 * Output: on_ipc_server_listening on success (also when already
 *         listening).  on_ipc_server_busy means another app instance holds
 *         this data directory's lock and this one must not go on to write
 *         the database.  on_ipc_server_failed (reported on stderr) means
 *         this app holds the lock but has no socket: `notes` commands will
 *         be refused while it runs — the caller must tell the user.
 * ------------------------------------------------------------------------- */
on_ipc_server_result_t on_ipc_server_start(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_ipc_server_stop() — stop listening, remove the socket file and release
 * the app lock.  Main thread.  A no-op when the server was never started.
 *
 * Output: none.
 * ------------------------------------------------------------------------- */
void on_ipc_server_stop(void);

#endif /* ON_IPC_H */
