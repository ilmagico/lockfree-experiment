# Experiment comparing lockfree vs locking queue with simulated workload
To compile, just use gcc (or your favorite compiler), e.g.:
```
g++ -W -Wall -Werror -ggdb3 -O3 -pthread --std=c++11 -o lockfree-experiment lockfree-experiment.cpp
```
There are some compile-time constants options to experiment with, look in lockfree-experiment.cpp.
