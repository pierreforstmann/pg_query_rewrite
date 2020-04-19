# pg_query_rewrite
pg_query_rewrite is a PostgreSQL extension which allows to translate a given source SQL statement into another pre-defined SQL statement.


# Installation
## Compiling

This module can be built using the standard PGXS infrastructure. For this to work, the pg_config program must be available in your $PATH:
  
`git clone https://github.com/pierreforstmann/pg_query_rewrite.git` <br>
`cd pg_query_rewrite` <br>
`make` <br>
`make install` <br>

## PostgreSQL setup

Extension must be loaded:

At server level with `shared_preload_libraries` parameter: <br> 
`shared_preload_libraries = 'pg_query_rewrite'` <br>
And following SQL statement must be run: <br>
`create extension pg_query_rewrite;`

This extension must be installed in each database where it is intented to be used. <br>

## Usage
pg_query_rewrite has no GUC.<br>
The extension is enabled if the related libraries is loaded and the table `pg_rewrite_rule` exists in the related database.<br>
<br>
Query rewrite rules must be inserted in the table pg_query_rules which has the following structure: <br>
`# \d pg_rewrite_rule `<br>
`                               Table "public.pg_rewrite_rule" `<br>
`   Column    |  Type   | Collation | Nullable |                   Default                   `<br>
`-------------+---------+-----------+----------+---------------------------------------------`<br>
` id          | integer |           | not null | nextval('pg_rewrite_rule_id_seq'::regclass) `<br>
` pattern     | text    |           | not null | <br>`
` replacement | text    |           | not null | <br>`
` enabled     | boolean |           | not null | <br>`


Note that the number of rules is currently hard coded in the extension code and is currently set to 10. <br>
Extension behaviour is not defined it the number of rows in pg_rewrite_rule exceeds this maximum. <br>
<br>
The query rewrites rules are cached in the backend private memory: if the rules are updated in the current database session,
only new database session will take care of the new state of rules (whether a new rule is created, updated
or deleted) - existing database session including the current one cannot take into account the updated rules.<br>

# Example

In postgresql.conf:

`shared_preload_libraries = 'pg_query_rewrite'` <br>

In `pg_rewrite_rule` table:

`# select * from pg_rewrite_rule;` <br>
` id |   pattern   | replacement | enabled ` <br>
`----+-------------+-------------+---------` <br>
` 38 | select 1+1; | select 1+2; | t`        <br>
`(1 row)`

With above setup:

`# select 1+1;` <br>
` ?column? ` <br>
`----------` <br>
`        3`  <br>
`(1 row)`    <br>

`# select (1 + 1);` <br>
` ?column? ` <br>
`----------` <br>
`        3`  <br>
`(1 row)`    <br>
