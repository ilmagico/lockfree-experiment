/usr/bin/time --format='%e %U %S %P' ./lockfree-experiment 2>&1 | {
    read rt user sys perc &&
    python -c "print('CPUTIME:{:.2f} REALTIME={:.2f} PERCENT={}'.format($user + $sys, $rt, '$perc'))";
}