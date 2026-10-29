
/*-------------------------------------------------------------------------
 *
 * mys_tablecmds.c
 *	  MySQL sequence process
 *
 *
 * 版权所有 (c) 2019-2023, 易景科技保留所有权利。
 * Copyright (c) 2019-2023, Halo Tech Co.,Ltd. All rights reserved.
 *
 * 易景科技是Halo Database、Halo Database Management System、羲和数据
 * 库、羲和数据库管理系统（后面简称 Halo ）软件的发明人同时也为知识产权权
 * 利人。Halo 软件的知识产权，以及与本软件相关的所有信息内容（包括但不限
 * 于文字、图片、音频、视频、图表、界面设计、版面框架、有关数据或电子文档等）
 * 均受中华人民共和国法律法规和相应的国际条约保护，易景科技享有上述知识产
 * 权，但相关权利人依照法律规定应享有的权利除外。未免疑义，本条所指的“知识
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
 *	  src/backend/commands/mysql/mys_tablecmds.c
 *
 *-------------------------------------------------------------------------
 */



/* OpenHalo generated-object helpers extracted from the PG14 table executor. */
#include "postgres.h"
#include "mb/pg_wchar.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "commands/mysql/mys_tablecmds.h"
#include "nodes/makefuncs.h"
#include "parser/scansup.h"
#include "parser/parse_func.h"
#include "parser/mysql/mys_parse_utilcmd.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

static char *
get_trigger_name(Oid trigOid);
static char *
tailor(char *prefix, char *firstPart, char *secondPart);
static RenameStmt *
renameSeq(Oid oldSeqOid, char *namespaceName, char *relName);
static RenameStmt *
renameTrig(Oid oldTrigOid, char *namespaceName, char *oldRelName, char *newTrigName);
static RenameStmt *
renameFunc(Oid oldFuncOid, char *namespaceName, char *newFuncName);
static Oid
mysGetColumnAutoIncTrig(Relation rel, Oid seqOid, char *colName);
static Oid
mysGetColumnAutoIncFunc(Relation rel, Oid autoIncTrigOid, char *colName);

/* Generated ENUM/SET types are owned by one column, unlike shared user types. */
bool
mysColumnOwnsType(Oid relid, AttrNumber attnum, Oid typeoid)
{
	Relation	depRel;
	ScanKeyData keys[2];
	SysScanDesc scan;
	HeapTuple	tuple;
	bool		owned = false;

	depRel = table_open(DependRelationId, AccessShareLock);
	ScanKeyInit(&keys[0], Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(TypeRelationId));
	ScanKeyInit(&keys[1], Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(typeoid));
	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							 NULL, 2, keys);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_depend dependency = (Form_pg_depend) GETSTRUCT(tuple);

		if (dependency->objsubid == 0 &&
			dependency->refclassid == RelationRelationId &&
			dependency->refobjid == relid &&
			dependency->refobjsubid == attnum &&
			dependency->deptype == DEPENDENCY_AUTO)
		{
			owned = true;
			break;
		}
	}
	systable_endscan(scan);
	table_close(depRel, AccessShareLock);
	return owned;
}

/* Keep explicitly named MySQL indexes local to their owning table. */
char *
mysBuildInternalIndexName(Oid relOid, char *oriIndexName)
{
	char	   *prefix = psprintf("_%u_", relOid);
	char	   *name;

	if (strncmp(oriIndexName, prefix, strlen(prefix)) == 0)
		name = pstrdup(oriIndexName);
	else
		name = psprintf("%s%s", prefix, oriIndexName);
	truncate_identifier(name, strlen(name), false);
	pfree(prefix);
	return name;
}

/*
 * Resolve only indexes belonging to this table.  The fallback for an older
 * OID prefix also handles indexes restored from an OpenHalo logical dump.
 */
