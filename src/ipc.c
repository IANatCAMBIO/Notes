/* ===========================================================================
 * ipc.c — the socket between the command-line tool and the running app
 * (see ipc.h for the two protocols and the socket's location)
 *
 * The transport is BSD sockets: the app's listening socket is watched by a
 * dispatch source on the main queue, so a command is accepted and run on
 * the main thread, between two passes of the Cocoa run loop, where touching
 * windows and the database is safe.  (The GTK build used GIO's socket
 * service, which needs a GLib main loop that nothing runs here.)  The wire
 * framing is byte-for-byte the GTK build's.
 * =========================================================================== */

#include "ipc.h"
#include "cli.h"
#include "db.h"
#include "identity.h"

#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

/* Maximum number of arguments a valid CLI invocation can send.              */
#define IPC_MAX_ARGS      256

/* Wire-format sanity bounds.  No legitimate frame comes close to these;
 * they keep a stray or hostile client from wedging the app (which reads
 * the socket on the main thread) or forcing an absurd allocation from a
 * bogus length header.                                                      */
#define IPC_MAX_LINE  (64 * 1024)            /* command/header line bytes  */
#define IPC_MAX_BLOB  (64 * 1024 * 1024)     /* argv/stdin/output blob     */
#define IPC_TIMEOUT_S 10                     /* per-socket-op limit (s)    */

/* How many connections may wait while a command runs.                       */
#define IPC_BACKLOG 8

/* ===========================================================================
 * the socket's address
 *
 * The socket file is <data dir>/notes-appkit.sock.  A Unix socket address
 * holds at most 103 bytes of path, and a data directory under a deep
 * development checkout is longer than that, so the path is never passed
 * whole: bind() and connect() are made with the data directory as the
 * current directory and the bare file name as the address.  The previous
 * current directory is restored before the helper returns.
 * =========================================================================== */

/* Which of the two address-taking calls ipc_address_call makes.             */
typedef enum { IPC_BIND, IPC_CONNECT } ipc_call_t;

/*
 * ipc_address_call — bind or connect `fd` to the socket file in the data
 * directory (see the section comment for why this changes directory).
 *
 * Inputs:
 *   fd   — an AF_UNIX stream socket
 *   call — IPC_BIND or IPC_CONNECT
 *
 * Output: 0 on success; otherwise -1 with errno set — the call's own errno
 *         (ENOENT / ECONNREFUSED from connect mean "no app is listening"),
 *         or the errno of a failed directory change.  A failure to RESTORE
 *         the previous directory is reported as -1 / EXDEV even when the
 *         call itself succeeded, because the process is then in the wrong
 *         directory and must not carry on resolving relative paths.
 */
static int
ipc_address_call(int fd, ipc_call_t call)
{
    int saved = open(".", O_RDONLY | O_CLOEXEC);
    if (saved < 0) {
        return -1;
    }
    char *dir = on_paths_data_dir();
    int   rc  = chdir(dir);
    int   err = errno;
    free(dir);
    if (rc == 0) {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof addr);
        addr.sun_family = AF_UNIX;
        strlcpy(addr.sun_path, ON_SOCKET_FILENAME, sizeof addr.sun_path);
        rc = call == IPC_BIND
             ? bind(fd, (struct sockaddr *)&addr, sizeof addr)
             : connect(fd, (struct sockaddr *)&addr, sizeof addr);
        err = errno;
        if (fchdir(saved) != 0) {
            rc  = -1;
            err = EXDEV;
        }
    }
    close(saved);
    errno = err;
    return rc;
}

/*
 * ipc_socket_new — a Unix stream socket that reports a vanished peer as an
 * error instead of killing the process with SIGPIPE, and is not inherited
 * by child processes (the AI command, `open`).
 *
 * Output: the descriptor, or -1 with errno set.
 */
