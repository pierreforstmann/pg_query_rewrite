/*-------------------------------------------------------------------------
 *  
 * pg_query_rewrite is a PostgreSQL extension which allows 
 * to translate a given source SQL statement into another pre-defined 
 * SQL statement. 
 * 
 * This program is open source, licensed under the PostgreSQL license.
 * For license terms, see the LICENSE file.
 *          
 * Copyright (c) 2020, Pierre Forstmann.
 *            
 *-------------------------------------------------------------------------
*/
#include "postgres.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "storage/proc.h"
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
#include "nodes/extensible.h"
#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"
#include "utils/datum.h"

PG_MODULE_MAGIC;

/*
 * maximum number of rules processed
 * by the extension defined as GUC
 * and checked at run-time
 * with trigger on pg_rewrite_rules
 * table with trigger 
 */

static	int	pgqr_max_rules_number = 0;

static	bool 	pgqr_enabled = false;
static	bool	backend_init = false;
/*
 * to avoid recursion in pgqr_analyze
 * during backend initialization
 */
static	bool	backend_init_started = false;

/*
 * Private state: pg_rewrite_rule cache
 * in backend private memory.
 *
 */
typedef struct pgqrPrivateItem
{
	char	*source_stmt;
	char	*dest_stmt;
	List	*source_stmt_raw_parsetree_list;	
} pgqrPrivateItem;

static	pgqrPrivateItem	*pgqrPrivateArray;

static	ParseState 	*new_static_pstate = NULL;
static 	Query		*new_static_query = NULL;  
static	List		*source_stmt_raw_parsetree_list = NULL;

/* Saved hook values in case of unload */
static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;


/*
 * Global shared state
 */
typedef struct pgqrSharedState
{
	LWLock 		*lock;
	bool		init;
	int		max_rules;
} pgqrSharedState;

/* Links to shared memory state */
static pgqrSharedState *pgqr= NULL;

/*---- Function declarations ----*/

void		_PG_init(void);
void		_PG_fini(void);

static 	void 	pgqr_shmem_startup(void);
static 	void 	pgqr_shmem_shutdown(int code, Datum arg);

static 	void 	pgqr_analyze(ParseState *pstate, Query *query);
static	void	pgqr_reanalyze(const char *new_query_string);

/*
 *  Estimate shared memory space needed.
 * 
 */
static Size
pgqr_memsize(void)
{
	Size		size;

	size = MAXALIGN(sizeof(pgqrSharedState));

	return size;
}


/*
 *  shmem_startup hook: allocate or attach to shared memory.
 *  
 */
