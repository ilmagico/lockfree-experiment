# Experiment comparing lockfree vs locking queue with simulated workload
## Requirements
C++11 compliant compiler, Boost 1.60 or newer
## Compiling
To compile, just use gcc (or your favorite compiler). You need to link with `boost_program_options` and whatever options to enable threads (`-pthread` for gcc), the rest is header-only and doesn't require link flags:
```
g++ -W -Wall -Werror -ggdb3 -O3 -pthread --std=c++11 -lboost_program_options -o lockfree-experiment lockfree-experiment.cpp
```
There are some command-line options to experiment with (use `-h` to see them), and one compile-time option in lockfree-experiment.cpp (`#define USE_LOCKFREE`) which decides which queue implementation to use.
## Bench
Make sure to disable tracing (comment `#define DBG_TRACE`), then you can run the executable with `time`, or use the `timeit.sh` script which does that for you and print some useful metrics, e.g.:
```
./timeit.sh -n 50 -i 1 -s 100000
```
