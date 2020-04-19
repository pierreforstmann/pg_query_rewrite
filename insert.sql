truncate table pg_rewrite_rule;
insert into pg_rewrite_rule(pattern, replacement, enabled) values('select 1+1;','select 1+2;',true);
insert into pg_rewrite_rule(pattern, replacement, enabled) values('select 10;','select 11;',true);
insert into pg_rewrite_rule(pattern, replacement, enabled) values('select * from dual;', 'select ''dummy'';',true);