static int
ipc_socket_new(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

/*
 * ipc_set_timeout — bound every later send and receive on `fd`.
 *
 * Inputs:
 *   fd      — a connected socket
 *   seconds — the limit per operation; 0 = wait for ever
 *
 * Output: none.
 */
static void
ipc_set_timeout(int fd, int seconds)
{
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

/* ===========================================================================
 * framed stream I/O
 *
 * The RUN protocol (a delegated data command and its captured output) mixes
 * short header lines with arbitrary-length byte blobs, so it needs exact
 * reads: a line is bytes up to '\n'; a blob is a decimal length line
 * followed by exactly that many bytes.
 * =========================================================================== */

/*
 * stream_write_all — write every byte of `buf`.
 *
 * Inputs:
 *   fd  — a connected socket
 *   buf — the bytes
 *   n   — how many
 *
 * Output: TRUE when all were sent; FALSE on an error or a timeout.
 */
static gboolean
stream_write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n > 0) {
        ssize_t sent = send(fd, p, n, 0);
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent <= 0) {
            return FALSE;
        }
        p += sent;
        n -= (size_t)sent;
    }
    return TRUE;
}

/*
 * stream_read_all — read exactly `n` bytes.
 *
 * Inputs:
 *   fd  — a connected socket
 *   buf — receives the bytes
 *   n   — how many
 *
 * Output: TRUE when all arrived; FALSE at end of stream, on an error or a
 *         timeout.
 */
static gboolean
stream_read_all(int fd, void *buf, size_t n)
{
    char *p = buf;
    while (n > 0) {
        ssize_t got = recv(fd, p, n, 0);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            return FALSE;
        }
        p += got;
        n -= (size_t)got;
    }
    return TRUE;
}

/*
 * stream_read_line — read up to '\n'.  Byte at a time: header lines are
 * short.
 *
 * Inputs:
 *   fd — a connected socket
 *
 * Output: the line without its newline (g_free it), or NULL at end of
 *         stream before any byte, or past IPC_MAX_LINE.
 */
static gchar *
stream_read_line(int fd)
{
    GString *s = g_string_new(NULL);
    for (;;) {
        char c;
        if (!stream_read_all(fd, &c, 1)) {
            if (s->len == 0) {
                g_string_free(s, TRUE);
                return NULL;
            }
            break;
        }
        if (c == '\n') {
            break;
        }
        if (s->len >= IPC_MAX_LINE) {
            g_string_free(s, TRUE);
            return NULL;
        }
        g_string_append_c(s, c);
    }
    return g_string_free(s, FALSE);
}

/*
 * stream_read_uint_line — read a decimal length line.
 *
 * Inputs:
 *   fd — a connected socket
 *
 * Output: the value, or -1 at end of stream, on a non-numeric line (garbage
 *         must fail, not parse as 0 and desynchronise the protocol) or a
 *         negative value.
 */
static gssize
stream_read_uint_line(int fd)
{
    gchar *line = stream_read_line(fd);
    if (line == NULL) {
        return -1;
    }
    gchar   *end = NULL;
    gint64   v   = g_ascii_strtoll(line, &end, 10);
    gboolean ok  = end != line && *end == '\0' && v >= 0;
    g_free(line);
    return ok ? (gssize)v : -1;
}

/*
 * stream_read_blob — read exactly `n` bytes into a new buffer.
 *
 * Inputs:
 *   fd — a connected socket
 *   n  — the length the header claimed
 *
 * Output: the bytes, NUL-terminated (g_free them), or NULL on a short read
 *         or a claim over IPC_MAX_BLOB.
 */
static gchar *
stream_read_blob(int fd, gsize n)
{
    if (n > IPC_MAX_BLOB) {
        return NULL;
    }
    gchar *buf = g_malloc(n + 1);
    if (!stream_read_all(fd, buf, n)) {
        g_free(buf);
        return NULL;
    }
    buf[n] = '\0';
    return buf;
}

/*
 * stream_write_blob — write "<len>\n" then the bytes.
 *
 * Inputs:
 *   fd   — a connected socket
 *   data — the bytes
 *   n    — how many
 *
 * Output: TRUE when everything was sent.
 */
static gboolean
stream_write_blob(int fd, const gchar *data, gsize n)
{
    gchar   *hdr = g_strdup_printf("%" G_GSIZE_FORMAT "\n", n);
    gboolean ok  = stream_write_all(fd, hdr, strlen(hdr)) &&
                   stream_write_all(fd, data, n);
    g_free(hdr);
    return ok;
}

