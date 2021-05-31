/*-------------------------------------------------------------------------
 *  
 * pg_query_rewrite is a PostgreSQL extension which allows 
 * to translate a given source SQL statement into another pre-defined 
 * SQL statement. 
 * 
 * This program is open source, licensed under the PostgreSQL license.
 * For license terms, see the LICENSE file.
 *          
 * Copyright (c) 2020, 2021 Pierre Forstmann.
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

#define	PGQR_MAX_STMT_LENGTH		32768	
#define	PGQR_MAX_STMT_BUF_LENGTH	(PGQR_MAX_STMT_LENGTH + 10)

static	bool 	pgqr_enabled = false;
/*
 * maximum number of rules processed
 * by the extension defined as GUC
 */
static int pgqrMaxRules = 0;

/*
 * for pg_stat_statements assertion 
 */
static	bool	statement_rewritten = false;

static	ParseState 	*new_static_pstate = NULL;
static 	Query		*new_static_query = NULL;  

/* Saved hook values in case of unload */
static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static ExecutorStart_hook_type prev_executor_start_hook = NULL;


/*
 * Global shared state:
 * SQL translation rules are stored in shared memory 
 */

typedef struct pgqrSharedItem
{
	Oid	dbid;
	char	source_stmt[PGQR_MAX_STMT_LENGTH];
	char	target_stmt[PGQR_MAX_STMT_LENGTH];
	int	rewrite_count;
} pgqrSharedItem;

