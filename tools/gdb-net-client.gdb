# Command file for reproducing the net-race crash under gdb.
#
# Usage (from bin/, next to the resources):
#   gdb -q -batch -x ..\tools\gdb-net-client.gdb --args moonshine-runners.exe ^
#       -lang ru -netjoin 127.0.0.1 -netrace
#
# The host has to be up first: -nethost -netrace on another copy of the game.
#
# 0xC0000409 is a fast-fail, which Windows raises from ntdll rather than from
# the instruction that went wrong, so the backtrace this prints is the only
# useful thing the exception itself carries.

set pagination off
set confirm off

# The game owns its own input loop; do not stop on every Ctrl+C.
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
