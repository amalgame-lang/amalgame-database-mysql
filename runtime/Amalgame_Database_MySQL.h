/*
 * Amalgame Standard Library — Amalgame.Database.MySQL
 * Copyright (c) 2026 Bastien MOUGET
 * https://github.com/amalgame-lang/Amalgame
 *
 * MySQL / MariaDB binding — dynamic-linked to libmariadb (the C
 * connector that's drop-in compatible with libmysqlclient and the
 * de-facto standard on every modern distro). No vendored
 * implementation; the user binary links against the OS-provided
 * libmariadb.so / libmariadb.dylib / libmariadb.dll.
 *
 * Works against both MariaDB ≥ 10.x and MySQL ≥ 5.7. The wire
 * protocol is shared so the C client handles both transparently.
 *
 * Surface (v1):
 *   Open(host, port, user, password, database)
 *     / Close / IsOpen / LastError                lifecycle + diag
 *   Exec(sql)                                      DDL / INSERT / UPDATE / DELETE
 *   QueryAll(sql) -> List<List<string>>            SELECT all rows × cols
 *   Changes()                                      rows affected by last Exec
 *   LastInsertId()                                 AUTO_INCREMENT of last INSERT
 *   ServerVersion()                                "10.11.6-MariaDB", "8.0.35", …
 *
 * NULL cells in QueryAll materialise as the empty string —
 * callers that need to distinguish NULL from `""` should wait
 * for v2's parameter binding + typed accessors.
 *
 * Threading: AmalgameMySQL* is single-owner. Concurrent
 * Exec / QueryAll calls against the same handle from different
 * threads will interleave wire bytes — libmariadb is *not*
 * internally thread-safe for shared connections. Distinct
 * handles per thread are fine.
 *
 * Memory: MYSQL_RES* values returned by mysql_store_result are
 * mysql_free_result()'d as soon as we've copied the data into
 * AmalgameList* / code_string. MYSQL* lives for the lifetime of
 * the AmalgameMySQL* handle and is mysql_close()'d in Close().
 * GC finalizer registered so a leaked handle still releases the
 * connection eventually.
 *
 * Out of scope (v2):
 *   - mysql_stmt_* prepared statements + ? parameter binding
 *   - Typed column accessors (AsInt / AsBytes / AsTimestamp)
 *   - Async query mode (MYSQL_OPT_NONBLOCK)
 *   - LOAD DATA INFILE / streaming bulk loaders
 *   - SSL/TLS-specific control surface (libmariadb already
 *     honours MYSQL_OPT_SSL_* via env; explicit AM API in v2)
 *   - Multi-statement results (CLIENT_MULTI_STATEMENTS flag)
 */

#ifndef AMALGAME_DATABASE_MYSQL_H
#define AMALGAME_DATABASE_MYSQL_H

#include "_runtime.h"
#include "Amalgame_Collections.h"

/* Resolve mysql.h across the two common system layouts.
 * Debian / Ubuntu ship it under /usr/include/mariadb/.
 * Fedora / RHEL / Arch / macOS Homebrew / MSYS2 ship it under
 * /usr/include/ directly (or the equivalent prefix). */
#if defined(__has_include)
#  if __has_include(<mysql.h>)
#    include <mysql.h>
#  elif __has_include(<mariadb/mysql.h>)
#    include <mariadb/mysql.h>
#  elif __has_include(<mysql/mysql.h>)
#    include <mysql/mysql.h>
#  else
#    error "mysql.h not found. Install libmariadb-dev / libmariadb-devel / mariadb-connector-c."
#  endif
#else
#  /* Old compiler without __has_include — assume Debian layout. */
#  include <mariadb/mysql.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct AmalgameMySQL {
    MYSQL*   conn;         /* the libmariadb connection; NULL when closed */
    char*    last_error;   /* GC-strdup'd, or NULL */
    i64      last_changes; /* rows affected by the last Exec */
    i64      last_insert;  /* AUTO_INCREMENT of last INSERT */
} AmalgameMySQL;

/* ── Helpers ────────────────────────────────────────── */

static inline code_string _ammy_err_dup(const char* msg) {
    if (!msg) return NULL;
    size_t n = strlen(msg);
    char* p = (char*) code_alloc(n + 1);
    memcpy(p, msg, n + 1);
    return p;
}

/* libmariadb's mysql_error returns a static-or-conn-bound buffer;
 * we strdup it onto the GC heap so the AM-side string stays valid
 * past the next libmariadb call. */
