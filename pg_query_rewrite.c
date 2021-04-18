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
#include "nodes/extensible.h"
#if PG_VERSION_NUM > 120000
#include "nodes/pathnodes.h"
#endif
#include "nodes/plannodes.h"
#include "utils/datum.h"
#include "utils/builtins.h"
#include "unistd.h"

PG_MODULE_MAGIC;

extern 	PROC_HDR	*ProcGlobal;

static	bool 	pgqr_enabled = false;
/*
 * maximum number of rules processed
 * by the extension defined as GUC
 * and checked at run-time
 * with trigger on pg_rewrite_rules
 * table with trigger 
 */

static	int	pgqr_max_rules_number = 0;
static	int	pgqr_current_rules_number = 0;
/*
 * to avoid recursion in pgqr_analyze
 * during backend initialization
 */
static	bool	backend_initialized = false;
static	bool	pgqr_load_cache_started=false;
/* 
 * true par default but set to false
 * in pg_load_cache PG_CATCH block
 */
static	bool	pgqr_is_installed_in_current_db = true;

/*
 * for pg_stat_statements assertion 
 */
static	bool	statement_rewritten = false;

/*
 * Private state: 
 * pg_rewrite_rule cache in backend private memory.
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
static ExecutorStart_hook_type prev_executor_start_hook = NULL;


/*
 * Global shared state:
 * backend pid array with reload private cache flag.
 */

typedef struct pgqrSharedItem
{
	int	pid;
	bool	reload_table_into_cache;
} pgqrSharedItem;

typedef struct pgqrSharedState
{
	LWLock 		*lock;
	bool		init;
	int		max_rules;
	int		max_backend_no;
	int		current_backend_no;
	pgqrSharedItem	*proc_array;

        /* actually this structure is bigger 
 	 * than above fields to store
 	 * proc_array dynamically at
 	 * structure end.
 	 */	

} pgqrSharedState;

/* Links to shared memory state */
static pgqrSharedState *pgqr= NULL;

/*---- Function declarations ----*/

void		_PG_init(void);
void		_PG_fini(void);

static 	void 	pgqr_shmem_startup(void);
static 	void 	pgqr_shmem_shutdown(int code, Datum arg);

#if PG_VERSION_NUM < 140000
static 	void 	pgqr_analyze(ParseState *pstate, Query *query);
#else
static 	void 	pgqr_analyze(ParseState *pstate, Query *query, JumbleState *jstate);
#endif
static	void	pgqr_reanalyze(const char *new_query_string);
static	void	pgqr_exit(void);

static void 	pgqr_exec(QueryDesc *queryDesc, int eflags);

/*
 *  Estimate shared memory space needed.
 * 
 */
static Size
pgqr_memsize(void)
{
	Size		size;
	int		max_conn;
	const char	*max_conn_string;		

	size = MAXALIGN(sizeof(pgqrSharedState));
	max_conn_string = GetConfigOption("max_connections", false, false);	
	max_conn = pg_atoi(max_conn_string, 1, 0);
	elog(DEBUG5, "pg_query_rewrite: pgqr_memsize: max_connections=%d", max_conn);
	size += MAXALIGN(sizeof(pgqrSharedItem) * max_conn);

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
	const char	*max_conn_string;
	int		max_conn;
	int		i;

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
				pgqr_memsize(),
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

		max_conn_string = GetConfigOption("max_connections", false, false);	
		max_conn = pg_atoi(max_conn_string, 1, 0);
		pgqr->max_backend_no = max_conn;
		/* 
 		 * set proc_array address 
 		 */
		pgqr->proc_array = (pgqrSharedItem *)((char *)&(pgqr->proc_array) + sizeof(pgqr->proc_array));
		
		for (i=0 ; i < max_conn; i++)
		{
			pgqr->proc_array[i].pid = 0;
			pgqr->proc_array[i].reload_table_into_cache = false;	
		}
		pgqr->current_backend_no = 0;
		
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
		elog(LOG, "pg_query_rewrite:_PG_init(): pg_query_rewrite not enabled");
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
		prev_executor_start_hook = ExecutorStart_hook;
	 	ExecutorStart_hook = pgqr_exec;	



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
	ExecutorStart_hook = prev_executor_start_hook;
}

/*
 * pgqr_add_backend_in_proc_array
 */

