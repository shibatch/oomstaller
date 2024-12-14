// Written by Naoki Shibata  https://shibatch.github.io

#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <chrono>

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <cmath>
#include <ctime>

#include <signal.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/limits.h>

using namespace std;

const long pageSize = sysconf(_SC_PAGESIZE);
uid_t uid = getuid();
const pid_t pid = getpid();

double period = 1.0;
int maxParallel = 0, maxParallelThrash = 1;

bool showStat = false;
unordered_map<int, long> statInfo;

uint64_t readMemInfo(const string &s) {
  FILE *fp = fopen("/proc/meminfo", "r");
  if (!fp) throw(runtime_error("readMeminfo() : could not open /proc/meminfo"));
  vector<char> line(1024);

  while(!feof(fp)) {
    if (fgets(line.data(), line.size(), fp) == NULL) break;
    if (strncmp(line.data(), s.c_str(), s.size()) == 0) {
      unsigned long long ull;
      if (sscanf(line.data() + s.size(), " %llu", &ull) != 1)
	throw(runtime_error("readMeminfo() : /proc/meminfo format error"));
      fclose(fp);
      return ull;
    }
  }
  throw(runtime_error(("readMemInfo() : /proc/meminfo does not have entry for " + s).c_str()));
}

struct ProcInfo {
  string comm, d_name;
  int pid, ppid, pgrp, session;
  long long unsigned starttime;
  long unsigned vsize, vmswap = 0;
  long int rss, num_threads;
  char state;

  ProcInfo() {}

  ProcInfo(FILE *fpstat, const char *dn) {
    d_name = dn;
    vector<char> line(1024);
    if (fgets(line.data(), line.size(), fpstat) == NULL)
      throw(runtime_error("Could not read /proc/<pid>/stat"));

    string ss = line.data();
    size_t ps = ss.find_first_of('('), pe = ss.find_last_of(')');
    if (ps == string::npos || pe == string::npos)
      throw(runtime_error("Could not read process name in /proc/<pid>/stat"));

    int n = sscanf(line.data(), "%d", &pid);
    if (n != 1) throw(runtime_error("Could not read pid in /proc/<pid>/stat"));

    //                                 3  4  5  6  7   8   9   10  11  12  13  14  15  16  17  18  19  20  21  22   23  24
    n = sscanf(line.data() + pe + 1, " %c %d %d %d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %ld %*d %llu %lu %ld",
	       &state, &ppid, &pgrp, &session, &num_threads, &starttime, &vsize, &rss);
    if (n != 8) throw(runtime_error("/proc/<pid>/stat format error"));

    comm = ss.substr(ps + 1, pe - ps - 1);

    //

    {
      FILE *fp = fopen((string("/proc/") + d_name + "/status").c_str(), "r");

      if (fp) {
	while(!feof(fp)) {
	  if (fgets(line.data(), line.size(), fp) == NULL) break;
	  if (strncmp(line.data(), "VmSwap:", 7) == 0) {
	    if (sscanf(line.data(), "%*s %lu", &vmswap) == 1) vmswap = vmswap * 1024 / pageSize;
	    break;
	  }
	}
	fclose(fp);
      }
    }

    //

    if (num_threads > 1) {
      DIR *taskdir = opendir((string("/proc/") + dn + "/task").c_str());
      if (!taskdir) return;

      struct dirent *entry;
      while((entry = readdir(taskdir))) {
	char *p;
	strtoul(entry->d_name, &p, 10);
	if (*p != '\0') continue;

	FILE *fp = fopen((string("/proc/") + dn + "/task/" + entry->d_name + "/stat").c_str(), "r");
	if (!fp) continue;

	if (fgets(line.data(), line.size(), fp) != NULL) {
	  pe = string(line.data()).find_last_of(')');
	  char c;
	  if (pe != string::npos && sscanf(line.data() + pe + 1, " %c", &c) == 1 &&
	      (c == 'R' || c == 'T' || c == 'D')) state = c;
	}

	fclose(fp);
      }

      closedir(taskdir);
    }
  }
};

unordered_map<int, ProcInfo> getProcesses() {
  unordered_map<int, ProcInfo> ret;

  DIR *procdir = opendir("/proc");
  if (!procdir) throw(runtime_error("opendir(\"/proc\") failed"));

  struct dirent *entry;
  struct stat statbuf;
  while((entry = readdir(procdir))) {
    char *p;
    strtoul(entry->d_name, &p, 10);
    if (*p != '\0') continue;

    FILE *fp = fopen((string("/proc/") + entry->d_name + "/stat").c_str(), "r");
    if (!fp) continue;

    if (fstat(fileno(fp), &statbuf) == 0 && statbuf.st_uid == uid) {
      ProcInfo pi(fp, entry->d_name);
      ret[pi.pid] = pi;
    }

    fclose(fp);
  }
  closedir(procdir);

  return ret;
}

