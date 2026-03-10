#!/bin/sh

echo === SBUnix Test Suite ===

# C test binaries (created by parallel agents)
/bin/proctest
/bin/schedtest
/bin/sigpipetest
/bin/fstest
/bin/swaptest

# Shell-level integration tests (to be added later)

echo === SBUnix Test Suite Done ===