char *
mysCheckIndexName(Oid relOid, char *oriIndexName)
{
	Relation	rel = table_open(relOid, NoLock);
	List	   *indexes = RelationGetIndexList(rel);
	ListCell   *lc;
	char	   *internal = mysBuildInternalIndexName(relOid, oriIndexName);
	char	   *match = NULL;

	foreach(lc, indexes)
	{
		char	   *name = get_rel_name(lfirst_oid(lc));

		if (strcmp(name, oriIndexName) == 0 || strcmp(name, internal) == 0)
		{
			list_free(indexes);
			table_close(rel, NoLock);
			pfree(internal);
			return name;
		}
		pfree(name);
	}
	foreach(lc, indexes)
	{
		char	   *name = get_rel_name(lfirst_oid(lc));
		char	   *suffix = name;

		if (*suffix++ == '_' && *suffix >= '0' && *suffix <= '9')
		{
			while (*suffix >= '0' && *suffix <= '9')
				suffix++;
			if (*suffix == '_' && strcmp(suffix + 1, oriIndexName) == 0)
			{
				if (match)
					ereport(ERROR,
							(errcode(ERRCODE_AMBIGUOUS_ALIAS),
							 errmsg("index name \"%s\" is ambiguous on table \"%s\"",
									oriIndexName, RelationGetRelationName(rel))));
				match = name;
				continue;
			}
		}
		pfree(name);
	}
	list_free(indexes);
	table_close(rel, NoLock);
	if (match)
	{
		pfree(internal);
		return match;
	}
	return internal;
}

/* Keep the source product's rule that AUTO_INCREMENT remains indexed. */
void
mysCheckAutoIncIndexDrop(Relation rel, Oid indexOid)
{
	List	   *indexes = RelationGetIndexList(rel);
	TupleDesc	desc = RelationGetDescr(rel);
	Relation	dropped = index_open(indexOid, AccessShareLock);

	for (int att = 0; att < desc->natts; att++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc, att);
		bool		affected = false;
		bool		remaining = false;
		ListCell   *lc;

		if (attr->attisdropped || !attr->atthasdef)
			continue;
		for (int key = 0; key < dropped->rd_index->indnkeyatts; key++)
			if (dropped->rd_index->indkey.values[key] == attr->attnum)
				affected = true;
		if (!affected || !OidIsValid(getColumnDefaultSeq(rel, NameStr(attr->attname))))
			continue;
		foreach(lc, indexes)
		{
			Oid			oid = lfirst_oid(lc);
			Relation	other;

			if (oid == indexOid)
				continue;
			other = index_open(oid, AccessShareLock);
			if (other->rd_index->indisvalid)
				for (int key = 0; key < other->rd_index->indnkeyatts; key++)
					if (other->rd_index->indkey.values[key] == attr->attnum)
						remaining = true;
			index_close(other, AccessShareLock);
			if (remaining)
				break;
		}
		if (!remaining)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("Incorrect table definition; the auto increment column \"%s\" of table \"%s\" must be defined as a key",
							NameStr(attr->attname), RelationGetRelationName(rel))));
	}
	index_close(dropped, AccessShareLock);
	list_free(indexes);
}