static inline code_string _ammy_err_from_conn(MYSQL* c) {
    if (!c) return _ammy_err_dup("null connection");
    const char* raw = mysql_error(c);
    if (!raw || !*raw) return _ammy_err_dup("");
    return _ammy_err_dup(raw);
}

/* GC finalizer — runs mysql_close if the user dropped the handle
 * without calling Close. */
static void _ammy_finalize(void* obj, void* cd) {
    (void) cd;
    AmalgameMySQL* db = (AmalgameMySQL*) obj;
    if (db && db->conn) {
        mysql_close(db->conn);
        db->conn = NULL;
    }
}

static inline AmalgameMySQL* _ammy_alloc(void) {
    AmalgameMySQL* db =
        (AmalgameMySQL*) GC_MALLOC(sizeof(AmalgameMySQL));
    db->conn         = NULL;
    db->last_error   = NULL;
    db->last_changes = 0;
    db->last_insert  = 0;
    GC_register_finalizer(db, _ammy_finalize, NULL, NULL, NULL);
    return db;
}

/* ── Lifecycle ──────────────────────────────────────── */

static inline AmalgameMySQL* Amalgame_Database_MySQL_Open(
        code_string host, i64 port, code_string user,
        code_string password, code_string database) {
    AmalgameMySQL* db = _ammy_alloc();
    if (!host) {
        db->last_error = _ammy_err_dup("null host");
        return db;
    }
    MYSQL* c = mysql_init(NULL);
    if (!c) {
        db->last_error = _ammy_err_dup("mysql_init returned NULL");
        return db;
    }
    /* mysql_real_connect signature:
     *   MYSQL* mysql_real_connect(MYSQL*, host, user, passwd,
     *                              db, port, unix_socket, client_flag)
     * NULL passwd / db are accepted — defaults to empty. */
    MYSQL* res = mysql_real_connect(c, host,
                                     user ? user : (code_string) "",
                                     password ? password : (code_string) "",
                                     database ? database : (code_string) "",
                                     (unsigned int) port, NULL, 0);
    if (!res) {
        db->last_error = _ammy_err_from_conn(c);
        mysql_close(c);
        return db;
    }
    db->conn = c;
    return db;
}

static inline void Amalgame_Database_MySQL_Close(AmalgameMySQL* db) {
    if (!db || !db->conn) return;
    mysql_close(db->conn);
    db->conn = NULL;
}

static inline code_bool Amalgame_Database_MySQL_IsOpen(AmalgameMySQL* db) {
    return (db && db->conn) ? 1 : 0;
}

static inline code_string Amalgame_Database_MySQL_LastError(AmalgameMySQL* db) {
    if (!db || !db->last_error) return (code_string) "";
    return db->last_error;
}

/* ── Exec ───────────────────────────────────────────── */

/* Run a no-result SQL statement (DDL / INSERT / UPDATE / DELETE).
 * Returns true on mysql_query == 0. mysql_store_result is called
 * regardless to drain any rows the statement happened to produce
 * (CALL stored-procedure, for instance) and immediately freed.
 * Updates Changes() with mysql_affected_rows + LastInsertId()
 * with mysql_insert_id. */
static inline code_bool Amalgame_Database_MySQL_Exec(
        AmalgameMySQL* db, code_string sql) {
    if (!db || !db->conn) {
        if (db) db->last_error = _ammy_err_dup("connection not open");
        return 0;
    }
    if (!sql) {
        db->last_error = _ammy_err_dup("null sql");
        return 0;
    }
    if (mysql_query(db->conn, sql) != 0) {
        db->last_error = _ammy_err_from_conn(db->conn);
        return 0;
    }
    /* Drain any stray rows (e.g. CALL) so the connection isn't
     * left "in command" for the next query. */
    MYSQL_RES* r = mysql_store_result(db->conn);
    if (r) mysql_free_result(r);
    db->last_changes = (i64) mysql_affected_rows(db->conn);
    db->last_insert  = (i64) mysql_insert_id(db->conn);
    db->last_error   = _ammy_err_dup("");
    return 1;
}

/* ── QueryAll ───────────────────────────────────────── */

/* SELECT and return every row as a List<List<string>>. Each inner
 * list holds the row's columns stringified (libmariadb text mode).
 * NULL cells materialise as the empty string — callers that need
 * to distinguish NULL from "" should use prepared statements in v2.
 *
 * On error the outer list is empty and LastError is set. */
