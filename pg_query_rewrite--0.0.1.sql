drop table if exists pg_rewrite_rule cascade;
drop function if exists pg_rewrite_query_check_rules_number;
drop function if exists pgqr_signal();
drop function if exists pgqr_load_rules();
drop function if exists pgqr_log_proc_array();
drop function if exists pgqr_log_rules_cache();
drop function if exists pgqr_write_extension_flag();
--
--
create table pg_rewrite_rule(
 id 	 	serial,
 pattern 	text not null,
 replacement 	text not null,
 enabled 	boolean not null
);
--
create function pg_rewrite_query_check_rules_number ()
returns trigger as $$
declare
 l_rules_count integer;
 l_max_rules_number integer;
begin
 execute 'show pg_query_rewrite.max_rules' into l_max_rules_number;
 select count(*) into l_rules_count from pg_rewrite_rule;
 if l_rules_count >  l_max_rules_number
 then
  raise exception 'Too many rules (max_rules=%)', l_max_rules_number;
 end if;
return null;
end;
$$
language plpgsql;
--
create trigger pg_rewrite_rule_trigger
after insert on pg_rewrite_rule
execute procedure pg_rewrite_query_check_rules_number();
--
CREATE FUNCTION pgqr_write_extension_flag() RETURNS BOOLEAN 
 AS 'pg_query_rewrite.so', 'pgqr_write_extension_flag'
 LANGUAGE C STRICT;
--
select pgqr_write_extension_flag();
--
CREATE FUNCTION pgqr_signal() RETURNS int 
 AS 'pg_query_rewrite.so', 'pgqr_signal'
 LANGUAGE C STRICT;
--
CREATE FUNCTION pgqr_load_rules() RETURNS BOOLEAN 
 AS 'pg_query_rewrite.so', 'pgqr_load_rules'
 LANGUAGE C STRICT;
--
CREATE FUNCTION pgqr_log_proc_array() RETURNS BOOLEAN 
 AS 'pg_query_rewrite.so', 'pgqr_log_proc_array'
 LANGUAGE C STRICT;
--
CREATE FUNCTION pgqr_log_rules_cache() RETURNS BOOLEAN 
 AS 'pg_query_rewrite.so', 'pgqr_log_rules_cache'
 LANGUAGE C STRICT;
--