Oid
getColumnDefaultSeq(Relation rel, const char *colName)
{
    HeapTuple attTuple;
    Oid seqOid = InvalidOid;

    attTuple = SearchSysCacheCopyAttName(rel->rd_id, colName);
    if (!HeapTupleIsValid(attTuple))
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                 errmsg("column \"%s\" of relation \"%s\" does not exist",
                        colName, RelationGetRelationName(rel))));
    }

    if (((Form_pg_attribute)GETSTRUCT(attTuple))->atthasdef)
    {
        Relation attrdefRel;
        Relation depRel;
        ScanKeyData keys[2];
        SysScanDesc scan;
        HeapTuple attrdefTuple;
        Oid columnDefaultOid;
        HeapTuple depTuple;

        attrdefRel = table_open(AttrDefaultRelationId, AccessShareLock);
        depRel = table_open(DependRelationId, AccessShareLock);

        ScanKeyInit(&keys[0],
                    Anum_pg_attrdef_adrelid,
                    BTEqualStrategyNumber,
                    F_OIDEQ,
                    ObjectIdGetDatum(RelationGetRelid(rel)));
        ScanKeyInit(&keys[1],
                    Anum_pg_attrdef_adnum,
                    BTEqualStrategyNumber,
                    F_INT2EQ,
                    Int16GetDatum(((Form_pg_attribute)GETSTRUCT(attTuple))->attnum));
        scan = systable_beginscan(attrdefRel, AttrDefaultIndexId, true,
                                  NULL, 2, keys);
        attrdefTuple = systable_getnext(scan);
        if (!HeapTupleIsValid(attrdefTuple))
            elog(ERROR, "missing default for column \"%s\" of relation \"%s\"",
                 colName, RelationGetRelationName(rel));
        columnDefaultOid = ((Form_pg_attrdef)GETSTRUCT(attrdefTuple))->oid;
        systable_endscan(scan);

        ScanKeyInit(&keys[0],
                    Anum_pg_depend_classid,
                    BTEqualStrategyNumber, F_OIDEQ,
                    ObjectIdGetDatum(AttrDefaultRelationId));
        ScanKeyInit(&keys[1],
                    Anum_pg_depend_objid,
                    BTEqualStrategyNumber, F_OIDEQ,
                    ObjectIdGetDatum(columnDefaultOid));

        scan = systable_beginscan(depRel, DependDependerIndexId, true,
                                  NULL, 2, keys);
        while (HeapTupleIsValid(depTuple = systable_getnext(scan)))
        {
            Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(depTuple);

            if (foundDep->refclassid == RelationRelationId &&
                foundDep->objsubid == 0 &&
                foundDep->refobjsubid == 0 &&
                foundDep->deptype == DEPENDENCY_NORMAL &&
                get_rel_relkind(foundDep->refobjid) == RELKIND_SEQUENCE)
            {
                if (!OidIsValid(seqOid))
                    seqOid = foundDep->refobjid;
                else
                {
                    systable_endscan(scan);
                    elog(ERROR, "more than one sequence be dependent on column \"%s\" of relation \"%s\"",
                         colName, RelationGetRelationName(rel));
                }
            }
        }
        systable_endscan(scan);

        table_close(attrdefRel, AccessShareLock);
        table_close(depRel, AccessShareLock);
    }

    heap_freetuple(attTuple);

    return seqOid;
}

static char *
get_trigger_name(Oid trigOid)
{
    Relation trigRel;
    ScanKeyData trigKey;
    SysScanDesc trigScan;
    HeapTuple trigTuple;
    char *trigName = NULL;

    trigRel = table_open(TriggerRelationId, AccessShareLock);

    ScanKeyInit(&trigKey, Anum_pg_trigger_oid, BTEqualStrategyNumber,
                F_OIDEQ, ObjectIdGetDatum(trigOid));

    trigScan = systable_beginscan(trigRel, TriggerOidIndexId, true,
                                    NULL, 1, &trigKey);

    trigTuple = systable_getnext(trigScan);

    if (HeapTupleIsValid(trigTuple))
	{
		trigName = pstrdup(((Form_pg_trigger) GETSTRUCT(trigTuple))->tgname.data);
	}

    systable_endscan(trigScan);
	table_close(trigRel, AccessShareLock);

    return trigName;
}

char *
mysBuildCheckNameForSet(void)
{
    return pstrdup("halo_set_check");
}

char *
mysBuildSeqName(char *tableName)
{
    char *seqName;

    /* 如果表名过长则截断 */
    seqName = palloc0(NAMEDATALEN);
    pg_snprintf(seqName, NAMEDATALEN, "seq%.*s",
                pg_mbcliplen(tableName, strlen(tableName), NAMEDATALEN - 1 - 3),
                tableName);

    return seqName;
}

char *
mysBuildTrigNameForAutoInc(char *tableName)
{
    char *trigName;

    /* 如果表名过长则截断 */
    trigName = palloc0(NAMEDATALEN);
    pg_snprintf(trigName, NAMEDATALEN, "ait%.*s",
                pg_mbcliplen(tableName, strlen(tableName), NAMEDATALEN - 1 - 3),
                tableName);

    return trigName;
}

char *
mysBuildTrigFuncNameForAutoInc(char *tableName)
{
    char *funcName;

    /* 如果表名过长则截断 */
    funcName = palloc0(NAMEDATALEN);
    pg_snprintf(funcName, NAMEDATALEN, "aitf%.*s",
                pg_mbcliplen(tableName, strlen(tableName), NAMEDATALEN - 1 - 4),
                tableName);

    return funcName;
}

char *
mysBuildTrigNameForOnUpdateNow(char *tableName, char *colName)
{
    return tailor("aut", tableName, colName);
}

