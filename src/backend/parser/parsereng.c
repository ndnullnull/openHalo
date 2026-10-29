/*-------------------------------------------------------------------------
 *
 * parsereng.c
 *		Parser Engine Helper Routine
 *
 * Portions Copyright (c) 2021, Jet C.X. ZHANG
 *
 * IDENTIFICATION
 *	  src/backend/parser/parsereng.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "parser/parserapi.h"
#include "parser/mysql/mys_parser.h"
#include "utils/mysql/mys_compat.h"

/* GUC variables and the backend-local parser selected during session init. */
int			database_compat_mode = POSTGRESQL_COMPAT_MODE;
int			standard_parserengine_auxiliary = STANDARDARD_PARSERENGINE_AUXILIARY_ON;
const ParserRoutine *parserengine = NULL;

void
InitParserEngine(void)
{
	parserengine = GetStandardParserEngine();
	if (database_compat_mode == MYSQL_COMPAT_MODE && IsMySQLProtocol())
		parserengine = GetMysParserEngine();
}
