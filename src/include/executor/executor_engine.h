/*-------------------------------------------------------------------------
 *
 * executor_engine.h
 *		HEADER for executor engine
 *
 *
 * 版权所有 (c) 2019-2025, 易景科技保留所有权利。
 * Copyright (c) 2019-2025, Halo Tech Co.,Ltd. All rights reserved.
 *
 * src/include/executor/executor_engine.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXECUTOR_ENGINE_H
#define EXECUTOR_ENGINE_H

#include "executor/executor_api.h"

/* executor engine instance */
extern PGDLLIMPORT const ExecutorRoutine *executorengine;


/* initialize executor engine */
extern void InitExecutorEngine(void);
extern const ExecutorRoutine *GetStandardExecutorEngine(void);
extern void standard_ExecutorStartWithHooks(QueryDesc *queryDesc, int eflags);
extern void standard_ExecutorRunWithHooks(QueryDesc *queryDesc,
										ScanDirection direction, uint64 count);
extern void standard_ProcessUtilityWithHooks(PlannedStmt *pstmt,
											const char *queryString, bool readOnlyTree,
											ProcessUtilityContext context, ParamListInfo params,
											QueryEnvironment *queryEnv, DestReceiver *dest,
											QueryCompletion *qc);
extern PlanState *standard_ExecInitNode(Plan *node, EState *estate, int eflags);
extern void standard_ExecEndNode(PlanState *node);
extern void standard_ExecReScan(PlanState *node);
extern Node *standard_MultiExecProcNode(PlanState *node);
extern void standard_ExecShutdownNode(PlanState *node);
extern ResultRelInfo *standard_ExecFindPartition(ModifyTableState *mtstate,
													 ResultRelInfo *rootResultRelInfo,
													 PartitionTupleRouting *proute,
													 TupleTableSlot *slot, EState *estate);


#endif							/* EXECUTOR_ENGINE_H */
