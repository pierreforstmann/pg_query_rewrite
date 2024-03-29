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
CREATE FUNCTION pgqr_remove_rule(cstring) RETURNS BOOLEAN 
 AS 'pg_query_rewrite.so', 'pgqr_remove_rule'
 LANGUAGE C STRICT;
--
CREATE FUNCTION pgqr_truncate() RETURNS BOOLEAN
 AS 'pg_query_rewrite.so', 'pgqr_truncate'
 LANGUAGE C STRICT;
--
CREATE FUNCTION pgqr_test() RETURNS BOOLEAN
 AS 'pg_query_rewrite.so', 'pgqr_test'
 LANGUAGE C STRICT;
