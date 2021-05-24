# pg_query_rewrite
`pg_query_rewrite`  is a PostgreSQL extension which allows to translate a given source SQL statement into another pre-defined SQL statement.


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

This extension is installed at instance level: it does not need to be installed in each database. <br>

 `pg_query_rewrite` has been successfully tested with PostgreSQL 9.5, 9.6, 10, 11, 12 and 13.

## Usage
`pg_query_rewrite` has a single GUC : `pg_query_rewrite.max_rules` which is the maximum number of SQL statements that can be translated.
<br>
This extension is enabled if the related library is loaded and if `pg_query_rewrite.max_rules` parameter is set.
<br>
<br>
To create a new rule to translate SQL statement `<source>` into SQL statement `<target>` run: 
<br>
<br>
`select pgqr_add_rule(<source>, <target>);` 
<br>
<br>
To remove a translation rule for SQL statement `<source>`, run:
<br>
<br>
`select pgqr_remove_rule(<source>);`
<br>
<br>
To remove all existing translation rules, run:
<br>
<br>
`select pgqr_truncate_rule();`
<br>
<br>
To display current translation rules, run:
<br>
<br>
`select pgqr_rules();`
<br>
<br>
## Example

In postgresql.conf:

`shared_preload_libraries = 'pg_query_rewrite'` <br>
`pg_query_rewrite.max_rules=10`

Run with psql:
```
# create extension pg_query_rewrite;
CREATE EXTENSION
# select pgqr_add_rule('select 10;','select 11;');
 pgqr_add_rule 
---------------
 t
(1 row)

# select 10;
 ?column? 
----------
       11
(1 row)

# select pgqr_rules();
                        pgqr_rules                         
-----------------------------------------------------------
 ("source=select 10;","target=select 11;",rewrite_count=1)
 (source=NULL,target=NULL,rewrite_count=0)
 (source=NULL,target=NULL,rewrite_count=0)
 (source=NULL,target=NULL,rewrite_count=0)
 (source=NULL,target=NULL,rewrite_count=0)
 (source=NULL,target=NULL,rewrite_count=0)
 (source=NULL,target=NULL,rewrite_count=0)
 (source=NULL,target=NULL,rewrite_count=0)
 (source=NULL,target=NULL,rewrite_count=0)
 (source=NULL,target=NULL,rewrite_count=0)
(10 rows)

```
## Limitations

* SQL translations rules are available for all databases: there is no way to restrict a rule to a given database.
* Maximum SQL statement length is hard-coded: currently the maximum statement length is 100.
* SQL translation occurs only if the SQL statement matches exactly the source statement rule for *each* character (it is case sensitive, space sensitive, semicolon sensitive, etc.)
* SQL translation rules are only stored in shared memory. The extension does not provide any feature to have persistent settings. However [`pg_start_sql`] (https://github.com/pierreforstmann/pg_start_sql) can be used to store some SQL statements that are run at each PostgreSQL instance start.
