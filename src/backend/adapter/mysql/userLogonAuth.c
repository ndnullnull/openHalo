/*-------------------------------------------------------------------------
 *
 * userLogonAuth.c
 *	  MySQL adapter userLogonAuth routines
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
 *	  src/backend/adapter/mysql/userLogonAuth.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq/crypt.h"
#include "libpq/libpq.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "adapter/mysql/userLogonAuth.h"
#include "adapter/mysql/common.h"
#include "adapter/mysql/pwdEncryptDecrypt.h"
#include "adapter/mysql/netTransceiver.h"

/* Backend-private, shared by the greeting, plugin switch and verification. */
static uint8 authChallenge[SHA1_DIGEST_LENGTH + 1];
static uint32 serverCapabilities;

static bool
readLittle(const char *data, size_t length, size_t *offset, int width,
           uint64 *value)
{
    if (*offset > length || (size_t) width > length - *offset)
        return false;
    *value = 0;
    for (int i = 0; i < width; i++)
        *value |= (uint64) (uint8) data[(*offset)++] << (8 * i);
    return true;
}

static bool
readLength(const char *data, size_t length, size_t *offset, uint64 *value)
{
    uint64 prefix;

    if (!readLittle(data, length, offset, 1, &prefix))
        return false;
    if (prefix < 251)
    {
        *value = prefix;
        return true;
    }
    if (prefix == 252 || prefix == 253 || prefix == 254)
        return readLittle(data, length, offset,
                          prefix == 252 ? 2 : prefix == 253 ? 3 : 8, value);
    return false;               /* NULL/0xff are not lengths here */
}

static bool
readString(const char *data, size_t length, size_t *offset,
           char *output, size_t capacity)
{
    const char *end;
    size_t count;

    if (*offset >= length)
        return false;
    end = memchr(data + *offset, '\0', length - *offset);
    if (end == NULL)
        return false;
    count = end - (data + *offset);
    if (output)
    {
        if (count >= capacity)
            return false;
        memcpy(output, data + *offset, count);
        output[count] = '\0';
    }
    *offset += count + 1;
    return true;
}

static void
writeLittle(char *output, uint64 value, int width)
{
    for (int i = 0; i < width; i++)
    {
        output[i] = value & 0xff;
        value >>= 8;
    }
}

size_t
assembleHandshakePacketPayload(const char *haloMysVersion, char *payloadBuf,
                               size_t payloadBufLen)
{
    const char *plugin = "mysql_native_password";
    uint32 capability = 0x81fff6dfU; /* include bounded CLIENT_CONNECT_ATTRS */
    size_t versionLength = strlen(haloMysVersion);
    size_t offset = 0;

    if (versionLength > payloadBufLen || payloadBufLen - versionLength < 75)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("MySQL server version string is too long")));
    if (!pg_strong_random(authChallenge, SHA1_DIGEST_LENGTH))
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("could not generate MySQL authentication challenge")));
    /* The native-password greeting has a NUL terminator, not embedded NULs. */
    for (int i = 0; i < SHA1_DIGEST_LENGTH; i++)
        while (authChallenge[i] == 0)
            if (!pg_strong_random(&authChallenge[i], 1))
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("could not generate MySQL authentication challenge")));
    authChallenge[SHA1_DIGEST_LENGTH] = 0;
    if (EnableSSL)
        capability |= MYS_CLT_SSL;
    else
        capability &= ~MYS_CLT_SSL;
    serverCapabilities = capability;

    memset(payloadBuf, 0, payloadBufLen);
    payloadBuf[offset++] = 10;
    memcpy(payloadBuf + offset, haloMysVersion, versionLength + 1);
    offset += versionLength + 1;
    writeLittle(payloadBuf + offset, MyProcPid, 4);
    offset += 4;
    memcpy(payloadBuf + offset, authChallenge, 8);
    offset += 9;                /* first part plus filler */
    writeLittle(payloadBuf + offset, capability, 2);
    offset += 2;
    payloadBuf[offset++] = strncmp(haloMysVersion, "8.0", 3) < 0 ? 45 : 255;
    writeLittle(payloadBuf + offset, 2, 2); /* AUTOCOMMIT */
    offset += 2;
    writeLittle(payloadBuf + offset, capability >> 16, 2);
    offset += 2;
    payloadBuf[offset++] = SHA1_DIGEST_LENGTH + 1;
    offset += 10;
    memcpy(payloadBuf + offset, authChallenge + 8, 13);
    offset += 13;
    memcpy(payloadBuf + offset, plugin, strlen(plugin) + 1);
    return offset + strlen(plugin) + 1;
}

