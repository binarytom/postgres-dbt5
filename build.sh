#!/bin/bash
set -e

cmake CMakeLists.txt -DDBMS=pgsql
( cd egen/prj && make -f Makefile.pgsql )
( cd storedproc/pgsql/c && make && make install )
make && make install