static
void pgqr_add_backend_in_proc_array(void)
{
	PGPROC	*proc=MyProc;
	bool	found = false;
	int 	i;

	LWLockAcquire(pgqr->lock, LW_EXCLUSIVE);
	for (i = 0; i < pgqr->max_backend_no && found == false; i++)
	{
		if (pgqr->proc_array[i].pid == 0)
		{
			found = true;
			pgqr->proc_array[i].pid=proc->pid;
			pgqr->proc_array[i].reload_table_into_cache=false;
			pgqr->current_backend_no++;
		}
	}
	LWLockRelease(pgqr->lock);

	if (found == false)
		elog(WARNING, "pg_query_rewrite: add_backend: "
                           "proc_array is full");
}

/*
 * pgqr_del_backend_in_proc_array
 */

static
void pgqr_del_backend_in_proc_array(void)
{
	int	pid;
	int	i;
	bool	found = false;

	/* 
	 * cannot take LWlock in backend exit ... 
	 * cannot access MyProc in backend exit ...
	 * TODO: race condition with pgqr_add_backend_in_proc_array
	 * use spin lock instead of LWLock ?
 	 */
	pid = getpid();
	for (i = 0; i < pgqr->max_backend_no && found == false; i++)
	{
		if (pgqr->proc_array[i].pid == pid)
		{
			found = true;
			pgqr->proc_array[i].pid=0;
			pgqr->proc_array[i].reload_table_into_cache=false;
			pgqr->current_backend_no--;
		}
	}
	if (found == false)
		elog(WARNING, "pg_query_rewrite: del_backend: "
                           "pid=%d not found", pid);
	
}

/*
 * pgqr_backend_check_load_cache
 */
static bool pgqr_backend_check_reload_cache()
{
	PGPROC	*proc=MyProc;
	bool	found = false;
	int	i;

	if (backend_initialized == false)	
		return false;

	LWLockAcquire(pgqr->lock, LW_SHARED);
	for (i = 0; i < pgqr->max_backend_no && found == false; i++)
        {
                if (pgqr->proc_array[i].pid == proc->pid)
                {
                        found = true;
			break;
                }
        }

	LWLockRelease(pgqr->lock);
	if (found == true)
	        return pgqr->proc_array[i].reload_table_into_cache;

 	if (i == pgqr->max_backend_no && found == false)
		elog(WARNING, "pg_query_rewrite: pgqr_check_reload_cache: "
                           "pid=%d not found", proc->pid);
	return false;
		
}


/*
 * pgqr_log_proc_array 
 * 
 */
PG_FUNCTION_INFO_V1(pgqr_log_proc_array);

Datum
pgqr_log_proc_array(PG_FUNCTION_ARGS)
{
        int     i;
	int	pid;
	bool	flag;

	elog(LOG, "getpid=%d", getpid());
	elog(LOG, "pgqr->max_backend_no=%d", pgqr->max_backend_no);
	elog(LOG, "pgqr->current_backend_no=%d", pgqr->current_backend_no);
        for (i = 0; i < pgqr->max_backend_no; i++)
		if (pgqr->proc_array[i].pid != 0)
		{
			pid = pgqr->proc_array[i].pid;
			flag = pgqr->proc_array[i].reload_table_into_cache;
			elog(LOG, "index=%d pid=%d reload_table_into_cache=%d", i, pid, flag); 
		}
	PG_RETURN_BOOL(true);

}

/*
 * pgqr_log_rules_cache 
 * 
 */
PG_FUNCTION_INFO_V1(pgqr_log_rules_cache);

Datum
pgqr_log_rules_cache(PG_FUNCTION_ARGS)
{
        int     i;
	char	*source;
	char	*p_source;
	char	*destination;
	char	*p_destination;
	char	*null_string = "NULL";

	elog(LOG, "getpid=%d", getpid());
	elog(LOG, "pgqr_max_rules_number=%d", pgqr_max_rules_number);
	elog(LOG, "pgqr_current_rules_number=%d", pgqr_current_rules_number);
        for (i = 0; i < pgqr_max_rules_number; i++)
	{
		source = pgqrPrivateArray[i].source_stmt;
		destination = pgqrPrivateArray[i].dest_stmt;
		if (source == NULL)
			p_source = null_string;
		else	p_source = source;
		if (destination == NULL)
			p_destination = null_string;
		else	p_destination = destination;
		elog(LOG, "index=%d source=%s destination=%s", i, p_source, p_destination); 
	}
	PG_RETURN_BOOL(true);

}
/*
 * pgqr_reset_load_flag
 */