/* ===========================================================================
 * shared note-path resolution
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * ipc_resolve_note_path() — turn a "note path" into a note id.
 *   db     — open database.
 *   path   — a numeric note id, or "Folder/Sub/Title" (last part = title).
 *   out_id — receives the resolved note id.
 * Returns TRUE if exactly one note was found.  Title match is exact.
 * ------------------------------------------------------------------------- */
static gboolean
ipc_resolve_note_path(OnDatabase *db, const gchar *path, gint64 *out_id)
{
    *out_id = 0;
    if (path == NULL || *path == '\0')
        return FALSE;

    /* A bare number is a note id (mirrors the rest of the CLI, which
     * addresses notes by id).                                               */
    gboolean all_digits = TRUE;      /* is every character a digit?          */
    for (const char *p = path; *p != '\0'; p++)
        if (!g_ascii_isdigit((guchar)*p)) { all_digits = FALSE; break; }
    if (all_digits) {
        gint64 id = g_ascii_strtoll(path, NULL, 10);
        OnNoteMeta *m = (id > 0) ? on_db_note_get(db, id) : NULL;
        if (m != NULL) {
            *out_id = id;
            on_db_note_meta_free(m);
            return TRUE;
        }
        return FALSE;
    }

    /* Otherwise the last non-empty path component is a note title and the
     * rest is the folder path holding it.                                   */
    gchar **parts = g_strsplit(path, "/", -1);
    gint    n     = (gint)g_strv_length(parts);
    gint    title_idx = -1;          /* index of the title component         */
    for (gint i = n - 1; i >= 0; i--)
        if (parts[i] != NULL && *parts[i] != '\0') { title_idx = i; break; }
    if (title_idx < 0) {
        g_strfreev(parts);
        return FALSE;
    }
    const gchar *title = parts[title_idx];

    GString *folder_path = g_string_new(NULL);   /* "Folder/Sub" prefix      */
    for (gint i = 0; i < title_idx; i++) {
        if (parts[i] == NULL || *parts[i] == '\0')
            continue;
        if (folder_path->len > 0)
            g_string_append_c(folder_path, '/');
        g_string_append(folder_path, parts[i]);
    }

    gint64 folder = 0;               /* resolved folder id (0 = top level)   */
    gboolean ok = on_cli_resolve_folder_path(db, folder_path->str, FALSE,
                                             &folder);
    g_string_free(folder_path, TRUE);
    if (!ok) {
        g_strfreev(parts);
        return FALSE;
    }

    gint64 found = 0;                /* first note whose title matches       */
    GList *notes = on_db_note_list(db, folder);
    for (GList *l = notes; l != NULL; l = l->next) {
        OnNoteMeta *m = l->data;
        if (g_strcmp0(m->title, title) == 0) {
            found = m->id;
            break;
        }
    }
    on_db_note_list_free(notes);
    g_strfreev(parts);

    if (found == 0)
        return FALSE;
    *out_id = found;
    return TRUE;
}

/* ===========================================================================
 * actions (run in the app's process, whether reached over the socket or as
 * a pending action after the app was started for it)
 * =========================================================================== */

/*
 * ipc_do_quicknote — the quicknote action, through the UI's hook.
 *
 * Inputs:
 *   app — the application context (its quicknote hook installed)
 *
 * Output: a new "OK ..." / "ERR ..." reply line; g_free it.
 */
static gchar *
ipc_do_quicknote(OnApp *app)
{
    gint64 id = app->quicknote(app);
    if (id == 0) {
        return g_strdup("ERR could not create note");
    }
    return g_strdup_printf("OK created note %" G_GINT64_FORMAT, id);
}

/*
 * ipc_do_open — open the editor for the note named by `path`, to the
 * front, through the UI's hook.
 *
 * Inputs:
 *   app  — the application context (its open_note hook installed)
 *   path — a note id or "Folder/Sub/Title"
 *
 * Output: a new "OK ..." / "ERR ..." reply line; g_free it.
 */
static gchar *
ipc_do_open(OnApp *app, const gchar *path)
{
    gint64 id;
    if (!ipc_resolve_note_path(app->db, path, &id)) {
        return g_strdup_printf("ERR no note matches path: %s", path);
    }
    if (!app->open_note(app, id)) {
        return g_strdup_printf("ERR could not open note: %s", path);
    }
    return g_strdup_printf("OK opened note %" G_GINT64_FORMAT, id);
}

