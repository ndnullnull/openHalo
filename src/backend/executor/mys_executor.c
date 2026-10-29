/*-------------------------------------------------------------------------
 *
 * mys_executor.c
 *		Mysql Executor Engine
 *
 *
 * 版权所有 (c) 2019-2025, 易景科技保留所有权利。
 * Copyright (c) 2019-2025, Halo Tech Co.,Ltd. All rights reserved.
 *
 * IDENTIFICATION
 *	  src/backend/executor/mys_executor.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "executor/executor.h"
#include "executor/executor_engine.h"
#include "executor/mys_executor.h"

static PartitionRoutine mys_partition_engine = {
	.ExecFindPartition = mys_ExecFindPartition
};

static const ExecutorRoutine mys_executor_engine = {
	.compat_mode = MYSQL_COMPAT_MODE,
	.ExecutorStart = mys_ExecutorStart,
	.ExecutorRun = mys_ExecutorRun,
	.ProcessUtility = mys_standard_ProcessUtility,
	.ExecInitNode = mys_ExecInitNode,
	.ExecEndNode = mys_ExecEndNode,
	.ExecReScan = mys_ExecReScan,
	.partition = &mys_partition_engine
};

const ExecutorRoutine *
GetMysExecutorEngine(void)
{
	return &mys_executor_engine;
}

/* PG18's shared implementations include the ported MySQL node semantics.
 * Keeping the engine callbacks here avoids a divergent PG14 executor copy. */
void
mys_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	standard_ExecutorStartWithHooks(queryDesc, eflags);
}

void
mys_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count)
{
	standard_ExecutorRunWithHooks(queryDesc, direction, count);
}

void
mys_standard_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
							bool readOnlyTree, ProcessUtilityContext context,
							ParamListInfo params, QueryEnvironment *queryEnv,
							DestReceiver *dest, QueryCompletion *qc)
{
	standard_ProcessUtilityWithHooks(pstmt, queryString, readOnlyTree,
									context, params, queryEnv, dest, qc);
}

PlanState *
mys_ExecInitNode(Plan *node, EState *estate, int eflags)
{
	return standard_ExecInitNode(node, estate, eflags);
}

void
mys_ExecEndNode(PlanState *node)
{
	standard_ExecEndNode(node);
}

void
mys_ExecReScan(PlanState *node)
{
	standard_ExecReScan(node);
}

ResultRelInfo *
mys_ExecFindPartition(ModifyTableState *mtstate, ResultRelInfo *rootResultRelInfo,
					 PartitionTupleRouting *proute, TupleTableSlot *slot, EState *estate)
{
	return standard_ExecFindPartition(mtstate, rootResultRelInfo, proute, slot, estate);
}

void
ExecWithCheckOptions2(WCOKind kind, ResultRelInfo *resultRelInfo,
					 TupleTableSlot *slot, EState *estate)
{
	ExecWithCheckOptions(kind, resultRelInfo, slot, estate);
}