static inline AmalgameList* Amalgame_Database_MySQL_QueryAll(
        AmalgameMySQL* db, code_string sql) {
    AmalgameList* rows = AmalgameList_new();
    if (!db || !db->conn) {
        if (db) db->last_error = _ammy_err_dup("connection not open");
        return rows;
    }
    if (!sql) {
        db->last_error = _ammy_err_dup("null sql");
        return rows;
    }
    if (mysql_query(db->conn, sql) != 0) {
        db->last_error = _ammy_err_from_conn(db->conn);
        return rows;
    }
    MYSQL_RES* r = mysql_store_result(db->conn);
    if (!r) {
        /* Either a statement that returns no result set, or a
         * real error. mysql_field_count tells which: 0 == no
         * result set (success), nonzero with NULL res == error. */
        if (mysql_field_count(db->conn) != 0) {
            db->last_error = _ammy_err_from_conn(db->conn);
        } else {
            db->last_changes = (i64) mysql_affected_rows(db->conn);
            db->last_error = _ammy_err_dup("");
        }
        return rows;
    }

    unsigned int ncols = mysql_num_fields(r);
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(r)) != NULL) {
        unsigned long* lens = mysql_fetch_lengths(r);
        AmalgameList* one = AmalgameList_new();
        for (unsigned int j = 0; j < ncols; j++) {
            const char* v = row[j];
            size_t n = v ? (lens ? (size_t) lens[j] : strlen(v)) : 0;
            char* dup = (char*) code_alloc(n + 1);
            if (n > 0) memcpy(dup, v, n);
            dup[n] = '\0';
            AmalgameList_add(one, (void*) dup);
        }
        AmalgameList_add(rows, (void*) one);
    }
    db->last_changes = (i64) mysql_num_rows(r);
    mysql_free_result(r);
    db->last_error = _ammy_err_dup("");
    return rows;
}

/* Rows affected by the last Exec, or row count of the last QueryAll. */
static inline i64 Amalgame_Database_MySQL_Changes(AmalgameMySQL* db) {
    return db ? db->last_changes : 0;
}

/* AUTO_INCREMENT value generated by the last INSERT. */
static inline i64 Amalgame_Database_MySQL_LastInsertId(AmalgameMySQL* db) {
    return db ? db->last_insert : 0;
}

/* ────────────────────────────────────────────────────────────────
 * v0.3 surface — parameter binding + transactions.
 *
 * Placeholders are positional `?` (1-indexed in SQL, 0-indexed in
 * the AM list — same convention as SQLite/DuckDB; PostgreSQL uses
 * $1/$2 instead). Binding goes through the libmariadb prepared-
 * statement API (mysql_stmt_*); every value is bound as
 * MYSQL_TYPE_STRING. MySQL's type coercion converts to the
 * destination column type at server side.
 *
 * NULL list entries become SQL NULL via MYSQL_TYPE_NULL +
 * is_null=1.
 *
 * Arity mismatches surface explicitly as "param count mismatch:
 * got X, sql expects Y" via mysql_stmt_param_count — same shape
 * as the SQLite/DuckDB siblings.
 *
 * Transactions wrap BEGIN/COMMIT/ROLLBACK through Exec.
 * ──────────────────────────────────────────────────────────────── */

/* Internal: build the parallel arrays needed for mysql_stmt_bind_param.
 * Returns 1 on success, 0 on failure (with db->last_error set).
 * Caller frees binds/lengths/nulls. Strings are referenced in-place
 * (not copied) — caller keeps them alive until mysql_stmt_execute. */
static inline int _ammy_bind_params(
    AmalgameMySQL* db, MYSQL_STMT* stmt, AmalgameList* params,
    MYSQL_BIND** outBinds, unsigned long** outLens, char** outNulls)
{
    int n = params ? (int) AmalgameList_size(params) : 0;
    unsigned long pcount = mysql_stmt_param_count(stmt);
    if ((unsigned long) n != pcount) {
        char buf[128];
        snprintf(buf, sizeof(buf),
            "param count mismatch: got %d, sql expects %lu", n, pcount);
        db->last_error = _ammy_err_dup(buf);
        return 0;
    }
    if (n == 0) {
        *outBinds = NULL; *outLens = NULL; *outNulls = NULL;
        return 1;
    }
    MYSQL_BIND*    binds = (MYSQL_BIND*)    calloc((size_t)n, sizeof(MYSQL_BIND));
    unsigned long* lens  = (unsigned long*) calloc((size_t)n, sizeof(unsigned long));
    char*          nulls = (char*)          calloc((size_t)n, sizeof(char));
    for (int i = 0; i < n; i++) {
        code_string v = (code_string) AmalgameList_get(params, i);
        if (v) {
            lens[i]              = (unsigned long) strlen(v);
            nulls[i]             = 0;
            binds[i].buffer_type = MYSQL_TYPE_STRING;
            binds[i].buffer      = (char*) v;
            binds[i].buffer_length = lens[i];
            binds[i].length      = &lens[i];
            binds[i].is_null     = (bool*) &nulls[i];
        } else {
            nulls[i]             = 1;
            binds[i].buffer_type = MYSQL_TYPE_NULL;
            binds[i].is_null     = (bool*) &nulls[i];
        }
    }
    *outBinds = binds; *outLens = lens; *outNulls = nulls;
    return 1;
}