typedef struct pgqrSharedState
{
	LWLock 		*lock;
	int		current_rule_number;
	pgqrSharedItem	*rules;

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
static  void 	pgqr_exec(QueryDesc *queryDesc, int eflags);

static void 	pgqr_incr_rewrite_count(int index);

PG_FUNCTION_INFO_V1(pgqr_add_rule);
PG_FUNCTION_INFO_V1(pgqr_rules);
PG_FUNCTION_INFO_V1(pgqr_remove_rule);
PG_FUNCTION_INFO_V1(pgqr_truncate_rule);

/*
 *  Estimate shared memory space needed.
 * 
 */
static Size
pgqr_memsize(void)
{
	Size		size;

	size = MAXALIGN(sizeof(pgqrSharedState));
	size += MAXALIGN(sizeof(pgqrSharedItem) * pgqrMaxRules);

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
		pgqr->rules = (pgqrSharedItem *)ShmemAlloc(pgqrMaxRules * sizeof(pgqrSharedItem));
		MemSet(pgqr->rules, 0, pgqrMaxRules * sizeof(pgqrSharedItem));
		for (i=0; i < pgqrMaxRules; i++)
		{
			pgqr->rules[i].source_stmt[0] = '\0';
			pgqr->rules[i].target_stmt[0] = '\0'; 	
			pgqr->rules[i].rewrite_count = 0;
		}
		pgqr->current_rule_number = 0;

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
				&pgqrMaxRules,
				0,	
				0,
				50,
				PGC_POSTMASTER,	
				0,
				NULL,
				NULL,
				NULL);

	if (pgqrMaxRules == 0)
	{
		elog(LOG, "pg_query_rewrite:_PG_init(): pg_query_rewrite.max_rules not defined");
		elog(LOG, "pg_query_rewrite:_PG_init(): pg_query_rewrite not enabled");
	}
	else	pgqr_enabled = true;
	
	
	if (pgqr_enabled)
	{

		elog(LOG, "pg_query_rewrite:_PG_init(): pg_query_rewrite is enabled with %d rules", 
                          pgqrMaxRules);
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


static bool pgqr_add_rule_internal(char *source, char *target)
{

	LWLockAcquire(pgqr->lock, LW_EXCLUSIVE);

	if (pgqr->current_rule_number == pgqrMaxRules)
	{
		LWLockRelease(pgqr->lock);
		ereport(ERROR, (errmsg("Maximum rule number is reached %d", pgqrMaxRules)));
	}

	if (strlen(source) > PGQR_MAX_STMT_LENGTH - 1)
	{
		LWLockRelease(pgqr->lock);
		ereport(ERROR, (errmsg("Source statement length %zu is greater than %d", 
                               strlen(source), PGQR_MAX_STMT_LENGTH)));
	}

	if (strlen(target) > PGQR_MAX_STMT_LENGTH - 1)
	{
		LWLockRelease(pgqr->lock);
		ereport(ERROR, (errmsg("Target statement length %zu is greater than %d", 
                               strlen(target), PGQR_MAX_STMT_LENGTH)));
	}

	pgqr->rules[pgqr->current_rule_number].dbid = MyDatabaseId;
	strcpy(pgqr->rules[pgqr->current_rule_number].source_stmt, source);
	strcpy(pgqr->rules[pgqr->current_rule_number].target_stmt, target);
	pgqr->current_rule_number++;

	LWLockRelease(pgqr->lock);	
	
	return true;

}

/*
 * pgqr_add_rule
 *
 * SQL-callable function to add a SQL translation rule
 *
 */
Datum pgqr_add_rule(PG_FUNCTION_ARGS)
{

 	 char  *source;
         char  *target;

         source = PG_GETARG_CSTRING(0);
         target = PG_GETARG_CSTRING(1);
         elog(LOG, "pgqr_add_rule source=%s target=%s", source, target);

         PG_RETURN_BOOL(pgqr_add_rule_internal(source, target));	

}


static bool pgqr_remove_rule_internal(char *source)
{

	int	i, j;
	bool	found=false;

	LWLockAcquire(pgqr->lock, LW_EXCLUSIVE);

	for (i = 0; i < pgqr->current_rule_number && found == false; i++)
	{
		if (strcmp(pgqr->rules[i].source_stmt, source) == 0)
			found = true;
	}
	if (found == false)
	{
		LWLockRelease(pgqr->lock);	
		ereport(ERROR, (errmsg("Rule for %s not found", source)));		
	}

	for (j = i - 1; j < pgqr->current_rule_number; j++)	
	{
		pgqr->rules[j].dbid = pgqr->rules[j+1].dbid;
		strcpy(pgqr->rules[j].source_stmt, pgqr->rules[j+1].source_stmt);
		strcpy(pgqr->rules[j].target_stmt, pgqr->rules[j+1].target_stmt);
		pgqr->rules[j].rewrite_count = pgqr->rules[j+1].rewrite_count;
	}	
	pgqr->current_rule_number--;	

	LWLockRelease(pgqr->lock);	
	
	return true;

}

/*
 * pgqr_remove_rule
 *
 * SQL-callable function to remove a SQL translation rule
 *
 */
Datum pgqr_remove_rule(PG_FUNCTION_ARGS)
{

 	 char  *source;

         source = PG_GETARG_CSTRING(0);
         elog(LOG, "pgqr_remove_rule source=%s", source);

         PG_RETURN_BOOL(pgqr_remove_rule_internal(source));	

}


static bool pgqr_truncate_rule_internal()
{
	int	i;

	LWLockAcquire(pgqr->lock, LW_EXCLUSIVE);

	for (i=0; i < pgqrMaxRules; i++)
	{
		pgqr->rules[i].dbid = 0;
		pgqr->rules[i].source_stmt[0] = '\0';
		pgqr->rules[i].target_stmt[0] = '\0';
		pgqr->rules[i].rewrite_count = 0;
        }
	pgqr->current_rule_number = 0;

	LWLockRelease(pgqr->lock);	

	return true;
}

/*
 *  pgqr_truncate_rule
 *  
 *  SQL-callable function to remove all SQL translation rules
 * 
 */
Datum pgqr_truncate_rule(PG_FUNCTION_ARGS)
{

         elog(LOG, "pgqr_truncate_rule");

         PG_RETURN_BOOL(pgqr_truncate_rule_internal());

}


/*
 * check if the current query needs to be rewritten:
 * returns true if must be rewritten, otherwise false;
 * array_index is the position of the target query
 * in pgqr->rules array.
 */
static bool pgqr_check_rewrite(const char *current_query_source, int *rule_index) 
{

	int	i;

	*rule_index = pgqrMaxRules;	

	/*
 	 * To be checked: possible recursion issue ?
	 */

	for (i = 0 ; i < pgqrMaxRules; i++)	
		if (	pgqr->rules[i].dbid == MyDatabaseId &&
			strcmp(current_query_source, pgqr->rules[i].source_stmt) == 0)
		{
			*rule_index = i;
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

	/* 
 	 * >>> FROM parse_analyze in src/backend/parser/analyze.c <<< 
 	 */

	ParseState 	*new_pstate = make_parsestate(NULL);
	Query		*new_query = (Query *)NULL;

#if PG_VERSION_NUM >= 100000
	RawStmt		*new_parsetree;
#else
	Node      	*new_parsetree;
#endif
	List		*raw_parsetree_list;
	ListCell 	*lc1; 

	elog(DEBUG1, "pg_query_rewrite: pgqr_reanalyze: entry");
	new_pstate->p_sourcetext = new_query_string;

	/* 
 	 * missing data: 
         * 1. numParams 
         * 2. ParamTypes 
         * 3. queryEnv
         */

	new_parsetree =  NULL;
	raw_parsetree_list = pg_parse_query(new_query_string);	

	/*
 	 * we assume only one SQL statement
 	 */

	foreach(lc1, raw_parsetree_list)
	{
#if PG_VERSION_NUM >= 100000
		new_parsetree = lfirst_node(RawStmt, lc1);
#else
		new_parsetree = (Node *) lfirst(lc1);
#endif
	}

	new_query = transformTopLevelStmt(new_pstate, new_parsetree);	

	new_static_pstate = new_pstate;
	new_static_query = new_query;

	elog(DEBUG1, "pg_query_rewrite: pgqr_reanalyze: exit");
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
	
	int		rules_index;

	elog(DEBUG1,"pg_query_rewrite: pgqr_analyze: entry: %s",pstate->p_sourcetext);

	statement_rewritten = false;

	/* pstate->p_sourcetext is the current query text */	
	elog(DEBUG1,"pg_query_rewrite: pgqr_analyze: %s",pstate->p_sourcetext);

	if (pgqr_check_rewrite(pstate->p_sourcetext, &rules_index))
	{
		elog(DEBUG1,"pg_query_rewrite: pgqr_to_rewrite %s: rc=true", 
                                    pstate->p_sourcetext);
		/* 
 		** analyze destination statement 
		*/
		pgqr_reanalyze(pgqr->rules[rules_index].target_stmt);

		/* clone data */
		pgqr_clone_ParseState(new_static_pstate, pstate);
		elog(DEBUG1,"pg_query_rewrite: pgqr_analyze: rewrite=true pstate->p_source_text %s",
                                   pstate->p_sourcetext);
		pgqr_clone_Query(new_static_query, query);
		statement_rewritten = true;
		
		pgqr_incr_rewrite_count(rules_index);

		free_parsestate(new_static_pstate); 
	} else
		elog(DEBUG1,"pg_query_rewrite: pgqr_to_rewrite %s: rc=false", 
                             pstate->p_sourcetext);

	/* no "standard_analyze" to call 
  	 * according to parse_analyze in analyze.c 
  	 */
	if (prev_post_parse_analyze_hook)
	{
#if PG_VERSION_NUM < 140000
		prev_post_parse_analyze_hook(pstate, query);
#else
		prev_post_parse_analyze_hook(pstate, query, js);
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
#endif
	/*
 	 * must always execute here whatever PG_VERSION_NUM
 	 */

	if (prev_executor_start_hook)
                (*prev_executor_start_hook)(queryDesc, eflags);
	else	standard_ExecutorStart(queryDesc, eflags);
}

/*
 * 
 *  pgqr_rules: SQL-callable function to display shared rules 
 *  
 */


static Datum pgqr_rules_internal(FunctionCallInfo fcinfo)
{
        ReturnSetInfo   *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
        bool            randomAccess;
        TupleDesc       tupdesc;
        Tuplestorestate *tupstore;
        AttInMetadata    *attinmeta;
        MemoryContext   oldcontext;
        int             i;

        /* The tupdesc and tuplestore must be created in ecxt_per_query_memory */
        oldcontext = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
#if PG_VERSION_NUM <= 120000
        tupdesc = CreateTemplateTupleDesc(4, false);
#else
        tupdesc = CreateTemplateTupleDesc(4);
#endif
        TupleDescInitEntry(tupdesc, (AttrNumber) 1, "datname", TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 2, "source", TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 3, "target", TEXTOID, -1, 0);
        TupleDescInitEntry(tupdesc, (AttrNumber) 4, "rewrite_count", TEXTOID, -1, 0);

        randomAccess = (rsinfo->allowedModes & SFRM_Materialize_Random) != 0;
        tupstore = tuplestore_begin_heap(randomAccess, false, work_mem);
        rsinfo->returnMode = SFRM_Materialize;
        rsinfo->setResult = tupstore;
        rsinfo->setDesc = tupdesc;

        MemoryContextSwitchTo(oldcontext);

        attinmeta = TupleDescGetAttInMetadata(tupdesc);

        for (i=0; i < pgqrMaxRules; i++)
        {
                char            *values[4];
                HeapTuple       tuple;
                char            buf_v1[NAMEDATALEN];
                char            buf_v2[PGQR_MAX_STMT_BUF_LENGTH];
                char            buf_v3[PGQR_MAX_STMT_BUF_LENGTH];
                char            buf_v4[50];
		char		*source;
	        char   		*target;
		char		*p_source;
		char		*p_target;
        	char    	*null_string = "NULL";

		source = pgqr->rules[i].source_stmt;
                target = pgqr->rules[i].target_stmt;

		if (strlen(source) == 0)
                        p_source = null_string;
                else    p_source = source;
                if (strlen(target) == 0)
                        p_target = null_string;
                else    p_target = target;
               
		if (pgqr->rules[i].dbid != 0) 
			snprintf(buf_v1, sizeof(buf_v1), "datname=%s", get_database_name(pgqr->rules[i].dbid));
		else
			snprintf(buf_v1, sizeof(buf_v1), "datname=%s", null_string);
			
                values[0] = buf_v1;

		snprintf(buf_v2, sizeof(buf_v2), "source=%s", p_source);
                values[1] = buf_v2;

                snprintf(buf_v3, sizeof(buf_v3), "target=%s", p_target);
                values[2] = buf_v3;

                snprintf(buf_v4, sizeof(buf_v4), "rewrite_count=%d", pgqr->rules[i].rewrite_count);
                values[3] = buf_v4;

        	tuple = BuildTupleFromCStrings(attinmeta, values);
	        tuplestore_puttuple(tupstore, tuple);

        }

        return (Datum)0;

}

Datum pgqr_rules(PG_FUNCTION_ARGS)
{

        return (pgqr_rules_internal(fcinfo));
}

static void pgqr_incr_rewrite_count(int index)
{
	
        LWLockAcquire(pgqr->lock, LW_EXCLUSIVE);
        pgqr->rules[index].rewrite_count++ ;
        LWLockRelease(pgqr->lock);

}

