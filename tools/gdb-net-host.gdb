# Command file for running the net-race host under gdb.
#
# Mirror of gdb-net-client.gdb: the host is the other half of a two-instance
# test and is just as likely to be the one that dies. See that file for why the
# backtrace is the useful part of a 0xC0000409.

set pagination off
set confirm off
handle SIGINT nostop noprint pass

run

echo \n\n=== EXCEPTION BACKTRACE ===\n
bt 100

echo \n\n=== REGISTERS ===\n
info registers

echo \n\n=== THREADS ===\n
info threads

echo \n\n=== END ===\n
quit