/*
 * ipc_dispatch — run one request line of the one-line protocol.
 *
 * Inputs:
 *   app  — the application context
 *   line — "quicknote" or "open <path>"
 *
 * Output: a new reply line; g_free it.
 */
static gchar *
ipc_dispatch(OnApp *app, const gchar *line)
{
    if (g_strcmp0(line, "quicknote") == 0) {
        return ipc_do_quicknote(app);
    }
    if (g_str_has_prefix(line, "open ") && line[5] != '\0') {
        return ipc_do_open(app, line + 5);
    }
    return g_strdup_printf("ERR unknown command: %s", line);
}

/* ===========================================================================
 * pending action (no app was running; do it once this one is up)
 * =========================================================================== */

typedef enum {
    IPC_PENDING_NONE = 0,
    IPC_PENDING_QUICKNOTE,
    IPC_PENDING_OPEN,
} ipc_pending_t;

static ipc_pending_t  pending_kind = IPC_PENDING_NONE;
static gchar         *pending_path = NULL;   /* for IPC_PENDING_OPEN (owned) */

void
on_ipc_set_pending_quicknote(void)
{
    pending_kind = IPC_PENDING_QUICKNOTE;
}

void
on_ipc_set_pending_open(const gchar *path)
{
    pending_kind = IPC_PENDING_OPEN;
    g_free(pending_path);
    pending_path = g_strdup(path);
}

gboolean
on_ipc_has_pending(void)
{
    return pending_kind != IPC_PENDING_NONE;
}

void
on_ipc_run_pending(OnApp *app)
{
    if (pending_kind == IPC_PENDING_NONE) {
        return;
    }
    gchar *reply = pending_kind == IPC_PENDING_QUICKNOTE
                   ? ipc_do_quicknote(app)
                   : ipc_do_open(app, pending_path);
    /* The app is already on screen; surface only failures, on stderr.      */
    if (g_str_has_prefix(reply, "ERR ")) {
        fprintf(stderr, "notes: %s\n", reply + 4);
    }
    g_free(reply);
    g_clear_pointer(&pending_path, g_free);
    pending_kind = IPC_PENDING_NONE;
}

/* ===========================================================================
 * server (the running app)
 * =========================================================================== */

/* What the listening socket's dispatch source carries as its context: the
 * app the commands run against, and the descriptor, which the source owns
 * (its cancel handler closes it and frees this record).                     */
typedef struct {
    OnApp *app;
    int    fd;
} ipc_server_t;

/* The listening socket's dispatch source, or NULL while not serving.        */
static dispatch_source_t ipc_source = NULL;

/* The socket file, to unlink when the server stops (owned).                 */
static char *ipc_bound_path = NULL;

/* ---------------------------------------------------------------------------
 * ipc_capture_run() — execute a data command against the live database with
 * its stdout/stderr redirected to temp files, so the text can be shipped back
 * to the remote CLI.  Safe because we run synchronously on the main thread (no
 * other code writes fd 1/2 during the window).
 *   out_text, err_text — receive the captured streams (owned).
 * Returns the command's exit code.
 * ------------------------------------------------------------------------- */
static int
ipc_capture_run(OnApp *app, int argc, char **argv, const gchar *stdin_data,
                gchar **out_text, gchar **err_text)
{
    *out_text = NULL;
    *err_text = NULL;

    fflush(stdout);
    fflush(stderr);
    int   saved_out = dup(1);        /* real stdout/stderr fds              */
    int   saved_err = dup(2);
    FILE *fo = tmpfile();            /* capture files                       */
    FILE *fe = tmpfile();

    gboolean captured = saved_out >= 0 && saved_err >= 0 &&
                        fo != NULL && fe != NULL;
    if (captured) {
        dup2(fileno(fo), 1);
        dup2(fileno(fe), 2);
    }

    on_cli_set_stdin_data(stdin_data);
    int rc = on_cli_dispatch(app, argc, argv);
    on_cli_set_stdin_data(NULL);

    if (captured) {
        fflush(stdout);
        fflush(stderr);
        dup2(saved_out, 1);
        dup2(saved_err, 2);
        *out_text = on_app_read_stream(fo, TRUE);
        *err_text = on_app_read_stream(fe, TRUE);
    }
    if (saved_out >= 0) close(saved_out);
    if (saved_err >= 0) close(saved_err);
    if (fo != NULL) fclose(fo);
    if (fe != NULL) fclose(fe);
    return rc;
}

