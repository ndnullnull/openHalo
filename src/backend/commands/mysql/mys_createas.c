/*
 * MySQL CREATE TABLE ... SELECT on PG18's CREATE/INSERT paths.
 * Keep definition processing, constraints, triggers and permissions in the
 * standard machinery rather than copying the PG14 tuple receiver/executor.
 */
#include "postgres.h"

#include "access/table.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "commands/createas.h"
#include "commands/mysql/mys_createas.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/queryjumble.h"
#include "optimizer/optimizer.h"
#include "parser/analyze.h"
#include "parser/mysql/mys_parse_utilcmd.h"
#include "parser/parse_node.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteHandler.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

static bool
mysql_ctas_has_sequence(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, NextValueExpr) ||
		(IsA(node, FuncExpr) && ((FuncExpr *) node)->funcid == F_NEXTVAL))
		return true;
	return expression_tree_walker(node, mysql_ctas_has_sequence, context);
}

/* A direct source column can carry its default and NOT NULL into the copy.
 * Expressions and NULL-extended join columns must not inherit those properties.
 * Follow simple subqueries/CTEs and join aliases without matching by name.
 */
static bool
mysql_ctas_source_options(ColumnDef *col, Node *expr, List *parents)
{
	Var		   *var;
	RangeTblEntry *rte;
	Query	   *query;

	check_stack_depth();
	if (!IsA(expr, Var))
		return false;
	var = (Var *) expr;
	if (var->varlevelsup >= list_length(parents) || var->varattno <= 0 ||
		var->varnullingrels != NULL)
		return false;
	if (var->varlevelsup > 0)
		parents = list_copy_tail(parents, var->varlevelsup);
	query = linitial_node(Query, parents);
	rte = rt_fetch(var->varno, query->rtable);
	if (rte->rtekind == RTE_RELATION)
	{
		Relation rel = table_open(rte->relid, AccessShareLock);
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), var->varattno - 1);
		bool on_update = existAutoUpdateTrigOnThisAtt(rel, att);

		col->is_not_null = att->attnotnull;
		if (att->attgenerated == '\0')
		{
			Node *def = build_column_default(rel, var->varattno);

			/* An auto-increment source is copied as an ordinary DEFAULT 0
			 * column, never as another consumer of the original sequence. */
			if (att->attidentity != '\0' || mysql_ctas_has_sequence(def, NULL))
			{
				A_Const *zero = makeNode(A_Const);

				zero->val.ival.type = T_Integer;
				zero->val.ival.ival = 0;
				zero->location = -1;
				col->raw_default = (Node *) zero;
			}
			else
				col->cooked_default = def;
		}
		table_close(rel, NoLock);
		return on_update;
	}
	else if (rte->rtekind == RTE_SUBQUERY)
	{
		TargetEntry *tle = get_tle_by_resno(rte->subquery->targetList, var->varattno);

		if (tle && !tle->resjunk)
			return mysql_ctas_source_options(col, (Node *) tle->expr,
										 lcons(rte->subquery, parents));
	}
	else if (rte->rtekind == RTE_JOIN && rte->joinaliasvars != NIL)
		return mysql_ctas_source_options(col, list_nth(rte->joinaliasvars, var->varattno - 1), parents);
	else if (rte->rtekind == RTE_GROUP)
		return mysql_ctas_source_options(col, list_nth(rte->groupexprs, var->varattno - 1), parents);
	else if (rte->rtekind == RTE_CTE && !rte->self_reference &&
			 rte->ctelevelsup < list_length(parents))
	{
		ListCell *lc;

		parents = list_copy_tail(parents, rte->ctelevelsup);
		query = linitial_node(Query, parents);
		foreach(lc, query->cteList)
		{
			CommonTableExpr *cte = lfirst_node(CommonTableExpr, lc);

			if (strcmp(cte->ctename, rte->ctename) == 0 && IsA(cte->ctequery, Query))
			{
				Query *subquery = (Query *) cte->ctequery;
				TargetEntry *tle = get_tle_by_resno(subquery->targetList, var->varattno);

				if (tle && !tle->resjunk)
					return mysql_ctas_source_options(col, (Node *) tle->expr,
												 lcons(subquery, parents));
			}
		}
	}
	return false;
}

