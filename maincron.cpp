#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "datetime/datetime.h"
#include "par_easycurl.h"
#include "simpleini/SimpleIni.h"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#define APPVERSION "0.4"
#define APPNAME "main(v" APPVERSION ")"

#define PRINTCMD { "./my-words-memo", "-p -1", nullptr }

#define f_ssprintf(...) \
	({ int _ss_size = snprintf(0, 0, ##__VA_ARGS__);    \
    char *_ss_ret = (char*)alloca(_ss_size+1);          \
    snprintf(_ss_ret, _ss_size+1, ##__VA_ARGS__);       \
    _ss_ret; })

using namespace datetime_utils::crontab;

// Reference:
// ----------
// - https://github.com/peychart/croncpp/blob/main/main.cpp
// - format of a cron string : "S M H d m w [Y] cmd" - (Year is optional; default limit values of the year: +/- 8 years relative to the current year).

extern "C" char **environ;

int run_cmd(char *const *args) {
	pid_t pid;
	int status = posix_spawn(&pid, args[0], nullptr, nullptr, args, environ);
	if (status == 0) {
		if (waitpid(pid, &status, 0) != -1) {
			if (WIFEXITED(status)) {
				return WEXITSTATUS(status);
			} else {
				if (WIFSIGNALED(status)) {
					if (WCOREDUMP(status)) {
						fprintf(stderr, APPNAME ": the child process produced a core dump\n");
					}
					if (WTERMSIG(status)) {
						fprintf(stderr, APPNAME ": the child process was terminated\n");
					}
				}
			}
		} else {
			fprintf(stderr, APPNAME ": waitpid error\n");
		}
	} else {
		fprintf(stderr, APPNAME ": posix_spawn: %s\n", strerror(status));
	}
	return -1;
}

std::vector<std::string> read_file(const char *filename) {
	std::vector<std::string> r;
	std::ifstream file(filename);
	if (file.is_open()) {
		std::string line;
		while (std::getline(file, line)) {
			if (!line.empty() && line[0] != '#')
				r.push_back(line);
		}
		file.close();
	}
	return r;
}

int main(int argc, char **argv) {
	std::vector<std::string> crontab = {
		"* */15 9-17 * * mon,tue,thu,fri daily",
		"* */20 10-18 * * sat weekend1",
		"* */30 12-18 * * sun weekend2",
	};

	char *exec[] = PRINTCMD;

	for (;;) {
		time_t Now(time(NULL));
		cron c;

		unsigned pause = 0;

		for (int i(0); i < crontab.size(); i++) {
			c.clear() = crontab[i];
			time_t rawtime = c.next_date(Now);
			int schedule = rawtime - Now;

			char buffer[80];
			strftime(buffer, 80, "%Y/%m/%d %H:%M:%S", localtime(&rawtime));
			std::cout << "The job \"" << c.expression() << "\" lanched at: " << rawtime << " (" << buffer << "), in " << schedule << " sec. - \"" << crontab[i] << "\"" << std::endl;

			if (!pause || schedule < pause) {
				pause = schedule;
			}
		}

		if (pause > 0) {
			std::cout << "Waiting for " << pause << " sec." << std::endl;
			sleep(pause);
		}
		std::cout << "Executing." << std::endl;
		run_cmd(exec);
	}
}

// vim: expandtab tabstop=5 softtabstop=5 shiftwidth=5