/*
 * ipc_handle_run — server side of the RUN protocol: read the argv + stdin
 * frame, run the command against the live database, refresh the UI if it
 * changed anything, and write back the exit code and the captured output.
 *
 * Inputs:
 *   app — the application context
 *   fd  — the accepted connection, positioned after the "RUN" line
 *
 * Output: none; a malformed frame is dropped without a reply.
 */
static void
ipc_handle_run(OnApp *app, int fd)
{
    gssize nargs = stream_read_uint_line(fd);
    if (nargs < 0 || nargs > IPC_MAX_ARGS) {
        return;
    }

    /* Rebuild an argv the dispatcher understands: argv[0] is a placeholder
     * program name, argv[1..] the received words.                          */
    char    **argv = g_new0(char *, (gsize)nargs + 2);
    argv[0] = g_strdup(ON_CLI_NAME);
    gboolean bad = FALSE;            /* frame decode failed?                 */
    for (gssize i = 0; i < nargs && !bad; i++) {
        gssize len = stream_read_uint_line(fd);
        gchar *arg = (len >= 0) ? stream_read_blob(fd, (gsize)len) : NULL;
        if (arg == NULL) {
            bad = TRUE;
        } else {
            argv[i + 1] = arg;
        }
    }

    gssize slen = bad ? -1 : stream_read_uint_line(fd);
    gchar *stdin_data = (slen >= 0) ? stream_read_blob(fd, (gsize)slen) : NULL;

    if (!bad && slen >= 0 && stdin_data == NULL) {
        /* The stdin blob is too large or unreadable: answer with an error
         * so the client does not see a silent close and report exit code
         * 2 with no message.                                               */
        const gchar *errmsg =
            "error: stdin payload too large or unreadable\n";
        stream_write_all(fd, "2\n", 2);
        stream_write_blob(fd, "", 0);
        stream_write_blob(fd, errmsg, strlen(errmsg));
    } else if (!bad && stdin_data != NULL) {
        int argc = (int)nargs + 1;
        const gchar *sd = on_cli_command_reads_stdin(argc, argv)
                          ? stdin_data : NULL;
        gchar *out_text = NULL, *err_text = NULL;
        int rc = ipc_capture_run(app, argc, argv, sd, &out_text, &err_text);

        /* Let the running app reflect a change the command just made.      */
        if (on_cli_command_mutates(argc, argv) &&
            app->notify_notes_changed != NULL) {
            app->notify_notes_changed(app);
        }

        gchar *rc_line = g_strdup_printf("%d\n", rc);
        stream_write_all(fd, rc_line, strlen(rc_line));
        stream_write_blob(fd, out_text ? out_text : "",
                          out_text ? strlen(out_text) : 0);
        stream_write_blob(fd, err_text ? err_text : "",
                          err_text ? strlen(err_text) : 0);
        g_free(rc_line);
        g_free(out_text);
        g_free(err_text);
    }

    g_free(stdin_data);
    g_strfreev(argv);
}

/*
 * ipc_serve — one accepted connection, start to finish.  The first line
 * selects the protocol: "RUN" begins a delegated data command (framed);
 * anything else is a one-line GUI command.
 *
 * Inputs:
 *   app — the application context
 *   fd  — the accepted connection; closed by the caller
 *
 * Output: none.
 */
static void
ipc_serve(OnApp *app, int fd)
{
    gchar *first = stream_read_line(fd);
    if (first == NULL) {
        return;
    }
    if (g_strcmp0(first, "RUN") == 0) {
        ipc_handle_run(app, fd);
    } else if (*first != '\0') {
        gchar *reply = ipc_dispatch(app, first);
        gchar *wire  = g_strconcat(reply, "\n", NULL);
        stream_write_all(fd, wire, strlen(wire));
        g_free(wire);
        g_free(reply);
    }
    g_free(first);
}

/*
 * ipc_accept — the listening socket is readable: accept every waiting
 * connection and serve each in turn.  Runs on the main queue, so a command
 * executes on the main thread.  Reads block the main thread; the timeout
 * bounds how long a stalled or hostile client can freeze the app.
 *
 * Inputs:
 *   context — the ipc_server_t (the dispatch source's context)
 *
 * Output: none.
 */
