-- Live metadata must follow DDL and the invoking user's privileges.
-- Legacy scripts granted PUBLIC write access to the variable definitions.
-- Sessions need to read these definitions; PUBLIC must not alter them.
REVOKE ALL PRIVILEGES ON TABLE mys_informa_schema.base_variables FROM PUBLIC;
GRANT SELECT ON TABLE mys_informa_schema.base_variables TO PUBLIC;

DO $$ BEGIN
  IF EXISTS (SELECT FROM mys_informa_schema.statistics) THEN
    RAISE EXCEPTION 'cannot replace nonempty legacy statistics metadata' USING ERRCODE='0A000';
  END IF;
END $$;
-- Binary upgrade restores the original PG14 extension definitions verbatim.
-- Bring those 1.5 functions to the same state as a fresh PG18 installation.
ALTER FUNCTION mysql.set_system_variable(pg_catalog.text,pg_catalog.text,pg_catalog.bool) VOLATILE;
ALTER FUNCTION mysql.get_system_variable(pg_catalog.text,pg_catalog.bool) VOLATILE;
CREATE OR REPLACE FUNCTION mysql.set_system_next_transaction_variable(pg_catalog.text,pg_catalog.anycompatible)
RETURNS void AS '$libdir/mysm','setNextTransactionVariable' LANGUAGE C VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION mysql.quote_identifier(pg_catalog.text) RETURNS pg_catalog.text
LANGUAGE sql IMMUTABLE STRICT PARALLEL SAFE
SET search_path = pg_catalog
AS $$ SELECT '`' || pg_catalog.replace($1, '`', '``') || '`' $$;

CREATE FUNCTION mysql.index_logical_name(pg_catalog.oid) RETURNS pg_catalog.text
LANGUAGE sql STABLE STRICT PARALLEL SAFE
SET search_path = pg_catalog
AS $$
  SELECT CASE WHEN i.indisprimary THEN 'PRIMARY'
              WHEN c.relname ~ '^_[0-9]+_' THEN pg_catalog.regexp_replace(c.relname, '^_[0-9]+_', '')
              ELSE c.relname::text END
  FROM pg_catalog.pg_index i JOIN pg_catalog.pg_class c ON c.oid=i.indexrelid
  WHERE i.indexrelid=$1
$$;

CREATE FUNCTION mysql.metadata_table(pg_catalog.text,pg_catalog.text,require_table pg_catalog.bool DEFAULT false) RETURNS pg_catalog.oid
LANGUAGE plpgsql STABLE
AS $$
DECLARE
  schema_name text := coalesce($1,pg_catalog.current_schema());
  target oid;
BEGIN
  IF schema_name='information_schema' THEN schema_name:='mys_informa_schema'; END IF;
  SELECT c.oid INTO target FROM pg_catalog.pg_class c
    JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace
    WHERE n.nspname=schema_name AND c.relname=$2 AND c.relkind IN ('r','p','v','m','f');
  IF target IS NULL THEN
    RAISE EXCEPTION 'Table %.% does not exist',schema_name,$2 USING ERRCODE='42P01';
  END IF;
  IF NOT pg_catalog.has_schema_privilege(schema_name,'USAGE') OR NOT
    (pg_catalog.pg_has_role((SELECT relowner FROM pg_catalog.pg_class WHERE oid=target),'USAGE') OR
     pg_catalog.has_table_privilege(target,'SELECT,INSERT,UPDATE,DELETE,TRUNCATE,REFERENCES,TRIGGER') OR
     (NOT require_table AND pg_catalog.has_any_column_privilege(target,'SELECT,INSERT,UPDATE,REFERENCES'))) THEN
    RAISE EXCEPTION 'permission denied for table %.%',schema_name,$2 USING ERRCODE='42501';
  END IF;
  RETURN target;
END $$;

