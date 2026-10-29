/* MySQL connection-specific ADT behavior; native and background calls keep PG semantics. */
#ifndef MYS_COMPAT_H
#define MYS_COMPAT_H

#include <ctype.h>

#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "parser/parsereng.h"

extern bool isStrictTransTablesOn;

static inline bool
IsMySQLProtocol(void)
{
    return MyProcPort && MyProcPort->protocol_handler &&
        MyProcPort->protocol_handler->type == PROTOCOL_MYSQL;
}

static inline bool
IsMySQLCompatMode(void)
{
    return database_compat_mode == MYSQL_COMPAT_MODE && IsMySQLProtocol();
}

static inline bool
MySQLNonStrictEmptyInput(const char *str)
{
    if (!IsMySQLCompatMode() || isStrictTransTablesOn)
        return false;
    while (isspace((unsigned char) *str))
        str++;
    return *str == '\0';
}

#endif
