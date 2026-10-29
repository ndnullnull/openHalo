/*-------------------------------------------------------------------------
 *
 * systemVar.c
 *	  MySQL adapter systemVar routines
 *
 *
 * 版权所有 (c) 2019-2025, 易景科技保留所有权利。
 * Copyright (c) 2019-2025, Halo Tech Co.,Ltd. All rights reserved.
 *
 * 易景科技是Halo Database、Halo Database Management System、羲和数据
 * 库、羲和数据库管理系统（后面简称 Halo ）、openHalo软件的发明人同时也为
 * 知识产权权利人。Halo 软件的知识产权，以及与本软件相关的所有信息内容（包括
 * 但不限于文字、图片、音频、视频、图表、界面设计、版面框架、有关数据或电子文
 * 档等）均受中华人民共和国法律法规和相应的国际条约保护，易景科技享有上述知识
 * 产权，但相关权利人依照法律规定应享有的权利除外。未免疑义，本条所指的“知识
 * 产权”是指任何及所有基于 Halo 软件产生的：（a）版权、商标、商号、域名、与
 * 商标和商号相关的商誉、设计和专利；与创新、技术诀窍、商业秘密、保密技术、非
 * 技术信息相关的权利；（b）人身权、掩模作品权、署名权和发表权；以及（c）在
 * 本协议生效之前已存在或此后出现在世界任何地方的其他工业产权、专有权、与“知
 * 识产权”相关的权利，以及上述权利的所有续期和延长，无论此类权利是否已在相
 * 关法域内的相关机构注册。
 *
 * This software and related documentation are provided under a license
 * agreement containing restrictions on use and disclosure and are
 * protected by intellectual property laws. Except as expressly permitted
 * in your license agreement or allowed by law, you may not use, copy,
 * reproduce, translate, broadcast, modify, license, transmit, distribute,
 * exhibit, perform, publish, or display any part, in any form, or by any
 * means. Reverse engineering, disassembly, or decompilation of this
 * software, unless required by law for interoperability, is prohibited.
 *
 * This software is developed for general use in a variety of
 * information management applications. It is not developed or intended
 * for use in any inherently dangerous applications, including applications
 * that may create a risk of personal injury. If you use this software or
 * in dangerous applications, then you shall be responsible to take all
 * appropriate fail-safe, backup, redundancy, and other measures to ensure
 * its safe use. Halo Corporation and its affiliates disclaim any
 * liability for any damages caused by use of this software in dangerous
 * applications.
 *
 *
 * IDENTIFICATION
 *	  src/backend/adapter/mysql/systemVar.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "pgtime.h"
#include "parser/parserapi.h"
#include "commands/prepare.h"
#include "nodes/nodeFuncs.h"
#include "nodes/mysql/mys_parsenodes.h"
#include "utils/plancache.h"
#include "utils/builtins.h"
#include "access/relscan.h"
#include "access/sdir.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/namespace.h"
#include "executor/tuptable.h"
#include "storage/ipc.h"
#include "storage/lockdefs.h"
#include "storage/shmem.h"
#include "storage/lwlock.h"
#include "storage/spin.h"
#include "utils/elog.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/palloc.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/varlena.h"
#include "adapter/mysql/systemVar.h"
#include <time.h>


#define SYSTEM_VAR_NAME_MAX_LEN 128
#define SYSTEM_VAR_VALUE_MAX_LEN 512
#define SYSTEM_VAR_INT_NUM 512
#define SYSTEM_VAR_MAX_NUM 1024
#define SYSTEM_VAR_RESULT_RULE_NUM 16
#define SYSTEM_VAR_RESULT_RULE_LEN 64

uint64 mys_sqlMode = MYS_MODE_ONLY_FULL_GROUP_BY | MYS_MODE_STRICT_TRANS_TABLES | MYS_MODE_NO_ZERO_IN_DATE |
                     MYS_MODE_NO_ZERO_DATE | MYS_MODE_ERROR_FOR_DIVISION_BY_ZERO | MYS_MODE_NO_AUTO_CREATE_USER |
                     MYS_MODE_NO_ENGINE_SUBSTITUTION;
int autoCommit = 2;
int default_week_format = 0;
extern bool needCommitTrx;
extern bool needStartNewTrx;
extern bool isStrictTransTablesOn;

HTAB *globalSystemVars = NULL;

typedef struct
{
    LWLock mutex;
    int startupIsolation;
    bool startupReadOnly;
} GlobalSystemVarsMutex;
static GlobalSystemVarsMutex *globalSystemVarsMutex;

typedef struct
{
    char varName[SYSTEM_VAR_NAME_MAX_LEN];   /* hash key must be first */
    char varDefValue[SYSTEM_VAR_VALUE_MAX_LEN];
    char varConfValue[SYSTEM_VAR_VALUE_MAX_LEN];
    int sessionGlobalType;  /* 0:global & session, 1:global, 2:session, 3:sess-global */
    bool sessionDefValueFromGlobal;
    bool isReadWrite;
    int resultNum;
    char result[SYSTEM_VAR_RESULT_RULE_NUM][SYSTEM_VAR_RESULT_RULE_LEN];
    int selectResultNum;
    char selectResult[SYSTEM_VAR_RESULT_RULE_NUM][SYSTEM_VAR_RESULT_RULE_LEN];
    int showResultNum;
    char showResult[SYSTEM_VAR_RESULT_RULE_NUM][SYSTEM_VAR_RESULT_RULE_LEN];
} SystemVar;

static SystemVar *baseSystemVars[SYSTEM_VAR_MAX_NUM];
static int baseSystemVarsNum = 0;
static HTAB *sessionSystemVars = NULL;
static bool nextIsolationSet;
static bool nextReadOnlySet;
static bool nextTransactionApplied;
static int nextIsolation;
static bool nextReadOnly;
static char lastNativeTimeZone[TZ_STRLEN_MAX + 1];
static int lastNativeIsolation;
static bool lastNativeReadOnly;
static bool sessionTimeZoneFromNative;

static List *readBaseVariableRecord(TupleTableSlot *slot);
static List *readBaseVariablesTable();
static bool add2BaseSystemVars(SystemVar *systemVar);
static void add2GlobalSystemVars(SystemVar *systemVar);
static void add2SessionSystemVars(SystemVar *systemVar);
static SystemVar *initBaseSystemVar(List *systemVarColumnVals);
static void initSystemVariables(void);
static void initBaseSystemVars(void);
static void initGlobalSystemVars(SystemVar *systemVars[], int systemVarsNum);
static void initSessionSystemVars(SystemVar *systemVars[], int systemVarsNum);
static SystemVar *dynAddSystemVar(char *varName);
static void lockGlobalSystemVars(void);
static void unlockGlobalSystemVars(void);
static void lowerSystemVarName(char *varName);
static bool asignSystemVar(const char *varDefValue,
                           const char* varConfValue,
                           int sessionGlobalType,
                           bool sessionDefValueFromGlobal,
                           bool isReadWrite,
                           const char* result,
                           const char* selectResult,
                           const char* showResult,
                           SystemVar *systemVar);
static bool asignSystemVarResult(const char *srcResult,
                                 int *resultNum,
                                 char *result);
static void splitStr(const char *str, char separator, int itemSize,
                     char *items, int *itemsNum);
static void copySystemVar(SystemVar *srcSystemVar, SystemVar *destSystemVar);
static void setSystemVarValueByVarName(const char *varName,
                                       const char *value, char *varValue);
static void setSystemVarValueForOptimizerSwitch(const char *value,
                                                char *varValue);
