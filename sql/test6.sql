drop extension if exists pg_query_rewrite;
--
create extension pg_query_rewrite;
select pgqr_truncate_rule();
--
--
drop table if exists t;
create table t(i int);
--
drop function if exists fs;
create function fs(IN p int, OUT i int) returns int as $$
begin
    insert into t(i) values (fs.p) returning t.i into fs.i;
end$$
language plpgsql;
--
select fs(1);
select * from t;
select pgqr_rules();
--
select pgqr_add_rule('insert into t(i) values (fs.p) returning t.i', 'insert into t(i) values(2) returning t.i');
--
drop function fs;
create function fs(IN p int, OUT i int) returns int as $$
begin
    insert into t(i) values (fs.p) returning t.i into fs.i;
end$$
language plpgsql;
--
select fs(1);
select * from t;
select pgqr_rules();
--
--
--
truncate table t;
select fs(1);
select * from t;
--
drop function if exists fd;
create function fd(IN p int, OUT i int) returns int as $$
begin
    execute 'select count(*) from t where i = ' || p into fd.i using fd.p;
end$$
language plpgsql;
--
select fd(2);
select * from t;
select pgqr_rules();
--
select pgqr_add_rule('select count(*) from t where i = 2', 'select count(*) from t where i = 3');
--
select fd(2);
select * from t;
select pgqr_rules();