char *
mysBuildTrigFuncNameForOnUpdateNow(char *tableName, char *colName)
{
    return tailor("autf", tableName, colName);
}

char *
mysBuildSetDomainName(char *tablename, char *colname)
{
    return tailor("set_", tablename, colname);
}

char *
mysBuildEnumDomainName(char *tablename, char *colname)
{
    return tailor("enum_", tablename, colname);
}

char *
mysBuildPartitionTableName(char *tableName, char *pTableName)
{
    return tailor("", tableName, pTableName);
}

static char *
tailor(char *prefix, char *firstPart, char *secondPart)
{
    char *ret;
    int prefixLen;
    int firstPartLen;
    int secondPartLen;

    ret = palloc0(NAMEDATALEN);
    prefixLen = strlen(prefix);
    firstPartLen = strlen(firstPart);
    secondPartLen = strlen(secondPart);
    if ((prefixLen + firstPartLen + secondPartLen) <= (NAMEDATALEN - 1))
    {
        pg_snprintf(ret, NAMEDATALEN, "%s%s%s", prefix, firstPart, secondPart);
    }
    else
    {
        int firstPartAllocatedSize;
        int secondPartAllocatedSize;

        firstPartAllocatedSize = (NAMEDATALEN - 1 - prefixLen) / 3 * 2;
        secondPartAllocatedSize = (NAMEDATALEN - 1 - prefixLen) - firstPartAllocatedSize;
        if (firstPartLen < firstPartAllocatedSize)
        {
            secondPartAllocatedSize = (NAMEDATALEN - 1 - prefixLen) - firstPartLen;
            firstPartAllocatedSize = firstPartLen;
        }
        if (secondPartLen < secondPartAllocatedSize)
        {
            firstPartAllocatedSize = (NAMEDATALEN - 1 - prefixLen) - secondPartLen;
            secondPartAllocatedSize = secondPartLen;
        }

        firstPartAllocatedSize = pg_mbcliplen(firstPart, firstPartLen,
                                              firstPartAllocatedSize);
        secondPartAllocatedSize = pg_mbcliplen(secondPart, secondPartLen,
                                               secondPartAllocatedSize);
        memcpy(ret, prefix, prefixLen);
        memcpy(ret + prefixLen, firstPart, firstPartAllocatedSize);
        memcpy(ret + prefixLen + firstPartAllocatedSize, secondPart,
               secondPartAllocatedSize);
    }

    return ret;
}

static RenameStmt *
renameSeq(Oid oldSeqOid, char *namespaceName, char *relName)
{
    RenameStmt *renameStmt;
    char *oldSeqName;
    char *newSeqName;

    oldSeqName = get_rel_name(oldSeqOid);
    newSeqName = mysBuildSeqName(relName);
    if (strcmp(oldSeqName, newSeqName) == 0)
    {
        return NULL;
    }

    renameStmt = makeNode(RenameStmt);
    renameStmt->renameType = OBJECT_SEQUENCE;
    renameStmt->relation = makeRangeVar(namespaceName, oldSeqName, -1);
    renameStmt->subname = NULL;
    renameStmt->newname = pstrdup(newSeqName);
    renameStmt->missing_ok = false;

    return renameStmt;
}

static RenameStmt *
renameTrig(Oid oldTrigOid, char *namespaceName, char *oldRelName, char *newTrigName)
{
    RenameStmt *renameStmt;
    char *oldTrigName;

    Relation trigRel;
    ScanKeyData trigKey;
    SysScanDesc trigScan;
    HeapTuple trigTuple;

    trigRel = table_open(TriggerRelationId, AccessShareLock);
    ScanKeyInit(&trigKey, Anum_pg_trigger_oid, BTEqualStrategyNumber,
                F_OIDEQ, ObjectIdGetDatum(oldTrigOid));
    trigScan = systable_beginscan(trigRel, TriggerOidIndexId, true,
                                    NULL, 1, &trigKey);
    trigTuple = systable_getnext(trigScan);
    if (HeapTupleIsValid(trigTuple))
    {
        Form_pg_trigger trigInfo = (Form_pg_trigger) GETSTRUCT(trigTuple);

        oldTrigName = pstrdup(trigInfo->tgname.data);
        systable_endscan(trigScan);
        table_close(trigRel, AccessShareLock);
    }
    else
    {
        systable_endscan(trigScan);
        table_close(trigRel, AccessShareLock);
        elog(ERROR, "Oid为%d的触发器不存在。", oldTrigOid);
    }

    if (strcmp(oldTrigName, newTrigName) == 0)
    {
        return NULL;
    }

    renameStmt = makeNode(RenameStmt);
    renameStmt->renameType = OBJECT_TRIGGER;
    renameStmt->relation = makeRangeVar(namespaceName, oldRelName, -1);
    renameStmt->subname = pstrdup(oldTrigName);
    renameStmt->newname = pstrdup(newTrigName);
    renameStmt->missing_ok = false;

    return renameStmt;
}

