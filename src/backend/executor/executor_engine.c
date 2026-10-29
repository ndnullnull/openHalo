/*-------------------------------------------------------------------------
 *
 * executor_engine.c
 *		Executor Engine Helper Routine
 *
 *
 * 版权所有 (c) 2019-2025, 易景科技保留所有权利。
 * Copyright (c) 2019-2025, Halo Tech Co.,Ltd. All rights reserved.
 *
 * IDENTIFICATION
 *	  src/backend/executor/executor_engine.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "executor/executor_engine.h"
#include "executor/mys_executor.h"
#include "utils/mysql/mys_compat.h"

const ExecutorRoutine *executorengine = NULL;

static const ExecutorRoutine standard_executor_engine = {
	.compat_mode = POSTGRESQL_COMPAT_MODE
};

const ExecutorRoutine *
GetStandardExecutorEngine(void)
{
	return &standard_executor_engine;
}

void
InitExecutorEngine(void)
{
	executorengine = IsMySQLCompatMode() ?
		GetMysExecutorEngine() : GetStandardExecutorEngine();
	/* Public PG hooks remain installed and are invoked inside the engine. */
}
