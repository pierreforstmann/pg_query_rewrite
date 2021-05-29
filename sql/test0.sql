drop extension if exists pg_query_rewrite;
--
create extension pg_query_rewrite;
select pgqr_truncate_rule();
--
select pgqr_add_rule('select 10;','select 11;');
--
select 10;
--
drop extension pg_query_rewrite;
