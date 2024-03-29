/*
Copyright (c) 2014, Rick van Hattem <Wolph at wol.ph> - http://wol.ph/
All rights reserved.

This Trigger makes denormalization of `updated_at` type of columns possible
with much better performance than regular triggers.

Usage:
-- Creating a trigger to automatically update the `updated_at` column on the
-- `topic` table through the `topic_id` foreign key on `post` if `post` was
-- created, updated or deleted.
--
--
-- cascade_timestamp(
--     destination_table,
--     destination_timestamp_column,
--     destination_key (primary key),
--     source_key (foreign key),
-- )
DROP TRIGGER IF EXISTS post_update_trigger ON post;

CREATE CONSTRAINT TRIGGER post_update_trigger
AFTER UPDATE OR INSERT OR DELETE ON post
DEFERRABLE INITIALLY DEFERRED FOR EACH ROW 
EXECUTE PROCEDURE cascade_timestamp(topic, updated_at, id, topic_id);

*/

#include "postgres.h"
#include "access/htup.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include <ctype.h>

/* Since Postgres 9.3 we need the `htup_details.h` include */
#ifndef HeapTupleHeaderGetOid
#include "access/htup_details.h"
#endif

/* Since Postgres 9.2 we need the `rel.h` include */
#ifndef RelationData
#include "utils/rel.h"
#endif

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

extern Datum cascade_timestamp(PG_FUNCTION_ARGS);

typedef struct {
    char *ident;
    SPIPlanPtr plan;
} EPlan;

static EPlan *FPlans = NULL;
static int nFPlans = 0;

static EPlan *find_plan(char *ident, EPlan **eplan, int *nplans);
PG_FUNCTION_INFO_V1(cascade_timestamp)
;

