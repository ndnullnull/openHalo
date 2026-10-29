/*-------------------------------------------------------------------------
 *
 * mys_parsenodes.h
 *		Definitions for the "mys" parser (flex and bison phases only)
 *
 * Portions Copyright (c) 2021, Jet C.X. ZHANG
 *
 * src/include/parser/mysql/mys_parsenodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MYS_PARSENODES_H
#define MYS_PARSENODES_H

#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"


typedef struct UserVarRef
{
	pg_node_attr(nodetag_number(1000))

	NodeTag type;
	char *userVarName;
	ParseLoc location;
} UserVarRef;


typedef struct UserVarAssign
{
	pg_node_attr(nodetag_number(1001))

	NodeTag type;
	char *userVarName;
	Node *expr;
	ParseLoc location;
} UserVarAssign;


typedef struct SysVarRef
{
	pg_node_attr(nodetag_number(1002))

	NodeTag type;
	char *sysVarName;
	ParseLoc location;
} SysVarRef;


typedef struct MysVariableSetStmt
{
	pg_node_attr(nodetag_number(1003))

	NodeTag type;
	Node *varSetStmt;   /* 目前是SelectStmt */
} MysVariableSetStmt;


typedef struct MysSelectIntoStmt
{
	pg_node_attr(nodetag_number(1004))

	NodeTag type;
	List *target;
	Node *expr;   /* 目前是SelectStmt */
} MysSelectIntoStmt;


#endif							/* MYS_PARSENODES_H */
