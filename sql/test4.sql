drop database db1;
drop database db2;
select pgqr_truncate();
--
--
create database db1;
create database db2;
--
--
\c db1
create extension pg_query_rewrite;
select pgqr_add_rule('select 1;','select 11;');
select 1;
select pgqr_rules();
--
\c db2
create extension pg_query_rewrite;
select pgqr_add_rule('select 2;','select 22;');
select 2;
select pgqr_rules();
--
\c db1
select 1;
select 2;
select pgqr_rules();
--
\c db2
select 1;
select 2;
--
select pgqr_remove_rule('select 2;');
select 1;
select 2;
select pgqr_rules();
--
--
\c db1
select 1;
select 2;
select pgqr_remove_rule('select 1;');
select 1;
select 2;
select pgqr_rules();

