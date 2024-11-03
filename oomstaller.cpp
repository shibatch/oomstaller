// Written by Naoki Shibata  https://github.com/shibatch

#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <vector>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <signal.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/limits.h>

using namespace std;

double memThres = 0.75, period = 1.0, minFreeMem = 256 * 1024;

const long pageSize = sysconf(_SC_PAGESIZE);
uid_t uid = getuid();
const pid_t pid = getpid();

unordered_map<string, uint64_t> readMeminfo() {
  FILE *fp = fopen("/proc/meminfo", "r");
  if (!fp) throw(runtime_error("readMeminfo() : could not open /proc/meminfo"));
  vector<char> line(1024), key(1024);
  unordered_map<string, uint64_t> map;

  while(!feof(fp)) {
    if (fgets(line.data(), line.size(), fp) == NULL) break;
    unsigned long long ull;
    if (sscanf(line.data(), "%1000s %llu", key.data(), &ull) != 2) continue;
    map[key.data()] = ull;
  }
  fclose(fp);

  return map;
}

uint64_t readMemInfo(const string &s) {
  auto m = readMeminfo();
  if (m.count(s) == 0) throw(runtime_error(("readMemInfo() : /proc/meminfo does not have entry for " + s).c_str()));
  return m[s];
}

struct ProcInfo {
  string comm;
  int pid, ppid, pgrp, session;
  long long unsigned starttime;
  long unsigned vsize;
  long int rss;
  char state;

  ProcInfo() {}

  ProcInfo(const char *s) {
    vector<char> t(1024);
    //                 1  2      3  4  5  6  7   8   9   10  11  12  13  14  15  16  17  18  19  20  21  22   23  24
    int n = sscanf(s, "%d %1020s %c %d %d %d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %llu %lu %ld",
		   &pid, t.data(), &state, &ppid, &pgrp, &session, &starttime, &vsize, &rss);
    if (n != 9) throw(runtime_error("/proc/<pid>/stat format error"));
    comm = t.data();
    comm = comm.substr(1, comm.size()-2);
  }
};

unordered_map<int, ProcInfo> getProcesses() {
  vector<char> line(1024);
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

    if (fstat(fileno(fp), &statbuf) == 0) {
      if (statbuf.st_uid == uid && fgets(line.data(), line.size(), fp) != NULL) {
	ProcInfo pi(line.data());
	ret[pi.pid] = pi;
      }
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
  unique_lock<mutex> lock(mtx);

  while(!exiting) {
    set<ProcInfo, bool(*)(const ProcInfo &lhs, const ProcInfo &rhs)>
    proc { [](const ProcInfo &lhs, const ProcInfo &rhs) {
      if (lhs.starttime > rhs.starttime) return true;
      if (lhs.starttime < rhs.starttime) return false;
      return lhs.pid > rhs.pid; } };

    long usedMem = 0;
    int pidLargestRSS = 0;

    {
      auto m = getProcesses();
      for(auto e : m) {
	if ((e.second.state != 'R' && e.second.state != 'T' && e.second.state != 'D') ||
	    !isTarget(m, &e.second)) continue;
	usedMem += e.second.rss;
	proc.insert(e.second);
	if (pidLargestRSS == 0 || e.second.rss > m.at(pidLargestRSS).rss) {
	  pidLargestRSS = e.second.pid;
	}
      }
    }

    long freeMem = readMemInfo("MemAvailable:"), usableMem = freeMem / pageSize * 1024 + usedMem;

    long m = usedMem;
    for(auto e : proc) {
      char nextState = '\0';
      if (e.pid == pidLargestRSS) {
	nextState = 'R';
      } else {
	if (m >= usableMem * memThres || freeMem < minFreeMem) {
	  nextState = 'T';
	  m -= e.rss;
	} else {
	  nextState = 'R';
	}
      }

      if (nextState == 'R') {
	kill(e.pid, SIGCONT);
	stoppedProcs.erase(e.pid);
      } else {
	stoppedProcs.insert(e.pid);
	kill(e.pid, SIGSTOP);
      }
    }

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

void handler(int n) {
  auto m = getProcesses();
  for(auto e : m) {
    if (e.second.ppid != pid) continue;
    kill(-e.second.pgrp, SIGCONT);
    kill(-e.second.pgrp, n);
  }

  unique_lock<mutex> lock(mtx);
  for(auto e : stoppedProcs) kill(e, SIGCONT);
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
  cerr << "     build, and suspends processes as necessary to prevent swapping from" << endl;
  cerr << "     occurring." << endl;
  cerr << endl;
  cerr << "  --thres <percentage>                     default:  75.0" << endl;
  cerr << endl;
  cerr << "     This tool suspends processes so that memory usage by running build" << endl;
  cerr << "     processes does not exceed the specified percentage of available memory." << endl;
  cerr << endl;
  cerr << "  --period <seconds>                       default:   1.0" << endl;
  cerr << endl;
  cerr << "     Specifies the interval at which memory usage of each process is checked" << endl;
  cerr << "     and processes are controlled." << endl;
  cerr << endl;
  cerr << "  --thrash <minimum available memory (MB)>  default: 256.0" << endl;
  cerr << endl;
  cerr << "     If the amount of available memory falls below the specified value, it is" << endl;
  cerr << "     assumed that swap thrashing is occurring." << endl;
  cerr << endl;
  cerr << "  --uid <uid>  default: the UID of the process" << endl;
  cerr << endl;
  cerr << "     When oomstaller is executed inside a fakeroot environment, the real UID" << endl;
  cerr << "     has to be provided to work correctly." << endl;
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
  cerr << "oomstaller 0.2.0" << endl;
  cerr << endl;

  exit(-1);
}

int main(int argc, char **argv) {
  if (argc < 2) showUsage(argv[0], "");

  int nextArg;
  for(nextArg = 1;nextArg < argc;nextArg++) {
    if (string(argv[nextArg]) == "--thres") {
      if (nextArg+1 >= argc) showUsage(argv[0]);
      char *p;
      memThres = strtod(argv[nextArg+1], &p) * 0.01;
      if (p == argv[nextArg+1]) showUsage(argv[0], "A real value is expected after --thres.");
      nextArg++;
    } else if (string(argv[nextArg]) == "--period") {
      if (nextArg+1 >= argc) showUsage(argv[0]);
      char *p;
      period = strtod(argv[nextArg+1], &p);
      if (p == argv[nextArg+1]) showUsage(argv[0], "A real value is expected after --period.");
      nextArg++;
    } else if (string(argv[nextArg]) == "--thrash") {
      if (nextArg+1 >= argc) showUsage(argv[0]);
      char *p;
      minFreeMem = strtod(argv[nextArg+1], &p) * 1024;
      if (p == argv[nextArg+1]) showUsage(argv[0], "A real value is expected after --thrash.");
      nextArg++;
    } else if (string(argv[nextArg]) == "--uid") {
      if (nextArg+1 >= argc) showUsage(argv[0]);
      char *p;
      uid = strtol(argv[nextArg+1], &p, 0);
      if (p == argv[nextArg+1]) showUsage(argv[0], "An integer is expected after --uid.");
      nextArg++;
    } else if (string(argv[nextArg]).substr(0, 2) == "--") {
      showUsage(argv[0], string("Unrecognized option : ") + argv[nextArg]);
    } else {
      break;
    }
  }

  if (nextArg >= argc) showUsage(argv[0], "");

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
    kill(pid, SIGTERM);
  }

  childTh->join();

  unique_lock<mutex> lock(mtx);
  for(auto e : stoppedProcs) kill(e, SIGCONT);

  exit(childExitCode);
}