static void setSystemVarValue(const char* value, char *varValue);
static void rectifySystemVarValue(const char *varName, char **varValue,
                                  bool isSessionSystemVar);
static void getDefaultValue(const char *varName, bool isSessionSystemVar,
                            char **varValue);
static void applySystemVarValue(const char *varName, const char *varValue);
static int getSystemVariableValueImpl(char *varName,
                                      bool isSessionSystemVar,
                                      int selectShowType,
                                      char **varValue);
static char *getConfValue(SystemVar *systemVar, int selectShowType);
static char *getOSTimeZone(void);
static bool isIsolationVariable(const char *name);
static bool isReadOnlyVariable(const char *name);
static char *canonicalVariableName(char *name);
static const char *isolationName(int level);
static char *nativeIsolationName(const char *value);
static char *nativeTimeZoneValue(const char *value);
static void validateBackendVariable(const char *name, char **value);
static void rememberBackendVariable(const char *name);


Size
GlobalSystemVariablesShmemSize(void)
{
	Size size = 0;

	size = add_size(MAXALIGN(sizeof(GlobalSystemVarsMutex)),
                    hash_estimate_size(SYSTEM_VAR_MAX_NUM, sizeof(SystemVar)));

	return size;
}


void
GlobalSystemVariablesShmemInit(void)
{
    HASHCTL hashctl = {0};
    bool found;

    globalSystemVarsMutex = ShmemInitStruct("OpenHalo MySQL system variable lock",
                                           sizeof(GlobalSystemVarsMutex), &found);
    if (!found)
    {
        LWLockInitialize(&globalSystemVarsMutex->mutex, LWLockNewTrancheId());
        globalSystemVarsMutex->startupIsolation = DefaultXactIsoLevel;
        globalSystemVarsMutex->startupReadOnly = DefaultXactReadOnly;
    }
    LWLockRegisterTranche(globalSystemVarsMutex->mutex.tranche,
                          "OpenHalo MySQL system variables");

    hashctl.keysize = SYSTEM_VAR_NAME_MAX_LEN;
    hashctl.entrysize = sizeof(SystemVar);
    globalSystemVars = ShmemInitHash("MySQL Global System Variables (914)",
                                     SYSTEM_VAR_INT_NUM,
                                     SYSTEM_VAR_MAX_NUM,
                                     &hashctl,
                                     HASH_ELEM | HASH_STRINGS);
}


void
getSystemVariableValueForSelect(char *varName,
                                bool isSessionSystemVar,
                                char **varValue)
{
    int getRet;

    getRet = getSystemVariableValueImpl(varName,
                                        isSessionSystemVar,
                                        1,
                                        varValue);
    if (getRet == 0)
    {
        return;
    }
    else if (getRet == 1)
    {
        elog(ERROR, "Unknown system variable '%s'", varName);
    }
    else
    {
        elog(ERROR, "Variable '%s' is a SESSION variable", varName);
    }
}


bool
getSystemVariableValueForShow(char *varName,
                              bool isSessionSystemVar,
                              char **varValue)
{
    int getRet;

    getRet = getSystemVariableValueImpl(varName,
                                        isSessionSystemVar,
                                        2,
                                        varValue);
    if (getRet == 0)
    {
        return true;
    }
    else if (getRet == 1)
    {
        elog(ERROR, "Unknown system variable '%s'", varName);
    }
    else
    {
        elog(ERROR, "Variable '%s' is a SESSION variable", varName);
    }
}


void
setSystemVariableValue(char *varName, char *varValue, bool isSessionSystemVar)
{
    SystemVar *systemVar;
    bool found;

    if (sessionSystemVars == NULL)
    {
        initSystemVariables();
    }

    if (varName == NULL)
    {
        elog(ERROR, "Invalid system variable name(NULL)");
        return;
    }

    varName = canonicalVariableName(varName);

    rectifySystemVarValue(varName, &varValue, isSessionSystemVar);
    validateBackendVariable(varName, &varValue);

    if (!isSessionSystemVar && !superuser())
        ereport(ERROR,
                (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                 errmsg("permission denied to set global MySQL system variable")));

    if (isSessionSystemVar)
    {
        systemVar = (SystemVar *)hash_search(sessionSystemVars,
                                             varName,
                                             HASH_FIND,
                                             &found);
        if (found || (NULL != (systemVar = dynAddSystemVar(varName))))
        {
            if ((systemVar->sessionGlobalType == 0) ||
                (systemVar->sessionGlobalType == 2) ||
                (systemVar->sessionGlobalType == 3))
            {
                if (systemVar->isReadWrite)
                {
                    /* A rejected backend setting must not poison the cache. */
                    applySystemVarValue(varName, varValue);
                    setSystemVarValueByVarName(varName,
                                               varValue,
                                               systemVar->varConfValue);
                }
                else
                {
                    elog(ERROR,
                         "Variable '%s' is a read only variable",
                         varName);
                }
            }
            else
            {
                elog(ERROR,
                     "Variable '%s' is a GLOBAL variable and should be set with SET GLOBAL",
                     varName);
            }
        }
        else
        {
            elog(ERROR, "Unknown system variable '%s'", varName);
        }
    }
    else
    {
        lockGlobalSystemVars();
        systemVar = (SystemVar *)hash_search(globalSystemVars,
                                             varName,
                                             HASH_FIND,
                                             &found);
        if (found || (NULL != (systemVar = dynAddSystemVar(varName))))
        {
            if ((systemVar->sessionGlobalType == 0) ||
                (systemVar->sessionGlobalType == 1))
            {
                if (systemVar->isReadWrite)
                {
                    setSystemVarValueByVarName(varName,
                                               varValue,
                                               systemVar->varConfValue);
                    unlockGlobalSystemVars();

                    if ((systemVar->sessionGlobalType == 1) &&
                        systemVar->sessionDefValueFromGlobal)
                    {
                        systemVar = (SystemVar *)hash_search(sessionSystemVars,
                                                             varName,
                                                             HASH_FIND,
                                                             &found);
                        if (found)
                        {
                            setSystemVarValueByVarName(varName,
                                                       varValue,
                                                       systemVar->varConfValue);
                        }
                        else
                        {
                            elog(ERROR,
                                 "System session variable '%s' does not exists(set)",
                                 varName);
                        }
                    }
                }
                else
                {
                    unlockGlobalSystemVars();
                    elog(ERROR,
                         "Variable '%s' is a read only variable",
                         varName);
                }
            }
            else
            {
                unlockGlobalSystemVars();
                elog(ERROR,
                     "Variable '%s' is a SESSION variable and can't be used with SET GLOBAL",
                     varName);
            }
        }
        else
        {
            unlockGlobalSystemVars();
            elog(ERROR, "Unknown system variable '%s'", varName);
        }
    }
}


void
setSystemVariableDatum(char *varName, Datum varConfValue,
                       Oid varValueType, bool isNull,
                       bool isSessionSystemVar)
{
    char *varValue;

    if (!isNull)
    {
        Oid	typoutput;
	    bool typIsVarlena;

        getTypeOutputInfo(varValueType, &typoutput, &typIsVarlena);
        varValue = OidOutputFunctionCall(typoutput, varConfValue);
    }
    else
    {
        varValue = NULL;
    }
    setSystemVariableValue(varName, varValue, isSessionSystemVar);
}


bool
isSystemVariable(char *varName)
{
    bool found;

    if (sessionSystemVars == NULL)
    {
        initSystemVariables();
    }

    lowerSystemVarName(varName);

    (SystemVar *)hash_search(sessionSystemVars,
                             varName,
                             HASH_FIND,
                             &found);

    return found;
}


