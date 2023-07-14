/*-------------------------------------------------------------------------
 *  
 * pgds.c
 * 
 * This program is open source, licensed under the PostgreSQL license.
 * For license terms, see the LICENSE file.
 *          
 * Copyright (c) 2023 Pierre Forstmann.
 *            
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "access/xact.h"
#include "parser/parse_node.h"
#include "parser/analyze.h"
#include "parser/parser.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/guc.h"
#include "utils/snapmgr.h"
#include "utils/memutils.h"
#if PG_VERSION_NUM <= 90600
#include "storage/lwlock.h"
#endif
#include "pgstat.h"
#include "storage/ipc.h"
#include "storage/spin.h"
#include "miscadmin.h"
#if PG_VERSION_NUM >= 90600
#include "nodes/extensible.h"
#endif
#if PG_VERSION_NUM > 120000
#include "nodes/pathnodes.h"
#endif
#include "nodes/plannodes.h"
#include "utils/datum.h"
#include "utils/builtins.h"
#include "unistd.h"
#include "funcapi.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"

PG_MODULE_MAGIC;

typedef struct pgdsSharedState
{
	LWLock 		*lock;
	
} pgdsSharedState;

static pgdsSharedState *pgds = NULL;

static int called = 0;

/* Saved hook values in case of unload */
#if PG_VERSION_NUM >= 150000
static shmem_request_hook_type prev_shmem_request_hook = NULL;
#endif
static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static ExecutorStart_hook_type prev_executor_start_hook = NULL;

/*---- Function declarations ----*/

void		_PG_init(void);
void		_PG_fini(void);

static 	void 	pgds_shmem_startup(void);
static 	void 	pgds_shmem_shutdown(int code, Datum arg);

#if PG_VERSION_NUM < 140000
static 	void 	pgds_analyze(ParseState *pstate, Query *query);
#else
static 	void 	pgds_analyze(ParseState *pstate, Query *query, JumbleState *jstate);
#endif

static  void 	pgds_exec(QueryDesc *queryDesc, int eflags);


/*
 *  Estimate shared memory space needed.
 * 
 */
static Size
pgds_memsize(void)
{
	Size		size;

	size = 1024;

	return size;
}

/*
 * shmen_request_hook
 *
 */
static void
pgds_shmem_request(void)
{
	/*
 	 * Request additional shared resources.  (These are no-ops if we're not in
 	 * the postmaster process.)  We'll allocate or attach to the shared
 	 * resources in pgls_shmem_startup().
	 */

#if PG_VERSION_NUM >= 150000
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();
#endif

	RequestAddinShmemSpace(pgds_memsize());
#if PG_VERSION_NUM >= 90600
	RequestNamedLWLockTranche("pgds", 1);
#endif

}


/*
 *  shmem_startup hook: allocate or attach to shared memory.
 *  
 */
static void
pgds_shmem_startup(void)
{
	bool		found;

	elog(DEBUG5, "pgds: pgds_shmem_startup: entry");

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	
	/*
 	 * Create or attach to the shared memory state
 	 */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	pgds = ShmemInitStruct("pgds",
				pgds_memsize(),
			        &found);

	if (!found)
	{
		/* First time through ... */
#if PG_VERSION_NUM <= 90600
		RequestAddinLWLocks(1);
		pgds->lock = LWLockAssign();
#else
		pgds->lock = &(GetNamedLWLockTranche("pgds"))->lock;
#endif

	}

	LWLockRelease(AddinShmemInitLock);


	/*
 	 *  If we're in the postmaster (or a standalone backend...), set up a shmem
 	 *  exit hook (no current need ???) 
 	 */ 
        if (!IsUnderPostmaster)
		on_shmem_exit(pgds_shmem_shutdown, (Datum) 0);


	/*
    	 * Done if some other process already completed our initialization.
    	 */
	elog(DEBUG5, "pgds: pgds_shmem_startup: exit");
	if (found)
		return;


}