static RenameStmt *
renameFunc(Oid oldFuncOid, char *namespaceName, char *newFuncName)
{
    RenameStmt *renameStmt;
    char *oldFuncName;

    HeapTuple funcTuple;
    ObjectWithArgs *renameObject;

    funcTuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(oldFuncOid));
    if (HeapTupleIsValid(funcTuple))
    {
        Form_pg_proc funcInfo = (Form_pg_proc) GETSTRUCT(funcTuple);
        oldFuncName = pstrdup(NameStr(funcInfo->proname));
        ReleaseSysCache(funcTuple);
    }
    else
    {
        elog(ERROR, "Oid为%d的函数不存在。", oldFuncOid);
    }

    if (strcmp(oldFuncName, newFuncName) == 0)
    {
        return NULL;
    }

    renameObject = makeNode(ObjectWithArgs);
    renameObject->objname = list_make2(makeString(namespaceName),
                                       makeString(oldFuncName));
    renameObject->args_unspecified = true;
    renameStmt = makeNode(RenameStmt);
    renameStmt->renameType = OBJECT_FUNCTION;
    renameStmt->object = (Node *)renameObject;
    renameStmt->newname = pstrdup(newFuncName);
    renameStmt->missing_ok = false;

    return renameStmt;
}

static Oid
mysGetColumnAutoIncTrig(Relation rel, Oid seqOid, char *colName)
{
    Relation depRel;
    ScanKeyData depKeys[2];
    SysScanDesc depScan;
    HeapTuple depTuple;
    char *oldTrigName;
    Oid autoIncTrigOid = InvalidOid;

    oldTrigName = mysBuildTrigNameForAutoInc(RelationGetRelationName(rel));

    if (!SearchSysCacheExistsAttName(rel->rd_id, colName))
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                 errmsg("column \"%s\" of relation \"%s\" does not exist",
                        colName, RelationGetRelationName(rel))));
    }

    if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(seqOid)))
    {
        elog(ERROR, "Sequence don't exist.");
    }

    depRel = table_open(DependRelationId, AccessShareLock);

    ScanKeyInit(&depKeys[0],
                    Anum_pg_depend_refclassid,
                    BTEqualStrategyNumber, F_OIDEQ,
                    ObjectIdGetDatum(RelationRelationId));
    ScanKeyInit(&depKeys[1],
                Anum_pg_depend_refobjid,
                BTEqualStrategyNumber, F_OIDEQ,
                ObjectIdGetDatum(seqOid));

    depScan = systable_beginscan(depRel, DependReferenceIndexId, true,
                                 NULL, 2, depKeys);

    while (HeapTupleIsValid(depTuple = systable_getnext(depScan)))
    {
        Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(depTuple);

        if (foundDep->classid == TriggerRelationId &&
            foundDep->objsubid == 0 &&
            foundDep->deptype == DEPENDENCY_INTERNAL)
        {
            autoIncTrigOid = foundDep->objid;
            break;
        }
    }
    systable_endscan(depScan);

    table_close(depRel, AccessShareLock);

    if (!OidIsValid(autoIncTrigOid))
    {
        autoIncTrigOid = get_trigger_oid(rel->rd_id, oldTrigName, true);

        if (OidIsValid(autoIncTrigOid))
        {
            ObjectAddress objAddr;
            ObjectAddress refObjAddr;

            objAddr.classId = TriggerRelationId;
            objAddr.objectId = autoIncTrigOid;
            objAddr.objectSubId = 0;

            refObjAddr.classId = RelationRelationId;
            refObjAddr.objectId = seqOid;
            refObjAddr.objectSubId = 0;

            recordDependencyOn(&objAddr, &refObjAddr, DEPENDENCY_INTERNAL);

            CommandCounterIncrement();
        }
    }

    if (OidIsValid(autoIncTrigOid))
    {
        char *trigName = get_trigger_name(autoIncTrigOid);

        if (strcmp(oldTrigName, trigName) != 0)
        {
            elog(NOTICE, "实现auto increment的触发器不符合命名规则.");
        }
    }
    else
    {
        // 不应该发生
        elog(ERROR, "Halo中自增字段必须有相应的触发器.");
    }

    return autoIncTrigOid;
}

