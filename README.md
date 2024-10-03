## oomstaller - A tool for suppressing swap thrashing at build time

Original distribution site : https://github.com/shibatch/oomstaller


### Introduction

Modern computers have many CPU cores, but in order to use them
effectively, a lot of memory is sometimes required. When building a
large application, the build time can become very long due to lack of
memory. Usually, most processes in the build do not require much
memory, but a very small portion of the build requires a large amount
of memory. If swapping occurs when performing those processes, the
build will take an extremely long time to complete. This problem can
be avoided by setting the number of CPU cores used for builds to a
small number, but this is a waste of valuable CPU. Also, there is no
good way to know the best number of CPU cores to use beforehand.

This tool monitors the memory usage of each process when performing a
build, and suspends processes as necessary to prevent swapping from
occurring. This allows you to build using all CPU cores without
worrying about swapping occurring.


#### How it works

Thrashing is a situation in which a CPU takes longer to perform
swapping than the process that the CPU would normally perform if there
were sufficient memory. When thrashing is occurring, it takes longer
to respond to user input because the memory for not only the process
taking up the most memory but also the memory for other processes is
swapped out. In addition, the OS may kill the process that is
occupying the largest amount of memory, which can make the system
unstable.

When thrashing occurs while multiple processes are running in
parallel, the OS may swap out the memory of one running process to
free up the physical memory needed for the other processes to continue
execution. However, even after swapping out the memory of the running
process, the swapped-out memory is soon needed again to continue
execution of that process, resulting in a state of frequent
swap-in/swap-out.

To avoid this situation, this tool suspends part of processes and
stops their execution completely. This ensures that once the memory
for a process is swapped out, it will not be swapped in again until
its execution is resumed, and processes that are still running can
continue their execution with the necessary physical memory allocated.
Because the advantage of not having to swap frequently is much greater
than the disadvantage of some CPU cores not being utilized, this tool
can reduce the total execution time compared to when thrashing is
occurring.


### How to build

1. Check out the source code from our GitHub repository :
`git clone https://github.com/shibatch/oomstaller`

2. Run make :
`cd oomstaller && make`


### Synopsis

`oomstaller [<options>] command [arg] ...`


### Description

This tool monitors the memory usage of each process when performing a
build, and suspends processes as necessary to prevent swapping from
occurring.

To perform a build using this tool, specify make or ninja as the
argument of this tool and execute as follows.

```
oomstaller make -j `nproc`
```


### Options

`--thres <percentage>`                     default:  75.0

This tool suspends processes so that memory usage by running build
processes does not exceed the specified percentage of available
memory.

`--period <seconds>`                       default:   1.0

Specifies the interval at which memory usage of each process is checked
and processes are controlled.

`--thrash <minimum available memory (MB)>`  default: 256.0

If the amount of available memory falls below the specified value, it is
assumed that swap thrashing is occurring.


### Tips

Lowering the value of --thres results in increased time where some
cores are not used in order to reduce memory usage. Increasing this
value too much would increase the time where only one process can run
due to swap thrashing.

We recommend specifying "-j `nproc`" option to ninja. ninja usually runs
jobs with more threads than CPU cores. This is effective to reduce build
time if there is sufficient memory. However, this will only consume extra
memory in situations where there is not enough memory in which you might
want to use this tool.

If you kill this tool with SIGKILL, a large number of build processes
will remain suspended with SIGSTOP. To prevent this from happening,
use SIGTERM or SIGINT to kill this tool. You can send SIGCONT to all
processes run by you with the following command.

```
killall -v -s CONT -u $USER -r '.*'
```


### License

The software is distributed under the Boost Software License, Version 1.0.
See accompanying file LICENSE.txt or copy at :
http://www.boost.org/LICENSE_1_0.txt.

Contributions to this project are accepted under the same license.

The fact that this software is released under an open source license
only means that you can use the current version of the software for
free. If you want this software to be maintained, you need to
financially support the project. Please see
[CODE_OF_CONDUCT.md](https://github.com/shibatch/nofreelunch?tab=coc-ov-file).

Copyright Naoki Shibata 2024.
