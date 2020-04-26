-- \echo Use "CREATE EXTENSION pg_query_rewrite" to load this file . \quit
drop table pg_rewrite_rule cascade;
drop function pg_rewrite_query_check_rules_number;
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
  raise exception 'Too many rules';
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