static void
pgqr_shmem_startup(void)
{
	bool		found;

	elog(DEBUG5, "pg_query_rewrite: pgqr_shmem_startup: entry");

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	/* reset in case this is a restart within the postmaster */
	pgqr = NULL;


	/*
 	 * Create or attach to the shared memory state
 	 */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	pgqr = ShmemInitStruct("pg_query_rewrite",
			        sizeof(pgqrSharedState),
			        &found);

	if (!found)
	{
		/* First time through ... */
#if PG_VERSION_NUM <= 90600
		RequestAddinLWLocks(1);
		pgqr->lock = LWLockAssign();
#else
		pgqr->lock = &(GetNamedLWLockTranche("pg_query_rewrite"))->lock;
#endif
		pgqr->init = false;	
	}

	LWLockRelease(AddinShmemInitLock);

	/*
 	 *  If we're in the postmaster (or a standalone backend...), set up a shmem
 	 *  exit hook (no current need ???) 
 	 */ 
        if (!IsUnderPostmaster)
		on_shmem_exit(pgqr_shmem_shutdown, (Datum) 0);

	/*
    	 * Done if some other process already completed our initialization.
    	 */
	elog(DEBUG5, "pg_query_rewrite: pgqr_shmem_startup: exit");
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
pgqr_shmem_shutdown(int code, Datum arg)
{
	elog(DEBUG5, "pg_query_rewrite: pgqr_shmem_shutdown: entry");

	/* Don't do anything during a crash. */
	if (code)
		return;

	/* Safety check ... shouldn't get here unless shmem is set up. */
	if (!pgqr)
		return;
	
	/* currently: no action */

	elog(DEBUG5, "pg_query_rewrite: pgqr_shmem_shutdown: exit");
}


/*
 * Module load callback
 */
void
_PG_init(void)
{
	elog(DEBUG5, "pg_query_rewrite:_PG_init():entry");

	if (!process_shared_preload_libraries_in_progress)
		return;

	/* get the configuration */
	DefineCustomIntVariable("pg_query_rewrite.max_rules",
				"Maximum of number of rules.",
				NULL,
				&pgqr_max_rules_number,
				0,	
				0,
				50,
				PGC_POSTMASTER,	
				0,
				NULL,
				NULL,
				NULL);
	
	if (pgqr_max_rules_number == 0)
	{
		elog(LOG, "pg_query_rewrite:_PG_init(): pg_query_rewrite.max_rules not defined");
		elog(LOG, "pg_query_rewrite:_PG_init(): pg_query_rewrite it not enabled");
	}
	else	pgqr_enabled = true;
	
	
	if (pgqr_enabled)
	{

		elog(LOG, "pg_query_rewrite:_PG_init(): pg_query_rewrite is enabled with %d rules", 
                          pgqr_max_rules_number);
		/*
 		 *  Request additional shared resources.  (These are no-ops if we're not in
 		 *  the postmaster process.)  We'll allocate or attach to the shared
 		 *   resources in pgqr_shmem_startup().
 		 */ 
		RequestAddinShmemSpace(pgqr_memsize());
#if PG_VERSION_NUM >= 90600
		RequestNamedLWLockTranche("pg_query_rewrite", 1);
#endif

		prev_shmem_startup_hook = shmem_startup_hook;
		shmem_startup_hook = pgqr_shmem_startup;
		prev_post_parse_analyze_hook = post_parse_analyze_hook;
		post_parse_analyze_hook = pgqr_analyze;

	}

	elog(DEBUG5, "pg_query_rewrite:_PG_init():exit");
}


/*
 *  Module unload callback
 */
void
_PG_fini(void)
{
	shmem_startup_hook = prev_shmem_startup_hook;	
	post_parse_analyze_hook = prev_post_parse_analyze_hook;
}

/*
 * check if the current query needs to be rewritten:
 * returns true if must be rewritten, otherwise false;
 * array_index is the position of the equivalent query
 * in pgqrPrivateArray.
 */
static bool pgqr_check_rewrite(const char *current_query_source, int *array_index) 
{

	List 	*raw_parsetree_list;
	int	i;

	*array_index = pgqr_max_rules_number;	
	if (backend_init == false)
		return false;

	raw_parsetree_list = raw_parser(current_query_source);

	for (i = 0 ; i < pgqr_max_rules_number; i++)	
		if (equal(raw_parsetree_list, 
        		pgqrPrivateArray[i].source_stmt_raw_parsetree_list))
		{
			*array_index = i;
			return true;
		}

	return false;
}

static void pgqr_clone_Query(Query *source, Query *target)
{
	target->type = source->type;
	target->commandType = source->commandType;
	target->querySource = source->querySource;
	target->queryId = source->queryId;
	target->canSetTag = source->canSetTag;
	target->utilityStmt = source->utilityStmt;
	target->resultRelation = source->resultRelation;
	target->hasAggs = source->hasAggs;
	target->hasWindowFuncs = source->hasWindowFuncs;
	target->hasTargetSRFs = source->hasTargetSRFs;
	target->hasSubLinks = source->hasSubLinks;
	target->hasDistinctOn = source->hasDistinctOn;
	target->hasRecursive = source->hasRecursive;
	target->hasModifyingCTE = source->hasModifyingCTE;
	target->hasForUpdate = source->hasForUpdate;
	target->hasRowSecurity = source->hasRowSecurity;
	target->cteList = source->cteList;
	target->rtable = source->rtable;
	target->jointree = source->jointree;
	target->targetList = source->targetList;
	target->override = source->override;
	target->onConflict = source->onConflict;
	target->returningList = source->returningList;
	target->groupClause= source->groupClause;
	target->groupingSets= source->groupingSets;
	target->havingQual= source->havingQual;
	target->distinctClause= source->distinctClause;
	target->sortClause= source->sortClause;
	target->limitOffset= source->limitOffset;
	target->limitCount= source->limitCount;
	target->rowMarks= source->rowMarks;
	target->setOperations= source->setOperations;
	target->constraintDeps= source->constraintDeps;
	target->stmt_location=source->stmt_location;
	target->stmt_len=source->stmt_len;

}

static void pgqr_clone_ParseState(ParseState *source, ParseState *target)
{
	target->parentParseState = source->parentParseState;
	target->p_sourcetext = source->p_sourcetext;
	target->p_rtable= source->p_rtable;
	target->p_joinexprs= source->p_joinexprs;
	target->p_joinlist= source->p_joinlist;
	target->p_namespace= source->p_namespace;
	target->p_lateral_active= source->p_lateral_active;
	target->p_ctenamespace= source->p_ctenamespace;
	target->p_future_ctes= source->p_future_ctes;
	target->p_parent_cte= source->p_parent_cte;
	target->p_target_relation= source->p_target_relation;
	target->p_is_insert= source->p_is_insert;
	target->p_windowdefs= source->p_windowdefs;
	target->p_expr_kind= source->p_expr_kind;
	target->p_next_resno= source->p_next_resno;
	target->p_multiassign_exprs= source->p_multiassign_exprs;
	target->p_locking_clause= source->p_locking_clause;
	target->p_locked_from_parent= source->p_locked_from_parent;
	target->p_resolve_unknowns= source->p_resolve_unknowns;
	target->p_queryEnv= source->p_queryEnv;
	target->p_hasAggs = source->p_hasAggs;
	target->p_hasWindowFuncs = source->p_hasWindowFuncs;
	target->p_hasTargetSRFs= source->p_hasTargetSRFs;
	target->p_hasSubLinks= source->p_hasSubLinks;
	target->p_hasModifyingCTE= source->p_hasModifyingCTE;
	target->p_last_srf = source->p_last_srf;
	target->p_pre_columnref_hook = source->p_pre_columnref_hook;
	target->p_post_columnref_hook = source->p_post_columnref_hook;
	target->p_paramref_hook = source->p_paramref_hook;
	target->p_coerce_param_hook = source->p_coerce_param_hook;
	target->p_ref_hook_state = source->p_ref_hook_state;
}

static void pgqr_reanalyze(const char *new_query_string)
{

	int num_stmt;

	/* >>> FROM parse_analyze in src/backend/parser/analyze.c <<< */
	ParseState *new_pstate = make_parsestate(NULL);
	Query	*new_query = (Query *)NULL;

	List	*raw_parsetree_list;
	ListCell *lc1; 

	elog(DEBUG1, "pg_query_rewrite: pgqr_reanalyze: entry");
	new_pstate->p_sourcetext = new_query_string;

	/* missing data: 
         * 1. numParams 
         * 2. ParamTypes 
         * 3. queryEnv
         */

	/* >>> FROM execute_sql_tring in src/backend/commands/extension.c <<< */
	/*
 	 * Parse the SQL string into a list of raw parse trees.
 	 *
 	 */
	raw_parsetree_list = pg_parse_query(new_query_string);	
	num_stmt = 0;
	foreach(lc1, raw_parsetree_list)
	{
		RawStmt    *parsetree = lfirst_node(RawStmt, lc1);
		new_query = transformTopLevelStmt(new_pstate, parsetree);	
		num_stmt++;
	}
	if (num_stmt > 1)
	{
		ereport(ERROR, 
                        (errmsg 
                         ("pg_query_rewrite: cannot rewrite multiple commands: %s", 
                          new_query_string)));
	}

	/* cannot run free_parsestate(new_pstate) */

	new_static_pstate = new_pstate;
	new_static_query = new_query;

	elog(DEBUG1, "pg_query_rewrite: pgqr_reanalyze: exit");
}
/*
 *
 * pqqr_analyze: main extension routine
 *
 */

static void pgqr_analyze(ParseState *pstate, Query *query)
{
	StringInfoData 	buf_select;
	int		spi_return_code;
	int		number_of_rows;

	int		i;
	char		*source_stmt_val = NULL;
	char		*dest_stmt_val = NULL;
	MemoryContext 	oldctx;
	
	int		array_index;

	elog(DEBUG1, "pg_query_rewrite: pgqr_analyze: entry");
	if (pgqr_enabled)
 	{	

		if (backend_init == false && backend_init_started == false)
		{
			/* parse once pg_query_rule.pattern 
 			 * to be able to make query comparison 
 			 * with "equal" routine 
 			 * and cache it in backend private memory
 			 */

			backend_init_started = true;
			initStringInfo(&buf_select);
					appendStringInfo(&buf_select,
					"SELECT id, pattern, replacement "
					"FROM pg_rewrite_rule "
      					"WHERE enabled = true "
					"ORDER BY id"
					);
			/* transaction already started in backend */
			SPI_connect();
			PushActiveSnapshot(GetTransactionSnapshot());
			pgstat_report_activity(STATE_RUNNING, buf_select.data);						

			spi_return_code = SPI_execute(buf_select.data, false, 0);
			if (spi_return_code != SPI_OK_SELECT)
			elog(FATAL, "cannot select from pg_query_rewrite: error code %d",
				     spi_return_code);
			number_of_rows = SPI_processed;
			elog(DEBUG1,"pg_query_rewrite: pgqr_analyze: number_of_rows=%d",
				    number_of_rows);
	
			i = 0;
			oldctx = MemoryContextSwitchTo(CacheMemoryContext);
			pgqrPrivateArray = (pgqrPrivateItem *)
                                            palloc(sizeof(pgqrPrivateItem)*pgqr_max_rules_number);
			if (pgqrPrivateArray == NULL)
				elog(FATAL, "pg_query_rewrite: palloc failed");
			

			for (i = 0; i < number_of_rows; i++)
			{
				bool 	val_is_null;
				int32	rule_id;

				rule_id = DatumGetInt32(SPI_getbinval(SPI_tuptable->vals[i],
							  SPI_tuptable->tupdesc,
							  1, &val_is_null));
				if (! val_is_null)
				{
					source_stmt_val = SPI_getvalue(SPI_tuptable->vals[i],
							    SPI_tuptable->tupdesc,
							    2);
					dest_stmt_val = SPI_getvalue(SPI_tuptable->vals[i],
                                                            SPI_tuptable->tupdesc,
                                                            3);
					
				}
				else 
				{
					elog(WARNING, "pg_query_rewrite : id is NULL");
				}
			
				if ( i > pgqr_max_rules_number )
				{
					elog(FATAL, "pg_query_rewrite: pgqr_analyze: too many rules");
				}

		 		source_stmt_raw_parsetree_list = raw_parser(source_stmt_val);		
				pgqrPrivateArray[i].source_stmt = source_stmt_val;
				pgqrPrivateArray[i].dest_stmt = dest_stmt_val;
				pgqrPrivateArray[i].source_stmt_raw_parsetree_list = 
						source_stmt_raw_parsetree_list;
				elog(DEBUG1,"pg_query_rewrite: pgqr_analyze: processed rule_id=%d",
				    	    rule_id);
			}
			MemoryContextSwitchTo(oldctx);

			backend_init = true;	
			SPI_finish();
			PopActiveSnapshot();
			pgstat_report_stat(false);
			pgstat_report_activity(STATE_IDLE, NULL);

			elog(DEBUG1,"pg_query_rewrite: pgqr_analyze: init done for %d rules", number_of_rows);
		}
		/* pstate->p_sourcetext is the current query text */	
		elog(DEBUG1,"pg_query_rewrite: pgqr_analyze: %s",pstate->p_sourcetext);
		if (pgqr_check_rewrite(pstate->p_sourcetext, &array_index))
		{
			elog(DEBUG1,"pg_query_rewrite: pgqr_to_rewrite %s: rc=true", 
                                     pstate->p_sourcetext);
			/* 
 			** analyze destination statement 
			*/
			pgqr_reanalyze(pgqrPrivateArray[array_index].dest_stmt);
			/* clone data */
			pgqr_clone_ParseState(new_static_pstate, pstate);
			elog(DEBUG1,"pg_query_rewrite: pgqr_analyze: rewrite=true pstate->p_source_text %s",
                                    pstate->p_sourcetext);
			pgqr_clone_Query(new_static_query, query);
		} else
			elog(DEBUG1,"pg_query_rewrite: pgqr_to_rewrite %s: rc=false", 
                                    pstate->p_sourcetext);

		/* no "standard_analyze" to call 
  		 * according to parse_analyze in analyze.c 
  		 */
		if (prev_post_parse_analyze_hook)
		 	prev_post_parse_analyze_hook(pstate, query);
	 }

	elog(DEBUG1, "pg_query_rewrite: pgqr_analyze: exit");
}
