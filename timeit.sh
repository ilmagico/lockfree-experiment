#!/bin/bash
MARKER='===time==='
/usr/bin/time --format="$MARKER %e %U %S %P" ./lockfree-experiment "$@" 2>&1 | \
    while read line; do
        read marker rt user sys perc <<< "$line"
        if [ "$marker" = "$MARKER" ]; then
            python -c "print('CPUTIME:{:.2f} REALTIME={:.2f} PERCENT={}'.format($user + $sys, $rt, '$perc'))";
        else
            echo "$line"
        fi
    done