static Oid
mysGetColumnAutoIncFunc(Relation rel, Oid autoIncTrigOid, char *colName)
{
    Relation depRel;
    ScanKeyData depKeys[2];
    SysScanDesc depScan;
    HeapTuple depTuple;
    char *oldFuncName;
    Oid autoIncFuncOid = InvalidOid;

    if (!OidIsValid(autoIncTrigOid))
        return InvalidOid;

    oldFuncName = mysBuildTrigFuncNameForAutoInc(RelationGetRelationName(rel));

    if (!SearchSysCacheExistsAttName(rel->rd_id, colName))
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                 errmsg("column \"%s\" of relation \"%s\" does not exist",
                        colName, RelationGetRelationName(rel))));
    }

    depRel = table_open(DependRelationId, AccessShareLock);

    ScanKeyInit(&depKeys[0],
                Anum_pg_depend_refclassid,
                BTEqualStrategyNumber, F_OIDEQ,
                ObjectIdGetDatum(TriggerRelationId));
    ScanKeyInit(&depKeys[1],
                Anum_pg_depend_refobjid,
                BTEqualStrategyNumber, F_OIDEQ,
                ObjectIdGetDatum(autoIncTrigOid));

    depScan = systable_beginscan(depRel, DependReferenceIndexId, true,
                                    NULL, 2, depKeys);

    while (HeapTupleIsValid(depTuple = systable_getnext(depScan)))
    {
        Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(depTuple);

        if (foundDep->classid == ProcedureRelationId &&
            foundDep->objsubid == 0 &&
            foundDep->deptype == DEPENDENCY_AUTO)
        {
            autoIncFuncOid = foundDep->objid;
            break;
        }
    }
    systable_endscan(depScan);

    table_close(depRel, AccessShareLock);

    if (!OidIsValid(autoIncFuncOid))
    {
        int i = 0;
        for (i = 0; i < rel->trigdesc->numtriggers; i++)
        {
            Trigger trig = rel->trigdesc->triggers[i];
            if (trig.tgoid == autoIncTrigOid)
            {
                autoIncFuncOid = trig.tgfoid;
                break;
            }
        }

        if (OidIsValid(autoIncFuncOid))
        {
            ObjectAddress objAddr;
            ObjectAddress refObjAddr;

            objAddr.classId = ProcedureRelationId;
            objAddr.objectId = autoIncFuncOid;
            objAddr.objectSubId = 0;

            refObjAddr.classId = TriggerRelationId;
            refObjAddr.objectId = autoIncTrigOid;
            refObjAddr.objectSubId = 0;

            recordDependencyOn(&objAddr, &refObjAddr, DEPENDENCY_AUTO);

            CommandCounterIncrement();
        }
    }

    if (OidIsValid(autoIncFuncOid))
    {
        char *funcName = get_func_name(autoIncFuncOid);
        if (strcmp(oldFuncName, funcName) != 0)
        {
            elog(NOTICE, "实现on update now的函数不符合命名规则.");
        }
    }
    else
    {
        // 不应该发生
    }

    return autoIncFuncOid;
}