static void
mysql_ctas_utility(Node *node, ParseState *pstate, ParamListInfo params,
				   QueryEnvironment *queryEnv)
{
	PlannedStmt *utility = makeNode(PlannedStmt);

	utility->commandType = CMD_UTILITY;
	utility->utilityStmt = node;
	utility->stmt_location = -1;
	ProcessUtility(utility, pstate->p_sourcetext, false, PROCESS_UTILITY_SUBCOMMAND,
				   params, queryEnv, None_Receiver, NULL);
	CommandCounterIncrement();
}

static bool
mysql_ctas_has_output(List *targetList, const char *name)
{
	ListCell *lc;

	foreach(lc, targetList)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		if (!tle->resjunk && pg_strcasecmp(tle->resname, name) == 0)
			return true;
	}
	return false;
}

Query *
mys_PrepareCreateTableAs(ParseState *pstate, CreateTableAsStmt *stmt,
						ParamListInfo params, QueryEnvironment *queryEnv,
						ObjectAddress *address)
{
	Query	   *source = castNode(Query, stmt->query);
	IntoClause *into = stmt->into;
	CreateStmt *create = makeNode(CreateStmt);
	InsertStmt *insert = makeNode(InsertStmt);
	List	   *columns = NIL;
	List	   *on_update = NIL;
	ListCell   *lc;
	Oid			relid;
	Query	   *query;
	JumbleState *jstate = NULL;

	*address = InvalidObjectAddress;
	if (CreateTableAsRelExists(stmt))
		return NULL;
	Assert(stmt->objtype == OBJECT_TABLE && into->viewQuery == NULL);

	/* Non-overlapping explicit columns precede columns produced by SELECT. */
	foreach(lc, into->colNames)
	{
		Node *node = lfirst(lc);

		if (IsA(node, ColumnDef))
		{
			ColumnDef *col = (ColumnDef *) node;

			if (!mysql_ctas_has_output(source->targetList, col->colname))
				columns = lappend(columns, copyObject(col));
		}
		else if (!IsA(node, Constraint))
			elog(ERROR, "unexpected MySQL CREATE TABLE AS definition node");
	}
	foreach(lc, source->targetList)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);
		ColumnDef *col = NULL;
		ListCell *dc;
		ResTarget *target;

		if (tle->resjunk)
			continue;
		foreach(dc, into->colNames)
		{
			Node *node = lfirst(dc);

			if (IsA(node, ColumnDef) &&
				pg_strcasecmp(((ColumnDef *) node)->colname, tle->resname) == 0)
			{
				if (col != NULL)
					ereport(ERROR, (errcode(ERRCODE_DUPLICATE_COLUMN),
								errmsg("column \"%s\" specified more than once", tle->resname)));
				col = copyObject((ColumnDef *) node);
			}
		}
		if (col == NULL)
		{
			col = makeColumnDef(tle->resname, exprType((Node *) tle->expr),
								exprTypmod((Node *) tle->expr), exprCollation((Node *) tle->expr));
			if (!OidIsValid(col->collOid) && type_is_collatable(col->typeName->typeOid))
				ereport(ERROR, (errcode(ERRCODE_INDETERMINATE_COLLATION),
								errmsg("no collation was derived for column \"%s\"", col->colname)));
			if (mysql_ctas_source_options(col, (Node *) tle->expr, list_make1(source)))
				on_update = lappend(on_update, col->colname);
		}
		columns = lappend(columns, col);
		target = makeNode(ResTarget);
		target->name = pstrdup(col->colname);
		target->location = -1;
		insert->cols = lappend(insert->cols, target);
	}
	foreach(lc, into->colNames)
		if (IsA(lfirst(lc), Constraint))
			columns = lappend(columns, copyObject(lfirst(lc)));

	create->relation = copyObject(into->rel);
	create->tableElts = columns;
	create->options = copyObject(into->options);
	create->oncommit = into->onCommit;
	create->tablespacename = into->tableSpaceName;
	create->accessMethod = into->accessMethod;
	mysql_ctas_utility((Node *) create, pstate, params, queryEnv);
	relid = RangeVarGetRelid(into->rel, NoLock, false);
	ObjectAddressSet(*address, RelationRelationId, relid);
	foreach(lc, on_update)
	{
		char *name = lfirst(lc);
		char *schema = get_namespace_name(get_rel_namespace(relid));
		char *table = get_rel_name(relid);
		CreateFunctionStmt *function = createAutoUpdateTimeStampTriggerFunc(schema, table, name);
		CreateTrigStmt *trigger = createAutoUpdateTimeStampTrigger(schema, table, name);

		mysql_ctas_utility((Node *) function, pstate, params, queryEnv);
		mysql_ctas_utility((Node *) trigger, pstate, params, queryEnv);
		mysql_ctas_utility((Node *) bindTriggerFunctionToTrigger(trigger->funcname,
															trigger->relation, trigger->trigname),
						 pstate, params, queryEnv);
		mysql_ctas_utility((Node *) bindTriggerToColumn(trigger->relation, trigger->trigname, name),
						 pstate, params, queryEnv);
	}

	if (into->skipData)
		return NULL;
	/* Qualify the newly created relation before transforming INSERT. */
	insert->relation = makeRangeVar(get_namespace_name(get_rel_namespace(relid)),
								   get_rel_name(relid), -1);
	insert->selectStmt = copyObject(stmt->mysql_query);
	query = parse_sub_analyze((Node *) insert, pstate, NULL, false, true);
	if (IsQueryIdEnabled())
		jstate = JumbleQuery(query);
	if (post_parse_analyze_hook)
		(*post_parse_analyze_hook) (pstate, query, jstate);
	return query;
}