-- Do not CASCADE: an existing user dependency must cause a transactional refusal.
DROP TABLE mys_informa_schema.statistics;
CREATE VIEW mys_informa_schema._index_statistics WITH (security_barrier=true) AS
SELECT 'def'::varchar(256) AS table_catalog,
       n.nspname::varchar(256) COLLATE mysql.case_insensitive AS table_schema,
       t.relname::varchar(256) COLLATE mysql.case_insensitive AS table_name,
       (CASE WHEN i.indisunique THEN 0 ELSE 1 END)::bigint AS non_unique,
       n.nspname::varchar(256) COLLATE mysql.case_insensitive AS index_schema,
       mysql.index_logical_name(i.indexrelid)::varchar(256) COLLATE mysql.case_insensitive AS index_name,
       k.ord::bigint AS seq_in_index,
       a.attname::varchar(256) COLLATE mysql.case_insensitive AS column_name,
       (CASE WHEN pg_catalog.pg_index_column_has_property(i.indexrelid,k.ord,'orderable')
             THEN CASE WHEN (i.indoption[k.ord-1] & 1)=1 THEN 'D' ELSE 'A' END END)::varchar(256) AS "collation",
       (CASE WHEN s.n_distinct<0 THEN pg_catalog.round((-s.n_distinct*greatest(t.reltuples,0))::numeric)
             WHEN s.n_distinct>=0 THEN pg_catalog.round(s.n_distinct::numeric)
             ELSE 0 END)::bigint AS cardinality,
       NULL::bigint AS sub_part,
       NULL::varchar(256) AS packed,
       (CASE WHEN a.attnotnull THEN '' ELSE 'YES' END)::varchar(256) AS nullable,
       pg_catalog.upper(am.amname)::varchar(256) AS index_type,
       ''::varchar(256) AS comment,
       coalesce(pg_catalog.obj_description(i.indexrelid,'pg_class'),'')::varchar(256) AS index_comment,
       'YES'::varchar(3) AS is_visible,
       (CASE WHEN i.indkey[k.ord-1]=0 THEN pg_catalog.pg_get_indexdef(i.indexrelid,k.ord,true) END)::text AS expression
FROM pg_catalog.pg_index i
JOIN pg_catalog.pg_class t ON t.oid=i.indrelid
JOIN pg_catalog.pg_namespace n ON n.oid=t.relnamespace
JOIN pg_catalog.pg_class ix ON ix.oid=i.indexrelid
JOIN pg_catalog.pg_am am ON am.oid=ix.relam
CROSS JOIN LATERAL pg_catalog.generate_series(1,i.indnkeyatts) AS k(ord)
LEFT JOIN pg_catalog.pg_attribute a ON a.attrelid=t.oid AND a.attnum=i.indkey[k.ord-1]
LEFT JOIN pg_catalog.pg_stats s ON s.schemaname=n.nspname AND s.tablename=t.relname
  AND s.attname=a.attname AND NOT s.inherited
WHERE t.relkind IN ('r','p','m') AND i.indisvalid AND i.indislive
  AND pg_catalog.has_schema_privilege(n.oid,'USAGE')
  AND (pg_catalog.pg_has_role(t.relowner,'USAGE') OR
       pg_catalog.has_table_privilege(t.oid,'SELECT,INSERT,UPDATE,DELETE,TRUNCATE,REFERENCES,TRIGGER') OR
       pg_catalog.has_any_column_privilege(t.oid,'SELECT,INSERT,UPDATE,REFERENCES'));
GRANT SELECT ON mys_informa_schema._index_statistics TO PUBLIC;
DO $$ BEGIN
  IF pg_catalog.current_setting('mysql.halo_mysql_version') LIKE '8.%' THEN
    EXECUTE 'CREATE VIEW mys_informa_schema.statistics WITH (security_barrier=true) AS SELECT * FROM mys_informa_schema._index_statistics';
  ELSE
    EXECUTE 'CREATE VIEW mys_informa_schema.statistics WITH (security_barrier=true) AS SELECT table_catalog,table_schema,table_name,non_unique,index_schema,index_name,seq_in_index,column_name,"collation",cardinality,sub_part,packed,nullable,index_type,comment,index_comment FROM mys_informa_schema._index_statistics';
  END IF;
END $$;
GRANT SELECT ON mys_informa_schema.statistics TO PUBLIC;

CREATE OR REPLACE FUNCTION mysql.show_table_indexs(pg_catalog.text,pg_catalog.text)
RETURNS SETOF record LANGUAGE plpgsql STABLE
SET standard_parserengine_auxiliary = on
AS $$
DECLARE target oid := mysql.metadata_table($1,$2);
BEGIN
  RETURN QUERY SELECT s.table_name::varchar(256),s.non_unique::int4,s.index_name::varchar(256),
    s.seq_in_index::int4,s.column_name::varchar(256),s.collation::varchar(256),
    s.cardinality::int8,s.sub_part::int8,s.packed::varchar(256),s.nullable::varchar(8),
    s.index_type::varchar(256),s.comment::varchar(512),s.index_comment::varchar(512)
  FROM mys_informa_schema._index_statistics s
  WHERE s.table_schema COLLATE "C"=(SELECT nspname FROM pg_catalog.pg_namespace WHERE oid=
                        (SELECT relnamespace FROM pg_catalog.pg_class WHERE oid=target))
    AND s.table_name COLLATE "C"=$2 COLLATE "C"
  ORDER BY s.non_unique,s.index_name COLLATE "C",s.seq_in_index;