static void pgqr_reset_load_flag(void)
{
	PGPROC	*proc=MyProc;
	bool	found = false;
	int	i;

	LWLockAcquire(pgqr->lock, LW_EXCLUSIVE);

	for (i = 0; i < pgqr->max_backend_no && found == false; i++)
        {
                if (pgqr->proc_array[i].pid == proc->pid)
                {
                        found = true;
	         	pgqr->proc_array[i].reload_table_into_cache = false;
                }
        }

	LWLockRelease(pgqr->lock);

 	if (found == false)
		elog(WARNING, "pg_query_rewrite: pgqr_reset_load_flag: "
                           "pid=%d not found", proc->pid);
	
}


PG_FUNCTION_INFO_V1(pgqr_load_rules);

/*
 * pgqr_load_rules
 *
 * SQL-callable function to run after
 * pg_rewrite_rules table has been modified.
 *
 */
Datum 
pgqr_load_rules(PG_FUNCTION_ARGS)
{
	int 	i;

	LWLockAcquire(pgqr->lock, LW_EXCLUSIVE);

	for (i = 0; i < pgqr->max_backend_no; i++)
	{
		if (pgqr->proc_array[i].pid != 0)
		{
			/*
 			 * set reload flag to true:
 			 * each backend is going to test its own flag
 			 * and reload its cache.
 			 */ 
			pgqr->proc_array[i].reload_table_into_cache=true;
		}
	}
	LWLockRelease(pgqr->lock);

	/* for backend that has just run CREATE EXTENSION */
	pgqr_is_installed_in_current_db = true;	
	PG_RETURN_BOOL(true);

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
	/* avoid recursion because pgqrPrivateArray may not be allocated */
	if (backend_initialized == false)
		return false;
#if PG_VERSION_NUM < 140000
	raw_parsetree_list = raw_parser(current_query_source);
#else
	raw_parsetree_list = raw_parser(current_query_source, RAW_PARSE_DEFAULT);
#endif

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
#if PG_VERSION_NUM > 100000 
	target->hasTargetSRFs = source->hasTargetSRFs;
#endif
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
#if PG_VERSION_NUM > 100000 
	target->override = source->override;
#endif
	target->onConflict = source->onConflict;
	target->returningList = source->returningList;
	target->groupClause = source->groupClause;
	target->groupingSets = source->groupingSets;
	target->havingQual = source->havingQual;
	target->windowClause = source->windowClause;
	target->distinctClause= source->distinctClause;
	target->sortClause= source->sortClause;
	target->limitOffset= source->limitOffset;
	target->limitCount= source->limitCount;
#if PG_VERSION_NUM > 130000 
	target->limitOption = source->limitOption;
#endif
	target->rowMarks= source->rowMarks;
	target->setOperations= source->setOperations;
	target->constraintDeps= source->constraintDeps;
#if PG_VERSION_NUM > 140000 
	target->withCheckOptions = source->limitOption;
#endif
#if PG_VERSION_NUM > 100000 
	target->stmt_location=source->stmt_location;
	target->stmt_len=source->stmt_len;
#endif

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
#if PG_VERSION_NUM > 100000 
	target->p_resolve_unknowns= source->p_resolve_unknowns;
	target->p_queryEnv= source->p_queryEnv;
#endif
	target->p_hasAggs = source->p_hasAggs;
	target->p_hasWindowFuncs = source->p_hasWindowFuncs;
#if PG_VERSION_NUM > 100000 
	target->p_hasTargetSRFs= source->p_hasTargetSRFs;
#endif
	target->p_hasSubLinks= source->p_hasSubLinks;
	target->p_hasModifyingCTE= source->p_hasModifyingCTE;
#if PG_VERSION_NUM > 100000 
	target->p_last_srf = source->p_last_srf;
#endif
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
#if PG_VERSION_NUM >= 100000
		RawStmt    *parsetree = lfirst_node(RawStmt, lc1);
#else
		 Node       *parsetree = (Node *) lfirst(lc1);
#endif
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
 * pqgr_load_cache
 *
 * first_run: true if first execution is current backend.
 */

static void pgqr_load_cache(bool first_run)
{
	/* parse once pg_query_rule.pattern 
 	 * to be able to make query comparison 
 	 * with "equal" routine 
 	 * and cache it in backend private memory
 	 */

	StringInfoData 	buf_select;
	int		spi_return_code;
	int		number_of_rows = 0;
	bool		spi_execute_has_failed = false;

	int		i;
	char		*source_stmt_val = NULL;
	char		*dest_stmt_val = NULL;
	MemoryContext 	oldctx;
	void		(*on_exit_fp)();
	
	pgqr_load_cache_started=true;

	initStringInfo(&buf_select);
			appendStringInfo(&buf_select,
			"SELECT id, pattern, replacement "
			"FROM pg_rewrite_rule "
      			"WHERE enabled = true "
			"ORDER BY id"
			);
	/* transaction already started in backend */
	SPI_connect();
	pgstat_report_activity(STATE_RUNNING, buf_select.data);						

	/*
 	* assume any error means that table pg_query_rewrite does not exit
	* and that extension pg_query_rewrite is not installed in current
	* database
	*/

	PG_TRY();
	{
	 	spi_return_code = SPI_execute(buf_select.data, false, 0);
	 	if (spi_return_code != SPI_OK_SELECT)
	 		elog(FATAL, "cannot select from pg_query_rewrite: error code %d",
				     spi_return_code);
    		 number_of_rows = SPI_processed;
        }
	PG_CATCH();
	{
                spi_execute_has_failed = true;         
                /*
                ** PG 9.5
		** to fix WARNING:  transaction left non-empty SPI stack  
                ** add AtEOXact_SPI(false)
                */
#if PG_VERSION_NUM < 100000
                AtEOXact_SPI(false);
		pgstat_report_activity(STATE_IDLE, NULL);
#endif

		pgqr_is_installed_in_current_db = false;
		elog(LOG,"pg_query_rewrite: pgqr_load_cache: SELECT error catched.");
	}
	PG_END_TRY();


	if (spi_execute_has_failed == false)
	{
		/* 
                ** PG 9.5
                ** SIGSEGV in PopActiveStnapshot
		*/
                /*
                ** PG 9.5
		** WARNING:  transaction left non-empty SPI stack  
                ** add AtEOXact_SPI(false)
                */
#if PG_VERSION_NUM < 100000
                AtEOXact_SPI(false);
		pgstat_report_activity(STATE_IDLE, NULL);
#endif
	}
		

	elog(DEBUG1,"pg_query_rewrite: pgqr_analyze: number_of_rows=%d",
		    number_of_rows);
	
	i = 0;
	/*
 	 * TODO: should release memory when loading new cache 
 	 */
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

#if PG_VERSION_NUM < 140000
		source_stmt_raw_parsetree_list = raw_parser(source_stmt_val);		
#else
		source_stmt_raw_parsetree_list = raw_parser(source_stmt_val, RAW_PARSE_DEFAULT);		
#endif
		pgqrPrivateArray[i].source_stmt = source_stmt_val;
		pgqrPrivateArray[i].dest_stmt = dest_stmt_val;
		pgqrPrivateArray[i].source_stmt_raw_parsetree_list = 
	           		   source_stmt_raw_parsetree_list;
		elog(DEBUG1,"pg_query_rewrite: pgqr_analyze: processed rule_id=%d",
			    	    rule_id);
	}
	MemoryContextSwitchTo(oldctx);

	pgqr_current_rules_number = number_of_rows;
	backend_initialized = true;	 

        SPI_finish();

	/*
 	 * assertion failure Assert(entry->trans == NULL);
	 * if called during CREATE EXTENSION step.
	 *
	 * pgstat_report_stat(false);
	 * pgstat_report_activity(STATE_IDLE, NULL);
	*/

	if (first_run == true)
	{
		pgqr_add_backend_in_proc_array();		
		on_exit_fp = pgqr_exit;
		on_proc_exit(on_exit_fp, (Datum)NULL);
	}
	else	pgqr_reset_load_flag();

	pgqr_load_cache_started = false;
	elog(DEBUG1,"pg_query_rewrite: pgqr_load_cache: %d rules loaded", number_of_rows);

}