Oid
mysGetColumnOnUpdateNowTrig(Relation rel, char *colName)
{
    HeapTuple attTuple;
    int attnum;
    char *oldTrigName;
    Oid onUpdateNowTrigOid = InvalidOid;
    Oid oriTypeOid = InvalidOid;
    Oid baseTypeOid = InvalidOid;

    oldTrigName = mysBuildTrigNameForOnUpdateNow(RelationGetRelationName(rel), colName);

    attTuple = SearchSysCacheCopyAttName(rel->rd_id, colName);
    if (!HeapTupleIsValid(attTuple))
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                 errmsg("column \"%s\" of relation \"%s\" does not exist",
                        colName, RelationGetRelationName(rel))));
    }

    attnum = ((Form_pg_attribute)GETSTRUCT(attTuple))->attnum;
    oriTypeOid = ((Form_pg_attribute)GETSTRUCT(attTuple))->atttypid;
    baseTypeOid = getBaseType(oriTypeOid);
    heap_freetuple(attTuple);

    if (baseTypeOid == TIMESTAMPOID || baseTypeOid == TIMESTAMPTZOID)
    {
        Relation depRel;
        ScanKeyData depKeys[3];
        SysScanDesc depScan;
        HeapTuple depTuple;

        depRel = table_open(DependRelationId, AccessShareLock);

        ScanKeyInit(&depKeys[0],
                    Anum_pg_depend_refclassid,
                    BTEqualStrategyNumber, F_OIDEQ,
                    ObjectIdGetDatum(RelationRelationId));
        ScanKeyInit(&depKeys[1],
                    Anum_pg_depend_refobjid,
                    BTEqualStrategyNumber, F_OIDEQ,
                    ObjectIdGetDatum(RelationGetRelid(rel)));
        ScanKeyInit(&depKeys[2],
                    Anum_pg_depend_refobjsubid,
                    BTEqualStrategyNumber, F_INT4EQ,
                    Int32GetDatum(attnum));

        depScan = systable_beginscan(depRel, DependReferenceIndexId, true,
                                     NULL, 3, depKeys);

        while (HeapTupleIsValid(depTuple = systable_getnext(depScan)))
        {
            Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(depTuple);

            if (foundDep->classid == TriggerRelationId &&
                foundDep->objsubid == 0 &&
                foundDep->deptype == DEPENDENCY_INTERNAL)
            {
                onUpdateNowTrigOid = foundDep->objid;
                break;
            }
        }
        systable_endscan(depScan);

        table_close(depRel, AccessShareLock);

        if (!OidIsValid(onUpdateNowTrigOid))
        {
            onUpdateNowTrigOid = get_trigger_oid(rel->rd_id, oldTrigName, true);

            if (OidIsValid(onUpdateNowTrigOid))
            {
                ObjectAddress objAddr;
	            ObjectAddress refObjAddr;

                objAddr.classId = TriggerRelationId;
                objAddr.objectId = onUpdateNowTrigOid;
                objAddr.objectSubId = 0;

                refObjAddr.classId = RelationRelationId;
                refObjAddr.objectId = RelationGetRelid(rel);
                refObjAddr.objectSubId = attnum;

                recordDependencyOn(&objAddr, &refObjAddr, DEPENDENCY_INTERNAL);

                CommandCounterIncrement();
            }
        }

        if (OidIsValid(onUpdateNowTrigOid))
        {
            char *trigName = get_trigger_name(onUpdateNowTrigOid);

            if (strcmp(oldTrigName, trigName) != 0)
            {
                elog(NOTICE, "实现on update now的触发器不符合命名规则.");
            }
        }
    }

    return onUpdateNowTrigOid;
}

