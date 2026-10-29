#ifndef MYS_CREATEAS_H
#define MYS_CREATEAS_H

#include "catalog/objectaddress.h"
#include "nodes/parsenodes.h"
#include "parser/parse_node.h"
#include "tcop/cmdtag.h"
#include "utils/queryenvironment.h"

extern Query *mys_PrepareCreateTableAs(ParseState *pstate, CreateTableAsStmt *stmt,
                                     ParamListInfo params, QueryEnvironment *queryEnv,
                                     ObjectAddress *address);
extern ObjectAddress mys_ExecCreateTableAs(ParseState *pstate,
                                         CreateTableAsStmt *stmt,
                                         ParamListInfo params,
                                         QueryEnvironment *queryEnv,
                                         QueryCompletion *qc);

#endif