bool isTarget(unordered_map<int, ProcInfo> &map, const ProcInfo *pc) {
  if (pc->pid == pid) return false;

  for(;;) {
    if (pc->pid == pid) return true;
    if (map.count(pc->ppid) == 0) return false;
    pc = &map[pc->ppid];
  }

  return false;
}

unordered_set<int> stoppedProcs;
volatile bool exiting = false;
mutex mtx;
condition_variable condvar;

void loop(shared_ptr<thread> childTh) {
  // Clear the thrashing detection if swap space is used but total
  // swap has not increased for 10 seconds
  const int THRASHCOUNT1 = 10.0 / period;

  // Clear the thrashing detection 3 seconds after total size of swap
  // space is reduced
  const int THRASHCOUNT2 = 3.0 / period;

  // The estimated amount of memory that each process could
  // potentially occupy is attenuated by this amount each time one
  // process finishes
  const double MEMMAXDECAY = pow(0.5, 1.0 / 10.0);

  //

  unique_lock<mutex> lock(mtx);
  unordered_set<int> lastActivePids;
  double memMax = 0;

  const long totalMem = readMemInfo("MemTotal:") * 1024 / pageSize;
  long lastSwapFree = readMemInfo("SwapFree:");
  int thrashTimer = 0;

  while(!exiting) {
    set<ProcInfo, bool(*)(const ProcInfo &lhs, const ProcInfo &rhs)>
    proc { [](const ProcInfo &lhs, const ProcInfo &rhs) {
      if (lhs.starttime > rhs.starttime) return true;
      if (lhs.starttime < rhs.starttime) return false;
      return lhs.pid > rhs.pid; } };

    long usedMem = 0, memMax2 = 0;
    int pidLargest = 0;
    unordered_set<int> activePids;
    bool swapUsed = false;

    {
      auto m = getProcesses();

      if (m.count(pid) == 0) throw(runtime_error("Could not retrieve process information of oomstaller"));

      for(auto e : m) {
	if ((e.second.state != 'R' && e.second.state != 'T' && e.second.state != 'D') ||
	    !isTarget(m, &e.second)) continue;
	if (e.second.rss + e.second.vmswap > memMax2) memMax2 = e.second.rss + e.second.vmswap;
	usedMem += e.second.rss;
	if (e.second.vmswap > 0) swapUsed = true;
	proc.insert(e.second);
	activePids.insert(e.second.pid);
	if (pidLargest == 0 || (e.second.rss + e.second.vmswap > m.at(pidLargest).rss + m.at(pidLargest).vmswap))
	    pidLargest = e.second.pid;
      }
    }

    long freeMem = readMemInfo("MemAvailable:") * 1024 / pageSize, usableMem = freeMem + usedMem;
    if (usableMem > totalMem) usableMem = totalMem;

    // Detection of swap thrashing

    long swapFree = readMemInfo("SwapFree:");
    if (swapFree < lastSwapFree) {
      thrashTimer = THRASHCOUNT1;
    } else if (swapFree > lastSwapFree) {
      if (thrashTimer > THRASHCOUNT2) thrashTimer = THRASHCOUNT2;
    } else if (!swapUsed) {
      thrashTimer = 0;
    } else {
      if (thrashTimer > 0) thrashTimer--;
    }
    lastSwapFree = swapFree;

    // Calculate the number of parallel executions

    for(auto a : lastActivePids) if (activePids.count(a) == 0) memMax *= MEMMAXDECAY;
    lastActivePids = activePids;

    // memMax2 is the maximum occupied memory by the currently running
    // processes
    // memMax is the estimated amount of memory that each process
    // could potentially occupy
    if (memMax2 > memMax) memMax = memMax2;

    int maxParallelOP = INT_MAX;
    if (memMax != 0) maxParallelOP = usableMem / memMax + 1;

    // Determine the next state of each process

    unordered_set<int> removedPids;
    long n = proc.size();
    int nRunningProcs = 0;
    for(auto e : proc) {
      char nextState = '\0';
      if (e.pid == pidLargest) {
	nextState = 'R';
      } else {
	if (n > maxParallelOP || (maxParallel > 0 && n > maxParallel) ||
	    ((thrashTimer > 0 || freeMem == 0) && maxParallelThrash > 0 && n > maxParallelThrash)) {
	  nextState = 'T';
	  n--;
	} else {
	  nextState = 'R';
	}
      }

      if (nextState == 'R') {
	kill(e.pid, SIGCONT);
	stoppedProcs.erase(e.pid);
	nRunningProcs++;
      } else {
	stoppedProcs.insert(e.pid);
	kill(e.pid, SIGSTOP);
      }
    }

    // Handling of processes that have terminated or slept on its own after sending SIGSTOP

    for(auto i : stoppedProcs) if (activePids.count(i) == 0) removedPids.insert(i);

    for(auto i : removedPids) {
      kill(i, SIGCONT);
      stoppedProcs.erase(i);
    }

    // Update statistics info

    if (thrashTimer > 0 || freeMem == 0) statInfo[-1]++;
    statInfo[nRunningProcs]++;

    //

    condvar.wait_for(lock, chrono::milliseconds((long)(period * 1000)));
  }
}