/*
 *
 * pqqr_analyze: main routine
 *
 */
#if PG_VERSION_NUM < 140000
static void pgqr_analyze(ParseState *pstate, Query *query)
#else
static void pgqr_analyze(ParseState *pstate, Query *query, JumbleState *js)
#endif
{
	
	int		array_index;

	elog(DEBUG1, "pg_query_rewrite: pgqr_analyze: entry");

	statement_rewritten = false;

	if (pgqr_is_installed_in_current_db)
 	{	

		if (backend_initialized == false && pgqr_load_cache_started == false)
		{
			pgqr_load_cache(true);
		}
		if (pgqr_backend_check_reload_cache() == true && pgqr_load_cache_started == false)
		{
			pgqr_load_cache(false);
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
			statement_rewritten = true;
		} else
			elog(DEBUG1,"pg_query_rewrite: pgqr_to_rewrite %s: rc=false", 
                                    pstate->p_sourcetext);

		/* no "standard_analyze" to call 
  		 * according to parse_analyze in analyze.c 
  		 */
		if (prev_post_parse_analyze_hook)
#if PG_VERSION_NUM < 140000
		 	prev_post_parse_analyze_hook(pstate, query);
#else
		 	prev_post_parse_analyze_hook(pstate, query,js);
#endif
	 }

	elog(DEBUG1, "pg_query_rewrite: pgqr_analyze: exit");
}


