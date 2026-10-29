/*-------------------------------------------------------------------------
 *
 * bpchar.c
 *	  Extend bpchar routines
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
 *	  src/backend/utils/ddsm/mysm/bpchar.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "varatt.h"

#include <ctype.h>

#include "access/detoast.h"
#include "catalog/pg_collation.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/pg_locale.h"
#include "miscadmin.h"
#include "adapter/mysql/adapter.h"

#include "../../adt/like.c"
#include "utils/varlena.h"

PG_MODULE_MAGIC;


// static int	text_cmp(text *arg1, text *arg2, Oid collid);

PG_FUNCTION_INFO_V1(char_eq_char_for_date_format);
Datum
char_eq_char_for_date_format(PG_FUNCTION_ARGS)
{
    bool result;
    BpChar *arg1;
    BpChar *arg2;
    int len1;
    int len2;
    char* str1;
    char* str2;

    if (PG_ARGISNULL(0))
    {
        PG_RETURN_BOOL(false);
    }
    if (PG_ARGISNULL(1))
    {
        PG_RETURN_BOOL(false);
    }

    arg1 = PG_GETARG_BPCHAR_PP(0);
    arg2 = PG_GETARG_BPCHAR_PP(1);
    len1 = VARSIZE_ANY_EXHDR(arg1);
    len2 = VARSIZE_ANY_EXHDR(arg2);
    str1 = VARDATA_ANY(arg1);
    str2 = VARDATA_ANY(arg2);
    if (len1 == len2)
    {
        if (0 < len1)
        {
            if (str1[0] == str2[0])
            {
                result = true;
            }
            else
            {
                result = false;
            }
        }
        else
        {
            result = false;
        }
    }
    else
    {
        result = false;
    }

    PG_FREE_IF_COPY(arg1, 0);
    PG_FREE_IF_COPY(arg2, 1);

    PG_RETURN_BOOL(result);
}


/* Compare trimmed slices without allocating C strings or modifying Datums. */
static int
mysql_trimmed_text_cmp(FunctionCallInfo fcinfo)
{
    text *left = PG_GETARG_TEXT_PP(0);
    text *right = PG_GETARG_TEXT_PP(1);
    int llen = VARSIZE_ANY_EXHDR(left);
    int rlen = VARSIZE_ANY_EXHDR(right);
    int result;

    while (llen > 0 && VARDATA_ANY(left)[llen - 1] == ' ')
        llen--;
    while (rlen > 0 && VARDATA_ANY(right)[rlen - 1] == ' ')
        rlen--;
    result = varstr_cmp(VARDATA_ANY(left), llen,
                        VARDATA_ANY(right), rlen, PG_GET_COLLATION());
    PG_FREE_IF_COPY(left, 0);
    PG_FREE_IF_COPY(right, 1);
    return result;
}

PG_FUNCTION_INFO_V1(texteq_mys);
Datum
texteq_mys(PG_FUNCTION_ARGS)
{
    PG_RETURN_BOOL(mysql_trimmed_text_cmp(fcinfo) == 0);
}

PG_FUNCTION_INFO_V1(textne_mys);
Datum
textne_mys(PG_FUNCTION_ARGS)
{
    PG_RETURN_BOOL(mysql_trimmed_text_cmp(fcinfo) != 0);
}

static Datum
mysql_trimmed_text_hash(FunctionCallInfo fcinfo, bool extended)
{
    text *value = PG_GETARG_TEXT_PP(0);
    int len = VARSIZE_ANY_EXHDR(value);
    text *trimmed;
    Datum result;

    while (len > 0 && VARDATA_ANY(value)[len - 1] == ' ')
        len--;
    trimmed = cstring_to_text_with_len(VARDATA_ANY(value), len);
    if (extended)
        result = DirectFunctionCall2Coll(hashtextextended, PG_GET_COLLATION(),
                                         PointerGetDatum(trimmed), PG_GETARG_DATUM(1));
    else
        result = DirectFunctionCall1Coll(hashtext, PG_GET_COLLATION(),
                                         PointerGetDatum(trimmed));
    pfree(trimmed);
    PG_FREE_IF_COPY(value, 0);
    return result;
}

PG_FUNCTION_INFO_V1(hashtext_pad);
Datum
hashtext_pad(PG_FUNCTION_ARGS)
{
    return mysql_trimmed_text_hash(fcinfo, false);
}

PG_FUNCTION_INFO_V1(hashtextextended_pad);
Datum
hashtextextended_pad(PG_FUNCTION_ARGS)
{
    return mysql_trimmed_text_hash(fcinfo, true);
}

static bool
mysql_bpchar_match(FunctionCallInfo fcinfo)
{
    BpChar *str = PG_GETARG_BPCHAR_PP(0);
    text *pattern = PG_GETARG_TEXT_PP(1);
    int len = VARSIZE_ANY_EXHDR(str);
    bool result;

    while (len > 0 && VARDATA_ANY(str)[len - 1] == ' ')
        len--;
    result = GenericMatchText(VARDATA_ANY(str), len,
                              VARDATA_ANY(pattern), VARSIZE_ANY_EXHDR(pattern),
                              PG_GET_COLLATION()) == LIKE_TRUE;
    PG_FREE_IF_COPY(str, 0);
    PG_FREE_IF_COPY(pattern, 1);
    return result;
}

PG_FUNCTION_INFO_V1(bpcharlike);
Datum
bpcharlike(PG_FUNCTION_ARGS)
{
    PG_RETURN_BOOL(mysql_bpchar_match(fcinfo));
}

PG_FUNCTION_INFO_V1(bpcharnlike);
Datum
bpcharnlike(PG_FUNCTION_ARGS)
{
    PG_RETURN_BOOL(!mysql_bpchar_match(fcinfo));
}

/* text_cmp()
 * Internal comparison function for text strings.
 * Returns -1, 0 or 1
 */
// static int
// text_cmp(text *arg1, text *arg2, Oid collid)
// {
//     char	   *a1p,
//                *a2p;
//     int			len1,
//                 len2;

//     a1p = VARDATA_ANY(arg1);
//     a2p = VARDATA_ANY(arg2);

//     len1 = VARSIZE_ANY_EXHDR(arg1);
//     len2 = VARSIZE_ANY_EXHDR(arg2);

//     return varstr_cmp(a1p, len1, a2p, len2, collid);
// }
