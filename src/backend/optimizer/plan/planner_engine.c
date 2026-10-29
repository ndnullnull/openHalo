/*-------------------------------------------------------------------------
 *
 * planner_engine.c
 *		Planner Engine Helper Routine
 *
 *
 * 版权所有 (c) 2019-2025, 易景科技保留所有权利。
 * Copyright (c) 2019-2025, Halo Tech Co.,Ltd. All rights reserved.
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/plan/planner_engine.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "optimizer/planner_engine.h"

const PlannerRoutine *plannerengine = NULL;

static const PlannerRoutine standard_planner_engine = {
	.compat_mode = POSTGRESQL_COMPAT_MODE,
	.planner = standard_plannerWithHooks
};

const PlannerRoutine *
GetStandardPlannerEngine(void)
{
	return &standard_planner_engine;
}

void
InitPlannerEngine(void)
{
	/* The frozen MySQL mode also selects the standard PG planner. */
	plannerengine = GetStandardPlannerEngine();
}