static void
initSystemVariables(void)
{
    MemoryContext oldContext;

    oldContext = MemoryContextSwitchTo(TopMemoryContext);

    initBaseSystemVars();

    lockGlobalSystemVars();
    initGlobalSystemVars(baseSystemVars, baseSystemVarsNum);
    initSessionSystemVars(baseSystemVars, baseSystemVarsNum);
    unlockGlobalSystemVars();

    for (int i = 0; i < baseSystemVarsNum; i++)
    {
        SystemVar *session = hash_search(sessionSystemVars, baseSystemVars[i]->varName,
                                          HASH_FIND, NULL);
        if ((!parserengine || parserengine->compat_mode != MYSQL_COMPAT_MODE) &&
            (strcmp(session->varName, "time_zone") == 0 ||
             isIsolationVariable(session->varName) || isReadOnlyVariable(session->varName)))
            continue;
        applySystemVarValue(session->varName, session->varConfValue);
    }

    MemoryContextSwitchTo(oldContext);
}


static bool
add2BaseSystemVars(SystemVar *systemVar)
{
    if (baseSystemVarsNum < SYSTEM_VAR_MAX_NUM)
    {
        baseSystemVars[baseSystemVarsNum] = systemVar;
        ++baseSystemVarsNum;
        return true;
    }
    else
    {
        return false;
    }
}


static SystemVar *
initBaseSystemVar(List *systemVarDatas)
{
    SystemVar *systemVar;
    char *varName;
    char *varDefValue;
    char *varConfValue;
    int sessionGlobalType;
    bool sessionDefValueFromGlobal;
    bool isReadWrite;
    char *result;
    char *selectResult;
    char *showResult;
    ListCell *lc;
    int ind;

    systemVar = NULL;
    varName = NULL;
    varDefValue = NULL;
    varConfValue = NULL;
    sessionGlobalType = 0;
    sessionDefValueFromGlobal = true;
    isReadWrite= true;
    result = NULL;
    selectResult = NULL;
    showResult = NULL;
    ind = 0;

    foreach (lc, systemVarDatas)
    {
        Node *nd = lfirst(lc);
        if (NULL != nd)
        {
            char *val = strVal(nd);
            if (ind == 0)
            {
                varName = val;
                lowerSystemVarName(varName);
            }
            else if (ind == 1)
            {
                varDefValue = val;
            }
            else if (ind == 2)
            {
                varConfValue = val;
            }
            else if (ind == 3)
            {
                if (val[0] == '0')
                {
                    sessionGlobalType = 0;
                }
                else if (val[0] == '1')
                {
                    sessionGlobalType = 1;
                }
                else if (val[0] == '2')
                {
                    sessionGlobalType = 2;
                }
                else if (val[0] == '3')
                {
                    sessionGlobalType = 3;
                }
                else
                {
                    return NULL;
                }
            }
            else if (ind == 4)
            {
                if (!parse_bool(val, &sessionDefValueFromGlobal))
                    elog(ERROR, "invalid boolean value in system variable catalog: %s", val);
            }
            else if (ind == 5)
            {
                if (!parse_bool(val, &isReadWrite))
                    elog(ERROR, "invalid boolean value in system variable catalog: %s", val);
            }
            else if (ind == 6)
            {
                result = val;
            }
            else if (ind == 7)
            {
                selectResult = val;
            }
            else
            {
                showResult = val;
            }
        }
        else
        {
            if (ind == 6)
            {
                result = NULL;
            }
            else if (ind == 7)
            {
                selectResult = NULL;
            }
            else if (ind == 8)
            {
                showResult = NULL;
            }
        }
        ++ind;
    }

    systemVar = (SystemVar *)palloc0(sizeof(SystemVar));
    snprintf(systemVar->varName, SYSTEM_VAR_NAME_MAX_LEN, "%s", varName);
    if (!asignSystemVar(varDefValue,
                        varConfValue,
                        sessionGlobalType,
                        sessionDefValueFromGlobal,
                        isReadWrite,
                        result,
                        selectResult,
                        showResult,
                        systemVar))
    {
        return NULL;
    }

    return systemVar;
}


static List *
readBaseVariableRecord(TupleTableSlot *slot)
{
    List *columnVals;
    int columnCount;

    columnVals = NIL;
    columnCount = slot->tts_tupleDescriptor->natts;
    for (int i = 0; i < columnCount; i++)
    {
        TupleDesc tupleDesc;
        Form_pg_attribute att;
        Datum attr;
        bool is_null;
        Oid typoutput;
        bool typisvarlena;

        tupleDesc = slot->tts_tupleDescriptor;
        att = TupleDescAttr(tupleDesc, i);
        attr = slot_getattr(slot, (i + 1), &is_null);
        if (!is_null)
        {
            char *columnVal;
            getTypeOutputInfo(att->atttypid, &typoutput, &typisvarlena);
            columnVal = OidOutputFunctionCall(typoutput, attr);
            columnVals = lappend(columnVals, makeString(columnVal));
        }
        else
        {
            columnVals = lappend(columnVals, NULL);
        }
    }

    return columnVals;
}


static List *
readBaseVariablesTable()
{
    List *systemVarsDatas;
    Relation relation;
    Snapshot snapshot;
    TableScanDesc scan;
    TupleTableSlot *slot;
    Oid schemaId;
    Oid tableId;
    char *schemaName;
    char *tableName;

    systemVarsDatas = NIL;
    schemaId = InvalidOid;
    tableId = InvalidOid;
    schemaName = "mys_informa_schema";
    tableName = "base_variables";

    schemaId = get_namespace_oid(schemaName, false);
    tableId = get_relname_relid(tableName, schemaId);
    relation = table_open(tableId, AccessShareLock);
    snapshot = RegisterSnapshot(GetLatestSnapshot());
    scan = table_beginscan(relation, snapshot, 0, NULL);
    slot = table_slot_create(relation, NULL);
    while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
    {
        List *systemVarDatas = readBaseVariableRecord(slot);
        systemVarsDatas = lappend(systemVarsDatas, systemVarDatas);
    }
    table_endscan(scan);
    UnregisterSnapshot(snapshot);
    ExecDropSingleTupleTableSlot(slot);
    table_close(relation, AccessShareLock);

    return systemVarsDatas;
}


static void
initBaseSystemVars(void)
{
    List *systemVarsColumnVals;
    ListCell *lc;

    baseSystemVarsNum = 0;
    systemVarsColumnVals = readBaseVariablesTable();
    foreach (lc, systemVarsColumnVals)
    {
        List *systemVarColumnVals = (List *)lfirst(lc);
        SystemVar *systemVar = initBaseSystemVar(systemVarColumnVals);
        if (NULL != systemVar)
        {
            /* Advertise the actual configured PG18 startup defaults. */
            if (isIsolationVariable(systemVar->varName))
            {
                const char *value = isolationName(globalSystemVarsMutex->startupIsolation);
                setSystemVarValue(value, systemVar->varDefValue);
                setSystemVarValue(value, systemVar->varConfValue);
            }
            else if (isReadOnlyVariable(systemVar->varName))
            {
                const char *value = globalSystemVarsMutex->startupReadOnly ? "1" : "0";
                setSystemVarValue(value, systemVar->varDefValue);
                setSystemVarValue(value, systemVar->varConfValue);
            }
            if (!add2BaseSystemVars(systemVar))
            {
                elog(ERROR,
                     "There are too many MySQL system variables. Connection will be disconnect.");
                proc_exit(1);
            }
        }
        else
        {
            char *varName = strVal(linitial(systemVarColumnVals));
            elog(ERROR,
                 "Invalid data '%s' in mys_informa_schema.base_variables table. Connection will be disconnect.",
                 varName);
            proc_exit(1);
        }
    }
}


