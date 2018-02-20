# Experiment comparing lockfree vs locking queue with simulated workload
## Requirements
C++11 compliant compiler, Boost 1.60 or newer
## Compiling
Just use gcc (or your favorite compiler), e.g.:
```
g++ -W -Wall -Werror -ggdb3 -O3 -pthread --std=c++11 -o lockfree-experiment lockfree-experiment.cpp
```
There are some compile-time constants options to experiment with, look in lockfree-experiment.cpp.
## Bench
Make sure to disable tracing (comment `#define DBG_TRACE`), then you can run the executable with `time`, or use the `timeit.sh` script which does that for you and print some useful metrics.
