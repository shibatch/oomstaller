## oomstaller - A tool for suppressing swap slashing at build time

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

This program monitors the memory usage of each process when performing
a build, and suspends processes as necessary to prevent swapping from
occurring. This allows you to build using all CPU cores without
worrying about swapping occurring.


### How to build

1. Check out the source code from our GitHub repository :
`git clone https://github.com/shibatch/oomstaller`

2. Run make :
`cd oomstaller && make`


### SYNOPSIS

`oomstaller [<options>] command [arg] ...`


### DESCRIPTION

This program monitors the memory usage of each process when performing a
build, and suspends processes as necessary to prevent swapping from
occurring.

--thres <percentage>                     default:  50.0

This tool suspends processes so that memory usage by running processes
does not exceed the specified percentage of available memory.

--period <seconds>                       default:   1.0

Specifies the interval at which memory usage of each process is checked
and processes are controlled.

--slash <minimum available memory (MB)>  default: 250.0

If the amount of available memory falls below the specified value, it is
assumed that swap slashing is occurring.

### TIPS
The default value of 50 for the --thres parameter would be a good choice
when the memory shortage is severe. If the memory shortage is not so
severe, increasing the value to nearly 100 may result in a faster build.

We recommend specifying "-j `nproc`" option to ninja. ninja usually runs
jobs with more threads than CPU cores. This is effective to reduce build
time if there is sufficient memory. However, this will only consume extra
memory in situations where there is not enough memory in which you might
want to use this tool.

You can send SIGCONT to all processes run by you with the following
command.

```
killall -v -s CONT -u $USER -r '.*'
```


### License

The software is distributed under any of the open source licenses, but
I have not yet decided which one.

Contributions to this project are accepted under the same license.

The fact that this software is released under an open source license
only means that you can use the current version of the software for
free. If you want this software to be maintained, you need to
financially support the project. Please see
[CODE_OF_CONDUCT.md](https://github.com/shibatch/nofreelunch?tab=coc-ov-file).

Copyright Naoki Shibata 2024.
