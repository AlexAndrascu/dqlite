#include <assert.h>
#include <stddef.h>

#include <sqlite3.h>

#include "../include/dqlite.h"

#include "db.h"
#include "lifecycle.h"
#include "registry.h"
#include "stmt.h"
#include "vfs.h"

/* Wrapper around sqlite3_exec that frees the memory allocated for the error
 * message in case of failure and sets the dqlite__db's error field
 * appropriately */
static int dqlite__db_exec(struct dqlite__db *db, const char *sql) {
	char *msg;
	int   rc;

	assert(db != NULL);

	rc = sqlite3_exec(db->db, sql, NULL, NULL, &msg);
	if (rc != SQLITE_OK) {

		assert(msg != NULL);
		sqlite3_free(msg);
		dqlite__error_printf(&db->error, sqlite3_errmsg(db->db));

		return rc;
	}

	return SQLITE_OK;
}

void dqlite__db_init(struct dqlite__db *db) {
	assert(db != NULL);

	dqlite__lifecycle_init(DQLITE__LIFECYCLE_DB);
	dqlite__error_init(&db->error);
	dqlite__stmt_registry_init(&db->stmts);

	db->in_a_tx = 0;
}

void dqlite__db_close(struct dqlite__db *db) {
	int rc;

	assert(db != NULL);

	dqlite__stmt_registry_close(&db->stmts);
	dqlite__error_close(&db->error);

	if (db->db != NULL) {
		rc = sqlite3_close(db->db);

		/* Since we cleanup all existing db resources, SQLite should
		 * never fail, according to the docs. */
		assert(rc == SQLITE_OK);
	}

	dqlite__lifecycle_close(DQLITE__LIFECYCLE_DB);
}

int dqlite__db_open(struct dqlite__db *db,
                    const char *       name,
                    int                flags,
                    const char *       replication) {
	const char *vfs;
	int         rc;

	assert(db != NULL);
	assert(name != NULL);
	assert(replication != NULL);

	/* The VFS registration name must match the one of the replication
	 * implementation. */
	vfs = replication;

	/* TODO: do some validation of the name (e.g. can't begin with a slash) */
	rc = sqlite3_open_v2(name, &db->db, flags, vfs);
	if (rc != SQLITE_OK) {
		dqlite__error_printf(&db->error, sqlite3_errmsg(db->db));
		return rc;
	}

	/* Enable extended result codes */
	rc = sqlite3_extended_result_codes(db->db, 1);
	if (rc != SQLITE_OK) {
		dqlite__error_printf(&db->error, sqlite3_errmsg(db->db));
		return rc;
	}

	/* Set the page size. TODO: make page size configurable? */
	rc = dqlite__db_exec(db, "PRAGMA page_size=4096");
	if (rc != SQLITE_OK) {
		dqlite__error_wrapf(
		    &db->error, &db->error, "unable to set page size");
		return rc;
	}

	/* Disable syncs. */
	rc = dqlite__db_exec(db, "PRAGMA synchronous=OFF");
	if (rc != SQLITE_OK) {
		dqlite__error_wrapf(
		    &db->error, &db->error, "unable to switch off syncs");
		return rc;
	}

	/* Set WAL journaling. */
	rc = dqlite__db_exec(db, "PRAGMA journal_mode=WAL");
	if (rc != SQLITE_OK) {
		dqlite__error_wrapf(
		    &db->error, &db->error, "unable to set WAL mode: %s");
		return rc;
	}

	/* Set WAL replication. */
	rc = sqlite3_wal_replication_leader(
	    db->db, "main", replication, (void *)db->db);

	if (rc != SQLITE_OK) {
		dqlite__error_printf(&db->error, "unable to set WAL replication");
		return rc;
	}

	/* TODO: make setting foreign keys optional. */
	rc = dqlite__db_exec(db, "PRAGMA foreign_keys=1");
	if (rc != SQLITE_OK) {
		dqlite__error_wrapf(
		    &db->error, &db->error, "unable to set foreign keys checks: %s");
		return rc;
	}

	return SQLITE_OK;
}

