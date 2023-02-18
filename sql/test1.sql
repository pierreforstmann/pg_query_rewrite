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
select pgqr_add_rule('delete from t;','delete from t where 1=0;');
select pgqr_add_rule('update t set x=y;','update t set x=y where 1=0;');
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