/*
 * pgqr_exec
 *
 */
static void pgqr_exec(QueryDesc *queryDesc, int eflags)
{
#if PG_VERSION_NUM > 100000 

	int stmt_loc;
	int stmt_len;
	const char *src;

	if (pgqr_enabled == true && statement_rewritten == true)
	{

		src = queryDesc->sourceText;
		stmt_loc = queryDesc->plannedstmt->stmt_location;
		stmt_len = queryDesc->plannedstmt->stmt_len;

		elog(DEBUG1, "pg_query_rewrite: pgqr_exec: src=%s", src);
		elog(DEBUG1, "pg_query_rewrite: pgqr_exec: stmt_loc=%d", stmt_loc);
		elog(DEBUG1, "pg_query_rewrite: pgqr_exec: stmt_len=%d", stmt_len);

		/*
	 	 * set stmt_location to -1 to avoid assertion failure in pgss_store:
 		 * Assert(query_len <= strlen(query)
	 	 */
		queryDesc->plannedstmt->stmt_location = -1;
		stmt_loc = queryDesc->plannedstmt->stmt_location;
		elog(DEBUG1, "pg_query_rewrite: pgqr_exec: stmt_loc=%d", stmt_loc);
	}

	if (prev_executor_start_hook)
                (*prev_executor_start_hook)(queryDesc, eflags);
	else	standard_ExecutorStart(queryDesc, eflags);
#endif
}

/*
 * on_proc_exit callback
 * 
 */
static void pgqr_exit()
{

	elog(DEBUG1, "pg_query_rewrite: pgqr_exit: entry");

	/* not possible to LWLockAcquire
 	 * TRAP: FailedAssertion("!(!(proc == ((void *)0) && IsUnderPostmaster)
	 * with PGPROC	   *proc = MyProc;
	 */

	pgqr_del_backend_in_proc_array();
	elog(DEBUG1, "pg_query_rewrite: pgqr_exit: exit");
}



PG_FUNCTION_INFO_V1(pgqr_signal);

/*
 * pgqr_signal:
 * could be used to notfiy backend to reload pg_rewrite_rule data
 * into private cache.
 *
 * (from CountDBConnections in procarray.c)
 */
Datum 
pgqr_signal(PG_FUNCTION_ARGS)
{


	int	count = 0;
	int	index;


	elog(DEBUG1, "pg_query_rewrite: pgqr_signal: entry");
	LWLockAcquire(ProcArrayLock, LW_SHARED);

	for (index = 0; index < ProcGlobal->allProcCount; index++)
	{
		PGPROC	*proc = &ProcGlobal->allProcs[index];

		if (proc->pid == 0)
			continue;			/* do not signal prepared xacts */
#if PG_VERSION_NUM > 90600
		if (proc->isBackgroundWorker)
			continue;			/* do not signal background workers */
#endif
		/* no right ProcSignalReason found */
		SendProcSignal(proc->pid , NUM_PROCSIGNALS, proc->backendId);
		elog(DEBUG1, "pg_query_rewrite: pgqr_signal: signal sent to %d", proc->pid);
		count++;
	}


	LWLockRelease(ProcArrayLock);
	elog(DEBUG1, "pg_query_rewrite: pgqr_signal: exit");

	PG_RETURN_INT32(count);
}


