/*-------------------------------------------------------------------------
 *
 * executor_ep.c
 *		Executor Engine Entry Point
 *
 *
 * 版权所有 (c) 2019-2025, 易景科技保留所有权利。
 * Copyright (c) 2019-2025, Halo Tech Co.,Ltd. All rights reserved.
 *
 * IDENTIFICATION
 *	  src/backend/executor/executor_ep.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "executor/executor.h"
#include "executor/executor_engine.h"

PlanState *
ExecInitNode(Plan *node, EState *estate, int eflags)
{
	if (executorengine && executorengine->ExecInitNode)
		return executorengine->ExecInitNode(node, estate, eflags);
	return standard_ExecInitNode(node, estate, eflags);
}

void
ExecEndNode(PlanState *node)
{
	if (executorengine && executorengine->ExecEndNode)
		executorengine->ExecEndNode(node);
	else
		standard_ExecEndNode(node);
}

void
ExecReScan(PlanState *node)
{
	if (executorengine && executorengine->ExecReScan)
		executorengine->ExecReScan(node);
	else
		standard_ExecReScan(node);
}

Node *
MultiExecProcNode(PlanState *node)
{
	if (executorengine && executorengine->MultiExecProcNode)
		return executorengine->MultiExecProcNode(node);
	return standard_MultiExecProcNode(node);
}

void
ExecShutdownNode(PlanState *node)
{
	if (executorengine && executorengine->ExecShutdownNode)
		executorengine->ExecShutdownNode(node);
	else
		standard_ExecShutdownNode(node);
}

ResultRelInfo *
ExecFindPartition(ModifyTableState *mtstate, ResultRelInfo *rootResultRelInfo,
				  PartitionTupleRouting *proute, TupleTableSlot *slot, EState *estate)
{
	if (executorengine && executorengine->partition &&
		executorengine->partition->ExecFindPartition)
		return executorengine->partition->ExecFindPartition(mtstate,
										rootResultRelInfo, proute, slot, estate);
	return standard_ExecFindPartition(mtstate, rootResultRelInfo, proute, slot, estate);
}
