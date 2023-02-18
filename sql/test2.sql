drop extension if exists pg_query_rewrite;
--
create extension pg_query_rewrite;
select pgqr_truncate();
--
drop table if exists t;
create table t(x int, y int);
--
insert into t values(1,2);
--
select pgqr_add_rule('select 10;','select 11;');
select pgqr_add_rule('select 1+1;','select 1+2;');
select pgqr_add_rule('select x from t;','select x,y from t;');
--
select 10;
select 1+1;
select x from t;
--
select pgqr_remove_rule('select 1+1;');
select pgqr_rules();
select 1+1;
--
select pgqr_remove_rule('select 10;');
select pgqr_rules();
select 10;
--
select pgqr_remove_rule('select x from t;');
select pgqr_rules();
select x from t;
--
select x from t;
select 1+1;
select 10;

