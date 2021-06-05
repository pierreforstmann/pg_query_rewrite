drop extension if exists pg_query_rewrite;
--
create extension pg_query_rewrite;
select pgqr_truncate_rule();
--
--
drop table if exists t;
create table t(i int);
--
drop function if exists f;
create function f(IN p int, OUT i int) returns int as $$
begin
    insert into t(i) values (f.p) returning t.i into f.i;
end$$
language plpgsql;
--
select f(1);
select * from t;
select pgqr_rules();
--
select pgqr_add_rule('insert into t(i) values (f.p) returning t.i', 'insert into t(i) values(2) returning t.i');
--
drop function f;
create function f(IN p int, OUT i int) returns int as $$
begin
    insert into t(i) values (f.p) returning t.i into f.i;
end$$
language plpgsql;
--
select f(1);
select * from t;
select pgqr_rules();
--
