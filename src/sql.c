#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sql.h"

/* Define queries */
static const char yubisql_select_data[] = "SELECT publicid,privateid,key FROM mapping WHERE username = \"%.*s\";\n";
static const char yubisql_select_state[] = "SELECT session,timecode,tokencount FROM mapping WHERE username = \"%.*s\";\n";
static const char yubisql_update_state[] = "UPDATE mapping SET session = %hu, timecode = %u, tokencount = %hhu WHERE username = \"%.*s\";\n";

/* Transactions */
static const char yubisql_begin[] = "BEGIN IMMEDIATE;";
static const char yubisql_end[] = "COMMIT;";
static const char yubisql_rollback[] = "ROLLBACK;";

sqlite3*
init(const char* dbname)
{
  sqlite3 *ppDb = NULL;

  if (sqlite3_open(dbname, &ppDb) != SQLITE_OK) {
    sqlite3_close(ppDb);
    return NULL;
  }
  return ppDb;
}

void
sql_close(sqlite3* db)
{
  sqlite3_close(db);
}

static void
rollback_r(sqlite3* db, int rec)
{
  int response;
  sqlite3_stmt *ppStmt = NULL;

  /* Prepare the query */
  response = sqlite3_prepare(db, yubisql_rollback, sizeof(yubisql_rollback) - 1, &ppStmt, NULL);
  if (response != SQLITE_OK) {
    /* Should never ever happen */
    sqlite3_finalize(ppStmt);
    return;
  }

  /* Run the query, and clean it immediately */
  response = sqlite3_step(ppStmt);
  sqlite3_finalize(ppStmt);

  /* If we didn't achieved to rollback, let's try another time */
  if ((response != SQLITE_OK)
      && (!rec)) {
    rollback_r(db, 1);
  }
}

void
rollback(sqlite3* db)
{
  rollback_r(db,0);
}

struct otp_data*
get_otp_data (sqlite3* db, const struct user* user)
{
  size_t len;
  int ilen;
  char *request;
  const unsigned char *ret;
  int response;
  sqlite3_stmt *ppStmt = NULL;
  struct otp_data *data;

  /* Format the request */
  len = sizeof(yubisql_select_data) + user->len;
  request = malloc(len);
  if (request == NULL) {
    return NULL;
  }
  ilen = snprintf(request, len, yubisql_select_data, user->len, user->name);

  /* Prepare the request ! */
  response = sqlite3_prepare(db, request, ilen, &ppStmt, NULL);
  free(request);
  if (response != SQLITE_OK) {
    sqlite3_finalize(ppStmt);
    return NULL;
  }

  /* Run it and verify the format of the response */
  response = sqlite3_step(ppStmt);
  if ((response != SQLITE_ROW)
      || (sqlite3_column_count(ppStmt) != 3)
      || (sqlite3_column_type(ppStmt, 0) != SQLITE_TEXT)
      || (sqlite3_column_type(ppStmt, 1) != SQLITE_TEXT)
      || (sqlite3_column_type(ppStmt, 2) != SQLITE_TEXT)) {
    sqlite3_finalize(ppStmt);
    return NULL;
  }

  /* Allocate the result struct */
  data = malloc(sizeof(struct otp_data));
  if (data == NULL) {
    sqlite3_finalize(ppStmt);
    return NULL;
  }

  /* Extract and copy each data */
  /* Public ID */
  ret = sqlite3_column_text(ppStmt,0);
  ilen = sqlite3_column_bytes(ppStmt,0);
  if (ilen == OTP_PUB_ID_HEX_LEN) {
    memcpy(data->pubid, ret, OTP_PUB_ID_HEX_LEN);
  } else {
    sqlite3_finalize(ppStmt);
    free(data);
    return NULL;
  }
  /* Private ID */
  ret = sqlite3_column_text(ppStmt,1);
  ilen = sqlite3_column_bytes(ppStmt,1);
  if (ilen == OTP_PRIVID_HEX_LEN) {
    memcpy(data->privid, ret, OTP_PRIVID_HEX_LEN);
  } else {
    sqlite3_finalize(ppStmt);
    free(data);
    return NULL;
  }
  /* AES key */
  ret = sqlite3_column_text(ppStmt,2);
  ilen = sqlite3_column_bytes(ppStmt,2);
  if (ilen == OTP_KEY_HEX_LEN) {
    memcpy(data->key, ret, OTP_KEY_HEX_LEN);
  } else {
    sqlite3_finalize(ppStmt);
    free(data);
    return NULL;
  }

  /* If there is more data, we failed */
  if (sqlite3_step(ppStmt) == SQLITE_DONE) {
    sqlite3_finalize(ppStmt);
    return data;
  } else {
    sqlite3_finalize(ppStmt);
    free(data);
    return NULL;
  }
}

