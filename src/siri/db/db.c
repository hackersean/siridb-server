/*
 * db.c - contains functions  and constants for a SiriDB database.
 *
 * author       : Jeroen van der Heijden
 * email        : jeroen@transceptor.technology
 * copyright    : 2016, Transceptor Technology
 *
 * changes
 *  - initial version, 10-03-2016
 *
 */
#include <siri/db/db.h>
#include <logger/logger.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <siri/db/time.h>
#include <siri/db/users.h>
#include <uuid/uuid.h>
#include <siri/db/series.h>
#include <siri/db/shard.h>
#include <siri/db/servers.h>
#include <math.h>
#include <procinfo/procinfo.h>

#define SIRIDB_SHEMA 1

static siridb_t * SIRIDB_new(void);
static void SIRIDB_free(siridb_t * siridb);

#define READ_DB_EXIT_WITH_ERROR(ERROR_MSG)  \
{                                           \
    sprintf(err_msg, "error: " ERROR_MSG);  \
    SIRIDB_free(*siridb);                   \
    qp_object_free(qp_obj);                 \
    return 1;                               \
}

int siridb_from_unpacker(
        qp_unpacker_t * unpacker,
        siridb_t ** siridb,
        char * err_msg)
{
    qp_obj_t * qp_obj = qp_object_new();

    if (!qp_is_array(qp_next(unpacker, NULL)) ||
            qp_next(unpacker, qp_obj) != QP_INT64)
    {
        sprintf(err_msg, "error: corrupted database file.");
        qp_object_free(qp_obj);
        return 1;
    }

    /* check schema */
    if (qp_obj->via->int64 != SIRIDB_SHEMA)
    {
        sprintf(err_msg, "error: unsupported schema found: %ld",
                qp_obj->via->int64);
        qp_object_free(qp_obj);
        return 1;
    }

    /* create a new SiriDB structure */
    *siridb = SIRIDB_new();

    /* read uuid */
    if (qp_next(unpacker, qp_obj) != QP_RAW ||
            qp_obj->len != 16)
        READ_DB_EXIT_WITH_ERROR("cannot read uuid.")

    /* copy uuid */
    memcpy(&(*siridb)->uuid, qp_obj->via->raw, qp_obj->len);

    /* read database name */
    if (qp_next(unpacker, qp_obj) != QP_RAW ||
            qp_obj->len >= SIRIDB_MAX_DBNAME_LEN)
        READ_DB_EXIT_WITH_ERROR("cannot read database name.")

    /* alloc mem for database name */
    (*siridb)->dbname = (char *) malloc(qp_obj->len + 1);

    /* copy datatabase name */
    memcpy((*siridb)->dbname, qp_obj->via->raw, qp_obj->len);

    /* set terminator */
    (*siridb)->dbname[qp_obj->len] = 0;

    /* read time precision */
    if (qp_next(unpacker, qp_obj) != QP_INT64 ||
            qp_obj->via->int64 < SIRIDB_TIME_SECONDS ||
            qp_obj->via->int64 > SIRIDB_TIME_NANOSECONDS)
        READ_DB_EXIT_WITH_ERROR("cannot read time-precision.")

    /* bind time precision to SiriDB */
    (*siridb)->time =
            siridb_new_time((siridb_timep_t) qp_obj->via->int64);

    /* read buffer size */
    if (qp_next(unpacker, qp_obj) != QP_INT64)
        READ_DB_EXIT_WITH_ERROR("cannot read buffer size.")

    /* bind buffer size and len to SiriDB */
    (*siridb)->buffer_size = (size_t) qp_obj->via->int64;
    (*siridb)->buffer_len = (*siridb)->buffer_size / sizeof(siridb_point_t);

    /* read number duration  */
    if (qp_next(unpacker, qp_obj) != QP_INT64)
        READ_DB_EXIT_WITH_ERROR("cannot read number duration.")

    /* bind number duration to SiriDB */
    (*siridb)->duration_num = (uint64_t) qp_obj->via->int64;

    /* calculate 'shard_mask_num' based on number duration */
    (*siridb)->shard_mask_num =
            (uint16_t) sqrt((double) siridb_time_in_seconds(
                    *siridb, (*siridb)->duration_num)) / 24;

    /* read log duration  */
    if (qp_next(unpacker, qp_obj) != QP_INT64)
        READ_DB_EXIT_WITH_ERROR("cannot read log duration.")

    /* bind log duration to SiriDB */
    (*siridb)->duration_log = (uint64_t) qp_obj->via->int64;

    /* calculate 'shard_mask_log' based on log duration */
    (*siridb)->shard_mask_log =
            (uint16_t) sqrt((double) siridb_time_in_seconds(
                    *siridb, (*siridb)->duration_log)) / 24;

    log_debug("Set number duration mask to %d", (*siridb)->shard_mask_num);
    log_debug("Set log duration mask to %d", (*siridb)->shard_mask_log);

    /* read timezone */
    if (qp_next(unpacker, qp_obj) != QP_RAW)
        READ_DB_EXIT_WITH_ERROR("cannot read timezone.")

    /* bind timezone to SiriDB */
    char tzname[qp_obj->len + 1];
    memcpy(tzname, qp_obj->via->raw, qp_obj->len);
    tzname[qp_obj->len] = 0;
    if (((*siridb)->tz = iso8601_tz(tzname)) < 0)
    {
        log_critical("Unknown timezone found: '%s'.", tzname);
        READ_DB_EXIT_WITH_ERROR("cannot read timezone.");
    }

    /* free qp_object */
    qp_object_free(qp_obj);

    return 0;
}

