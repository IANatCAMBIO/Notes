/* ===========================================================================
 * ipc_test.c — the CLI <-> app socket, between real processes
 *
 * This program is both sides.  Run with "--server" it is a stand-in for
 * the app: it opens the test database, installs hooks, starts the IPC
 * server and runs the main run loop.  Run plainly it is the test: it starts
 * that server as a child process and talks to it three ways —
 *   - raw bytes on the socket, to pin the WIRE FORMAT (it must stay what
 *     the GTK build speaks);
 *   - the real `notes` tool (NOTES_TEST_CLI), a third process, to prove a
 *     command typed in a terminal is executed by the server process;
 *   - the client functions in-process, for the no-server, busy and
 *     stale-socket cases.
 * It ends with a data directory whose path is longer than a socket address
 * can hold, the case a deep development checkout hits.
 * =========================================================================== */

#include "check.h"

#include "app_core.h"
#include "db.h"
#include "ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/* The file the server appends one byte to per notify_notes_changed: how
 * the test sees that a command ran INSIDE the server process.               */
#define CHANGED_FILE "changed"

/* ── The server side ────────────────────────────────────────────────────── */

/*
 * server_quicknote — OnApp.quicknote for the stand-in app: create the note.
 *
 * Inputs:
 *   app — the context
 *
 * Output: the new note's id.
 */
static gint64
server_quicknote(OnApp *app)
{
    return on_db_note_create(app->db, 0);
}

/*
 * server_open_note — OnApp.open_note for the stand-in app: there is no
 * window to open, so every existing note "opens".
 *
 * Inputs:
 *   app     — the context (unused)
 *   note_id — the note (unused)
 *
 * Output: TRUE.
 */
static gboolean
server_open_note(OnApp *app, gint64 note_id)
{
    (void)app;
    (void)note_id;
    return TRUE;
}

/*
 * server_changed — OnApp.notify_notes_changed for the stand-in app: leave a
 * mark the test process can see.
 *
 * Inputs:
 *   app — the context (unused)
 */
static void
server_changed(OnApp *app)
{
    (void)app;
    char *path = on_paths_data_file(CHANGED_FILE);
    FILE *f    = fopen(path, "ab");
    fputc('x', f);
    fclose(f);
    free(path);
}

/*
 * server_main — be the app until killed.
 *
 * Output: does not return normally; 3 when the server could not start.
 */
static int
server_main(void)
{
    static OnApp app;
    on_app_config_init();
    app.db                   = on_db_open(NULL);
    app.quicknote            = server_quicknote;
    app.open_note            = server_open_note;
    app.notify_notes_changed = server_changed;
    if (app.db == NULL || on_ipc_server_start(&app) != on_ipc_server_listening) {
        return 3;
    }
    CFRunLoopRun();
    return 0;
}

/* ── The test side ──────────────────────────────────────────────────────── */

/*
 * raw_connect — a socket connected to the server, by the socket file's bare
 * name (the test's current directory is the data directory).
 *
 * Output: the descriptor, or -1.
 */