static void
add2GlobalSystemVars(SystemVar *systemVar)
{
    SystemVar *globalSystemVar;
    bool found;

    globalSystemVar =
        (SystemVar *)hash_search(globalSystemVars,
                                 &(systemVar->varName[0]),
                                 HASH_ENTER,
                                 &found);
    if (!found)
    {
        copySystemVar(systemVar, globalSystemVar);
    }
}


static void
initGlobalSystemVars(SystemVar *systemVars[], int systemVarsNum)
{
    SystemVar *globalSystemVar;
    bool found;

    globalSystemVar =
        (SystemVar *)hash_search(globalSystemVars,
                                 "global_system_vars_init_flag",
                                 HASH_ENTER,
                                 &found);

    if (!found)
    {
        if (!asignSystemVar("1", "1", false, false, false, NULL, NULL, NULL,
                            globalSystemVar))
        {
            elog(NOTICE,
                 "Invalid data in mys_informa_schema.base_variables table");
        }

        for (int i = 0; i < systemVarsNum; i++)
        {
            SystemVar *systemVar = systemVars[i];
            add2GlobalSystemVars(systemVar);
        }
    }
}


static void
add2SessionSystemVars(SystemVar *systemVar)
{
    SystemVar *sessionSystemVar;
    bool found;

    sessionSystemVar = (SystemVar *)hash_search(sessionSystemVars,
                                                &(systemVar->varName),
                                                HASH_ENTER,
                                                &found);
    if (!found)
    {
        copySystemVar(systemVar, sessionSystemVar);
        if (systemVar->sessionDefValueFromGlobal &&
            (systemVar->sessionGlobalType == 0 || systemVar->sessionGlobalType == 3))
        {
            SystemVar *global = hash_search(globalSystemVars, systemVar->varName,
                                             HASH_FIND, NULL);

            if (global)
                copySystemVar(global, sessionSystemVar);
        }
    }

}


static void
initSessionSystemVars(SystemVar *systemVars[], int systemVarsNum)
{
    HASHCTL hashctl;

    hashctl.keysize = SYSTEM_VAR_NAME_MAX_LEN;
    hashctl.entrysize = sizeof(SystemVar);
    sessionSystemVars = hash_create("Halo MySQL Session System Variables (914)",
                                    SYSTEM_VAR_MAX_NUM,
                                    &hashctl,
                                    HASH_ELEM | HASH_STRINGS);

    for (int i = 0; i < systemVarsNum; i++)
    {
        SystemVar *systemVar = systemVars[i];
        add2SessionSystemVars(systemVar);
    }
}


static SystemVar *
dynAddSystemVar(char *varName)
{
    SystemVar *systemVar;
    List *systemVarsColumnVals;
    ListCell *lc;
    List *systemVarColumnVals;
    int varNameLen;
    MemoryContext oldContext;

    oldContext = MemoryContextSwitchTo(TopMemoryContext);

    systemVar = NULL;
    systemVarsColumnVals = readBaseVariablesTable();
    systemVarColumnVals = NIL;
    varNameLen = strlen(varName);
    foreach (lc, systemVarsColumnVals)
    {
        List *curSystemVarsColumnVals = (List *)lfirst(lc);
        char *variableName = strVal(linitial(curSystemVarsColumnVals));
        if ((varNameLen == strlen(variableName)) &&
            (strncasecmp(varName, variableName, strlen(varName)) == 0))
        {
            systemVarColumnVals = curSystemVarsColumnVals;
            break;
        }
    }
    if (NIL == systemVarColumnVals)
    {
        MemoryContextSwitchTo(oldContext);
        return NULL;
    }

    systemVar = initBaseSystemVar(systemVarColumnVals);
    if (NULL == systemVar)
    {
        MemoryContextSwitchTo(oldContext);
        return NULL;
    }

    if (!add2BaseSystemVars(systemVar))
    {
        MemoryContextSwitchTo(oldContext);
        return NULL;
    }

    add2GlobalSystemVars(systemVar);
    add2SessionSystemVars(systemVar);

    MemoryContextSwitchTo(oldContext);
    return systemVar;
}


static void
lockGlobalSystemVars(void)
{
    Assert(globalSystemVarsMutex != NULL);
    LWLockAcquire(&globalSystemVarsMutex->mutex, LW_EXCLUSIVE);
}


static void
unlockGlobalSystemVars(void)
{
    LWLockRelease(&globalSystemVarsMutex->mutex);
}


static void
lowerSystemVarName(char *varName)
{
    int varNameLen;

    if (varName == NULL)
    {
        return;
    }

    varNameLen = strlen(varName);
    for (int i = 0; i < varNameLen; i++)
    {
        if ((65 <= varName[i]) && (varName[i] <= 90))
        {
            varName[i] += 32;
        }
    }
}


static bool
asignSystemVar(const char *varDefValue,
               const char *varConfValue,
               int sessionGlobalType,
               bool sessionDefValueFromGlobal,
               bool isReadWrite,
               const char *result,
               const char *selectResult,
               const char *showResult,
               SystemVar *systemVar)
{
    setSystemVarValue(varDefValue, systemVar->varDefValue);
    setSystemVarValue(varConfValue, systemVar->varConfValue);
    systemVar->sessionGlobalType = sessionGlobalType;
    systemVar->sessionDefValueFromGlobal = sessionDefValueFromGlobal;
    systemVar->isReadWrite = isReadWrite;
    if (result != NULL)
    {
        if (!asignSystemVarResult(result,
                                  &(systemVar->resultNum),
                                  &(systemVar->result[0][0])))
        {
            return false;
        }
    }
    else
    {
        systemVar->resultNum = 0;
    }
    if (selectResult != NULL)
    {
        if (!asignSystemVarResult(selectResult,
                                  &(systemVar->selectResultNum),
                                  &(systemVar->selectResult[0][0])))
        {
            return false;
        }
    }
    else
    {
        systemVar->selectResultNum = 0;
    }
    if (showResult != NULL)
    {
        if (!asignSystemVarResult(showResult,
                                  &(systemVar->showResultNum),
                                  &(systemVar->showResult[0][0])))
        {
            return false;
        }
    }
    else
    {
        systemVar->showResultNum = 0;
    }

    if ((0 == systemVar->resultNum) &&
        ((0 < systemVar->selectResultNum) ||
         (0 < systemVar->showResultNum)))
    {
        return false;
    }

    if ((0 < systemVar->resultNum) &&
        (((systemVar->selectResultNum != systemVar->resultNum) &&
          (systemVar->showResultNum != systemVar->resultNum)) ||
         ((0 < systemVar->selectResultNum) &&
          (0 < systemVar->showResultNum) &&
          (systemVar->selectResultNum != systemVar->showResultNum))))
    {
        return false;
    }

    return true;
}


static bool
asignSystemVarResult(const char *srcResult,
                     int *resultNum,
                     char *result)
{
    char items[SYSTEM_VAR_RESULT_RULE_NUM][SYSTEM_VAR_RESULT_RULE_LEN];
    int itemsNum;

    itemsNum = 0;
    splitStr(srcResult, '|', SYSTEM_VAR_RESULT_RULE_LEN,
             &items[0][0], &itemsNum);
    *resultNum = itemsNum;
    for (int i = 0; i < itemsNum; i++)
    {
        if ((SYSTEM_VAR_RESULT_RULE_LEN - 1) <= (strlen(items[i])))
        {
            return false;
        }

        snprintf((result + (i * SYSTEM_VAR_RESULT_RULE_LEN)),
                 SYSTEM_VAR_RESULT_RULE_LEN, "%s", items[i]);
    }

    return true;
}


