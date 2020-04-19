-- \echo Use "CREATE EXTENSION pg_query_rewrite" to load this file . \quit
create table pg_rewrite_rule(
 id 	 	serial,
 pattern 	text not null,
 replacement 	text not null,
 enabled 	boolean not null
)