siridb_t * siridb_get(llist_t * siridb_list, const char * dbname)
{
    llist_node_t * node = siridb_list->first;
    siridb_t * siridb;

    while (node != NULL)
    {
        siridb = (siridb_t *) node->data;
        if (strcmp(siridb->dbname, dbname) == 0)
        {
            return siridb;
        }
        node = node->next;
    }

    return NULL;
}

void siridb_free_cb(siridb_t * siridb, void * args)
{
    siridb_decref(siridb);
}

inline void siridb_incref(siridb_t * siridb)
{
    siridb->ref++;
}

void siridb_decref(siridb_t * siridb)
{
    if (!--siridb->ref)
    {
        SIRIDB_free(siridb);
    }
}

int siridb_open_files(siridb_t * siridb)
{
    int open_files = procinfo_open_files(siridb->dbpath);

    if (    siridb->buffer_path != siridb->dbpath &&
            strncmp(
                siridb->dbpath,
                siridb->buffer_path,
                strlen(siridb->dbpath)))
    {
        open_files += procinfo_open_files(siridb->buffer_path);
    }

    return open_files;
}

static siridb_t * SIRIDB_new(void)
{
    siridb_t * siridb;
    siridb = (siridb_t *) malloc(sizeof(siridb_t));
    siridb->dbname = NULL;
    siridb->dbpath = NULL;
    siridb->ref = 0;
    siridb->buffer_path = NULL;
    siridb->time = NULL;
    siridb->users = NULL;
    siridb->servers = NULL;
    siridb->pools = NULL;
    siridb->max_series_id = 0;
    siridb->received_points = 0;
    siridb->series = ct_new();
    siridb->series_map = imap32_new();
    siridb->shards = imap64_new();
    siridb->buffer_size = -1;
    siridb->tz = -1;
    siridb->server = NULL;
    siridb->replica = NULL;
    siridb->fifo = NULL;
    siridb->replicate = NULL;

    /* make file pointers are NULL when file is closed */
    siridb->buffer_fp = NULL;
    siridb->dropped_fp = NULL;
    siridb->store = NULL;

    uv_mutex_init(&siridb->series_mutex);
    uv_mutex_init(&siridb->shards_mutex);

    return siridb;
}

static void SIRIDB_free(siridb_t * siridb)
{
#ifdef DEBUG
    log_debug("Free database: %s", siridb->dbname);
#endif

    /* first we should close all open files */
    if (siridb->buffer_fp != NULL)
    {
        fclose(siridb->buffer_fp);
    }

    if (siridb->dropped_fp != NULL)
    {
        fclose(siridb->dropped_fp);
    }

    if (siridb->store != NULL)
    {
        qp_close(siridb->store);
    }

    /* free users */
    if (siridb->users != NULL)
    {
        siridb_users_free(siridb->users);
    }

    /* we do not need to free server and replica since they exist in
     * this list and therefore will be freed.
     */
    if (siridb->servers != NULL)
    {
        siridb_servers_free(siridb->servers);
    }

    /*
     * Destroy replicate before fifo but after servers so open promises are
     * closed which might depend on siridb->replicate
     *
     * siridb->replicate must be closed, see 'SIRI_set_closing_state'
     */
    if (siridb->replicate != NULL)
    {
        siridb_replicate_destroy(siridb);
    }

    /* free fifo (in case we have a replica) */
    if (siridb->fifo != NULL)
    {
        siridb_fifo_free(siridb->fifo);
    }

    /* free pools */
    if (siridb->pools != NULL)
    {
        siridb_pools_free(siridb->pools);
    }

    /* free imap32 (series) */
    if (siridb->series_map != NULL)
    {
        imap32_free(siridb->series_map);
    }

    /* free c-tree lookup and series */
    if (siridb->series != NULL)
    {
        ct_free_cb(siridb->series, (ct_free_cb_t) &siridb_series_decref);
    }

    /* free shards using imap64 walk an free the imap64 */
    if (siridb->shards != NULL)
    {
        imap64_walk(siridb->shards, (imap64_cb_t) &siridb_shard_decref, NULL);
        imap64_free(siridb->shards);
    }

    /* only free buffer path when not equal to db_path */
    if (siridb->buffer_path != siridb->dbpath)
    {
        free(siridb->buffer_path);
    }

    free(siridb->dbpath);
    free(siridb->dbname);
    free(siridb->time);

    uv_mutex_destroy(&siridb->series_mutex);
    uv_mutex_destroy(&siridb->shards_mutex);

    free(siridb);
}