END $$;

CREATE OR REPLACE FUNCTION mysql.show_table_indexs_for_8(pg_catalog.text,pg_catalog.text)
RETURNS SETOF record LANGUAGE plpgsql STABLE
SET standard_parserengine_auxiliary = on
AS $$
DECLARE target oid := mysql.metadata_table($1,$2);
BEGIN
  RETURN QUERY SELECT s.table_name::varchar(256),s.non_unique::int4,s.index_name::varchar(256),
    s.seq_in_index::int4,s.column_name::varchar(256),s.collation::varchar(256),
    s.cardinality::int8,s.sub_part::int8,s.packed::varchar(256),s.nullable::varchar(8),
    s.index_type::varchar(256),s.comment::varchar(512),s.index_comment::varchar(512),
    s.is_visible::varchar(64),s.expression::text
  FROM mys_informa_schema._index_statistics s
  WHERE s.table_schema COLLATE "C"=(SELECT nspname FROM pg_catalog.pg_namespace WHERE oid=
                        (SELECT relnamespace FROM pg_catalog.pg_class WHERE oid=target))
    AND s.table_name COLLATE "C"=$2 COLLATE "C"
  ORDER BY s.non_unique,s.index_name COLLATE "C",s.seq_in_index;
END $$;

CREATE OR REPLACE FUNCTION mysql.show_create_table(pg_catalog.text,pg_catalog.text)
RETURNS pg_catalog.text LANGUAGE plpgsql STABLE
AS $$
DECLARE
  target oid := mysql.metadata_table($1,$2,true);
  relation record;
BEGIN
  SELECT c.relkind,c.relnamespace,c.relname,n.nspname INTO STRICT relation
  FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace
  WHERE c.oid=target;
  IF relation.relkind='v' THEN
    RETURN mysql.show_create_view_impl(relation.nspname::text,relation.relname::text);
  END IF;
  RETURN mysql.show_create_table_impl(relation.relnamespace,target,relation.nspname::text,relation.relname::text);
END $$;

-- Ordered SHOW CREATE TABLE implementation, derived from the existing helper.
CREATE OR REPLACE FUNCTION mysql.show_create_table_impl(pg_catalog.Oid, pg_catalog.Oid,
                                                        pg_catalog.text, pg_catalog.text)
RETURNS pg_catalog.text
AS
$$
DECLARE
    ret pg_catalog.text;
    schOid pg_catalog.Oid;
    tabOid pg_catalog.Oid;
    schName pg_catalog.text;
    tabName pg_catalog.text;
    columnIndex pg_catalog.int4;
    autoInc pg_catalog.int8;
    tmp pg_catalog.int4;
    tableComment pg_catalog.text;
    columnNames pg_catalog.text;
    columnNums pg_catalog.text;
    records RECORD;
