
-- Session values can change between executions of the same prepared statement.
ALTER FUNCTION mysql.set_system_variable(pg_catalog.text,pg_catalog.text,pg_catalog.bool) VOLATILE;
ALTER FUNCTION mysql.get_system_variable(pg_catalog.text,pg_catalog.bool) VOLATILE;
CREATE FUNCTION mysql.set_system_next_transaction_variable(pg_catalog.text,pg_catalog.anycompatible)
RETURNS void AS '$libdir/mysm','setNextTransactionVariable' LANGUAGE C VOLATILE PARALLEL UNSAFE;

CREATE OR REPLACE FUNCTION mysql.uuid()
RETURNS text
AS
$$
BEGIN
    return gen_random_uuid();
END;
$$
LANGUAGE plpgsql;


create or replace function mysql.setval(regclass, bigint, boolean)
returns pg_catalog.int8
as 'MODULE_PATHNAME', 'mysSetval3Oid'
language C
STRICT;