int
try_get_credentials(sqlite3* db, struct otp_state* store, const struct user* user)
{
  size_t len;
  int   ilen;
  char *request;
  int response;
  sqlite3_stmt *ppStmt = NULL;

  /* Begin transalation */

  /* Prepare the request */
  response = sqlite3_prepare(db, yubisql_begin, sizeof(yubisql_begin) - 1, &ppStmt, NULL);
  if (response != SQLITE_OK) {
    /* Should never ever happen */
    sqlite3_finalize(ppStmt);
    return OTP_SQL_ERR;
  }

  /* Run and verify response */
  response = sqlite3_step(ppStmt);
  sqlite3_finalize(ppStmt);
  switch (response) {
    case SQLITE_DONE:
      break;
    case SQLITE_BUSY:
      return OTP_SQL_MAY_RETRY;
    default:
      return OTP_SQL_ERR;
  }

  /* Obtain state */

  /* Prepare the request */
  len = sizeof(yubisql_select_state) + user->len;
  request = malloc(len);
  if (request == NULL) {
    return OTP_SQL_MALLOC_ERR;
  }
  ilen = snprintf(request, len, yubisql_select_state, user->len, user->name);
  response = sqlite3_prepare(db, request, ilen, &ppStmt, NULL);
  free(request);
  if (response != SQLITE_OK) {
    /* Should never ever happen */
    sqlite3_finalize(ppStmt);
    return OTP_SQL_ERR;
  }

  /* Run and verify response */
  response = sqlite3_step(ppStmt);
  switch (response) {
    case SQLITE_ROW:
      break;
    case SQLITE_BUSY:
      sqlite3_finalize(ppStmt);
      rollback(db);
      return OTP_SQL_MAY_RETRY;
    default:
      sqlite3_finalize(ppStmt);
      rollback(db);
      return OTP_SQL_ERR;
  }

  if ((sqlite3_column_count(ppStmt) != 3)
      || (sqlite3_column_type(ppStmt,0) != SQLITE_INTEGER)
      || (sqlite3_column_type(ppStmt,1) != SQLITE_INTEGER)
      || (sqlite3_column_type(ppStmt,2) != SQLITE_INTEGER)) {
    sqlite3_finalize(ppStmt);
    return OTP_SQL_ERR;
  }

  store->session_counter = (unsigned short) sqlite3_column_int(ppStmt, 0);
  store->timecode = (unsigned int) sqlite3_column_int(ppStmt, 1);
  store->token_count = (unsigned char) sqlite3_column_int(ppStmt, 2);

  /* Verify that it's the only response */
  response = sqlite3_step(ppStmt);
  sqlite3_finalize(ppStmt);
  switch (response) {
    case SQLITE_DONE:
      return OTP_SQL_OK;
    case SQLITE_BUSY:
      rollback(db);
      return OTP_SQL_MAY_RETRY;
    default:
      rollback(db);
      return OTP_SQL_ERR;
  }
}

int
try_update_credentials(sqlite3* db, const struct otp_state* otp, const struct user* user)
{
  size_t len;
  int   ilen;
  char *request;
  int response;
  sqlite3_stmt *ppStmt = NULL;

  /* Update the state */

  /* Format the request */
  /* Request + %hu + %u + %hhu + user->len */
  len = sizeof(yubisql_update_state) + 5 + 20 + 3 + user->len;
  request = malloc(len);
  ilen = snprintf(request, len, yubisql_update_state, otp->session_counter, otp->timecode, otp->token_count, user->len, user->name);

  /* Run the statement */
  response = sqlite3_prepare(db, request, ilen, &ppStmt, NULL);
  if (response != SQLITE_OK) {
    /* Should never ever happen */
    sqlite3_finalize(ppStmt);
    return OTP_SQL_ERR;
  }

  /* Verify that it's ok */
  response = sqlite3_step(ppStmt);
  sqlite3_finalize(ppStmt);
  switch (response) {
    case SQLITE_DONE:
      break;
    case SQLITE_BUSY:
      free(request);
      rollback(db);
      return OTP_SQL_MAY_RETRY;
    default:
      free(request);
      rollback(db);
      return OTP_SQL_ERR;
  }

  /* Close the translation */

  /* Prepare the request */
  response = sqlite3_prepare(db, yubisql_end, sizeof(yubisql_end) - 1, &ppStmt, NULL);
  if (response != SQLITE_OK) {
    /* Should never ever happen */
    sqlite3_finalize(ppStmt);
    rollback(db);
    return OTP_SQL_ERR;
  }

  /* Verify that it's ok*/
  response = sqlite3_step(ppStmt);
  sqlite3_finalize(ppStmt);
  switch (response) {
    case SQLITE_DONE:
      return OTP_SQL_OK;
    case SQLITE_BUSY:
      rollback(db);
      return OTP_SQL_MAY_RETRY;
    default:
      rollback(db);
      return OTP_SQL_ERR;
  }
}
