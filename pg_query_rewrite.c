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
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/guc.h"
#include "utils/snapmgr.h"
#include "pgstat.h"

PG_MODULE_MAGIC;

static	bool 	pgqr_enabled = false;
static	const 	char	*query_text;

static	ParseState *new_static_pstate;
static  Query	  *new_static_query;  

static char	*pgqr_source_stmt = NULL;
static char	*pgqr_destination_stmt = NULL;

/* Saved hook values in case of unload */
static post_parse_analyze_hook_type prev_post_parse_analyze_hook = NULL;

/*---- Function declarations ----*/

void		_PG_init(void);
void		_PG_fini(void);

static 	void 	pgqr_analyze(ParseState *pstate, Query *query);
static	void	pgqr_reanalyze(const char *new_query_string);


/*
 * Module load callback
 */
void
_PG_init(void)
{
	elog(DEBUG5, "pg_query_rewrite:_PG_init():entry");

	/* get the configuration */
	DefineCustomStringVariable("pg_query_rewrite.source",
				   "source statement",
			    	    NULL,
				    &pgqr_source_stmt,
				    NULL,
			     	    PGC_SIGHUP,
			            0,
				    NULL,
				    NULL,
				    NULL);
	DefineCustomStringVariable("pg_query_rewrite.destination",
				   "destination statement",
			    	    NULL,
				    &pgqr_destination_stmt,
				    NULL,
			     	    PGC_SIGHUP,
			            0,
				    NULL,
				    NULL,
				    NULL);
	if (pgqr_source_stmt != NULL && pgqr_destination_stmt != NULL)
		pgqr_enabled = true;

	if (pgqr_enabled)
	{
		ereport(LOG, (errmsg("pg_query_rewrite:_PG_init(): pg_query_rewrite is enabled")));
		prev_post_parse_analyze_hook = post_parse_analyze_hook;
		post_parse_analyze_hook = pgqr_analyze;
	}
	else
		ereport(LOG, (errmsg("pg_query_rewrite:_PG_init(): pg_query_rewrite is not enabled")));


	elog(DEBUG5, "pg_query_rewrite:_PG_init():exit");
}


/*
 *  Module unload callback
 */
void
_PG_fini(void)
{
	
	post_parse_analyze_hook = prev_post_parse_analyze_hook;
}

static bool pgqr_to_rewrite(const char *source) 
{
	if (strcmp(source, pgqr_source_stmt) == 0)
		return true;
	else
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
                         ("pg_query_rewrite: cannot rewrite multiple to commands: %s", 
                          new_query_string)));
	}

	/* cannot run free_parsestate(new_pstate) */

	new_static_pstate = new_pstate;
	new_static_query = new_query;

	elog(DEBUG1, "pg_query_rewrite: pgqr_reanalyze: exit");
}
/*
 *
 * pqqr_analyze
 *
 */

static void pgqr_analyze(ParseState *pstate, Query *query)
{

	elog(DEBUG1, "pg_query_rewrite: pgqr_analyze: entry");
	if (pgqr_enabled)
	{
		query_text = pstate->p_sourcetext;
		elog(DEBUG1,"pg_query_rewrite: pgqr_analyze: %s",pstate->p_sourcetext);
		if (pgqr_to_rewrite(query_text))
		{
			pgqr_reanalyze(pgqr_destination_stmt);
			/* clone data */
			pgqr_clone_ParseState(new_static_pstate, pstate);
			elog(DEBUG1,"pg_query_rewrite: pgqr_analyze: pstate %s",
                                    pstate->p_sourcetext);
			pgqr_clone_Query(new_static_query, query);
		}

		/* no "standard_analyze" to call 
  		 * according to parse_analyze in analyze.c 
  		 */
		if (prev_post_parse_analyze_hook)
		 	prev_post_parse_analyze_hook(pstate, query);
	 }

	elog(DEBUG1, "pg_query_rewrite: pgqr_analyze: exit");
}