static void
splitStr(const char *str, char separator,
         int itemSize, char *items, int *itemsNum)
{
    int strLen;
    int itemStartIndex;

    if (str == NULL)
    {
        return;
    }

    strLen = strlen(str);
    if (strLen == 0)
    {
        return;
    }

    *itemsNum = 0;
    itemStartIndex = -1;
    for (int i = 0; i <= strLen; i++)
    {
        if ((str[i] != separator) && (str[i] != '\0'))
        {
            if (itemStartIndex == -1)
            {
                itemStartIndex = i;
            }
        }
        else
        {
            if (itemStartIndex != -1)
            {
                int curItemLen = i - itemStartIndex;
                char *curItemBuf = items + (*itemsNum * itemSize);
                memcpy(curItemBuf, (str + itemStartIndex), curItemLen);
                curItemBuf[curItemLen] = '\0';
                (*itemsNum)++;
                itemStartIndex = -1;
            }
        }
    }
}


static void
copySystemVar(SystemVar *srcSystemVar, SystemVar *destSystemVar)
{
    setSystemVarValue(srcSystemVar->varDefValue,
                      destSystemVar->varDefValue);
    setSystemVarValue(srcSystemVar->varConfValue,
                      destSystemVar->varConfValue);
    destSystemVar->sessionGlobalType = srcSystemVar->sessionGlobalType;
    destSystemVar->sessionDefValueFromGlobal =
        srcSystemVar->sessionDefValueFromGlobal;
    destSystemVar->isReadWrite = srcSystemVar->isReadWrite;
    destSystemVar->resultNum = srcSystemVar->resultNum;
    for (int i = 0; i < destSystemVar->resultNum; i++)
    {
        setSystemVarValue(&(srcSystemVar->result[i][0]),
                          &(destSystemVar->result[i][0]));
    }
    destSystemVar->selectResultNum = srcSystemVar->selectResultNum;
    for (int i = 0; i < destSystemVar->selectResultNum; i++)
    {
        setSystemVarValue(&(srcSystemVar->selectResult[i][0]),
                          &(destSystemVar->selectResult[i][0]));
    }
    destSystemVar->showResultNum = srcSystemVar->showResultNum;
    for (int i = 0; i < destSystemVar->showResultNum; i++)
    {
        setSystemVarValue(&(srcSystemVar->showResult[i][0]),
                          &(destSystemVar->showResult[i][0]));
    }
}


static void
setSystemVarValueByVarName(const char *varName,
                           const char *value, char *varValue)
{
    if (strncasecmp(varName, "optimizer_switch", 16) == 0)
    {
        setSystemVarValueForOptimizerSwitch(value, varValue);
    }
    else
    {
        setSystemVarValue(value, varValue);
    }
}


static void
setSystemVarValueForOptimizerSwitch(const char *value, char *varValue)
{
    char newVarValue[SYSTEM_VAR_VALUE_MAX_LEN];
    int valueLen;
    int setItemStartIndex;

    valueLen = strlen(value);
    setItemStartIndex = 0;
    for (int i = 0; i <= valueLen; i++)
    {
        if (value[i] == '=')
        {
            char *setItemName;
            int setItemNameLen;
            int setItemLen;

            setItemNameLen = i - setItemStartIndex;
            if (setItemNameLen == 0)
            {
                elog(ERROR, "Invalid value '%s' for 'optimizer_switch'", value);
            }
            setItemName = palloc0(setItemNameLen + 1);
            memcpy(setItemName, (value + setItemStartIndex), setItemNameLen);

            for (int j = (i + 1); j <= valueLen; j++)
            {
                if ((value[j] == ',') || (value[j] == '\0'))
                {
                    int itemIndexInVarValue;
                    int nextItemIndexInVarValue;
                    int varValueLen;

                    setItemLen = j - setItemStartIndex;
                    itemIndexInVarValue = 0;
                    nextItemIndexInVarValue = 0;
                    varValueLen = strlen(varValue);
                    for (int k = 0; k < varValueLen; k++)
                    {
                        if (strncasecmp((varValue + k), setItemName, setItemNameLen) == 0)
                        {
                            itemIndexInVarValue = k;
                            for (int l = itemIndexInVarValue; l <= varValueLen; l++)
                            {
                                if ((varValue[l] == ',') || (varValue[l] == '\0'))
                                {
                                    nextItemIndexInVarValue = l;
                                    break;
                                }
                            }
                            break;
                        }
                    }
                    if ((itemIndexInVarValue == 0) &&
                        (nextItemIndexInVarValue == 0))
                    {
                        elog(ERROR, "Invalid value '%s' for 'optimizer_switch'", value);
                    }

                    memcpy(newVarValue, varValue, itemIndexInVarValue);
                    memcpy((newVarValue + itemIndexInVarValue),
                           (value + setItemStartIndex), setItemLen);
                    memcpy((newVarValue + itemIndexInVarValue + setItemLen),
                           (varValue + nextItemIndexInVarValue),
                           (varValueLen - nextItemIndexInVarValue + 1));
                    snprintf(varValue, SYSTEM_VAR_VALUE_MAX_LEN, "%s", newVarValue);

                    i = j;
                    setItemStartIndex = i + 1;

                    break;
                }
            }
        }
    }
}


static void
setSystemVarValue(const char* value, char *varValue)
{
    if (value != NULL)
    {
        snprintf(varValue, SYSTEM_VAR_VALUE_MAX_LEN, "%s", value);
    }
    else
    {
        varValue[0] = '\0';
    }
}


static void
rectifySystemVarValue(const char *varName, char **varValue,
                      bool isSessionSystemVar)
{
    if (*varValue != NULL)
    {
        if (strcmp(*varValue, "halo_mysql_system_var_default") == 0)
        {
            getDefaultValue(varName, isSessionSystemVar, varValue);
            return;
        }
        else
        {
            if (strncasecmp(varName, "query_cache_size", 16) == 0)
            {
                char buf[64];
                int val = atoi(*varValue);
                if (val < 0)
                {
                    elog(ERROR,
                         "Invalid value '%d' for query_cache_size",
                         val);
                }

                val = (val / 1048576) * 1048576;
                snprintf(buf, 64, "%d", val);
                *varValue = pstrdup(buf);
                return;
            }
            else
            {
                int varValueLen;
                SystemVar *systemVar;
                bool found;

                varValueLen = strlen(*varValue);
                systemVar = (SystemVar *)hash_search(sessionSystemVars,
                                                     varName,
                                                     HASH_FIND,
                                                     &found);
                if (!found)
                {
                    elog(ERROR, "Unknown system variable '%s'", varName);
                }

                if (0 < systemVar->resultNum)
                {
                    for (int i = 0; i < systemVar->resultNum; i++)
                    {
                        int resultItemLen = strlen(systemVar->result[i]);
                        if ((varValueLen == resultItemLen) &&
                            (strncasecmp(*varValue,
                                         systemVar->result[i],
                                         varValueLen) == 0))
                        {
                            /* do nothing; */
                            return;
                        }
                        else
                        {
                            int showResultItemLen = strlen(systemVar->showResult[i]);
                            if ((varValueLen == showResultItemLen) &&
                                (strncasecmp(*varValue,
                                             systemVar->showResult[i],
                                             varValueLen) == 0))
                            {
                                *varValue = pstrdup(systemVar->result[i]);
                                return;
                            }
                        }
                    }

                    elog(ERROR,
                         "Variable '%s' can't be set to the value of '%s'",
                         varName,
                         *varValue);
                }
                return;
            }
        }
    }
}


