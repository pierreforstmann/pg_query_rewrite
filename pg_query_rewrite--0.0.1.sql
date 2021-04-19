drop function if exists pgqr_add_rule();
drop function if exists pgqr_rules();
--
CREATE FUNCTION pgqr_add_rule(cstring, cstring) RETURNS BOOLEAN 
 AS 'pg_query_rewrite.so', 'pgqr_add_rule'
 LANGUAGE C STRICT;
--
CREATE FUNCTION pgqr_rules() RETURNS setof record
 AS 'pg_query_rewrite.so', 'pgqr_rules'
 LANGUAGE C STRICT;
--
