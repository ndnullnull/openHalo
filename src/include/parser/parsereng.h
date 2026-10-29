/*-------------------------------------------------------------------------
 *
 * parsereng.h
 *		HEADER for parser engine
 *
 *
 * Portions Copyright (c) 2021, Jet C.X. ZHANG
 *
 * src/include/parser/parsereng.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSERENG_H
#define PARSERENG_H

typedef enum
{
	POSTGRESQL_COMPAT_MODE,
	MYSQL_COMPAT_MODE,
} DatabaseCompatModeType;

#include "parser/parser.h"

typedef enum
{
	STANDARDARD_PARSERENGINE_AUXILIARY_OFF,
	STANDARDARD_PARSERENGINE_AUXILIARY_ON
} StandardParserengineAuxiliary;

/* 前向声明 */
typedef struct ParserRoutine ParserRoutine;

/* GUC variables */
extern unsigned long stmtLen;
extern PGDLLIMPORT int database_compat_mode;
extern PGDLLIMPORT int standard_parserengine_auxiliary;

/* Raw Parser Routine */
extern PGDLLIMPORT const ParserRoutine *parserengine;
extern const ParserRoutine *GetStandardParserEngine(void);

/* Set Gloabal Parser Engine */
extern void InitParserEngine(void);

#endif							/* PARSERENG_H */