int dqlite__db_prepare(struct dqlite__db *   db,
                       const char *          sql,
                       struct dqlite__stmt **stmt) {
	int err;
	int rc;

	assert(db != NULL);
	assert(db->db != NULL);

	assert(sql != NULL);

	err = dqlite__stmt_registry_add(&db->stmts, stmt);
	if (err != 0) {
		assert(err == DQLITE_NOMEM);
		dqlite__error_oom(&db->error, "unable to register statement");
		return SQLITE_NOMEM;
	}

	assert(stmt != NULL);

	(*stmt)->db = db->db;

	rc = sqlite3_prepare_v2(db->db, sql, -1, &(*stmt)->stmt, &(*stmt)->tail);
	if (rc != SQLITE_OK) {
		dqlite__error_printf(&db->error, sqlite3_errmsg(db->db));
		dqlite__stmt_registry_del(&db->stmts, *stmt);
		return rc;
	}

	return SQLITE_OK;
}

/* Lookup a stmt object by ID */
struct dqlite__stmt *dqlite__db_stmt(struct dqlite__db *db, uint32_t stmt_id) {
	return dqlite__stmt_registry_get(&db->stmts, stmt_id);
}

int dqlite__db_finalize(struct dqlite__db *db, struct dqlite__stmt *stmt) {
	int rc;
	int err;

	assert(db != NULL);
	assert(stmt != NULL);

	if (stmt->stmt != NULL) {
		rc = sqlite3_finalize(stmt->stmt);
		if (rc != SQLITE_OK) {
			dqlite__error_printf(&db->error, sqlite3_errmsg(db->db));
		}

		/* Unset the stmt member, to prevent dqlite__stmt_registry_del from
		 * trying to finalize the statement too */
		stmt->stmt = NULL;
	} else {
		rc = SQLITE_OK;
	}

	err = dqlite__stmt_registry_del(&db->stmts, stmt);

	/* Deleting the statement from the registry can't fail, because the
	 * given statement was obtained with dqlite__db_stmt(). */
	assert(err == 0);

	return rc;
}

/* Helper to to update the transaction refcount on the in-memory file object
 * associated with the db. */
static void dqlite__db_update_tx_refcount(struct dqlite__db *db, int delta) {
	struct dqlite__vfs_file *file;
	int                      rc;

	rc = sqlite3_file_control(db->db, "main", SQLITE_FCNTL_FILE_POINTER, &file);
	assert(rc == SQLITE_OK); /* Should never fail */

	file->content->tx_refcount += delta;
}

int dqlite__db_begin(struct dqlite__db *db) {
	int rc;

	assert(db != NULL);

	rc = dqlite__db_exec(db, "BEGIN");
	if (rc != SQLITE_OK) {
		return rc;
	}

	/* SQLite doesn't allow to start a transaction twice in the same
	 * connection, so our in_a_tx flag should be false. */
	assert(db->in_a_tx == 0);

	db->in_a_tx = 1;

	dqlite__db_update_tx_refcount(db, 1);

	return SQLITE_OK;
}

int dqlite__db_commit(struct dqlite__db *db) {
	int rc;

	assert(db != NULL);

	rc = dqlite__db_exec(db, "COMMIT");
	if (rc != SQLITE_OK) {
		/* Since we're in single-thread mode, contention should never
		 * happen. */
		assert(rc != SQLITE_BUSY);

		return rc;
	}

	/* SQLite doesn't allow a commit to succeed if a transaction isn't
	 * started, so our in_a_tx flags should be true. */
	assert(db->in_a_tx == 1);

	db->in_a_tx = 0;

	dqlite__db_update_tx_refcount(db, -1);

	return SQLITE_OK;
}

int dqlite__db_rollback(struct dqlite__db *db) {
	int rc;

	assert(db != NULL);

	rc = dqlite__db_exec(db, "ROLLBACK");

	/* TODO: what are the failure modes of a ROLLBACK statement? is it
	 * possible that it leaves a transaction open?. */
	db->in_a_tx = 0;

	dqlite__db_update_tx_refcount(db, -1);

	if (rc != SQLITE_OK) {
		return rc;
	}

	return SQLITE_OK;
}

DQLITE__REGISTRY_METHODS(dqlite__db_registry, dqlite__db);