static void
getDefaultValue(const char *varName, bool isSessionSystemVar,
                char **varValue)
{
    bool sessionDefValueFromGlobal;

    if (strncasecmp(varName, "last_insert_id", 14) == 0)
    {
        elog(ERROR,
             "Variable '%s' doesn't have a default value",
             varName);
    }

    if (isSessionSystemVar)
    {
        SystemVar *systemVar;
        bool found;

        systemVar = (SystemVar *)hash_search(sessionSystemVars,
                                             varName,
                                             HASH_FIND,
                                             &found);
        if (found)
        {
            sessionDefValueFromGlobal = systemVar->sessionDefValueFromGlobal;
        }
        else
        {
            elog(ERROR,
                 "Can not find system variable \"%s\" from global.",
                 varName);
        }
    }

    if (isSessionSystemVar && sessionDefValueFromGlobal)
    {
        SystemVar *systemVar;
        bool found;

        lockGlobalSystemVars();
        systemVar = (SystemVar *)hash_search(globalSystemVars,
                                             varName,
                                             HASH_FIND,
                                             &found);
        if (found)
        {
            *varValue = pstrdup(systemVar->varConfValue);
            unlockGlobalSystemVars();
        }
        else
        {
            unlockGlobalSystemVars();
            elog(ERROR,
                 "Can not find system variable \"%s\" from global.",
                 varName);
        }
    }
    else
    {
        int varNameLen = strlen(varName);
        for (int i = 0; i < baseSystemVarsNum; i++)
        {
            SystemVar *systemVar = baseSystemVars[i];
            int baseVarNameLen = strlen(systemVar->varName);
            if ((varNameLen == baseVarNameLen) &&
                (strncasecmp(varName, systemVar->varName, varNameLen) == 0))
            {
                *varValue = pstrdup(systemVar->varDefValue);
                return;
            }
        }

        elog(ERROR,
             "Can not find system variable \"%s\" from base.",
             varName);
    }
}


static bool
isIsolationVariable(const char *name)
{
    return strcmp(name, "transaction_isolation") == 0 || strcmp(name, "tx_isolation") == 0;
}

static bool
isReadOnlyVariable(const char *name)
{
    return strcmp(name, "transaction_read_only") == 0 || strcmp(name, "tx_read_only") == 0;
}

static char *
canonicalVariableName(char *name)
{
    lowerSystemVarName(name);
    if (strcmp(name, "tx_isolation") == 0)
        return pstrdup("transaction_isolation");
    if (strcmp(name, "tx_read_only") == 0)
        return pstrdup("transaction_read_only");
    return name;
}

static const char *
isolationName(int level)
{
    switch (level)
    {
        case XACT_READ_UNCOMMITTED: return "READ-UNCOMMITTED";
        case XACT_READ_COMMITTED: return "READ-COMMITTED";
        case XACT_REPEATABLE_READ: return "REPEATABLE-READ";
        case XACT_SERIALIZABLE: return "SERIALIZABLE";
    }
    elog(ERROR, "invalid transaction isolation level %d", level);
    return NULL;
}

static char *
nativeIsolationName(const char *value)
{
    char *native = pstrdup(value);

    for (char *p = native; *p; p++)
        *p = *p == '-' ? ' ' : pg_tolower((unsigned char) *p);
    if (strcmp(native, "read uncommitted") && strcmp(native, "read committed") &&
        strcmp(native, "repeatable read") && strcmp(native, "serializable"))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("invalid transaction isolation level: \"%s\"", value)));
    return native;
}

/* Use SQL interval signs; a bare POSIX time-zone name reverses their meaning. */
static char *
nativeTimeZoneValue(const char *value)
{
    if (pg_strcasecmp(value, "SYSTEM") == 0)
        return getOSTimeZone();
    if (value[0] == '+' || value[0] == '-')
    {
        int len = strlen(value);
        int hours;
        int minutes;
        int offset;

        if ((len != 5 && len != 6) || value[len - 3] != ':' ||
            value[1] < '0' || value[1] > '9' ||
            (len == 6 && (value[2] < '0' || value[2] > '9')) ||
            value[len - 2] < '0' || value[len - 2] > '9' ||
            value[len - 1] < '0' || value[len - 1] > '9')
            goto invalid;
        hours = value[1] - '0';
        if (len == 6)
            hours = hours * 10 + value[2] - '0';
        minutes = (value[len - 2] - '0') * 10 + value[len - 1] - '0';
        offset = (hours * 60 + minutes) * (value[0] == '-' ? -1 : 1);
        if (minutes > 59 || offset < -839 || offset > 840)
            goto invalid;
        return psprintf("INTERVAL '%c%02d:%02d'", value[0], hours, minutes);
    }
    else
    {
        pg_tz *zone = pg_tzset(value);

        if (zone && pg_tz_acceptable(zone))
            return pstrdup(value);
    }
invalid:
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("unknown or incorrect time zone: \"%s\"", value)));
    return NULL;
}

static void
validateBackendVariable(const char *name, char **value)
{
    if (strcmp(name, "time_zone") != 0 &&
        !isIsolationVariable(name) && !isReadOnlyVariable(name))
        return;
    if (*value == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("system variable \"%s\" cannot be NULL", name)));
    if (strcmp(name, "time_zone") == 0)
    {
        char *native = nativeTimeZoneValue(*value);

        if ((*value)[0] == '+' || (*value)[0] == '-')
        {
            /* Native value is INTERVAL '+HH:MM'; preserve MySQL's format. */
            *value = pnstrdup(native + 10, 6);
            if (strcmp(*value, "-00:00") == 0)
                (*value)[0] = '+';
        }
        else if (pg_strcasecmp(*value, "SYSTEM") == 0)
            *value = pstrdup("SYSTEM");
        pfree(native);
    }
    else if (isIsolationVariable(name))
    {
        *value = nativeIsolationName(*value);
        for (char *p = *value; *p; p++)
            *p = *p == ' ' ? '-' : pg_toupper((unsigned char) *p);
    }
    else
    {
        bool flag;

        if (!parse_bool(*value, &flag))
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("invalid transaction read-only value: \"%s\"", *value)));
        *value = pstrdup(flag ? "1" : "0");
    }
}

