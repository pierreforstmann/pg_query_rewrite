drop extension if exists pg_query_rewrite;
--
create extension pg_query_rewrite;
select pgqr_truncate();
--
select pgqr_add_rule('select 10;','select 11;');
select pgqr_add_rule('select ''Hello'';','select ''Good Bye'';');
--
select 10;
select 100;
select 'Hello';
select 'Hello ...';
--
select pgqr_add_rule('select ''Hello'';','select ''Good Bye'';');
select pgqr_add_rule('select 10;','select 11;');
--
select 10;
select 100;
select 'Hello';
select 'Hello ...';
--
select pgqr_remove_rule('select 10;');
select 10;
select 'Hello';
select pgqr_remove_rule('select ''Hello'';');
select 'Hello';
--
select pgqr_rules();
--
drop extension pg_query_rewrite;
