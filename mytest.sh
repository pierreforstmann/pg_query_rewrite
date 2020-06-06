#!/bin/sh
#
# mytest.sh
#
# 1. CREATE EXTENSION pg_query_rewrite 
# 2. insert row int pg_rewrite_rule
# 2. pg_ctl stop 
# 4. pg_ctl start
# 5. check that step 2 rule still works.
#
#---------------------------------------------------------------
export PGDATA=/tmp/5555 
export PGPORT=5555

export PG_BASE="-d postgres"
export PG_CHECK=$PGDATA/check.out

PATH=$bindir:$PATH
initdb 
echo "pg_query_rewrite.max_rules=10" >> $PGDATA/postgresql.conf
echo "shared_preload_libraries=pg_query_rewrite" >> $PGDATA/postgresql.conf
pg_ctl start
sleep 2
psql $PG_BASE -c "create extension pg_query_rewrite"
psql $PG_BASE -c "show shared_preload_libraries"
psql $PG_BASE -c "insert into pg_rewrite_rule(pattern, replacement, enabled) values('select 10;','select 11;',true);"
psql $PG_BASE -c "select pgqr_load_rules()"

pg_ctl stop
pg_ctl start
sleep 2
psql $PG_BASE -c "select 10;" > $PG_CHECK 
cat $PG_CHECK | grep "11"
if [ $? -eq 0 ]
then
 echo
 echo "mytest.sh: test SUCCEEDED."
 echo
else
 echo
 echo "mytest.sh: test FAILED."
 echo
fi

pg_ctl stop
rm -rf $PGDATA

exit 0