static inline code_bool Amalgame_Database_MySQL_ExecBind(
        AmalgameMySQL* db, code_string sql, AmalgameList* params)
{
    if (!db || !db->conn) {
        if (db) db->last_error = _ammy_err_dup("connection not open");
        return 0;
    }
    if (!sql) { db->last_error = _ammy_err_dup("null sql"); return 0; }
    MYSQL_STMT* stmt = mysql_stmt_init(db->conn);
    if (!stmt) {
        db->last_error = _ammy_err_from_conn(db->conn);
        return 0;
    }
    if (mysql_stmt_prepare(stmt, sql, (unsigned long) strlen(sql)) != 0) {
        db->last_error = _ammy_err_dup(mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return 0;
    }
    MYSQL_BIND* binds = NULL;
    unsigned long* lens = NULL;
    char* nulls = NULL;
    if (!_ammy_bind_params(db, stmt, params, &binds, &lens, &nulls)) {
        mysql_stmt_close(stmt);
        return 0;
    }
    if (binds && mysql_stmt_bind_param(stmt, binds) != 0) {
        db->last_error = _ammy_err_dup(mysql_stmt_error(stmt));
        free(binds); free(lens); free(nulls);
        mysql_stmt_close(stmt);
        return 0;
    }
    if (mysql_stmt_execute(stmt) != 0) {
        db->last_error = _ammy_err_dup(mysql_stmt_error(stmt));
        free(binds); free(lens); free(nulls);
        mysql_stmt_close(stmt);
        return 0;
    }
    db->last_changes = (i64) mysql_stmt_affected_rows(stmt);
    db->last_insert  = (i64) mysql_stmt_insert_id(stmt);
    free(binds); free(lens); free(nulls);
    mysql_stmt_close(stmt);
    db->last_error = _ammy_err_dup("");
    return 1;
}

static inline AmalgameList* Amalgame_Database_MySQL_QueryBindAll(
        AmalgameMySQL* db, code_string sql, AmalgameList* params)
{
    AmalgameList* rows = AmalgameList_new();
    if (!db || !db->conn) {
        if (db) db->last_error = _ammy_err_dup("connection not open");
        return rows;
    }
    if (!sql) { db->last_error = _ammy_err_dup("null sql"); return rows; }
    MYSQL_STMT* stmt = mysql_stmt_init(db->conn);
    if (!stmt) { db->last_error = _ammy_err_from_conn(db->conn); return rows; }
    if (mysql_stmt_prepare(stmt, sql, (unsigned long) strlen(sql)) != 0) {
        db->last_error = _ammy_err_dup(mysql_stmt_error(stmt));
        mysql_stmt_close(stmt); return rows;
    }
    MYSQL_BIND* pbinds = NULL;
    unsigned long* plens = NULL;
    char* pnulls = NULL;
    if (!_ammy_bind_params(db, stmt, params, &pbinds, &plens, &pnulls)) {
        mysql_stmt_close(stmt); return rows;
    }
    if (pbinds && mysql_stmt_bind_param(stmt, pbinds) != 0) {
        db->last_error = _ammy_err_dup(mysql_stmt_error(stmt));
        free(pbinds); free(plens); free(pnulls);
        mysql_stmt_close(stmt); return rows;
    }
    /* Force the server to compute max_length for every column so
     * we can size our text buffers exactly. Without this we'd hit
     * MYSQL_DATA_TRUNCATED on long values. */
    bool attr_true = 1;
    mysql_stmt_attr_set(stmt, STMT_ATTR_UPDATE_MAX_LENGTH, &attr_true);
    if (mysql_stmt_execute(stmt) != 0) {
        db->last_error = _ammy_err_dup(mysql_stmt_error(stmt));
        free(pbinds); free(plens); free(pnulls);
        mysql_stmt_close(stmt); return rows;
    }
    MYSQL_RES* meta = mysql_stmt_result_metadata(stmt);
    if (!meta) {
        /* No result set (statement was DML masquerading as SELECT?).
         * Treat as empty rows; success. */
        free(pbinds); free(plens); free(pnulls);
        mysql_stmt_close(stmt);
        db->last_error = _ammy_err_dup("");
        return rows;
    }
    if (mysql_stmt_store_result(stmt) != 0) {
        db->last_error = _ammy_err_dup(mysql_stmt_error(stmt));
        mysql_free_result(meta);
        free(pbinds); free(plens); free(pnulls);
        mysql_stmt_close(stmt); return rows;
    }
    unsigned int ncols = mysql_num_fields(meta);
    MYSQL_FIELD* fields = mysql_fetch_fields(meta);
    MYSQL_BIND*    rbinds = (MYSQL_BIND*)    calloc(ncols, sizeof(MYSQL_BIND));
    unsigned long* rlens  = (unsigned long*) calloc(ncols, sizeof(unsigned long));
    char*          rnulls = (char*)          calloc(ncols, sizeof(char));
    char**         rbufs  = (char**)         calloc(ncols, sizeof(char*));
    for (unsigned int j = 0; j < ncols; j++) {
        unsigned long cap = fields[j].max_length + 1;
        if (cap < 16) cap = 16;
        rbufs[j] = (char*) malloc(cap);
        rbinds[j].buffer_type   = MYSQL_TYPE_STRING;
        rbinds[j].buffer        = rbufs[j];
        rbinds[j].buffer_length = cap;
        rbinds[j].length        = &rlens[j];
        rbinds[j].is_null       = (bool*) &rnulls[j];
    }
    if (mysql_stmt_bind_result(stmt, rbinds) != 0) {
        db->last_error = _ammy_err_dup(mysql_stmt_error(stmt));
        for (unsigned int j = 0; j < ncols; j++) free(rbufs[j]);
        free(rbinds); free(rlens); free(rnulls); free(rbufs);
        mysql_free_result(meta);
        free(pbinds); free(plens); free(pnulls);
        mysql_stmt_close(stmt); return rows;
    }
    int rc;
    while ((rc = mysql_stmt_fetch(stmt)) == 0) {
        AmalgameList* row = AmalgameList_new();
        for (unsigned int j = 0; j < ncols; j++) {
            size_t n = rnulls[j] ? 0 : (size_t) rlens[j];
            char* dup = (char*) code_alloc(n + 1);
            if (n > 0) memcpy(dup, rbufs[j], n);
            dup[n] = '\0';
            AmalgameList_add(row, (void*) dup);
        }
        AmalgameList_add(rows, (void*) row);
    }
    db->last_changes = (i64) AmalgameList_size(rows);
    for (unsigned int j = 0; j < ncols; j++) free(rbufs[j]);
    free(rbinds); free(rlens); free(rnulls); free(rbufs);
    mysql_free_result(meta);
    free(pbinds); free(plens); free(pnulls);
    mysql_stmt_close(stmt);
    db->last_error = _ammy_err_dup("");
    return rows;
}

static inline code_bool Amalgame_Database_MySQL_Begin(AmalgameMySQL* db) {
    return Amalgame_Database_MySQL_Exec(db, "START TRANSACTION");
}

static inline code_bool Amalgame_Database_MySQL_Commit(AmalgameMySQL* db) {
    return Amalgame_Database_MySQL_Exec(db, "COMMIT");
}

static inline code_bool Amalgame_Database_MySQL_Rollback(AmalgameMySQL* db) {
    return Amalgame_Database_MySQL_Exec(db, "ROLLBACK");
}

/* libmariadb's mysql_get_server_info returns a version string
 * like "10.11.6-MariaDB-1:10.11.6+maria~deb12" or "8.0.35". */
static inline code_string Amalgame_Database_MySQL_ServerVersion(AmalgameMySQL* db) {
    if (!db || !db->conn) return (code_string) "";
    const char* v = mysql_get_server_info(db->conn);
    if (!v || !*v) return (code_string) "";
    return _ammy_err_dup(v);
}

#endif /* AMALGAME_DATABASE_MYSQL_H */