static bool
parseResponse(const char *payload, size_t length, bool protocol41,
              unsigned long *clientFlag, int *maxPacketSize,
              char *user, size_t userCapacity,
              char *auth, size_t authCapacity, size_t *authLength,
              char *database, size_t databaseCapacity)
{
    size_t offset = 0;
    uint64 flags;
    uint64 maximum;
    uint64 count;
    char plugin[100] = "mysql_native_password";

    *authLength = 0;
    user[0] = auth[0] = database[0] = '\0';
    if (!readLittle(payload, length, &offset, protocol41 ? 4 : 2, &flags) ||
        !readLittle(payload, length, &offset, protocol41 ? 4 : 3, &maximum))
        return false;
    if (protocol41)
    {
        if (length < 32 || !(flags & MYS_CLT_PROTOCOL_41))
            return false;
        offset = 32;           /* charset and reserved bytes */
    }
    *clientFlag = flags;
    *maxPacketSize = maximum > INT_MAX ? INT_MAX : (int) maximum;
    /* libmysqlclient can retain unsupported bits in its header, while only
     * emitting the optional fields negotiated in the server greeting. */
    flags &= serverCapabilities;
    if (!readString(payload, length, &offset, user, userCapacity))
        return false;

    if (!protocol41 && !(flags & MYS_CLT_CONNECT_WITH_DB))
        count = length - offset; /* 3.20 response without a database ends at EOF */
    else if (protocol41 && (flags & MYS_CLT_PLUGIN_AUTH_LENENC_CLIENT_DATA))
    {
        if (!readLength(payload, length, &offset, &count))
            return false;
    }
    else if (protocol41 && (flags & MYS_CLT_SECURE_CONNECTION))
    {
        if (!readLittle(payload, length, &offset, 1, &count))
            return false;
    }
    else
    {
        size_t start = offset;

        if (!readString(payload, length, &offset, auth, authCapacity))
            return false;
        *authLength = offset - start - 1;
        count = 0;
    }
    if (count >= authCapacity || count > length - offset)
        return false;
    if (count)
    {
        memcpy(auth, payload + offset, count);
        auth[count] = '\0';
        *authLength = count;
        offset += count;
    }
    if ((flags & MYS_CLT_CONNECT_WITH_DB) &&
        !readString(payload, length, &offset, database, databaseCapacity))
        return false;
    if (protocol41 && (flags & MYS_CLT_PLUGIN_AUTH) &&
        !readString(payload, length, &offset, plugin, sizeof(plugin)))
        return false;
    if (protocol41 && (flags & MYS_CLT_CONNECT_ATTRS))
    {
        size_t end;

        if (!readLength(payload, length, &offset, &count) || count > length - offset)
            return false;
        end = offset + count;
        while (offset < end)
        {
            /* Attribute pairs are ignored, but every key and value is bounded. */
            for (int field = 0; field < 2; field++)
            {
                if (!readLength(payload, end, &offset, &count) || count > end - offset)
                    return false;
                offset += count;
            }
        }
    }
    if (offset != length)
        return false;

    if (protocol41 && (flags & MYS_CLT_PLUGIN_AUTH) &&
        strcmp(plugin, "mysql_native_password") != 0)
    {
        const char *name = "mysql_native_password";
        char request[64];
        size_t nameLength = strlen(name) + 1;
        StringInfoData response;

        request[0] = (char) 254;
        memcpy(request + 1, name, nameLength);
        memcpy(request + 1 + nameLength, authChallenge, sizeof(authChallenge));
        netTransceiver->writePacketHeaderPayloadFlush(request,
                            1 + nameLength + sizeof(authChallenge));
        initStringInfo(&response);
        netTransceiver->readPayloadForLogon(&response);
        if ((size_t) response.len >= authCapacity)
        {
            pfree(response.data);
            return false;
        }
        memcpy(auth, response.data + response.offset, response.len);
        auth[response.len] = '\0';
        *authLength = response.len;
        pfree(response.data);
    }
    return true;
}

int
parseHandshakeRespPacketPayload(char *payload, size_t payloadLen,
                               unsigned long *clientFlag, int *maxPacketSize,
                               char *user, size_t userCapacity,
                               char *auth, size_t authCapacity, size_t *authLength,
                               char *database, size_t databaseCapacity)
{
    size_t offset = 0;
    uint64 flags;
    bool protocol41;

    if (!readLittle(payload, payloadLen, &offset, 2, &flags))
        return 0;
    *clientFlag = flags;        /* ERR format is known even if the rest is invalid */
    protocol41 = (flags & MYS_CLT_PROTOCOL_41) != 0;
    if (flags & MYS_CLT_SSL)
    {
#ifdef USE_SSL
        StringInfoData response;
        bool valid;

        /* An SSLRequest is exactly the fixed 4.1 header, with no username. */
        if (!protocol41 || !EnableSSL || payloadLen != 32 ||
            secure_open_server(MyProcPort) != 0)
            return 0;
        initStringInfo(&response);
        netTransceiver->readPayloadForLogon(&response);
        valid = response.len >= 32 &&
                memcmp(payload, response.data + response.offset, 32) == 0 &&
                parseResponse(response.data + response.offset, response.len, true,
                              clientFlag, maxPacketSize, user, userCapacity,
                              auth, authCapacity, authLength, database, databaseCapacity);
        pfree(response.data);
        return valid;
#else
        return 0;
#endif
    }
    return parseResponse(payload, payloadLen, protocol41, clientFlag, maxPacketSize,
                         user, userCapacity, auth, authCapacity, authLength,
                         database, databaseCapacity);
}

bool
mysCheckAuth(const char *userName, const char *userPwd, size_t userPwdLength)
{
    const char *logdetail = NULL;
    char *shadow;
    bool valid;

    Assert(MyProcPort->hba->auth_method == uaMD5);
    shadow = get_role_password(userName, &logdetail);
    if (shadow == NULL)
        return false;
    if (userPwdLength != SHA1_DIGEST_LENGTH ||
        strncmp(shadow, mysql_native_password_prefix, MYS_NATIVE_PASSWORD_PREFIX_LEN) != 0 ||
        strlen(shadow) != MYS_NATIVE_PASSWORD_LEN ||
        strspn(shadow + MYS_NATIVE_PASSWORD_PREFIX_LEN, "0123456789abcdef") !=
            MYS_NATIVE_PASSWORD_LEN - MYS_NATIVE_PASSWORD_PREFIX_LEN)
    {
        pfree(shadow);
        return false;
    }
    valid = checkMysNativeAuth(userPwd, shadow, authChallenge);
    pfree(shadow);
    return valid;
}
