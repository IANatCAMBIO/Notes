/* ===========================================================================
 * migrate_v5.h — the one-time migration to PRAGMA user_version 5: action
 * items become task rows, and every '!' line's uid moves into its note
 * blob (BNBF v6, the ACTION record).
 *
 * TEMPORARY.  This module exists to carry the one live database across
 * the change and is removed once it has (CLAUDE.md, "Data & formats").
 * It is deliberately self-contained — its own SQL over db->handle for
 * what db.c does not export — so that its deletion leaves nothing behind.
 * =========================================================================== */

#ifndef ON_MIGRATE_V5_H
#define ON_MIGRATE_V5_H

#include "db.h"

/* ---------------------------------------------------------------------------
 * on_migrate_v5_run() — bring `db` to user_version 5 if it is below it.
 * Runs once per database, at every open (GUI start, CLI, File → Open
 * Database File…), and is a no-op at 5 or above.
 *
 *   1. VACUUM INTO "<path>.pre-v5.bak" — a verified-consistent copy of the
 *      file as it was, unless one is already there (an earlier attempt
 *      that rolled back left the file untouched, so its backup stands).
 *   2. One BEGIN IMMEDIATE transaction for everything below; any failure
 *      rolls it back, the version stays where it was, and FALSE is
 *      returned — the caller refuses to run on the file.
 *   3. The old action_items rows are read (a database from before the uid
 *      column has none worth keeping and is treated as empty).
 *   4. Every note (trashed included) is loaded through the block model
 *      (nothing decoded); one with no real action line is left as it is.
 *      Otherwise each action line is matched to an old row — identical
 *      text first, then the same ord, else a fresh uid (the last home of
 *      the old carry heuristic; the table's ord can be stale for a note
 *      unsaved since 2026-09-16) — its task is created from the LINE's
 *      text / done / due, the uid goes onto the block, and the blob is
 *      re-saved: a content-only UPDATE, updated_at untouched.  A note
 *      whose blob round-trips byte-identically gets exactly its old bytes
 *      plus the ACTION records and the v6 header; one the loader
 *      normalizes gets the bytes its next save would write anyway (counted
 *      and logged by id).
 *      The managed "Action Items" list comes into being with the first
 *      task, as it does at run time.
 *   5. DROP TABLE action_items.
 *   6. user_version 5, COMMIT, and a summary line in the log.
 * ------------------------------------------------------------------------- */
gboolean on_migrate_v5_run(OnDatabase *db);

#endif /* ON_MIGRATE_V5_H */