Datum cascade_timestamp(PG_FUNCTION_ARGS){
    TriggerData *trigdata = (TriggerData *)fcinfo->context;
    HeapTuple newtuple = trigdata->tg_newtuple, oldtuple =
            trigdata->tg_trigtuple, rettuple = NULL;
    HeapTupleHeader newheader, oldheader;
    Trigger *trigger = trigdata->tg_trigger;
    Datum kval;
    int fnumber;
    char **args;
    char *relname;
    Oid argtype;
    bool update;
    bool isnull;
    int ret;
    char ident[2 * NAMEDATALEN];
    Relation rel;
    TupleDesc tupdesc;
    EPlan *plan;
    char sql[1024];
    char *newval;
    int i;

    /* make sure it's called as a trigger */
    if(!CALLED_AS_TRIGGER(fcinfo)){
        elog(ERROR, "cascade_timestamp: must be called as a trigger");
        SPI_finish();
        return PointerGetDatum(NULL);
    }

    /* make sure it's called after update */
    if(!TRIGGER_FIRED_AFTER(trigdata->tg_event)){
        elog(ERROR, "cascade_timestamp: must be called after the event");
        SPI_finish();
        return PointerGetDatum(NULL);
    }

    /* make sure it's called for each row */
    if(!TRIGGER_FIRED_FOR_ROW(trigdata->tg_event)){
        elog(ERROR, "cascade_timestamp: must be called for each row");
        SPI_finish();
        return PointerGetDatum(NULL);
    }

    /* and that it's called with the right arguments */
    if(trigger->tgnargs < 3){
        elog(ERROR, "cascade_timestamp: A destination table, timestamp column, primary key column and a foreign key column were expected, got %d arguments", trigger->tgnargs);
        SPI_finish();
        return PointerGetDatum(NULL);
    }

    rettuple = trigdata->tg_trigtuple;
    update = true;

    if (TRIGGER_FIRED_BY_UPDATE(trigdata->tg_event)){
        newheader = newtuple->t_data;
        oldheader = oldtuple->t_data;

        /* copy the OID just in case it changed somehow */
        if (trigdata->tg_relation->rd_rel->relhasoids &&
                !OidIsValid(HeapTupleHeaderGetOid(newheader))){
            HeapTupleHeaderSetOid(newheader, HeapTupleHeaderGetOid(oldheader));
        }

        /* if the tuple payload is the same ... */
        if (newtuple->t_len == oldtuple->t_len &&
                newheader->t_hoff == oldheader->t_hoff &&
                (HeapTupleHeaderGetNatts(newheader) ==
                        HeapTupleHeaderGetNatts(oldheader)) &&
                ((newheader->t_infomask & ~HEAP_XACT_MASK) ==
                        (oldheader->t_infomask & ~HEAP_XACT_MASK)) &&
                memcmp(((char *) newheader) + offsetof(HeapTupleHeaderData, t_bits),
                        ((char *) oldheader) + offsetof(HeapTupleHeaderData, t_bits),
                        newtuple->t_len - offsetof(HeapTupleHeaderData, t_bits)) == 0){

            update = false;
        } else{
            update = true;
        }
    }

    rel = trigdata->tg_relation;
    relname = SPI_getrelname(rel);
    args = trigger->tgargs;
    tupdesc = rel->rd_att;

    /* Make sure the foreign key actually exists and has a value */
    for(i=4; i<trigger->tgnargs; i+=2){
        fnumber = SPI_fnumber(tupdesc, args[i]);
        if (fnumber < 0){
            elog(ERROR, "\"%s\" has no attribute \"%s\"",
                    relname, args[3]);
        }

        newval = SPI_getvalue(rettuple, tupdesc, fnumber);
        if(newval != NULL && strcmp(newval, args[i+1]) != 0){
            update = false;
            break;
        }
    }

    /* No value for the foreign key */
    if(!update){
        SPI_finish();
        return PointerGetDatum(rettuple);
    }

    /* Connect to SPI manager */
    if ((ret = SPI_connect()) < 0){
        /* internal error */
        elog(ERROR, "cascade_timestamp: SPI_connect returned %d", ret);
    }

    /*
     * Construct ident string as TriggerName $ TriggeredRelationId and try to
     * find prepared execution plan(s).
     */
    snprintf(ident, sizeof(ident), "%s$%u", trigger->tgname, rel->rd_id);
    plan = find_plan(ident, &FPlans, &nFPlans);

    fnumber = SPI_fnumber(tupdesc, args[2]);
    if (fnumber < 0){
        elog(ERROR, "\"%s\" has no attribute \"%s\"",
                relname, args[2]);
    }

    kval = SPI_getbinval(rettuple, tupdesc, fnumber, &isnull);

    if (plan->plan == NULL){
        if (isnull)
        {
            SPI_finish();
            return PointerGetDatum(rettuple);
        }

        /* Get typeId of column */
        argtype = SPI_gettypeid(tupdesc, fnumber);

        snprintf(
                sql,
                sizeof(sql),
                "UPDATE %s SET %s = NOW() WHERE %s = $1",
                args[0],
                args[1],
                args[2]
        );

        plan->plan = SPI_prepare(sql, 1, &argtype);
        if(plan->plan == NULL){
            /* internal error */
            elog(ERROR, "cascade_timestamp: SPI_prepare returned %d", SPI_result);
        }

        /*
         * Remember that SPI_prepare places plan in current memory context
         * - so, we have to save plan in Top memory context for latter
         * use.
         */
        plan->plan = SPI_saveplan(plan->plan);
        if (plan->plan == NULL){
            /* internal error */
            elog(ERROR, "cascade_timestamp: SPI_saveplan returned %d", SPI_result);
        }
    }

    ret = SPI_execp(plan->plan, &kval, NULL, 1);
    if (ret < 0){
        elog(ERROR, "SPI_execp returned %d", ret);
    }

    pfree(relname);
    SPI_finish();
    return PointerGetDatum(rettuple);
}

static EPlan *
find_plan(char *ident, EPlan **eplan, int *nplans){
    EPlan *newp;
    int i;

    if(*nplans > 0){
        for(i = 0;i < *nplans;i++){
            if(strcmp((*eplan)[i].ident, ident) == 0)
                break;
        }
        if(i != *nplans)
            return (*eplan + i);
        *eplan = (EPlan *)realloc(*eplan, (i + 1) * sizeof(EPlan));
        newp = *eplan + i;
    }else{
        newp = *eplan = (EPlan *)malloc(sizeof(EPlan));
        (*nplans) = i = 0;
    }

    newp->ident = (char *)malloc(strlen(ident) + 1);
    strcpy(newp->ident, ident);
    newp->plan = NULL;
    (*nplans)++;

    return (newp);
}