ObjectAddress
mys_ExecCreateTableAs(ParseState *pstate, CreateTableAsStmt *stmt,
					 ParamListInfo params, QueryEnvironment *queryEnv,
					 QueryCompletion *qc)
{
	ObjectAddress address;
	uint64 processed = 0;
	Query *query = mys_PrepareCreateTableAs(pstate, stmt, params, queryEnv, &address);

	if (query)
	{
		List *rewritten;
		List *plans;
		ListCell *lc;

		rewritten = QueryRewrite(query);
		plans = pg_plan_queries(rewritten, pstate->p_sourcetext,
								CURSOR_OPT_PARALLEL_OK, params);
		PushCopiedSnapshot(GetActiveSnapshot());
		UpdateActiveSnapshotCommandId();
		foreach(lc, plans)
		{
			PlannedStmt *plan = lfirst_node(PlannedStmt, lc);
			QueryDesc *desc;

			if (plan->commandType == CMD_UTILITY)
				ProcessUtility(plan, pstate->p_sourcetext, false, PROCESS_UTILITY_SUBCOMMAND,
							   params, queryEnv, None_Receiver, NULL);
			else
			{
				desc = CreateQueryDesc(plan, pstate->p_sourcetext,
									   GetActiveSnapshot(), InvalidSnapshot,
									   None_Receiver, params, queryEnv, 0);
				ExecutorStart(desc, 0);
				ExecutorRun(desc, ForwardScanDirection, 0);
				if (plan->canSetTag)
					processed = desc->estate->es_processed;
				ExecutorFinish(desc);
				ExecutorEnd(desc);
				FreeQueryDesc(desc);
			}
			CommandCounterIncrement();
			UpdateActiveSnapshotCommandId();
		}
		PopActiveSnapshot();
	}
	if (qc)
		SetQueryCompletion(qc, CMDTAG_CREATE_TABLE_AS, processed);
	return address;
}