int childExitCode = -1;

void execChild(string cmd) {
  childExitCode = WEXITSTATUS(system(cmd.c_str()));
  exiting = true;

  unique_lock<mutex> lock(mtx);
  condvar.notify_all();
}

void handlerThread(int n) {
  try {
    auto m = getProcesses();
    for(auto e : m) {
      if (e.second.ppid != pid) continue;
      kill(-e.second.pgrp, SIGCONT);
      kill(-e.second.pgrp, n);
    }
  } catch(exception &ex) {
    cerr << ex.what() << endl;
  }

  unique_lock<mutex> lock(mtx);
  for(auto e : stoppedProcs) kill(e, SIGCONT);
}

shared_ptr<thread> handlerTh;

void handler(int n) {
  signal(SIGINT , SIG_IGN);
  signal(SIGTERM, SIG_IGN);
  signal(SIGQUIT, SIG_IGN);
  signal(SIGHUP , SIG_IGN);

  handlerTh = make_shared<thread>(handlerThread, n);
}

void showUsage(const string& argv0, const string& mes = "") {
  if (mes != "") cerr << mes << endl << endl;
  cerr << endl;
  cerr << "NAME" << endl;
  cerr << "     oomstaller - suppress swap thrashing at build time" << endl;
  cerr << endl;
  cerr << "SYNOPSIS" << endl;
  cerr << "     " << argv0 << " [<options>] command [arg] ..." << endl;
  cerr << endl;
  cerr << "DESCRIPTION" << endl;
  cerr << "     This tool monitors the memory usage of each process when performing a" << endl;
  cerr << "     build, and suspends processes as necessary to prevent swap thrashing" << endl;
  cerr << "     from occurring." << endl;
  cerr << endl;
  cerr << "  --max-parallel <number of processes>            default:   0" << endl;
  cerr << endl;
  cerr << "     Suspends processes so that the number of running build processes" << endl;
  cerr << "     does not exceed the specified number. 0 means no limit. A process is" << endl;
  cerr << "     counted as one process even if it has multiple threads." << endl;
  cerr << endl;
  cerr << "  --max-parallel-thrash <number of processes>     default:   1" << endl;
  cerr << endl;
  cerr << "     Specifies the maximum number of processes to run when thrashing is" << endl;
  cerr << "     detected. 0 means no limit." << endl;
  cerr << endl;
  cerr << "  --period <seconds>                              default:   1.0" << endl;
  cerr << endl;
  cerr << "     Specifies the interval at which memory usage of each process is checked" << endl;
  cerr << "     and processes are controlled." << endl;
  cerr << endl;
  cerr << "  --show-stat" << endl;
  cerr << endl;
  cerr << "     Displays statistics when finished." << endl;
  cerr << endl;
  cerr << "TIPS" << endl;
  cerr << "     If you kill this tool with SIGKILL, a large number of build processes" << endl;
  cerr << "     will remain suspended with SIGSTOP. To prevent this from happening," << endl;
  cerr << "     use SIGTERM or SIGINT to kill this tool. You can send SIGCONT to all" << endl;
  cerr << "     processes run by you with the following command." << endl;
  cerr << endl;
  cerr << "     killall -v -s CONT -u $USER -r '.*'" << endl;
  cerr << endl;
  cerr << "AUTHOR" << endl;
  cerr << "     Written by Naoki Shibata." << endl;
  cerr << endl;
  cerr << "     See https://github.com/shibatch/oomstaller" << endl;
  cerr << endl;
  cerr << "oomstaller 0.4.0 rc2" << endl;
  cerr << endl;

  exit(-1);
}

