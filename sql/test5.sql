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
select * from t;
--
select pgqr_add_rule('delete from t;','delete from t where 1=0;');
--
delete from t;
--
select * from t;
--
select pgqr_rules();