static void
applySystemVarValue(const char *varName, const char *varValue)
{
    if (strcmp(varName, "tx_isolation") == 0 || strcmp(varName, "tx_read_only") == 0)
        return;
    if (strcmp(varName, "autocommit") == 0)
    {
        if ((strncasecmp(varValue, "1", 1) == 0) ||
            (strncasecmp(varValue, "on", 2) == 0))
        {
            if (autoCommit == 0)
            {
                /* 0 --> 1 */
                needCommitTrx = true;
                needStartNewTrx = false;
            }
            else
            {
                /* 1 --> 1 */
                needCommitTrx = false;
                needStartNewTrx = false;
            }

            autoCommit = 2;
        }
        else
        {
            if (autoCommit == 0)
            {
                /* 0 --> 0 */
                needCommitTrx = false;
                needStartNewTrx = false;
            }
            else
            {
                /* 1 --> 0 */
                needCommitTrx = false;
                needStartNewTrx = true;
            }

            autoCommit = 0;
        }
    }
    else if (strcmp(varName, "time_zone") == 0)
    {
        char *native = nativeTimeZoneValue(varValue);

        SetConfigOption("TimeZone", native, PGC_USERSET, PGC_S_SESSION);
        sessionTimeZoneFromNative = false;
        rememberBackendVariable(varName);
        pfree(native);
    }
    else if (isIsolationVariable(varName))
    {
        char *native = nativeIsolationName(varValue);

        SetConfigOption("default_transaction_isolation", native, PGC_USERSET, PGC_S_SESSION);
        rememberBackendVariable(varName);
        if (!IsTransactionBlock())
            nextIsolationSet = false;
        pfree(native);
    }
    else if (isReadOnlyVariable(varName))
    {
        SetConfigOption("default_transaction_read_only", varValue, PGC_USERSET, PGC_S_SESSION);
        rememberBackendVariable(varName);
        if (!IsTransactionBlock())
            nextReadOnlySet = false;
    }
    else if (strcmp(varName, "sql_mode") == 0)
    {
        char *rawValue;
        List *valueList;
        uint64 tempSqlMode = 0;

        isStrictTransTablesOn = false;

        /* Need a modifiable copy of string */
        rawValue = pstrdup(varValue);
        if (SplitIdentifierString(rawValue, ',', &valueList))
        {
            ListCell *lc;

            foreach(lc, valueList)
            {
                char *curValue = (char *)lfirst(lc);
                if (strncasecmp(curValue, "STRICT_TRANS_TABLES", 19) == 0)
                {
                    //isStrictTransTablesOn = true;
                    isStrictTransTablesOn = false;
                }
                else if (strncasecmp(curValue, "NO_ZERO_IN_DATE", 15) == 0)
                {
                    tempSqlMode |= MYS_MODE_NO_ZERO_IN_DATE;
                }
                else if (strncasecmp(curValue, "NO_ZERO_DATE", 12) == 0)
                {
                    tempSqlMode |= MYS_MODE_NO_ZERO_DATE;
                }
                else if (strncasecmp(curValue, "ALLOW_INVALID_DATES", 12) == 0)
                {
                    tempSqlMode |= MYS_MODE_INVALID_DATES;
                }
            }

            mys_sqlMode = tempSqlMode;
        }
        else
        {
            pfree(rawValue);
		    list_free(valueList);
            elog(ERROR, "List syntax is invalid.");
        }

        pfree(rawValue);
		list_free(valueList);
    }
    else if (strncasecmp(varName, "default_week_format", 19) == 0)
    {
        char *endPtr;
        int value;

        errno = 0;
        value = strtol(varValue, &endPtr, 10);
        if (errno == ERANGE)
        {
            elog(ERROR, "incorrect default_week_format value: %s", varValue);
        }

        if (*endPtr != '\0')
        {
            elog(ERROR, "incorrect default_week_format value: %s", varValue);
        }

        if (value > 7 || value < 0)
        {
            elog(ERROR, "incorrect default_week_format value: %s", varValue);
        }
        else
        {
            default_week_format = value;
        }
    }
    else
    {
        /* do nothing; */
    }
}


static int
getSystemVariableValueImpl(char *varName,
                           bool isSessionSystemVar,
                           int selectShowType,
                           char **varValue)
{
    int ret;
    SystemVar *systemVar;
    bool found;

    if (sessionSystemVars == NULL)
    {
        initSystemVariables();
    }

    if (varName == NULL)
    {
        return 1;
    }

    varName = canonicalVariableName(varName);

    ret = 0;
    if (isSessionSystemVar)
    {
        systemVar = (SystemVar *)hash_search(sessionSystemVars,
                                             varName,
                                             HASH_FIND,
                                             &found);
        if (found || (NULL != (systemVar = dynAddSystemVar(varName))))
        {
            *varValue = getConfValue(systemVar, selectShowType);
            ret = 0;
        }
        else
        {
            *varValue = NULL;
            ret = 1;
        }
    }
    else
    {
        lockGlobalSystemVars();
        systemVar = (SystemVar *)hash_search(globalSystemVars,
                                             varName,
                                             HASH_FIND,
                                             &found);
        if (found || (NULL != (systemVar = dynAddSystemVar(varName))))
        {
            if ((systemVar->sessionGlobalType == 0) ||
                (systemVar->sessionGlobalType == 1) ||
                (systemVar->sessionGlobalType == 3))
            {
                *varValue = getConfValue(systemVar, selectShowType);
                unlockGlobalSystemVars();
                ret = 0;
            }
            else
            {
                unlockGlobalSystemVars();
                *varValue = NULL;
                ret = 2;
            }
        }
        else
        {
            unlockGlobalSystemVars();
            *varValue = NULL;
            ret = 1;
        }
    }

    return ret;
}


static char *
getConfValue(SystemVar *systemVar, int selectShowType)
{
    char *ret;

    if (selectShowType == 2 && isReadOnlyVariable(systemVar->varName))
        return pstrdup(strcmp(systemVar->varConfValue, "1") == 0 ? "ON" : "OFF");
    ret = NULL;
    if (selectShowType == 1)
    {
        if (systemVar->selectResultNum == 0)
        {
            ret = pstrdup(systemVar->varConfValue);
        }
        else
        {
            int varConfValueLen = strlen(systemVar->varConfValue);
            for (int i = 0; i < systemVar->selectResultNum; i++)
            {
                int resultItemLen = strlen(systemVar->result[i]);
                if ((varConfValueLen == resultItemLen) &&
                    (strncasecmp(systemVar->varConfValue,
                                 systemVar->result[i],
                                 varConfValueLen) == 0))
                {
                    ret = pstrdup(systemVar->selectResult[i]);
                    break;
                }
            }
        }
    }
    else
    {
        if (systemVar->showResultNum == 0)
        {
            ret = pstrdup(systemVar->varConfValue);
        }
        else
        {
            int varConfValueLen = strlen(systemVar->varConfValue);
            for (int i = 0; i < systemVar->showResultNum; i++)
            {
                int resultItemLen = strlen(systemVar->result[i]);
                if ((varConfValueLen == resultItemLen) &&
                    (strncasecmp(systemVar->varConfValue,
                                 systemVar->result[i],
                                 varConfValueLen) == 0))
                {
                    ret = pstrdup(systemVar->showResult[i]);
                    break;
                }
            }
        }
    }

    return ret;
}


static char *
getOSTimeZone(void)
{
    time_t now = time(NULL);
    struct tm local;
    char offset[32];

    if (!localtime_r(&now, &local) ||
        strftime(offset, sizeof(offset), "%z", &local) != 5)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("could not determine the system time zone offset")));
    return psprintf("INTERVAL '%c%c%c:%c%c'", offset[0], offset[1], offset[2],
                    offset[3], offset[4]);
}


void
initializeSystemVariableSession(void)
{
    Oid namespace = get_namespace_oid("mys_informa_schema", true);

    if (sessionSystemVars == NULL && OidIsValid(namespace) &&
        OidIsValid(get_relname_relid("base_variables", namespace)))
        initSystemVariables();
}

/* MySQL session SET survives transaction rollback; restore it before starting
 * the next command, without a catalog lookup or a snapshot. */
void
restoreSystemVariableSession(void)
{
    static const char *names[] = {"time_zone", "transaction_isolation", "transaction_read_only"};

    if (sessionSystemVars == NULL || !parserengine ||
        parserengine->compat_mode != MYSQL_COMPAT_MODE)
        return;
    for (int i = 0; i < lengthof(names); i++)
    {
        SystemVar *value = hash_search(sessionSystemVars, names[i], HASH_FIND, NULL);

        if (value)
        {
            char *native;
            const char *guc;

            if (i == 0)
            {
                native = sessionTimeZoneFromNative ? pstrdup(value->varConfValue) :
                         nativeTimeZoneValue(value->varConfValue);
                guc = "TimeZone";
            }
            else if (i == 1)
            {
                native = nativeIsolationName(value->varConfValue);
                guc = "default_transaction_isolation";
            }
            else
            {
                native = pstrdup(value->varConfValue);
                guc = "default_transaction_read_only";
            }
            SetConfigOption(guc, native, PGC_USERSET, PGC_S_SESSION);
            rememberBackendVariable(names[i]);
            pfree(native);
        }
    }
}