static void
ipc_accept(void *context)
{
    const ipc_server_t *server = context;
    for (;;) {
        int fd = accept(server->fd, NULL, NULL);
        if (fd < 0) {
            return;                  /* EWOULDBLOCK: the queue is empty      */
        }
        /* An accepted socket inherits the listener's non-blocking mode;
         * the framed reads need blocking with a timeout instead.           */
        int on = 1;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
        ipc_set_timeout(fd, IPC_TIMEOUT_S);
        ipc_serve(server->app, fd);
        close(fd);
    }
}

/*
 * ipc_server_free — the dispatch source's cancel handler: the source no
 * longer watches the descriptor, so it can be closed (closing it while the
 * source is live is a GCD error) and the context released.
 *
 * Inputs:
 *   context — the ipc_server_t
 *
 * Output: none.
 */
static void
ipc_server_free(void *context)
{
    ipc_server_t *server = context;
    close(server->fd);
    free(server);
}

OnIpcServerResult
on_ipc_server_start(OnApp *app)
{
    if (ipc_source != NULL) {
        return ON_IPC_SERVER_LISTENING;
    }

    /* A socket file may be left by a crash, or belong to a live instance.
     * Connecting tells them apart: only a dead one may be cleared.         */
    int probe = ipc_socket_new();
    if (probe < 0) {
        return ON_IPC_SERVER_FAILED;
    }
    int connected = ipc_address_call(probe, IPC_CONNECT);
    int probe_err = errno;
    close(probe);
    if (connected == 0) {
        return ON_IPC_SERVER_BUSY;
    }
    if (probe_err != ENOENT && probe_err != ECONNREFUSED) {
        fprintf(stderr, "notes: cannot probe the app socket: %s\n",
                strerror(probe_err));
        return ON_IPC_SERVER_FAILED;
    }
    char *path = on_paths_data_file(ON_SOCKET_FILENAME);
    if (probe_err == ECONNREFUSED) {
        unlink(path);
    }

    int fd = ipc_socket_new();
    if (fd < 0 || ipc_address_call(fd, IPC_BIND) != 0 ||
        listen(fd, IPC_BACKLOG) != 0) {
        fprintf(stderr, "notes: cannot listen on %s: %s\n", path,
                strerror(errno));
        if (fd >= 0) {
            close(fd);
        }
        free(path);
        return ON_IPC_SERVER_FAILED;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

    ipc_server_t *server = calloc(1, sizeof *server);
    server->app = app;
    server->fd  = fd;
    ipc_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,
                                        (uintptr_t)fd, 0,
                                        dispatch_get_main_queue());
    dispatch_set_context(ipc_source, server);
    dispatch_source_set_event_handler_f(ipc_source, ipc_accept);
    dispatch_source_set_cancel_handler_f(ipc_source, ipc_server_free);
    dispatch_resume(ipc_source);
    ipc_bound_path = path;           /* kept for on_ipc_server_stop          */
    return ON_IPC_SERVER_LISTENING;
}

void
on_ipc_server_stop(void)
{
    if (ipc_source == NULL) {
        return;
    }
    /* Cancelling stops further accepts; the cancel handler then closes
     * the descriptor and frees the context on the main queue.             */
    dispatch_source_cancel(ipc_source);
    dispatch_release(ipc_source);
    ipc_source = NULL;

    unlink(ipc_bound_path);
    free(ipc_bound_path);
    ipc_bound_path = NULL;
}

/* ===========================================================================
 * client (a short-lived command-line invocation)
 * =========================================================================== */

/*
 * ipc_connect — connect to the running app's socket, with IPC_TIMEOUT_S on
 * every socket operation so a wedged app cannot hang the command line.
 *
 * Inputs:
 *   no_server — receives TRUE when nothing is listening (no socket file,
 *               or a dead one): the caller may then act on its own
 *
 * Output: the connected descriptor, or -1.  With -1 and *no_server FALSE
 *         the socket could not even be tried (errno says why) and it is
 *         NOT known that no app is running: the caller must not fall back
 *         to opening the database itself.
 */