void
mysProcessAutoIncForRenameAtt(Relation targetRel, char *oldColName,
                              char *newColName, List **stmts)
{
    Oid autoIncSeqOid;

    if (OidIsValid(autoIncSeqOid = getColumnDefaultSeq(targetRel, oldColName)))
    {
        RenameStmt *renameSeqStmt;
        RenameStmt *renameTrigStmt;
        RenameStmt *renameFunStmt;
        CreateFunctionStmt *createFuncStmt;
        AlterObjectDependsStmt *bindStmt;
        char *namespaceName;
        char *relName;
        RangeVar *rangeVar;
        char *newTrigName;
        char *newFuncName;
        Oid oldTrigOid;
        Oid oldFuncOid;

        namespaceName = get_namespace_name(RelationGetNamespace(targetRel));
        relName = RelationGetRelationName(targetRel);

        renameSeqStmt = renameSeq(autoIncSeqOid, namespaceName, relName);
        if (renameSeqStmt != NULL)
            *stmts = lappend(*stmts, renameSeqStmt);

        oldTrigOid = mysGetColumnAutoIncTrig(targetRel, autoIncSeqOid, oldColName);
        newTrigName = mysBuildTrigNameForAutoInc(relName);
        renameTrigStmt = renameTrig(oldTrigOid, namespaceName, relName, newTrigName);
        if (renameTrigStmt != NULL)
        {
            *stmts = lappend(*stmts, renameTrigStmt);
        }

        oldFuncOid = mysGetColumnAutoIncFunc(targetRel, oldTrigOid, oldColName);
        newFuncName = mysBuildTrigFuncNameForAutoInc(relName);
        renameFunStmt = renameFunc(oldFuncOid, namespaceName, newFuncName);
        if (renameFunStmt != NULL)
            *stmts = lappend(*stmts, renameFunStmt);

        createFuncStmt = createAutoIncrementTriggerFunc(namespaceName,
                                                        relName,
                                                        newColName);
        *stmts = lappend(*stmts, createFuncStmt);

        rangeVar = makeRangeVar(namespaceName, relName, -1);
        bindStmt = bindTriggerFunctionToTrigger(createFuncStmt->funcname,
                                                rangeVar,
                                                newTrigName);
        *stmts = lappend(*stmts, bindStmt);
    }
}

void
mysProcessSetEnumForRenameAtt(Relation targetRel, char *oldColName,
                              char *newColName, List **stmts)
{
    char *namespaceName;
    char *tableName;
    HeapTuple attTuple;
    Form_pg_attribute attInfo;
    HeapTuple typTuple;
    char *oldTypeName;
    char *newTypeName;
    RenameStmt *renameStmt;

    namespaceName = get_namespace_name(RelationGetNamespace(targetRel));
    tableName = RelationGetRelationName(targetRel);
    attTuple = SearchSysCacheCopyAttName(targetRel->rd_id, oldColName);

    if (!HeapTupleIsValid(attTuple))
    {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_COLUMN),
                 errmsg("column \"%s\" of relation \"%s\" does not exist",
                        oldColName, RelationGetRelationName(targetRel))));
    }

    attInfo = (Form_pg_attribute) GETSTRUCT(attTuple);
    if (!mysColumnOwnsType(targetRel->rd_id, attInfo->attnum, attInfo->atttypid))
    {
        heap_freetuple(attTuple);
        return;
    }
    typTuple = SearchSysCacheCopy1(TYPEOID, ObjectIdGetDatum(attInfo->atttypid));
    if (!HeapTupleIsValid(typTuple))
        elog(ERROR, "cache lookup failed for type %u", attInfo->atttypid);
    oldTypeName = pstrdup(NameStr(((Form_pg_type)GETSTRUCT(typTuple))->typname));
    heap_freetuple(typTuple);
    heap_freetuple(attTuple);

    if (strncasecmp(oldTypeName, "set_", 4) == 0)
    {
        newTypeName = mysBuildSetDomainName(tableName, newColName);
        renameStmt = makeNode(RenameStmt);
        renameStmt->renameType = OBJECT_DOMAIN;
        renameStmt->object = (Node*)list_make2(makeString(namespaceName),
                                               makeString(pstrdup(oldTypeName)));
        renameStmt->subname = NULL;
        renameStmt->newname = pstrdup(newTypeName);
        renameStmt->missing_ok = false;
        *stmts = lappend(*stmts, renameStmt);
    }
    else if (strncasecmp(oldTypeName, "enum_", 5) == 0)
    {
        newTypeName = mysBuildEnumDomainName(tableName, newColName);
        renameStmt = makeNode(RenameStmt);
        renameStmt->renameType = OBJECT_TYPE;
        renameStmt->object = (Node*)list_make2(makeString(namespaceName),
                                               makeString(pstrdup(oldTypeName)));
        renameStmt->subname = NULL;
        renameStmt->newname = pstrdup(newTypeName);
        renameStmt->missing_ok = false;
        *stmts = lappend(*stmts, renameStmt);
    }
    else
    {
        // do nothing;
    }
}
