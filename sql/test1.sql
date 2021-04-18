drop extension if exists pg_query_rewrite;
--
create extension pg_query_rewrite;
--
drop table if exists t;
create table t(x int, y int);
--
insert into t values(1,2);
insert into pg_rewrite_rule(pattern, replacement, enabled) values('select 10;','select 11;',true);
insert into pg_rewrite_rule(pattern, replacement, enabled) values('select 1+1;','select 1+2;',true);
insert into pg_rewrite_rule(pattern, replacement, enabled) values('select x from t;','select x,y from t;',true);
insert into pg_rewrite_rule(pattern, replacement, enabled) values('delete from t;','delete from t where 1=0;',true);
insert into pg_rewrite_rule(pattern, replacement, enabled) values('update t set x=y;','update t set x=y where 1=0;',true);
select count(*) from pg_rewrite_rule;
--
select pgqr_load_rules();
--
select 10;
select 1+1;
select x from t;
begin;
delete from t;
select * from t;
rollback;
select * from t;
begin;
update t set x=y;
select * from t;
rollback;
select * from t;
