/*-------------------------------------------------------------------------
 *
 * planner_engine.h
 *		HEADER for planner engine
 *
 *
 * 版权所有 (c) 2019-2025, 易景科技保留所有权利。
 * Copyright (c) 2019-2025, Halo Tech Co.,Ltd. All rights reserved.
 *
 * src/include/optimizer/planner_engine.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANNER_ENGINE_H
#define PLANNER_ENGINE_H

#include "optimizer/planner_api.h"

/* planner engine instance */
extern PGDLLIMPORT const PlannerRoutine *plannerengine;


/* initialize planner engine */
extern void InitPlannerEngine(void);
extern const PlannerRoutine *GetStandardPlannerEngine(void);
extern PlannedStmt *standard_plannerWithHooks(Query *parse, const char *query_string,
											int cursorOptions, ParamListInfo boundParams);

#endif							/* PLANNER_ENGINE_H */
