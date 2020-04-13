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

Extension can be loaded:

At server level with `shared_preload_libraries` parameter: <br> 
`shared_preload_libraries = 'pg_query_rewrite'` <br>


## Usage
pg_query_rewrite has 2 specific GUC: <br>
`pg_query_rewrite.source`: source SQL statement <br>
`pg_query_rewrite.destination`: destination SQL statement <br>
Only 1 single statement can be defined. The SQL statement must exactly match the source statement (lowercase/uppercase and number of space characters).

## Example

In postgresql.conf:

`shared_preload_libraries = 'pg_query_rewrite'` <br>
`pg_query_rewrite.source='select 1+1;'` <br>
`pg_query_rewrite.destination='select 1+2;'` <br>

With this setup:

`pierre=# select 1+1;` <br>
` ?column? ` <br>
`----------` <br>
`        3`  <br>
`(1 row)`    <br>

`pierre=# select 1 + 1;` <br>
` ?column? ` <br>
`----------` <br>
`        2`  <br>
`(1 row)`    <br>