/*
 *  
 *     shmem_shutdown hook
 *       
 *     Note: we don't bother with acquiring lock, because there should be no
 *     other processes running when this is called.
 */
static void
pgds_shmem_shutdown(int code, Datum arg)
{
	elog(DEBUG5, "pgds: pgds_shmem_shutdown: entry");

	/* Don't do anything during a crash. */
	if (code)
		return;

	/* Safety check ... shouldn't get here unless shmem is set up. */
	if (!pgds)
		return;
	
	/* currently: no action */

	elog(DEBUG5, "pgds: pgds_shmem_shutdown: exit");
}


/*
 * Module load callback
 */
void
_PG_init(void)
{
	elog(DEBUG5, "pgds:_PG_init():entry");

	if (!process_shared_preload_libraries_in_progress)
		return;

	elog(LOG, "pgds:_PG_init(): pgds is enabled ");

#if PG_VERSION_NUM >= 150000
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = pgds_shmem_request;
#else
	pgqr_shmem_request();
#endif
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = pgds_shmem_startup;
	prev_post_parse_analyze_hook = post_parse_analyze_hook;
	post_parse_analyze_hook = pgds_analyze;
	prev_executor_start_hook = ExecutorStart_hook;
 	ExecutorStart_hook = pgds_exec;	

	elog(DEBUG5, "pgds:_PG_init():exit");
}


/*
 *  Module unload callback
 */
void
_PG_fini(void)
{
	shmem_startup_hook = prev_shmem_startup_hook;	
	post_parse_analyze_hook = prev_post_parse_analyze_hook;
	ExecutorStart_hook = prev_executor_start_hook;
}


/*
 *
 * pgds_analyze: main routine
 *
 */
#if PG_VERSION_NUM < 140000
static void pgds_analyze(ParseState *pstate, Query *query)
#else
static void pgds_analyze(ParseState *pstate, Query *query, JumbleState *js)
#endif
{
	StringInfoData buf_select;
	StringInfoData buf_analyze;
	int ret;
	int nr;

	elog(DEBUG1,"pgds: pgds_analyze: entry: %s",pstate->p_sourcetext);

	/* pstate->p_sourcetext is the current query text */	
	elog(DEBUG1,"pgds: pgds_analyze: %s",pstate->p_sourcetext);

	if (called == 0)
	{
		called = 1;
		initStringInfo(&buf_select);
		appendStringInfo(&buf_select, 
						 " select count(*) from (select schemaname, tablename, attname, n_distinct, count(*)" 
						                         "from pg_stats where tablename='t'" 
										         "group by schemaname, tablename, attname, n_distinct) s;");

		initStringInfo(&buf_analyze);
		appendStringInfo(&buf_analyze, "analyze verbose t;");

		SPI_connect();
		ret = SPI_execute(buf_select.data, false, 0);
		if (ret != SPI_OK_SELECT)
			elog(FATAL, "cannot select from pg_stats: error code %d", ret);
		nr = SPI_processed;		
		elog(DEBUG1,"pgds: pgds_analyze: found %d rows in pg_stats query",  nr);

		ret = SPI_execute(buf_analyze.data, false, 0);
		if (ret != SPI_OK_SELECT)
			elog(FATAL, "cannot run analyze: error code %d", ret);
		SPI_finish();
	} 


	if (prev_post_parse_analyze_hook)
	{
#if PG_VERSION_NUM < 140000
		prev_post_parse_analyze_hook(pstate, query);
#else
		prev_post_parse_analyze_hook(pstate, query, js);
#endif
	 }

	elog(DEBUG1, "pgds: pgds_analyze: exit");
}


/*
 * pgds_exec
 *
 */
static void pgds_exec(QueryDesc *queryDesc, int eflags)
{
#if PG_VERSION_NUM > 100000 

#endif
	/*
 	 * must always execute here whatever PG_VERSION_NUM
 	 */

	if (prev_executor_start_hook)
                (*prev_executor_start_hook)(queryDesc, eflags);
	else	standard_ExecutorStart(queryDesc, eflags);
}