int main(int argc, char **argv) {
  if (argc < 2) showUsage(argv[0], "");

  int nextArg;
  for(nextArg = 1;nextArg < argc;nextArg++) {
    if (string(argv[nextArg]) == "--max-parallel") {
      if (nextArg+1 >= argc) showUsage(argv[0]);
      char *p;
      maxParallel = strtol(argv[nextArg+1], &p, 0);
      if (p == argv[nextArg+1] || *p || maxParallel < 0)
	showUsage(argv[0], "A non-negative integer is expected after --max-parallel.");
      nextArg++;
    } else if (string(argv[nextArg]) == "--max-parallel-thrash") {
      if (nextArg+1 >= argc) showUsage(argv[0]);
      char *p;
      maxParallelThrash = strtol(argv[nextArg+1], &p, 0);
      if (p == argv[nextArg+1] || *p || maxParallelThrash < 0)
	showUsage(argv[0], "A non-negative integer is expected after --max-parallel-thrash.");
      nextArg++;
    } else if (string(argv[nextArg]) == "--period") {
      if (nextArg+1 >= argc) showUsage(argv[0]);
      char *p;
      period = strtod(argv[nextArg+1], &p);
      if (p == argv[nextArg+1] || *p || period <= 0)
	showUsage(argv[0], "A positive value is expected after --period.");
      nextArg++;
    } else if (string(argv[nextArg]) == "--uid") {
      if (nextArg+1 >= argc) showUsage(argv[0]);
      char *p;
      long l = strtol(argv[nextArg+1], &p, 0);
      uid = l;
      if (p == argv[nextArg+1] || *p || l < 0)
	showUsage(argv[0], "A non-negative integer is expected after --uid.");
      nextArg++;
    } else if (string(argv[nextArg]) == "--show-stat") {
      showStat = true;
    } else if (string(argv[nextArg]).substr(0, 2) == "--") {
      showUsage(argv[0], string("Unrecognized option : ") + argv[nextArg]);
    } else {
      break;
    }
  }

  if (nextArg >= argc) showUsage(argv[0], "");

  if (getProcesses().count(pid) == 0) {
    cerr << argv[0] << " : Could not retrieve process information of oomstaller" << endl;
    exit(-1);
  }

  auto startTime = chrono::system_clock::now();

  signal(SIGINT , handler);
  signal(SIGTERM, handler);
  signal(SIGQUIT, handler);
  signal(SIGHUP , handler);

  string cmd = argv[nextArg];
  for(int i=nextArg+1;i<argc;i++) {
    string in = argv[i], out = "'";
    for(int i=0;i<(int)in.length();i++) {
      if (in[i] == '\'') {
	out += "'\\''";
      } else {
	out += in[i];
      }
    }
    cmd = cmd + " " + out + "'";
  }

  auto childTh = make_shared<thread>(execChild, cmd);

  try {
    loop(childTh);
  } catch(exception &ex) {
    cerr << argv[0] << " : " << ex.what() << endl;
    kill(0, SIGTERM);
  }

  childTh->join();
  if (handlerTh) handlerTh->join();

  unique_lock<mutex> lock(mtx);
  for(auto e : stoppedProcs) kill(e, SIGCONT);

  auto endTime = chrono::system_clock::now();

  if (showStat) {
    if (statInfo[0] > 0) statInfo[0]--;
    if (statInfo[0] == 0) statInfo.erase(0);

    time_t tStartTime = chrono::system_clock::to_time_t(startTime);
    time_t tEndTime = chrono::system_clock::to_time_t(endTime);

    cout << endl;
    cout << "Start   : " << ctime(&tStartTime);
    cout << "End     : " << ctime(&tEndTime);

    chrono::duration<double> elapsed = endTime - startTime;
    cout << "Elapsed : " << elapsed.count() << " seconds" << endl;

    set<int> statKeys;
    for(auto a : statInfo) statKeys.insert(a.first);
    long pt = 0;
    for(auto a : statKeys) {
      if (a == -1) continue;
      pt += statInfo[a];
      cout << a << " processes running : " << statInfo[a] << " periods" << endl;
    }
    cout << "Thrashing detected : " << statInfo[-1] << " periods" << endl;
    cout << "Total : " << pt << " periods" << endl;
  }

  return childExitCode;
}