static int
ipc_connect(gboolean *no_server)
{
    *no_server = FALSE;
    int fd = ipc_socket_new();
    if (fd < 0) {
        return -1;
    }
    ipc_set_timeout(fd, IPC_TIMEOUT_S);
    if (ipc_address_call(fd, IPC_CONNECT) != 0) {
        int err = errno;
        close(fd);
        *no_server = err == ENOENT || err == ECONNREFUSED;
        errno = err;
        return -1;
    }
    return fd;
}

OnIpcResult
on_ipc_try_remote(const gchar *command, gchar **reply_out)
{
    if (reply_out != NULL) {
        *reply_out = NULL;
    }
    gboolean no_server;
    int fd = ipc_connect(&no_server);
    if (fd < 0) {
        if (no_server) {
            return ON_IPC_NO_SERVER;
        }
        if (reply_out != NULL) {
            *reply_out = g_strdup_printf("cannot reach the app's socket: %s",
                                         strerror(errno));
        }
        return ON_IPC_ERROR;
    }

    /* The whole exchange is one short line each way.                       */
    gchar *wire = g_strconcat(command, "\n", NULL);
    stream_write_all(fd, wire, strlen(wire));
    g_free(wire);
    gchar *reply = stream_read_line(fd);
    close(fd);

    /* Split the OK/ERR status prefix off the message.                      */
    const gchar *text   = reply != NULL ? g_strchomp(reply) : "";
    OnIpcResult  result = g_str_has_prefix(text, "OK") ? ON_IPC_OK
                                                       : ON_IPC_ERROR;
    if (g_str_has_prefix(text, "OK ") || g_str_has_prefix(text, "ERR ")) {
        text += (result == ON_IPC_OK) ? 3 : 4;
    }
    if (reply_out != NULL) {
        *reply_out = g_strdup(text);
    }
    g_free(reply);
    return result;
}

int
on_ipc_try_remote_run(int argc, char **argv, gboolean *ran)
{
    *ran = FALSE;
    gboolean no_server;
    int fd = ipc_connect(&no_server);
    if (fd < 0) {
        if (no_server) {
            return 0;                /* the caller runs headless             */
        }
        /* Unknown whether an app is running: running headless now could
         * write the database behind a live app's back.  Refuse.           */
        fprintf(stderr, "error: cannot reach the app's socket: %s\n",
                strerror(errno));
        *ran = TRUE;
        return 2;
    }

    /* Slurp our stdin only for the commands that consume it — the app
     * cannot read this process's file descriptor.                          */
    gchar *stdin_data = on_cli_command_reads_stdin(argc, argv)
                        ? on_app_read_stream(stdin, FALSE) : g_strdup("");

    /* Request frame: "RUN", the word count, each word, then stdin.         */
    stream_write_all(fd, "RUN\n", 4);
    gchar *count = g_strdup_printf("%d\n", argc - 1);
    stream_write_all(fd, count, strlen(count));
    g_free(count);
    for (int i = 1; i < argc; i++) {
        stream_write_blob(fd, argv[i], strlen(argv[i]));
    }
    stream_write_blob(fd, stdin_data, strlen(stdin_data));
    g_free(stdin_data);

    /* The command may take as long as it takes (a full export): only the
     * connect and the request above are timeout-guarded.                   */
    ipc_set_timeout(fd, 0);

    /* Response frame: exit code, captured stdout, captured stderr.         */
    gssize rc    = stream_read_uint_line(fd);
    gssize olen  = stream_read_uint_line(fd);
    gchar *otext = (olen >= 0) ? stream_read_blob(fd, (gsize)olen) : NULL;
    gssize elen  = (otext != NULL) ? stream_read_uint_line(fd) : -1;
    gchar *etext = (elen >= 0) ? stream_read_blob(fd, (gsize)elen) : NULL;

    if (otext != NULL && olen > 0) {
        fwrite(otext, 1, (size_t)olen, stdout);
    }
    if (etext != NULL && elen > 0) {
        fwrite(etext, 1, (size_t)elen, stderr);
    }
    fflush(stdout);
    fflush(stderr);

    g_free(otext);
    g_free(etext);
    close(fd);

    *ran = TRUE;
    return (rc < 0) ? 2 : (int)rc;   /* decode failure -> generic error      */
}