static void
rememberBackendVariable(const char *name)
{
    if (strcmp(name, "time_zone") == 0)
        strlcpy(lastNativeTimeZone, pg_get_timezone_name(session_timezone), sizeof(lastNativeTimeZone));
    else if (isIsolationVariable(name))
        lastNativeIsolation = DefaultXactIsoLevel;
    else if (isReadOnlyVariable(name))
        lastNativeReadOnly = DefaultXactReadOnly;
}

/* Honor successful native SET/set_config calls made through the MySQL port.
 * Transaction-exit callbacks do not call this: rollback must not overwrite
 * MySQL's persistent session copy with the GUC stack's older value. */
void
captureSystemVariableSession(void)
{
    const char *zone;
    SystemVar *variable;

    if (sessionSystemVars == NULL || !parserengine ||
        parserengine->compat_mode != MYSQL_COMPAT_MODE)
        return;
    zone = pg_get_timezone_name(session_timezone);
    variable = hash_search(sessionSystemVars, "time_zone", HASH_FIND, NULL);
    if (variable && strcmp(lastNativeTimeZone, zone) != 0)
    {
        setSystemVarValue(zone, variable->varConfValue);
        sessionTimeZoneFromNative = true;
        rememberBackendVariable("time_zone");
    }
    variable = hash_search(sessionSystemVars, "transaction_isolation", HASH_FIND, NULL);
    if (variable && lastNativeIsolation != DefaultXactIsoLevel)
    {
        setSystemVarValue(isolationName(DefaultXactIsoLevel), variable->varConfValue);
        rememberBackendVariable("transaction_isolation");
    }
    variable = hash_search(sessionSystemVars, "transaction_read_only", HASH_FIND, NULL);
    if (variable && lastNativeReadOnly != DefaultXactReadOnly)
    {
        setSystemVarValue(DefaultXactReadOnly ? "1" : "0", variable->varConfValue);
        rememberBackendVariable("transaction_read_only");
    }
}

void
setMySQLNextTransactionVariable(char *name, char *value)
{
    if (!parserengine || parserengine->compat_mode != MYSQL_COMPAT_MODE)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("next-transaction settings require the MySQL protocol")));
    name = canonicalVariableName(name);
    if (!isIsolationVariable(name) && !isReadOnlyVariable(name))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("invalid next-transaction variable \"%s\"", name)));
    if (IsTransactionBlock())
        ereport(ERROR,
                (errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
                 errmsg("transaction characteristics cannot be changed while a transaction is in progress")));
    validateBackendVariable(name, &value);
    if (isIsolationVariable(name))
    {
        for (int i = XACT_READ_UNCOMMITTED; i <= XACT_SERIALIZABLE; i++)
            if (strcmp(value, isolationName(i)) == 0)
                nextIsolation = i;
        nextIsolationSet = true;
    }
    else
    {
        nextReadOnly = strcmp(value, "1") == 0;
        nextReadOnlySet = true;
    }
}

static bool
containsRelation(Node *node, void *context)
{
    if (node == NULL)
        return false;
    if (IsA(node, RangeVar))
        return true;
    return raw_expression_tree_walker(node, containsRelation, context);
}

/* Invoked after raw parsing, before acquiring the statement's snapshot. */
void
applyMySQLNextTransaction(Node *stmt)
{
    bool starts = false;
    bool explicitBegin = false;

    nextTransactionApplied = false;
    if ((!nextIsolationSet && !nextReadOnlySet && autoCommit != 0) || !parserengine ||
        parserengine->compat_mode != MYSQL_COMPAT_MODE || IsTransactionBlock())
        return;
    if (IsA(stmt, ExecuteStmt))
    {
        PreparedStatement *prepared = FetchPreparedStatement(castNode(ExecuteStmt, stmt)->name, true);

        stmt = prepared->plansource->raw_parse_tree->stmt;
    }
    if (IsA(stmt, TransactionStmt))
    {
        TransactionStmt *transaction = castNode(TransactionStmt, stmt);

        explicitBegin = transaction->kind == TRANS_STMT_BEGIN ||
                        transaction->kind == TRANS_STMT_START;
        starts = explicitBegin || (autoCommit == 0 && transaction->kind == TRANS_STMT_SAVEPOINT);
    }
    else if (IsA(stmt, SelectStmt))
        starts = !castNode(SelectStmt, stmt)->mysql_show && containsRelation(stmt, NULL);
    else
        starts = !IsA(stmt, MysVariableSetStmt) && !IsA(stmt, VariableSetStmt) &&
                 !IsA(stmt, VariableShowStmt) && !IsA(stmt, PrepareStmt) &&
                 !IsA(stmt, DeallocateStmt) && !IsA(stmt, DiscardStmt);
    if (!starts)
        return;
    if (nextIsolationSet)
    {
        char *native = nativeIsolationName(isolationName(nextIsolation));

        set_config_option("transaction_isolation", native, PGC_USERSET, PGC_S_SESSION,
                          GUC_ACTION_LOCAL, true, ERROR, false);
        pfree(native);
    }
    if (nextReadOnlySet)
        set_config_option("transaction_read_only", nextReadOnly ? "on" : "off",
                          PGC_USERSET, PGC_S_SESSION, GUC_ACTION_LOCAL, true, ERROR, false);
    /* A failed autocommit statement does not consume MySQL's one-shot
     * characteristics.  BEGIN (including the autocommit=0 implicit BEGIN)
     * does consume them, even if a later statement in that block fails. */
    if (autoCommit == 0 || explicitBegin)
        nextIsolationSet = nextReadOnlySet = false;
    else
        nextTransactionApplied = true;
    if (autoCommit == 0 && !explicitBegin)
        mysqlBeginImplicitTransaction();
}

void
finishMySQLNextTransaction(bool transactionEnd)
{
    if (nextTransactionApplied || transactionEnd)
    {
        nextIsolationSet = nextReadOnlySet = false;
        nextTransactionApplied = false;
    }
}

/* Reset only the per-session copy; global variable values belong to the server. */
void
resetSystemVariableSession(void)
{
    MemoryContext oldcontext;

    if (sessionSystemVars == NULL)
        return;
    nextTransactionApplied = false;
    nextIsolationSet = nextReadOnlySet = false;
    oldcontext = MemoryContextSwitchTo(TopMemoryContext);
    hash_destroy(sessionSystemVars);
    sessionSystemVars = NULL;
    lockGlobalSystemVars();
    initSessionSystemVars(baseSystemVars, baseSystemVarsNum);
    for (int i = 0; i < baseSystemVarsNum; i++)
    {
        SystemVar *base = baseSystemVars[i];
        SystemVar *global;
        SystemVar *session;

        if (!base->sessionDefValueFromGlobal ||
            (base->sessionGlobalType != 0 && base->sessionGlobalType != 3))
            continue;
        global = hash_search(globalSystemVars, base->varName, HASH_FIND, NULL);
        session = hash_search(sessionSystemVars, base->varName, HASH_FIND, NULL);
        if (global && session)
        {
            copySystemVar(global, session);
        }
    }

    unlockGlobalSystemVars();
    for (int i = 0; i < baseSystemVarsNum; i++)
    {
        SystemVar *session = hash_search(sessionSystemVars, baseSystemVars[i]->varName,
                                          HASH_FIND, NULL);
        applySystemVarValue(session->varName, session->varConfValue);
    }
    MemoryContextSwitchTo(oldcontext);
}