BEGIN
    schOid := $1;
    tabOid := $2;
    schName := $3;
    tabName := $4;
    IF mysql.metadata_table(schName,tabName,true) <> tabOid OR
       (SELECT relnamespace FROM pg_catalog.pg_class WHERE oid=tabOid) <> schOid THEN
        RAISE EXCEPTION 'table identity does not match its metadata request';
    END IF;
    columnIndex := 0;

    ret := pg_catalog.concat('CREATE TABLE ',mysql.quote_identifier(tabName),' (');

    FOR records IN
        select att.attname as column_name,
                (pg_get_expr((select ad.adbin from pg_catalog.pg_attrdef ad where ad.adrelid = tabOid and ad.adnum = att.attnum), tabOid)) as column_default,
                (case when att.attnotnull then 'NO' else 'YES' end) as is_nullable,
                (select
                    case
                        when ty.typname='tinyint' then 'tinyint'
                        when ty.typname='tinyint signed' then 'tinyint signed'
                        when ty.typname='tinyint unsigned' then 'tinyint unsigned'
                        when ty.typname='smallint' then 'smallint'
                        when ty.typname='smallint signed' then 'smallint signed'
                        when ty.typname='smallint unsigned' then 'smallint unsigned'
                        when ty.typname='mediumint' then 'mediumint'
                        when ty.typname='mediumint signed' then 'mediumint signed'
                        when ty.typname='mediumint unsigned' then 'mediumint unsigned'
                        when ty.typname='int4' then case when mysql.version() like '8.%' then 'int' else 'int(11)' end
                        when ty.typname='int signed' then 'int signed'
                        when ty.typname='int unsigned' then 'int unsigned'
                        when ty.typname='int8' then 'bigint'
                        when ty.typname='bigint signed' then 'bigint signed'
                        when ty.typname='bigint unsigned' then 'bigint unsigned'
                        when ty.typname='varchar' then pg_catalog.concat('varchar', pg_catalog.replace(format_type(att.atttypid, att.atttypmod), 'character varying', ''))
                        when ty.typname='bpchar' then pg_catalog.concat('char', pg_catalog.replace(format_type(att.atttypid, att.atttypmod), 'character', ''))
                        when ty.typname='text' then 'text'
                        when ty.typname='date' then 'date'
                        when ty.typname='time' then 'time'
                        when ty.typname='datetime' then 'datetime(6)'
                        when ty.typname='timestamp' then (pg_catalog.concat('timestamp(', att.atttypmod, ')'))
                        when ty.typname='bit' then (pg_catalog.concat('bit(', att.atttypmod, ')'))
                        when ty.typname='varbit' and att.atttypmod != -1 then (pg_catalog.concat('bit(', att.atttypmod, ')'))
                        when ty.typname='varbit' and att.atttypmod = -1 then 'bit(1)'
                        when ty.typname='binary' then 'binary'
                        when ty.typname='varbinary' then 'varbinary'
                        when ty.typname='tinyblob' then 'tinyblob'
                        when ty.typname='blob' then 'blob'
                        when ty.typname='mediumblob' then 'mediumblob'
                        when ty.typname='longblob' then 'longblob'
                        when mysql.left(ty.typname, 5)='float' then 'float'
                        when ty.typname='double' then 'double'
                        when ty.typname='decimal' then 'decimal'
                        when ty.typname='numeric' then 'decimal'
                        when ty.typname='real' then 'double'
                        when ty.typname='year_' then 'year'
                        else ty.typname
                    end
                    from pg_catalog.pg_type ty
                    where att.atttypid = ty.oid)::varchar(64) as column_type,
                col_description(att.attrelid, att.attnum) as column_comment,
                att.attnum::int4 as attnum,
                (select coll.collname from pg_collation coll where coll.oid = att.attcollation)::varchar(128) as column_collation
            from pg_catalog.pg_attribute att
            WHERE att.attrelid = tabOid and 0 < att.attnum and att.attisdropped = false
    LOOP
        if (0 < records.attnum) then
            if (0 < columnIndex) then
                ret := pg_catalog.concat(ret, ',\n  ');
            else
                ret := pg_catalog.concat(ret, '\n  ');
            end if;

            ret := pg_catalog.concat(ret,mysql.quote_identifier(records.column_name));
            ret := pg_catalog.concat(ret, ' ', records.column_type);
            if (records.is_nullable = 'NO') then
                ret := pg_catalog.concat(ret, ' NOT NULL');
            end if;
            if (records.column_default is not null) then
                if (0 < position('nextval(' in records.column_default)) then
                    ret := pg_catalog.concat(ret, ' AUTO_INCREMENT');
                    execute format('select max(%I) from %I.%I', records.column_name, schName, tabName)
                    into autoInc;
                else
                    tmp := position('::character varying' in records.column_default);
                    if (0 < tmp) then
                        ret := pg_catalog.concat(ret, ' DEFAULT ', substring(records.column_default from 1 for (tmp - 1)));
                    else
                        ret := pg_catalog.concat(ret, ' DEFAULT ', records.column_default);
                    end if;
                end if;
            else
                if (records.is_nullable != 'NO') then
                    ret := pg_catalog.concat(ret, ' DEFAULT NULL');
                end if;
            end if;

            if (records.column_collation is not null) then
                ret := pg_catalog.concat(ret, ' COLLATE ', records.column_collation);
            end if;

            if (records.column_comment is not null) then
                ret := pg_catalog.concat(ret, ' COMMENT \'', records.column_comment, '\'');
            end if;

            columnIndex := columnIndex + 1;
        end if;
    END LOOP;

    FOR records IN
        SELECT i.indexrelid,i.indisprimary,i.indisunique,
               mysql.index_logical_name(i.indexrelid) AS indname
        FROM pg_catalog.pg_index i WHERE i.indrelid=tabOid AND i.indisvalid
        ORDER BY i.indisprimary DESC,i.indisunique DESC,mysql.index_logical_name(i.indexrelid) COLLATE "C"
    LOOP
        SELECT pg_catalog.string_agg(
            CASE WHEN a.attnum IS NULL THEN '('||pg_catalog.pg_get_indexdef(records.indexrelid,k.ord,true)||')'
                 ELSE mysql.quote_identifier(a.attname) ||
                      CASE WHEN (i.indoption[k.ord-1] & 1)=1 THEN ' DESC' ELSE '' END END,
            ',' ORDER BY k.ord) INTO columnNames
        FROM pg_catalog.pg_index i
        CROSS JOIN LATERAL pg_catalog.generate_series(1,i.indnkeyatts) k(ord)
        LEFT JOIN pg_catalog.pg_attribute a ON a.attrelid=i.indrelid AND a.attnum=i.indkey[k.ord-1]
        WHERE i.indexrelid=records.indexrelid;
        ret := pg_catalog.concat(ret, ',\n  ');
        IF records.indisprimary THEN
            ret := pg_catalog.concat(ret,'PRIMARY KEY (',columnNames,')');
        ELSIF records.indisunique THEN
            ret := pg_catalog.concat(ret,'UNIQUE KEY ',mysql.quote_identifier(records.indname),' (',columnNames,')');
        ELSE
            ret := pg_catalog.concat(ret,'KEY ',mysql.quote_identifier(records.indname),' (',columnNames,')');
        END IF;
    END LOOP;

    FOR records IN
        select
            cons.conname,
            (select pg_catalog.string_agg(mysql.quote_identifier(a.attname),',' ORDER BY k.ord)
             FROM pg_catalog.unnest(cons.conkey) WITH ORDINALITY k(attnum,ord)
             JOIN pg_catalog.pg_attribute a ON a.attrelid=cons.conrelid AND a.attnum=k.attnum) AS column_name,
            (select cl.relname from pg_catalog.pg_class cl where cl.oid=cons.confrelid) AS ref_table_name,
            (select pg_catalog.string_agg(mysql.quote_identifier(a.attname),',' ORDER BY k.ord)
             FROM pg_catalog.unnest(cons.confkey) WITH ORDINALITY k(attnum,ord)
             JOIN pg_catalog.pg_attribute a ON a.attrelid=cons.confrelid AND a.attnum=k.attnum) AS ref_column_name,
            cons.confdeltype,
            cons.confupdtype
        from pg_constraint cons
        where (cons.contype = 'f') and (cons.conrelid = tabOid)
    LOOP
        ret := pg_catalog.concat(ret, ',\n  ');
        ret := pg_catalog.concat(ret,'CONSTRAINT ',mysql.quote_identifier(records.conname),
                                 ' FOREIGN KEY (',records.column_name,') REFERENCES ',
                                 mysql.quote_identifier(records.ref_table_name),' (',records.ref_column_name,') ON DELETE ');

        if (records.confdeltype = 'a') then
            ret := pg_catalog.concat(ret, 'NO ACTION ');
        elsif (records.confdeltype = 'c') then
            ret := pg_catalog.concat(ret, 'CASCADE ');
        elsif (records.confdeltype = 'd') then
            ret := pg_catalog.concat(ret, 'SET DEFAULT ');
        elsif (records.confdeltype = 'n') then
            ret := pg_catalog.concat(ret, 'SET NULL ');
        else
            ret := pg_catalog.concat(ret, 'RESTRICT ');
        end if;
        ret := pg_catalog.concat(ret, ' ON UPDATE ');
        if (records.confupdtype = 'a') then
            ret := pg_catalog.concat(ret, 'NO ACTION ');
        elsif (records.confupdtype = 'c') then
            ret := pg_catalog.concat(ret, 'CASCADE ');
        elsif (records.confupdtype = 'd') then
            ret := pg_catalog.concat(ret, 'SET DEFAULT ');
        elsif (records.confupdtype = 'n') then
            ret := pg_catalog.concat(ret, 'SET NULL ');
        else
            ret := pg_catalog.concat(ret, 'RESTRICT ');
        end if;
    END LOOP;

    FOR records IN
        select cons.conname, (select pg_get_constraintdef(cons.oid)) as consrc
        from pg_constraint cons
        where (cons.contype = 'c') and (cons.conrelid = tabOid)
    LOOP
        ret := pg_catalog.concat(ret, ',\n  ');
        ret := pg_catalog.concat(ret, 'CONSTRAINT ');
        ret := pg_catalog.concat(ret, mysql.quote_identifier(records.conname));
        ret := pg_catalog.concat(ret, ' ');
        ret := pg_catalog.concat(ret, records.consrc);
    END LOOP;

    if (0 < autoInc) then
        autoInc := autoInc + 1;
        ret := pg_catalog.concat(ret, '\n) ENGINE=InnoDB AUTO_INCREMENT=', autoInc::text, ' DEFAULT CHARSET=utf8mb4');
    else
        ret := pg_catalog.concat(ret, '\n) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4');
    end if;

    tableComment := '';
    execute format('select ds.description from pg_description ds where ds.objoid = %s and objsubid = 0', tabOid) into tableComment;
    if (tableComment != '') then
        ret := pg_catalog.concat(ret, ' COMMENT ''', tableComment, '''');
    end if;

    return ret;
END;
$$
STABLE LANGUAGE plpgsql SET search_path=pg_catalog
SET standard_parserengine_auxiliary = on;

CREATE OR REPLACE FUNCTION mysql.show_table_columns(pg_catalog.text, pg_catalog.text)
RETURNS setof RECORD
AS
$$
DECLARE
    schemaName pg_catalog.varchar(128);
    tableName pg_catalog.varchar(128);
    schOid pg_catalog.Oid;
    tabOid pg_catalog.Oid;
    sch RECORD;
    tab RECORD;
    columns RECORD;
BEGIN
    if ($1 is not null) then
        schemaName := $1;
    else
        schemaName := (select pg_catalog.current_schema());
    end if;
    if ($2 is not null) then
        tableName := $2;
    else
        raise EXCEPTION 'invalid table name';
    end if;

    schOid := 0;
    tabOid := 0;

    FOR sch IN
        select oid from pg_catalog.pg_namespace where nspname = schemaName
    LOOP
        schOid := sch.oid;
    END LOOP;
    if (schOid = 0) then
        raise EXCEPTION 'invalid schema name';
    end if;

    FOR tab IN
        select oid from pg_catalog.pg_class where (16384 <= oid) and (relname = tableName) and (relnamespace = schOid)
    LOOP
        tabOid := tab.oid;
    END LOOP;
    if (tabOid = 0) then
        raise EXCEPTION 'invalid table name';
    end if;

    PERFORM mysql.metadata_table(schemaName,tableName);

    FOR columns IN
        select
            att.attname::varchar(256) as Field,
            (select
                case
                    when ty.typname='tinyint' then 'tinyint'
                    when ty.typname='tinyint signed' then 'tinyint signed'
                    when ty.typname='tinyint unsigned' then 'tinyint unsigned'
                    when ty.typname='smallint' then 'smallint'
                    when ty.typname='smallint signed' then 'smallint signed'
                    when ty.typname='smallint unsigned' then 'smallint unsigned'
                    when ty.typname='mediumint' then 'mediumint'
                    when ty.typname='mediumint signed' then 'mediumint signed'
                    when ty.typname='mediumint unsigned' then 'mediumint unsigned'
                    when ty.typname='int4' then case when mysql.version() like '8.%' then 'int' else 'int(11)' end
                    when ty.typname='int signed' then 'int signed'
                    when ty.typname='int unsigned' then 'int unsigned'
                    when ty.typname='int8' then 'bigint'
                    when ty.typname='bigint signed' then 'bigint signed'
                    when ty.typname='bigint unsigned' then 'bigint unsigned'
                    when ty.typname='varchar' then pg_catalog.concat('varchar', pg_catalog.replace(format_type(att.atttypid, att.atttypmod), 'character varying', ''))
                    when ty.typname='bpchar' then pg_catalog.concat('char', pg_catalog.replace(format_type(att.atttypid, att.atttypmod), 'character', ''))
                    when ty.typname='text' then 'text'
                    when ty.typname='date' then 'date'
                    when ty.typname='time' then 'time'
                    when ty.typname='datetime' then 'datetime(6)'
                    when ty.typname='timestamp' then (pg_catalog.concat('timestamp(', att.atttypmod, ')'))
                    when ty.typname='bit' then (pg_catalog.concat('bit(', att.atttypmod, ')'))
                    when ty.typname='varbit' and att.atttypmod != -1 then (pg_catalog.concat('bit(', att.atttypmod, ')'))
                    when ty.typname='varbit' and att.atttypmod = -1 then 'bit(1)'
                    when ty.typname='binary' then 'binary'
                    when ty.typname='varbinary' then 'varbinary'
                    when ty.typname='tinyblob' then 'tinyblob'
                    when ty.typname='blob' then 'blob'
                    when ty.typname='mediumblob' then 'mediumblob'
                    when ty.typname='longblob' then 'longblob'
                    when mysql.left(ty.typname, 5)='float' then 'float'
                    when ty.typname='double' then 'double'
                    when ty.typname='decimal' then 'decimal'
                    when ty.typname='numeric' then 'decimal'
                    when ty.typname='real' then 'double'
                    when ty.typname='year_' then 'year'
                    else ty.typname
                end
                from pg_catalog.pg_type ty
                where att.atttypid = ty.oid)::varchar(64) as Type,
            (case when att.attnotnull then 'NO' else 'YES' end)::varchar(8) as Null,
            (mysql.get_column_key(att.attrelid, att.attname))::varchar(256) as Key,
            (mysql.amend_def_val(pg_get_expr((select ad.adbin from pg_catalog.pg_attrdef ad where ad.adnum = att.attnum and ad.adrelid = att.attrelid), tabOid)))::text as Default,
            (mysql.get_column_extra(mysql.amend_def_val_for_internal(pg_get_expr((select ad.adbin from pg_catalog.pg_attrdef ad where ad.adnum = att.attnum and ad.adrelid = att.attrelid), tabOid))))::text as Extra
        FROM pg_catalog.pg_attribute att
        WHERE att.attrelid=tabOid AND att.attnum>0 AND NOT att.attisdropped
          AND (pg_catalog.pg_has_role((SELECT relowner FROM pg_catalog.pg_class WHERE oid=tabOid),'USAGE')
               OR pg_catalog.has_column_privilege(tabOid,att.attnum,'SELECT,INSERT,UPDATE,REFERENCES'))
                order by att.attnum
    LOOP
        return next columns;
    END LOOP;

    return;
END;
$$
STABLE LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION mysql.show_table_full_columns(pg_catalog.text, pg_catalog.text)
RETURNS setof RECORD
AS
$$
DECLARE
    schemaName pg_catalog.varchar(128);
    tableName pg_catalog.varchar(128);
    schOid pg_catalog.Oid;
    tabOid pg_catalog.Oid;
    sch RECORD;
    tab RECORD;
    columns RECORD;
BEGIN
    if ($1 is not null) then
        schemaName := $1;
    else
        schemaName := (select pg_catalog.current_schema());
    end if;
    if ($2 is not null) then
        tableName := $2;
    else
        raise EXCEPTION 'invalid table name';
    end if;

    schOid := 0;
    tabOid := 0;

    FOR sch IN
        select oid from pg_catalog.pg_namespace where nspname = schemaName
    LOOP
        schOid := sch.oid;
    END LOOP;
    if (schOid = 0) then
        raise EXCEPTION 'invalid schema name';
    end if;

    FOR tab IN
        select oid from pg_catalog.pg_class where (16384 <= oid) and (relname = tableName) and (relnamespace = schOid)
    LOOP
        tabOid := tab.oid;
    END LOOP;
    if (tabOid = 0) then
        raise EXCEPTION 'invalid table name';
    end if;

    PERFORM mysql.metadata_table(schemaName,tableName);

    FOR columns IN
        select
            att.attname::varchar(256) as Field,
            (select
                case
                    when ty.typname='tinyint' then 'tinyint'
                    when ty.typname='tinyint signed' then 'tinyint signed'
                    when ty.typname='tinyint unsigned' then 'tinyint unsigned'
                    when ty.typname='smallint' then 'smallint'
                    when ty.typname='smallint signed' then 'smallint signed'
                    when ty.typname='smallint unsigned' then 'smallint unsigned'
                    when ty.typname='mediumint' then 'mediumint'
                    when ty.typname='mediumint signed' then 'mediumint signed'
                    when ty.typname='mediumint unsigned' then 'mediumint unsigned'
                    when ty.typname='int4' then case when mysql.version() like '8.%' then 'int' else 'int(11)' end
                    when ty.typname='int signed' then 'int signed'
                    when ty.typname='int unsigned' then 'int unsigned'
                    when ty.typname='int8' then 'bigint'
                    when ty.typname='bigint signed' then 'bigint signed'
                    when ty.typname='bigint unsigned' then 'bigint unsigned'
                    when ty.typname='varchar' then pg_catalog.concat('varchar', pg_catalog.replace(format_type(att.atttypid, att.atttypmod), 'character varying', ''))
                    when ty.typname='bpchar' then pg_catalog.concat('char', pg_catalog.replace(format_type(att.atttypid, att.atttypmod), 'character', ''))
                    when ty.typname='text' then 'text'
                    when ty.typname='date' then 'date'
                    when ty.typname='time' then 'time'
                    when ty.typname='datetime' then 'datetime(6)'
                    when ty.typname='timestamp' then (pg_catalog.concat('timestamp(', att.atttypmod, ')'))
                    when ty.typname='bit' then (pg_catalog.concat('bit(', att.atttypmod, ')'))
                    when ty.typname='varbit' and att.atttypmod != -1 then (pg_catalog.concat('bit(', att.atttypmod, ')'))
                    when ty.typname='varbit' and att.atttypmod = -1 then 'bit(1)'
                    when ty.typname='binary' then 'binary'
                    when ty.typname='varbinary' then 'varbinary'
                    when ty.typname='blob' then 'blob'
                    when ty.typname='mediumblob' then 'mediumblob'
                    when ty.typname='longblob' then 'longblob'
                    when mysql.left(ty.typname, 5)='float' then 'float'
                    when ty.typname='double' then 'double'
                    when ty.typname='decimal' then 'decimal'
                    when ty.typname='numeric' then 'decimal'
                    when ty.typname='real' then 'double'
                    when ty.typname='year_' then 'year'
                    else ty.typname
                end
                from pg_catalog.pg_type ty
                where att.atttypid = ty.oid)::varchar(64) as Type,
            (select colla.collname from pg_catalog.pg_collation colla where colla.oid = att.attcollation)::varchar(256) as Collation,
            (case when att.attnotnull then 'NO' else 'YES' end)::varchar(8) as Null,
            (mysql.get_column_key(att.attrelid, att.attname))::varchar(256) as Key,
            (mysql.amend_def_val(pg_get_expr((select ad.adbin from pg_catalog.pg_attrdef ad where ad.adnum = att.attnum and ad.adrelid = att.attrelid), tabOid)))::text as Default,
            (mysql.get_column_extra(mysql.amend_def_val_for_internal(pg_get_expr((select ad.adbin from pg_catalog.pg_attrdef ad where ad.adnum = att.attnum and ad.adrelid = att.attrelid), tabOid))))::text as Extra,
            (SELECT pg_catalog.string_agg(pg_catalog.lower(p.priv),',' ORDER BY p.ord)
             FROM pg_catalog.unnest(ARRAY['SELECT','INSERT','UPDATE','REFERENCES']) WITH ORDINALITY p(priv,ord)
             WHERE pg_catalog.has_column_privilege(tabOid,att.attnum,p.priv))::varchar(256) AS Privileges,
            (case when col_description(att.attrelid, att.attnum) is not null then col_description(att.attrelid, att.attnum) else '' end)::varchar(512) as Comment
        FROM pg_catalog.pg_attribute att
        WHERE att.attrelid=tabOid AND att.attnum>0 AND NOT att.attisdropped
          AND (pg_catalog.pg_has_role((SELECT relowner FROM pg_catalog.pg_class WHERE oid=tabOid),'USAGE')
               OR pg_catalog.has_column_privilege(tabOid,att.attnum,'SELECT,INSERT,UPDATE,REFERENCES'))
                order by att.attnum
    LOOP
        return next columns;
    END LOOP;

    return;
END;
$$
STABLE LANGUAGE plpgsql;

CREATE OR REPLACE FUNCTION mysql.database() RETURNS pg_catalog.text
LANGUAGE sql STABLE AS $$ SELECT CASE pg_catalog.current_schema()
  WHEN 'mys_informa_schema' THEN 'information_schema' WHEN 'mys_sys' THEN 'sys'
  ELSE pg_catalog.current_schema()::pg_catalog.text END $$;
CREATE OR REPLACE FUNCTION mysql.schema() RETURNS pg_catalog.text
LANGUAGE sql STABLE AS $$ SELECT mysql.database() $$;

-- PAD SPACE equality is distinct from PostgreSQL's index/hash equality.
-- Its hash family trims the same slices and uses PG18's collation hash.
-- Native btree/hash families retain PostgreSQL's equality relation.
CREATE OR REPLACE FUNCTION mysql.textne(pg_catalog.text,pg_catalog.text)
RETURNS pg_catalog.bool AS '$libdir/mysm','textne_mys'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;
CREATE OPERATOR mysql.= (
  FUNCTION=mysql.texteq, LEFTARG=pg_catalog.text, RIGHTARG=pg_catalog.text,
  COMMUTATOR=OPERATOR(mysql.=), NEGATOR=OPERATOR(mysql.<>),
  RESTRICT=eqsel, JOIN=eqjoinsel, HASHES);
CREATE OPERATOR mysql.<> (
  FUNCTION=mysql.textne, LEFTARG=pg_catalog.text, RIGHTARG=pg_catalog.text,
  COMMUTATOR=OPERATOR(mysql.<>), NEGATOR=OPERATOR(mysql.=),
  RESTRICT=neqsel, JOIN=neqjoinsel);
CREATE FUNCTION mysql.hashtext_pad(pg_catalog.text)
RETURNS pg_catalog.int4 AS '$libdir/mysm','hashtext_pad'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;
CREATE FUNCTION mysql.hashtextextended_pad(pg_catalog.text,pg_catalog.int8)
RETURNS pg_catalog.int8 AS '$libdir/mysm','hashtextextended_pad'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;
CREATE OPERATOR CLASS mysql.text_pad_ops FOR TYPE pg_catalog.text USING hash AS
  OPERATOR 1 mysql.= (pg_catalog.text,pg_catalog.text),
  FUNCTION 1 mysql.hashtext_pad(pg_catalog.text),
  FUNCTION 2 mysql.hashtextextended_pad(pg_catalog.text,pg_catalog.int8);