static int
raw_connect(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strlcpy(addr.sun_path, ON_SOCKET_FILENAME, sizeof addr.sun_path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/*
 * raw_exchange — send bytes to the server and collect everything it sends
 * back until it closes the connection.
 *
 * Inputs:
 *   request — the bytes to send
 *   n       — how many
 *   reply   — receives the response, NUL-terminated
 *   size    — its capacity
 *
 * Output: the response length, or -1 when the server could not be reached.
 */
static ssize_t
raw_exchange(const char *request, size_t n, char *reply, size_t size)
{
    int fd = raw_connect();
    if (fd < 0) {
        return -1;
    }
    send(fd, request, n, 0);
    size_t total = 0;
    for (;;) {
        ssize_t got = recv(fd, reply + total, size - 1 - total, 0);
        if (got <= 0) {
            break;
        }
        total += (size_t)got;
    }
    reply[total] = '\0';
    close(fd);
    return (ssize_t)total;
}

/*
 * line_reply — one protocol-A exchange.
 *
 * Inputs:
 *   command — the request line, with its newline
 *   expect  — the exact reply expected, with its newline
 *
 * Output: true when the reply matched byte for byte.
 */
static bool
line_reply(const char *command, const char *expect)
{
    char reply[512];
    ssize_t n = raw_exchange(command, strlen(command), reply, sizeof reply);
    return n == (ssize_t)strlen(expect) && strcmp(reply, expect) == 0;
}

/*
 * changed_count — how many times the server's notify hook has fired.
 *
 * Output: the count (0 before the first).
 */
static long
changed_count(void)
{
    struct stat sb;
    return stat(CHANGED_FILE, &sb) == 0 ? (long)sb.st_size : 0;
}

/*
 * run_cli — run the real `notes` tool and capture its output.
 *
 * Inputs:
 *   args — the command line after the program name (shell syntax)
 *   out  — receives stdout and stderr together, NUL-terminated
 *   size — its capacity
 *
 * Output: the tool's exit status, or -1 if it could not be run.
 */
static int
run_cli(const char *args, char *out, size_t size)
{
    char command[PATH_MAX + 512];
    snprintf(command, sizeof command, "'%s' %s 2>&1", getenv("NOTES_TEST_CLI"), args);
    FILE *p = popen(command, "r");
    if (p == NULL) {
        return -1;
    }
    size_t n = fread(out, 1, size - 1, p);
    out[n] = '\0';
    int status = pclose(p);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/*
 * start_server — launch this program as the stand-in app and wait until it
 * answers on its socket.
 *
 * Inputs:
 *   self — this executable
 *
 * Output: the server's pid, or -1 when it did not come up within 10 s.
 */
static pid_t
start_server(const char *self)
{
    pid_t pid = fork();
    if (pid == 0) {
        execl(self, self, "--server", (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 500; i++) {
        int fd = raw_connect();
        if (fd >= 0) {
            close(fd);
            return pid;
        }
        usleep(20000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
}

/*
 * stop_server — kill the stand-in app as a crash would, leaving its socket
 * file behind.
 *
 * Inputs:
 *   pid — the server
 */
static void
stop_server(pid_t pid)
{
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

/*
 * test_wire_format — both protocols, byte for byte.
 */
static void
test_wire_format(void)
{
    /* Protocol B first, on a fresh database, so the ids are known.         */
    const char *run_folder = "RUN\n3\n6\nfolder3\nadd9\nIpcFolder0\n1\n.";
    const char *want       = "0\n24\nfolder IpcFolder (id 1)\n0\n";
    char reply[4096];
    long before = changed_count();
    ssize_t n = raw_exchange(run_folder, strlen(run_folder), reply, sizeof reply);
    check(n == (ssize_t)strlen(want) && strcmp(reply, want) == 0,
          "RUN: <rc>\\n<len>\\n<stdout><len>\\n<stderr>, byte for byte");
    check(changed_count() == before + 1, "a mutating command fires notify_notes_changed in the server");

    const char *run_stdin = "RUN\n3\n4\nnote3\nnew1\n-16\nFrom stdin\n\nbody1\n.";
    n = raw_exchange(run_stdin, strlen(run_stdin), reply, sizeof reply);
    check(n > 0 && strcmp(reply, "0\n18\nnote 1\tFrom stdin\n0\n") == 0,
          "RUN ships stdin to the command");

    before = changed_count();
    const char *run_usage = "RUN\n1\n3\ntag0\n1\n.";
    n = raw_exchange(run_usage, strlen(run_usage), reply, sizeof reply);
    check(n > 4 && strncmp(reply, "1\n0\n", 4) == 0 && strstr(reply, "Usage: notes") != NULL,
          "a usage error comes back as exit 1 with the text on stderr");
    check(changed_count() == before, "a command that does not mutate fires no notify");

    check(line_reply("quicknote\n", "OK created note 2\n"), "protocol A: quicknote");
    check(line_reply("open 1\n", "OK opened note 1\n"), "protocol A: open by id");
    check(line_reply("open IpcFolder/Missing\n", "ERR no note matches path: IpcFolder/Missing\n"),
          "protocol A: open reports a path that matches nothing");
    check(line_reply("bogus\n", "ERR unknown command: bogus\n"), "protocol A: unknown command");

    const char *bad_count = "RUN\nnot-a-number\n";
    n = raw_exchange(bad_count, strlen(bad_count), reply, sizeof reply);
    check(n == 0, "a malformed frame is dropped without a reply");
    const char *bad_length = "RUN\n1\n999999999999\n";
    n = raw_exchange(bad_length, strlen(bad_length), reply, sizeof reply);
    check(n == 0, "an absurd length claim is dropped without a reply");
    check(line_reply("open 1\n", "OK opened note 1\n"), "and the server is still serving");
}

/*
 * test_real_cli — the `notes` tool, a third process, served by the server.
 *
 * Inputs:
 *   data_dir — the data directory both use
 */
static void
test_real_cli(const char *data_dir)
{
    char out[8192];
    long before = changed_count();
    int rc = run_cli("folder add ViaCli", out, sizeof out);
    check(rc == 0 && strcmp(out, "folder ViaCli (id 2)\n") == 0,
          "the tool prints the server's stdout and returns its exit code");
    check(changed_count() == before + 1, "the tool's command ran inside the server process");

    rc = run_cli("quicknote", out, sizeof out);
    check(rc == 0 && strcmp(out, "created note 3\n") == 0, "notes quicknote reaches the app's hook");

    rc = run_cli("note open 999", out, sizeof out);
    check(rc == 2 && strcmp(out, "no note matches path: 999\n") == 0,
          "notes note open reports failure with exit 2");

    rc = run_cli("stats", out, sizeof out);
    char expect[PATH_MAX + 32];
    snprintf(expect, sizeof expect, "database\t%s/notes.db\n", data_dir);
    check(rc == 0 && strstr(out, expect) != NULL, "stats names the test database");

    rc = run_cli("note cat 1", out, sizeof out);
    check(rc == 0 && strcmp(out, "From stdin\n\nbody\n") == 0, "a read returns the note's text");

    /* A relative path is the CLI's directory, not the app's.               */
    char elsewhere[PATH_MAX];
    snprintf(elsewhere, sizeof elsewhere, "%s/elsewhere", data_dir);
    mkdir(elsewhere, 0700);
    check(chdir(elsewhere) == 0, "the CLI runs from another directory");
    rc = run_cli("backup relative.db", out, sizeof out);
    check(chdir(data_dir) == 0, "and the test returns");
    struct stat sb;
    check(rc == 0 && stat("elsewhere/relative.db", &sb) == 0 && stat("relative.db", &sb) != 0,
          "a relative path names a file in the CLI's directory, not the app's");

    rc = run_cli("frobnicate", out, sizeof out);
    check(rc == 1 && strstr(out, "unknown command: frobnicate") != NULL,
          "an unknown command is exit 1, and starts nothing");
}

/*
 * main — run the test, or be the server.
 *
 * Inputs:
 *   argc, argv — "--server" selects the stand-in app
 *
 * Output: 0 when every check held.
 */
int
main(int argc, char **argv)
{
    char data_dir[PATH_MAX];
    snprintf(data_dir, sizeof data_dir, "%s", check_data_dir());
    if (argc > 1 && strcmp(argv[1], "--server") == 0) {
        return server_main();
    }

    char self[PATH_MAX];
    if (realpath(argv[0], self) == NULL || getenv("NOTES_TEST_CLI") == NULL) {
        fprintf(stderr, "run this test with `make test`\n");
        return 2;
    }
    on_app_config_init();
    if (chdir(data_dir) != 0) {
        return 2;
    }

    /* Nobody listening: the client says so and the caller runs headless.   */
    gchar *reply = NULL;
    check(on_ipc_try_remote("quicknote", &reply) == ON_IPC_NO_SERVER && reply == NULL,
          "no socket file: ON_IPC_NO_SERVER");
    char    *argv_stats[] = { "notes", "stats", NULL };
    gboolean ran = TRUE;
    check(on_ipc_try_remote_run(2, argv_stats, NULL, &ran) == 0 && !ran,
          "no socket file: the data command is not run remotely");

    pid_t server = start_server(self);
    check(server > 0, "the stand-in app starts and listens");
    if (server <= 0) {
        return check_result();
    }
    test_wire_format();
    test_real_cli(data_dir);

    /* A second app on the same data directory must be told to stand down.  */
    static OnApp second;
    check(on_ipc_server_start(&second) == on_ipc_server_busy,
          "a second server on a served data directory is BUSY");

    /* A crash leaves the socket file: clients see no server, and the next
     * app clears it and listens.                                           */
    stop_server(server);
    struct stat sb;
    check(stat(ON_SOCKET_FILENAME, &sb) == 0, "a killed server leaves its socket file");
    check(on_ipc_try_remote("quicknote", &reply) == ON_IPC_NO_SERVER,
          "a dead socket file reads as no server");
    check(on_ipc_server_start(&second) == on_ipc_server_listening,
          "the next server clears the stale file and listens");
    check(on_ipc_server_start(&second) == on_ipc_server_listening, "starting twice is harmless");
    on_ipc_server_stop();
    check(stat(ON_SOCKET_FILENAME, &sb) != 0, "stopping removes the socket file");
    on_ipc_server_stop();

    /* An app that holds the lock but is not accepting (starting up, queue
     * full, or its socket could not be made) must make the tool REFUSE,
     * not run headless behind it.  The test plays that app by holding the
     * lock itself.                                                         */
    int lock = open(ON_LOCK_FILENAME, O_RDWR | O_CREAT, 0600);
    check(lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) == 0, "the test takes the app lock");
    char out[1024];
    int  rc = run_cli("folder add BehindTheApp", out, sizeof out);
    check(rc == 2 && strstr(out, "cannot reach the app") != NULL,
          "with the lock held and nothing accepting, the tool refuses to run headless");
    check(on_ipc_try_remote("quicknote", &reply) == ON_IPC_ERROR,
          "and a GUI command is an error, not 'no server'");
    g_free(reply);
    check(on_ipc_server_start(&second) == on_ipc_server_busy,
          "an app starting while the lock is held is told BUSY");
    close(lock);
    rc = run_cli("folder list", out, sizeof out);
    check(rc == 0 && strstr(out, "BehindTheApp") == NULL,
          "once the lock is free the tool runs headless, and the refused command never ran");

    /* A data directory too long for a socket address.                      */
    char long_dir[PATH_MAX];
    snprintf(long_dir, sizeof long_dir, "%s/%s/%s", data_dir,
             "a-deliberately-long-directory-name-to-exceed-the-socket-limit",
             "and-another-long-component-so-the-path-passes-104-bytes");
    check(strlen(long_dir) + strlen(ON_SOCKET_FILENAME) + 1 >
          sizeof ((struct sockaddr_un *)0)->sun_path,
          "the long data directory really exceeds sun_path");
    setenv(ON_DATA_DIR_ENV, long_dir, 1);
    char mkdir_cmd[PATH_MAX + 16];
    snprintf(mkdir_cmd, sizeof mkdir_cmd, "mkdir -p '%s'", long_dir);
    if (system(mkdir_cmd) != 0 || chdir(long_dir) != 0) {
        return 2;
    }
    server = start_server(self);
    check(server > 0, "a server starts on the long data directory");
    if (server > 0) {
        rc = run_cli("quicknote", out, sizeof out);
        check(rc == 0 && strcmp(out, "created note 1\n") == 0,
              "and the tool reaches it there");
        stop_server(server);
    }
    return check_result();
}
