/*
 * Copyright (C) 2014-2018 Firejail Authors
 *
 * This file is part of firejail project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include "firejail.h"
#include "../include/pid.h"
#include "../include/firejail_user.h"
#define _GNU_SOURCE
#include <sys/utsname.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <pwd.h>
#include <errno.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <signal.h>
#include <time.h>
#include <net/if.h>
#include <sys/utsname.h>

uid_t firejail_uid = 0;
gid_t firejail_gid = 0;

#define STACK_SIZE (1024 * 1024)
static char child_stack[STACK_SIZE];		// space for child's stack
Config cfg;					// configuration
int arg_private = 0;				// mount private /home and /tmp directoryu
int arg_private_template = 0;		// mount private /home using a template
int arg_private_cache = 0;		// mount private home/.cache
int arg_debug = 0;				// print debug messages
int arg_nonetwork = 0;				// --net=none
int arg_command = 0;				// -c

int arg_seccomp = 0;				// enable default seccomp filter
int arg_seccomp_postexec = 0;			// need postexec ld.preload library?
int arg_seccomp_block_secondary = 0;		// block any secondary architectures

int arg_caps_default_filter = 0;			// enable default capabilities filter
int arg_caps_drop = 0;				// drop list
int arg_caps_drop_all = 0;			// drop all capabilities
int arg_caps_keep = 0;			// keep list
char *arg_caps_list = NULL;			// optional caps list

int arg_nogroups = 0;				// disable supplementary groups
int arg_nonewprivs = 0;			// set the NO_NEW_PRIVS prctl
int arg_noroot = 0;				// create a new user namespace and disable root user
int arg_netfilter;				// enable netfilter
int arg_netfilter6;				// enable netfilter6
char *arg_netfilter_file = NULL;			// netfilter file
char *arg_netfilter6_file = NULL;		// netfilter6 file
char *arg_netns = NULL;			// "ip netns"-created network namespace to use
int arg_doubledash = 0;			// double dash
int arg_shell_none = 0;			// run the program directly without a shell
int arg_private_dev = 0;			// private dev directory

int arg_private_tmp = 0;			// private tmp directory
int arg_scan = 0;				// arp-scan all interfaces
int arg_whitelist = 0;				// whitelist commad
int arg_nosound = 0;				// disable sound
int arg_noautopulse = 0;			// disable automatic ~/.config/pulse init
int arg_novideo = 0;			//disable video devices in /dev
int arg_no3d;					// disable 3d hardware acceleration
int arg_quiet = 0;				// no output for scripting
int arg_join_network = 0;			// join only the network namespace
int arg_join_filesystem = 0;			// join only the mount namespace
int arg_ipc = 0;					// enable ipc namespace
int arg_writable_etc = 0;			// writable etc
int arg_writable_var = 0;			// writable var
int arg_keep_var_tmp = 0;                       // don't overwrite /var/tmp
int arg_writable_run_user = 0;			// writable /run/user
int arg_writable_var_log = 0;		// writable /var/log
int arg_appimage = 0;				// appimage
int arg_apparmor = 0;				// apparmor
int arg_allusers = 0;				// all user home directories visible
int arg_disable_mnt = 0;			// disable /mnt and /media
int arg_noprofile = 0; // use default.profile if none other found/specified
int arg_memory_deny_write_execute = 0;		// block writable and executable memory
int arg_notv = 0;	// --notv
int arg_nodvd = 0; // --nodvd
int arg_nou2f = 0; // --nou2f
int login_shell = 0;


int parent_to_child_fds[2];
int child_to_parent_fds[2];

char *fullargv[MAX_ARGS];			// expanded argv for restricted shell
int fullargc = 0;
static pid_t child = 0;
pid_t sandbox_pid;
unsigned long long start_timestamp;

static void clear_atexit(void) {
	EUID_ROOT();
	delete_run_files(getpid());
}

static void myexit(int rv) {
	logmsg("exiting...");
	if (!arg_command)
		fmessage("\nParent is shutting down, bye...\n");


	// delete sandbox files in shared memory
	EUID_ROOT();
	delete_run_files(sandbox_pid);
	appimage_clear();
	flush_stdin();
	exit(rv);
}

static void my_handler(int s){
	EUID_ROOT();
	fmessage("\nParent received signal %d, shutting down the child process...\n", s);
	logsignal(s);
	kill(child, SIGTERM);
	myexit(1);
}

// return 1 if error, 0 if a valid pid was found
static int extract_pid(const char *name, pid_t *pid) {
	int retval = 0;
	EUID_ASSERT();
	if (!name || strlen(name) == 0) {
		fprintf(stderr, "Error: invalid sandbox name\n");
		exit(1);
	}

	EUID_ROOT();
	if (name2pid(name, pid)) {
		retval = 1;
	}
	EUID_USER();
	return retval;
}

// return 1 if error, 0 if a valid pid was found
static int read_pid(const char *name, pid_t *pid) {
	char *endptr;
	errno = 0;
	long int pidtmp = strtol(name, &endptr, 10);
	if ((errno == ERANGE && (pidtmp == LONG_MAX || pidtmp == LONG_MIN))
		|| (errno != 0 && pidtmp == 0)) {
		return extract_pid(name,pid);
	}
	// endptr points to '\0' char in name if the entire string is valid
	if (endptr == NULL || endptr[0]!='\0') {
		return extract_pid(name,pid);
	}
	*pid =(pid_t)pidtmp;
	return 0;
}

static pid_t require_pid(const char *name) {
	pid_t pid;
	if (read_pid(name,&pid)) {
		fprintf(stderr, "Error: cannot find sandbox %s\n", name);
		exit(1);
	}
	return pid;
}

// init configuration
static void init_cfg(int argc, char **argv) {
	EUID_ASSERT();
	memset(&cfg, 0, sizeof(cfg));

	cfg.original_argv = argv;
	cfg.original_argc = argc;
	cfg.bridge0.devsandbox = "eth0";
	cfg.bridge1.devsandbox = "eth1";
	cfg.bridge2.devsandbox = "eth2";
	cfg.bridge3.devsandbox = "eth3";

	// extract user data
	EUID_ROOT(); // rise permissions for grsecurity
	struct passwd *pw = getpwuid(getuid());
	if (!pw)
		errExit("getpwuid");
	EUID_USER();
	cfg.username = strdup(pw->pw_name);
	if (!cfg.username)
		errExit("strdup");

	// build home directory name
	cfg.homedir = NULL;
	if (pw->pw_dir != NULL) {
		cfg.homedir = strdup(pw->pw_dir);
		if (!cfg.homedir)
			errExit("strdup");
	}
	else {
		fprintf(stderr, "Error: user %s doesn't have a user directory assigned\n", cfg.username);
		exit(1);
	}
	cfg.cwd = getcwd(NULL, 0);

	// check user database
	if (!firejail_user_check(cfg.username)) {
		fprintf(stderr, "Error: the user is not allowed to use Firejail. "
			"Please add the user in %s/firejail.users file, "
			"either by running \"sudo firecfg\", or by editing the file directly.\n"
			"See \"man firejail-users\" for more details.\n", SYSCONFDIR);

		// attempt to run the program as is
		run_symlink(argc, argv, 1);
		exit(1);
	}

	// initialize random number generator
	sandbox_pid = getpid();
	time_t t = time(NULL);
	srand(t ^ sandbox_pid);
}

static void check_network(Bridge *br) {
	assert(br);
	if (br->macvlan == 0) // for bridge devices check network range or arp-scan and assign address
		net_configure_sandbox_ip(br);
	else if (br->ipsandbox) { // for macvlan check network range
		char *rv = in_netrange(br->ipsandbox, br->ip, br->mask);
		if (rv) {
			fprintf(stderr, "%s", rv);
			exit(1);
		}
	}
}

#ifdef HAVE_USERNS
void check_user_namespace(void) {
	EUID_ASSERT();
	if (getuid() == 0)
		goto errout;

	// test user namespaces available in the kernel
	struct stat s1;
	struct stat s2;
	struct stat s3;
	if (stat("/proc/self/ns/user", &s1) == 0 &&
	    stat("/proc/self/uid_map", &s2) == 0 &&
	    stat("/proc/self/gid_map", &s3) == 0)
		arg_noroot = 1;
	else
		goto errout;

	return;

errout:
	fwarning("noroot option is not available\n");
	arg_noroot = 0;

}
#endif


static void exit_err_feature(const char *feature) {
	fprintf(stderr, "Error: %s feature is disabled in Firejail configuration file\n", feature);
	exit(1);
}

// run independent commands and exit program
// this function handles command line options such as --version and --help
static void run_cmd_and_exit(int i, int argc, char **argv) {
	EUID_ASSERT();

	//*************************************
	// basic arguments
	//*************************************
	if (strcmp(argv[i], "--help") == 0 ||
	    strcmp(argv[i], "-?") == 0) {
		usage();
		exit(0);
	}
	else if (strcmp(argv[i], "--version") == 0) {
		printf("firejail version %s\n", VERSION);
		printf("\n");
		exit(0);
	}
#ifdef HAVE_NETWORK
	else if (strncmp(argv[i], "--bandwidth=", 12) == 0) {
		if (checkcfg(CFG_NETWORK)) {
			logargs(argc, argv);

			// extract the command
			if ((i + 1) == argc) {
				fprintf(stderr, "Error: command expected after --bandwidth option\n");
				exit(1);
			}
			char *cmd = argv[i + 1];
			if (strcmp(cmd, "status") && strcmp(cmd, "clear") && strcmp(cmd, "set")) {
				fprintf(stderr, "Error: invalid --bandwidth command.\nValid commands: set, clear, status.\n");
				exit(1);
			}

			// extract network name
			char *dev = NULL;
			int down = 0;
			int up = 0;
			if (strcmp(cmd, "set") == 0 || strcmp(cmd, "clear") == 0) {
				// extract device name
				if ((i + 2) == argc) {
					fprintf(stderr, "Error: network name expected after --bandwidth %s option\n", cmd);
					exit(1);
				}
				dev = argv[i + 2];

				// check device name
				if (if_nametoindex(dev) == 0) {
					fprintf(stderr, "Error: network device %s not found\n", dev);
					exit(1);
				}

				// extract bandwidth
				if (strcmp(cmd, "set") == 0) {
					if ((i + 4) >= argc) {
						fprintf(stderr, "Error: invalid --bandwidth set command\n");
						exit(1);
					}

					down = atoi(argv[i + 3]);
					if (down < 0) {
						fprintf(stderr, "Error: invalid download speed\n");
						exit(1);
					}
					up = atoi(argv[i + 4]);
					if (up < 0) {
						fprintf(stderr, "Error: invalid upload speed\n");
						exit(1);
					}
				}
			}

			// extract pid or sandbox name
			pid_t pid = require_pid(argv[i] + 12);
			bandwidth_pid(pid, cmd, dev, down, up);
		}
		else
			exit_err_feature("networking");
		exit(0);
	}
	else if (strncmp(argv[i], "--netfilter.print=", 18) == 0) {
		// extract pid or sandbox name
		pid_t pid = require_pid(argv[i] + 18);
		netfilter_print(pid, 0);
		exit(0);
	}
	else if (strncmp(argv[i], "--netfilter6.print=", 19) == 0) {
		// extract pid or sandbox name
		pid_t pid = require_pid(argv[i] + 19);
		netfilter_print(pid, 1);
		exit(0);
	}
#endif
	//*************************************
	// independent commands - the program will exit!
	//*************************************
#ifdef HAVE_SECCOMP
	else if (strcmp(argv[i], "--debug-syscalls") == 0) {
		if (checkcfg(CFG_SECCOMP)) {
			int rv = sbox_run(SBOX_USER | SBOX_CAPS_NONE | SBOX_SECCOMP, 2, PATH_FSECCOMP, "debug-syscalls");
			exit(rv);
		}
		else
			exit_err_feature("seccomp");
	}
	else if (strcmp(argv[i], "--debug-errnos") == 0) {
		if (checkcfg(CFG_SECCOMP)) {
			int rv = sbox_run(SBOX_USER | SBOX_CAPS_NONE | SBOX_SECCOMP, 2, PATH_FSECCOMP, "debug-errnos");
			exit(rv);
		}
		else
			exit_err_feature("seccomp");
		exit(0);
	}
	else if (strncmp(argv[i], "--seccomp.print=", 16) == 0) {
		if (checkcfg(CFG_SECCOMP)) {
			// print seccomp filter for a sandbox specified by pid or by name
			pid_t pid = require_pid(argv[i] + 16);
			seccomp_print_filter(pid);
		}
		else
			exit_err_feature("seccomp");
		exit(0);
	}
	else if (strcmp(argv[i], "--debug-protocols") == 0) {
		int rv = sbox_run(SBOX_USER | SBOX_CAPS_NONE | SBOX_SECCOMP, 2, PATH_FSECCOMP, "debug-protocols");
		exit(rv);
	}
	else if (strncmp(argv[i], "--protocol.print=", 17) == 0) {
		if (checkcfg(CFG_SECCOMP)) {
			// print seccomp filter for a sandbox specified by pid or by name
			pid_t pid = require_pid(argv[i] + 17);
			protocol_print_filter(pid);
		}
		else
			exit_err_feature("seccomp");
		exit(0);
	}
#endif
	else if (strncmp(argv[i], "--profile.print=", 16) == 0) {
		pid_t pid = require_pid(argv[i] + 16);

		// print /run/firejail/profile/<PID> file
		char *fname;
		if (asprintf(&fname, RUN_FIREJAIL_PROFILE_DIR "/%d", pid) == -1)
			errExit("asprintf");
		FILE *fp = fopen(fname, "r");
		if (!fp) {
			fprintf(stderr, "Error: sandbox %s not found\n", argv[i] + 16);
			exit(1);
		}
#define MAXBUF 4096
		char buf[MAXBUF];
		if (fgets(buf, MAXBUF, fp))
			printf("%s", buf);
		fclose(fp);
		exit(0);

	}
	else if (strncmp(argv[i], "--apparmor.print=", 12) == 0) {
		// join sandbox by pid or by name
		pid_t pid = require_pid(argv[i] + 17);
		char *pidstr;
		if (asprintf(&pidstr, "%u", pid) == -1)
			errExit("asprintf");
		sbox_run(SBOX_USER| SBOX_CAPS_NONE | SBOX_SECCOMP, 3, PATH_FIREMON, "--apparmor", pidstr);
		free(pidstr);
		exit(0);
	}
	else if (strncmp(argv[i], "--caps.print=", 13) == 0) {
		// join sandbox by pid or by name
		pid_t pid = require_pid(argv[i] + 13);
		caps_print_filter(pid);
		exit(0);
	}
	else if (strncmp(argv[i], "--fs.print=", 11) == 0) {
		// join sandbox by pid or by name
		pid_t pid = require_pid(argv[i] + 11);
		fs_logger_print_log(pid);
		exit(0);
	}
	else if (strncmp(argv[i], "--dns.print=", 12) == 0) {
		// join sandbox by pid or by name
		pid_t pid = require_pid(argv[i] + 12);
		net_dns_print(pid);
		exit(0);
	}
	else if (strcmp(argv[i], "--debug-caps") == 0) {
		caps_print();
		exit(0);
	}
	else if (strcmp(argv[i], "--list") == 0) {
		if (pid_hidepid())
			sbox_run(SBOX_ROOT| SBOX_CAPS_HIDEPID | SBOX_SECCOMP, 2, PATH_FIREMON, "--list");
		else
			sbox_run(SBOX_USER| SBOX_CAPS_NONE | SBOX_SECCOMP, 2, PATH_FIREMON, "--list");
		exit(0);
	}
	else if (strcmp(argv[i], "--tree") == 0) {
		if (pid_hidepid())
			sbox_run(SBOX_ROOT | SBOX_CAPS_HIDEPID | SBOX_SECCOMP, 2, PATH_FIREMON, "--tree");
		else
			sbox_run(SBOX_USER | SBOX_CAPS_NONE | SBOX_SECCOMP, 2, PATH_FIREMON, "--tree");
		exit(0);
	}
	else if (strcmp(argv[i], "--top") == 0) {
		if (pid_hidepid())
			sbox_run(SBOX_ROOT | SBOX_CAPS_HIDEPID | SBOX_SECCOMP | SBOX_ALLOW_STDIN,
				2, PATH_FIREMON, "--top");
		else
			sbox_run(SBOX_USER | SBOX_CAPS_NONE | SBOX_SECCOMP | SBOX_ALLOW_STDIN,
				2, PATH_FIREMON, "--top");
		exit(0);
	}
#ifdef HAVE_NETWORK
	else if (strcmp(argv[i], "--netstats") == 0) {
		if (checkcfg(CFG_NETWORK)) {
			struct stat s;
			if (stat("/proc/sys/kernel/grsecurity", &s) == 0 || pid_hidepid())
				sbox_run(SBOX_ROOT | SBOX_CAPS_HIDEPID | SBOX_SECCOMP | SBOX_ALLOW_STDIN,
					2, PATH_FIREMON, "--netstats");
			else
				sbox_run(SBOX_USER | SBOX_CAPS_NONE | SBOX_SECCOMP | SBOX_ALLOW_STDIN,
					2, PATH_FIREMON, "--netstats");
			exit(0);
		}
		else
			exit_err_feature("networking");
	}
#endif
	else if (strncmp(argv[i], "--join=", 7) == 0) {
		if (checkcfg(CFG_JOIN) || getuid() == 0) {
			logargs(argc, argv);

			if (arg_shell_none) {
				if (argc <= (i+1)) {
					fprintf(stderr, "Error: --shell=none set, but no command specified\n");
					exit(1);
				}
				cfg.original_program_index = i + 1;
			}

			if (!cfg.shell && !arg_shell_none)
				cfg.shell = guess_shell();

			// join sandbox by pid or by name
			pid_t pid = require_pid(argv[i] + 7);
			join(pid, argc, argv, i + 1);
			exit(0);
		}
		else
			exit_err_feature("join");

	}
	else if (strncmp(argv[i], "--join-or-start=", 16) == 0) {
		// NOTE: this is first part of option handler,
		// 		 sandbox name is set in other part
		logargs(argc, argv);

		if (arg_shell_none) {
			if (argc <= (i+1)) {
				fprintf(stderr, "Error: --shell=none set, but no command specified\n");
				exit(1);
			}
			cfg.original_program_index = i + 1;
		}

		// try to join by name only
		pid_t pid;
		if (!read_pid(argv[i] + 16, &pid)) {
			if (!cfg.shell && !arg_shell_none)
				cfg.shell = guess_shell();

			join(pid, argc, argv, i + 1);
			exit(0);
		}
		// if there no such sandbox continue argument processing
	}
#ifdef HAVE_NETWORK
	else if (strncmp(argv[i], "--join-network=", 15) == 0) {
		if (checkcfg(CFG_NETWORK)) {
			logargs(argc, argv);
			arg_join_network = 1;
			if (getuid() != 0) {
				fprintf(stderr, "Error: --join-network is only available to root user\n");
				exit(1);
			}

			if (!cfg.shell && !arg_shell_none)
				cfg.shell = guess_shell();

			// join sandbox by pid or by name
			pid_t pid = require_pid(argv[i] + 15);
			join(pid, argc, argv, i + 1);
		}
		else
			exit_err_feature("networking");
		exit(0);
	}
#endif
	else if (strncmp(argv[i], "--join-filesystem=", 18) == 0) {
		logargs(argc, argv);
		arg_join_filesystem = 1;
		if (getuid() != 0) {
			fprintf(stderr, "Error: --join-filesystem is only available to root user\n");
			exit(1);
		}

		if (!cfg.shell && !arg_shell_none)
			cfg.shell = guess_shell();

		// join sandbox by pid or by name
		pid_t pid = require_pid(argv[i] + 18);
		join(pid, argc, argv, i + 1);
		exit(0);
	}
	else if (strncmp(argv[i], "--shutdown=", 11) == 0) {
		logargs(argc, argv);

		// shutdown sandbox by pid or by name
		pid_t pid = require_pid(argv[i] + 11);
		shut(pid);
		exit(0);
	}

}



char *guess_shell(void) {
	char *shell = NULL;
	struct stat s;

	shell = getenv("SHELL");
	if (shell) {
		// TODO: handle rogue shell variables?
		if (stat(shell, &s) == 0 && access(shell, R_OK) == 0) {
			return shell;
		}
	}

	// shells in order of preference
	char *shells[] = {"/bin/bash", "/bin/csh", "/usr/bin/zsh", "/bin/sh", "/bin/ash", NULL };

	int i = 0;
	while (shells[i] != NULL) {
		// access call checks as real UID/GID, not as effective UID/GID
		if (stat(shells[i], &s) == 0 && access(shells[i], R_OK) == 0) {
			shell = shells[i];
			break;
		}
		i++;
	}

	return shell;
}

static int check_arg(int argc, char **argv, const char *argument, int strict) {
	int i;
	int found = 0;
	for (i = 1; i < argc; i++) {
		if (strict) {
			if (strcmp(argv[i], argument) == 0) {
				found = 1;
				break;
			}
		}
		else {
			if (strncmp(argv[i], argument, strlen(argument)) == 0) {
				found = 1;
				break;
			}
		}

		// detect end of firejail params
		if (strcmp(argv[i], "--") == 0)
			break;
		if (strncmp(argv[i], "--", 2) != 0)
			break;
	}

	return found;
}

//*******************************************
// Main program
//*******************************************
int main(int argc, char **argv) {
	int i;
	int prog_index = -1;			  // index in argv where the program command starts
	int lockfd_network = -1;
	int lockfd_directory = -1;
	int custom_profile = 0;	// custom profile loaded

	// drop permissions by default and rise them when required
	EUID_INIT();
	EUID_USER();

	// check if the user is allowed to use firejail
	init_cfg(argc, argv);

	// get starting timestamp, process --quiet
	start_timestamp = getticks();
	if (check_arg(argc, argv, "--quiet", 1))
		arg_quiet = 1;

	// cleanup at exit
	EUID_ROOT();
	atexit(clear_atexit);

	// build /run/firejail directory structure
	preproc_build_firejail_dir();
	char *container_name = getenv("container");
	if (!container_name || strcmp(container_name, "firejail")) {
		lockfd_directory = open(RUN_DIRECTORY_LOCK_FILE, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
		if (lockfd_directory != -1) {
			int rv = fchown(lockfd_directory, 0, 0);
			(void) rv;
			flock(lockfd_directory, LOCK_EX);
		}
		preproc_clean_run();
		flock(lockfd_directory, LOCK_UN);
		close(lockfd_directory);
	}
	EUID_USER();


	// check argv[0] symlink wrapper if this is not a login shell
	if (*argv[0] != '-')
		run_symlink(argc, argv, 0); // if symlink detected, this function will not return

	// check if we already have a sandbox running
	// If LXC is detected, start firejail sandbox
	// otherwise try to detect a PID namespace by looking under /proc for specific kernel processes and:
	//	- start the application in a /bin/bash shell
	if (check_namespace_virt() == 0) {
		EUID_ROOT();
		int rv = check_kernel_procs();
		EUID_USER();
		if (rv == 0) {
			if (check_arg(argc, argv, "--version", 1)) {
				printf("firejail version %s\n", VERSION);
				exit(0);
			}

			// start the program directly without sandboxing
			run_no_sandbox(argc, argv);
			// it will never get here!
			assert(0);
		}
	}
	EUID_ASSERT();


	// check firejail directories
	EUID_ROOT();
	delete_run_files(sandbox_pid);
	EUID_USER();

	// check for force-nonewprivs in /etc/firejail/firejail.config file
	if (checkcfg(CFG_FORCE_NONEWPRIVS))
		arg_nonewprivs = 1;

	// parse arguments
	for (i = 1; i < argc; i++) {
		run_cmd_and_exit(i, argc, argv); // will exit if the command is recognized

		if (strcmp(argv[i], "--debug") == 0 && !arg_quiet)
			arg_debug = 1;
		else if (strcmp(argv[i], "--quiet") == 0) {
			arg_quiet = 1;
			arg_debug = 0;
		}

		//*************************************
		// filtering
		//*************************************
#ifdef HAVE_APPARMOR
		else if (strcmp(argv[i], "--apparmor") == 0)
			arg_apparmor = 1;
#endif
#ifdef HAVE_SECCOMP
		else if (strncmp(argv[i], "--protocol=", 11) == 0) {
			if (checkcfg(CFG_SECCOMP)) {
				if (cfg.protocol) {
					fwarning("two protocol lists are present, \"%s\" will be installed\n", cfg.protocol);
				}
				else {
					// store list
					cfg.protocol = strdup(argv[i] + 11);
					if (!cfg.protocol)
						errExit("strdup");
				}
			}
			else
				exit_err_feature("seccomp");
		}
		else if (strcmp(argv[i], "--seccomp") == 0) {
			if (checkcfg(CFG_SECCOMP)) {
				if (arg_seccomp) {
					fprintf(stderr, "Error: seccomp already enabled\n");
					exit(1);
				}
				arg_seccomp = 1;
			}
			else
				exit_err_feature("seccomp");
		}
		else if (strncmp(argv[i], "--seccomp=", 10) == 0) {
			if (checkcfg(CFG_SECCOMP)) {
				if (arg_seccomp) {
					fprintf(stderr, "Error: seccomp already enabled\n");
					exit(1);
				}
				arg_seccomp = 1;
				cfg.seccomp_list = seccomp_check_list(argv[i] + 10);
			}
			else
				exit_err_feature("seccomp");
		}
		else if (strncmp(argv[i], "--seccomp.drop=", 15) == 0) {
			if (checkcfg(CFG_SECCOMP)) {
				if (arg_seccomp) {
					fprintf(stderr, "Error: seccomp already enabled\n");
					exit(1);
				}
				arg_seccomp = 1;
				cfg.seccomp_list_drop = seccomp_check_list(argv[i] + 15);
			}
			else
				exit_err_feature("seccomp");
		}
		else if (strncmp(argv[i], "--seccomp.keep=", 15) == 0) {
			if (checkcfg(CFG_SECCOMP)) {
				if (arg_seccomp) {
					fprintf(stderr, "Error: seccomp already enabled\n");
					exit(1);
				}
				arg_seccomp = 1;
				cfg.seccomp_list_keep = seccomp_check_list(argv[i] + 15);
			}
			else
				exit_err_feature("seccomp");
		}
		else if (strcmp(argv[i], "--seccomp.block-secondary") == 0) {
			if (checkcfg(CFG_SECCOMP)) {
				arg_seccomp_block_secondary = 1;
			}
			else
				exit_err_feature("seccomp");
		}
		else if (strcmp(argv[i], "--memory-deny-write-execute") == 0) {
			if (checkcfg(CFG_SECCOMP))
				arg_memory_deny_write_execute = 1;
			else
				exit_err_feature("seccomp");
		}
#endif
		else if (strcmp(argv[i], "--caps") == 0)
			arg_caps_default_filter = 1;
		else if (strcmp(argv[i], "--caps.drop=all") == 0)
			arg_caps_drop_all = 1;
		else if (strncmp(argv[i], "--caps.drop=", 12) == 0) {
			arg_caps_drop = 1;
			arg_caps_list = strdup(argv[i] + 12);
			if (!arg_caps_list)
				errExit("strdup");
			// verify caps list and exit if problems
			caps_check_list(arg_caps_list, NULL);
		}
		else if (strncmp(argv[i], "--caps.keep=", 12) == 0) {
			arg_caps_keep = 1;
			arg_caps_list = strdup(argv[i] + 12);
			if (!arg_caps_list)
				errExit("strdup");
			// verify caps list and exit if problems
			caps_check_list(arg_caps_list, NULL);
		}


		else if (strncmp(argv[i], "--ipc-namespace", 15) == 0)
			arg_ipc = 1;

		//*************************************
		// filesystem
		//*************************************
		else if (strcmp(argv[i], "--allusers") == 0)
			arg_allusers = 1;
#ifdef HAVE_BIND
		else if (strncmp(argv[i], "--bind=", 7) == 0) {
			if (checkcfg(CFG_BIND)) {
				char *line;
				if (asprintf(&line, "bind %s", argv[i] + 7) == -1)
					errExit("asprintf");

				profile_check_line(line, 0, NULL);	// will exit if something wrong
				profile_add(line);
			}
			else
				exit_err_feature("bind");
		}
#endif
		else if (strncmp(argv[i], "--tmpfs=", 8) == 0) {
			char *line;
			if (asprintf(&line, "tmpfs %s", argv[i] + 8) == -1)
				errExit("asprintf");

			profile_check_line(line, 0, NULL);	// will exit if something wrong
			profile_add(line);
		}
		else if (strncmp(argv[i], "--blacklist=", 12) == 0) {
			char *line;
			if (asprintf(&line, "blacklist %s", argv[i] + 12) == -1)
				errExit("asprintf");

			profile_check_line(line, 0, NULL);	// will exit if something wrong
			profile_add(line);
		}
		else if (strncmp(argv[i], "--noblacklist=", 14) == 0) {
			char *line;
			if (asprintf(&line, "noblacklist %s", argv[i] + 14) == -1)
				errExit("asprintf");

			profile_check_line(line, 0, NULL);	// will exit if something wrong
			profile_add(line);
		}

#ifdef HAVE_WHITELIST
		else if (strncmp(argv[i], "--whitelist=", 12) == 0) {
			if (checkcfg(CFG_WHITELIST)) {
				char *line;
				if (asprintf(&line, "whitelist %s", argv[i] + 12) == -1)
					errExit("asprintf");

				profile_check_line(line, 0, NULL);	// will exit if something wrong
				profile_add(line);
			}
			else
				exit_err_feature("whitelist");
		}
		else if (strncmp(argv[i], "--nowhitelist=", 14) == 0) {
			char *line;
			if (asprintf(&line, "nowhitelist %s", argv[i] + 14) == -1)
				errExit("asprintf");

			profile_check_line(line, 0, NULL);	// will exit if something wrong
			profile_add(line);
		}
#endif

		else if (strncmp(argv[i], "--read-only=", 12) == 0) {
			char *line;
			if (asprintf(&line, "read-only %s", argv[i] + 12) == -1)
				errExit("asprintf");

			profile_check_line(line, 0, NULL);	// will exit if something wrong
			profile_add(line);
		}
		else if (strncmp(argv[i], "--noexec=", 9) == 0) {
			char *line;
			if (asprintf(&line, "noexec %s", argv[i] + 9) == -1)
				errExit("asprintf");

			profile_check_line(line, 0, NULL);	// will exit if something wrong
			profile_add(line);
		}
		else if (strncmp(argv[i], "--read-write=", 13) == 0) {
			char *line;
			if (asprintf(&line, "read-write %s", argv[i] + 13) == -1)
				errExit("asprintf");

			profile_check_line(line, 0, NULL);	// will exit if something wrong
			profile_add(line);
		}
		else if (strcmp(argv[i], "--disable-mnt") == 0)
			arg_disable_mnt = 1;
		else if (strncmp(argv[i], "--profile=", 10) == 0) {
			// multiple profile files are allowed!

			if (arg_noprofile) {
				fprintf(stderr, "Error: --noprofile and --profile options are mutually exclusive\n");
				exit(1);
			}

			char *ppath = expand_home(argv[i] + 10, cfg.homedir);
			if (!ppath)
				errExit("strdup");

			profile_read(ppath);
			custom_profile = 1;
			free(ppath);
		}
		else if (strcmp(argv[i], "--noprofile") == 0) {
			if (custom_profile) {
				fprintf(stderr, "Error: --profile and --noprofile options are mutually exclusive\n");
				exit(1);
			}
			arg_noprofile = 1;
		}
		else if (strncmp(argv[i], "--ignore=", 9) == 0) {
			if (custom_profile) {
				fprintf(stderr, "Error: please use --profile after --ignore\n");
				exit(1);
			}

			if (*(argv[i] + 9) == '\0') {
				fprintf(stderr, "Error: invalid ignore option\n");
				exit(1);
			}

			// find an empty entry in profile_ignore array
			int j;
			for (j = 0; j < MAX_PROFILE_IGNORE; j++) {
				if (cfg.profile_ignore[j] == NULL)
					break;
			}
			if (j >= MAX_PROFILE_IGNORE) {
				fprintf(stderr, "Error: maximum %d --ignore options are permitted\n", MAX_PROFILE_IGNORE);
				exit(1);
			}
			// ... and configure it
			else
				cfg.profile_ignore[j] = argv[i] + 9;
		}
		else if (strcmp(argv[i], "--writable-etc") == 0) {
			arg_writable_etc = 1;
		}
		else if (strcmp(argv[i], "--writable-var") == 0) {
			arg_writable_var = 1;
		}
		else if (strcmp(argv[1], "--keep-var-tmp") == 0) {
		        arg_keep_var_tmp = 1;
		}
		else if (strcmp(argv[i], "--writable-run-user") == 0) {
			arg_writable_run_user = 1;
		}
		else if (strcmp(argv[i], "--writable-var-log") == 0) {
			arg_writable_var_log = 1;
		}
		else if (strcmp(argv[i], "--private") == 0) {
			arg_private = 1;
		}
		else if (strncmp(argv[i], "--private=", 10) == 0) {
			// extract private home dirname
			cfg.home_private = argv[i] + 10;
			if (*cfg.home_private == '\0') {
				fprintf(stderr, "Error: invalid private option\n");
				exit(1);
			}
			fs_check_private_dir();

			// downgrade to --private if the directory is the user home directory
			if (strcmp(cfg.home_private, cfg.homedir) == 0) {
				free(cfg.home_private);
				cfg.home_private = NULL;
			}
			arg_private = 1;
		}
		else if (strcmp(argv[i], "--private-dev") == 0) {
			arg_private_dev = 1;
		}
		else if (strcmp(argv[i], "--private-tmp") == 0) {
			arg_private_tmp = 1;
		}
		else if (strcmp(argv[i], "--private-cache") == 0) {
			arg_private_cache = 1;
		}

		//*************************************
		// hostname, etc
		//*************************************
		else if (strncmp(argv[i], "--name=", 7) == 0) {
			cfg.name = argv[i] + 7;
			if (strlen(cfg.name) == 0) {
				fprintf(stderr, "Error: please provide a name for sandbox\n");
				return 1;
			}
		}
		else if (strncmp(argv[i], "--hostname=", 11) == 0) {
			cfg.hostname = argv[i] + 11;
			if (strlen(cfg.hostname) == 0) {
				fprintf(stderr, "Error: please provide a hostname for sandbox\n");
				return 1;
			}
		}
		else if (strcmp(argv[i], "--nogroups") == 0)
			arg_nogroups = 1;
#ifdef HAVE_USERNS
		else if (strcmp(argv[i], "--noroot") == 0) {
			if (checkcfg(CFG_USERNS))
				check_user_namespace();
			else
				exit_err_feature("noroot");
		}
#endif
		else if (strcmp(argv[i], "--nonewprivs") == 0)
			arg_nonewprivs = 1;
		else if (strncmp(argv[i], "--env=", 6) == 0)
			env_store(argv[i] + 6, SETENV);
		else if (strncmp(argv[i], "--rmenv=", 8) == 0)
			env_store(argv[i] + 8, RMENV);
		else if (strcmp(argv[i], "--nosound") == 0)
			arg_nosound = 1;
		else if (strcmp(argv[i], "--noautopulse") == 0)
			arg_noautopulse = 1;
		else if (strcmp(argv[i], "--novideo") == 0)
			arg_novideo = 1;
		else if (strcmp(argv[i], "--no3d") == 0)
			arg_no3d = 1;
		else if (strcmp(argv[i], "--notv") == 0)
			arg_notv = 1;
		else if (strcmp(argv[i], "--nodvd") == 0)
			arg_nodvd = 1;
		else if (strcmp(argv[i], "--nou2f") == 0)
			arg_nou2f = 1;

		//*************************************
		// network
		//*************************************
#ifdef HAVE_NETWORK
		else if (strncmp(argv[i], "--interface=", 12) == 0) {
			if (checkcfg(CFG_NETWORK)) {
#ifdef HAVE_NETWORK_RESTRICTED
				// compile time restricted networking
				if (getuid() != 0) {
					fprintf(stderr, "Error: --interface is allowed only to root user\n");
					exit(1);
				}
#endif
				// run time restricted networking
				if (checkcfg(CFG_RESTRICTED_NETWORK) && getuid() != 0) {
					fprintf(stderr, "Error: --interface is allowed only to root user\n");
					exit(1);
				}

				// checks
				if (arg_nonetwork) {
					fprintf(stderr, "Error: --net=none and --interface are incompatible\n");
					exit(1);
				}
				if (strcmp(argv[i] + 12, "lo") == 0) {
					fprintf(stderr, "Error: cannot use lo device in --interface command\n");
					exit(1);
				}
				int ifindex = if_nametoindex(argv[i] + 12);
				if (ifindex <= 0) {
					fprintf(stderr, "Error: cannot find interface %s\n", argv[i] + 12);
					exit(1);
				}

				Interface *intf;
				if (cfg.interface0.configured == 0)
					intf = &cfg.interface0;
				else if (cfg.interface1.configured == 0)
					intf = &cfg.interface1;
				else if (cfg.interface2.configured == 0)
					intf = &cfg.interface2;
				else if (cfg.interface3.configured == 0)
					intf = &cfg.interface3;
				else {
					fprintf(stderr, "Error: maximum 4 interfaces are allowed\n");
					return 1;
				}

				intf->dev = strdup(argv[i] + 12);
				if (!intf->dev)
					errExit("strdup");

				if (net_get_if_addr(intf->dev, &intf->ip, &intf->mask, intf->mac, &intf->mtu)) {
					fwarning("interface %s is not configured\n", intf->dev);
				}
				intf->configured = 1;
			}
			else
				exit_err_feature("networking");
		}

		else if (strncmp(argv[i], "--net=", 6) == 0) {
			if (checkcfg(CFG_NETWORK)) {
				if (strcmp(argv[i] + 6, "none") == 0) {
					arg_nonetwork  = 1;
					cfg.bridge0.configured = 0;
					cfg.bridge1.configured = 0;
					cfg.bridge2.configured = 0;
					cfg.bridge3.configured = 0;
					cfg.interface0.configured = 0;
					cfg.interface1.configured = 0;
					cfg.interface2.configured = 0;
					cfg.interface3.configured = 0;
					continue;
				}

#ifdef HAVE_NETWORK_RESTRICTED
				// compile time restricted networking
				if (getuid() != 0) {
					fprintf(stderr, "Error: only --net=none is allowed to non-root users\n");
					exit(1);
				}
#endif
				// run time restricted networking
				if (checkcfg(CFG_RESTRICTED_NETWORK) && getuid() != 0) {
					fprintf(stderr, "Error: only --net=none is allowed to non-root users\n");
					exit(1);
				}
				if (strcmp(argv[i] + 6, "lo") == 0) {
					fprintf(stderr, "Error: cannot attach to lo device\n");
					exit(1);
				}

				Bridge *br;
				if (cfg.bridge0.configured == 0)
					br = &cfg.bridge0;
				else if (cfg.bridge1.configured == 0)
					br = &cfg.bridge1;
				else if (cfg.bridge2.configured == 0)
					br = &cfg.bridge2;
				else if (cfg.bridge3.configured == 0)
					br = &cfg.bridge3;
				else {
					fprintf(stderr, "Error: maximum 4 network devices are allowed\n");
					return 1;
				}
				net_configure_bridge(br, argv[i] + 6);
			}
			else
				exit_err_feature("networking");
		}

		else if (strncmp(argv[i], "--veth-name=", 12) == 0) {
			if (checkcfg(CFG_NETWORK)) {
				Bridge *br = last_bridge_configured();
				if (br == NULL) {
					fprintf(stderr, "Error: no network device configured\n");
					exit(1);
				}
				br->veth_name = strdup(argv[i] + 12);
				if (br->veth_name == NULL)
					errExit("strdup");
				if (*br->veth_name == '\0') {
					fprintf(stderr, "Error: no veth-name configured\n");
					exit(1);
				}
			}
			else
				exit_err_feature("networking");
		}

		else if (strcmp(argv[i], "--scan") == 0) {
			if (checkcfg(CFG_NETWORK)) {
				arg_scan = 1;
			}
			else
				exit_err_feature("networking");
		}
		else if (strncmp(argv[i], "--iprange=", 10) == 0) {
			if (checkcfg(CFG_NETWORK)) {
				Bridge *br = last_bridge_configured();
				if (br == NULL) {
					fprintf(stderr, "Error: no network device configured\n");
					return 1;
				}
				if (br->iprange_start || br->iprange_end) {
					fprintf(stderr, "Error: cannot configure the IP range twice for the same interface\n");
					return 1;
				}

				// parse option arguments
				char *firstip = argv[i] + 10;
				char *secondip = firstip;
				while (*secondip != '\0') {
					if (*secondip == ',')
						break;
					secondip++;
				}
				if (*secondip == '\0') {
					fprintf(stderr, "Error: invalid IP range\n");
					return 1;
				}
				*secondip = '\0';
				secondip++;

				// check addresses
				if (atoip(firstip, &br->iprange_start) || atoip(secondip, &br->iprange_end) ||
				    br->iprange_start >= br->iprange_end) {
					fprintf(stderr, "Error: invalid IP range\n");
					return 1;
				}
				if (in_netrange(br->iprange_start, br->ip, br->mask) || in_netrange(br->iprange_end, br->ip, br->mask)) {
					fprintf(stderr, "Error: IP range addresses not in network range\n");
					return 1;
				}
			}
			else
				exit_err_feature("networking");
		}

		else if (strncmp(argv[i], "--mac=", 6) == 0) {
			if (checkcfg(CFG_NETWORK)) {
				Bridge *br = last_bridge_configured();
				if (br == NULL) {
					fprintf(stderr, "Error: no network device configured\n");
					exit(1);
				}
				if (mac_not_zero(br->macsandbox)) {
					fprintf(stderr, "Error: cannot configure the MAC address twice for the same interface\n");
					exit(1);
				}

				// read the address
				if (atomac(argv[i] + 6, br->macsandbox)) {
					fprintf(stderr, "Error: invalid MAC address\n");
					exit(1);
				}
			}
			else
				exit_err_feature("networking");
		}

		else if (strncmp(argv[i], "--mtu=", 6) == 0) {
			if (checkcfg(CFG_NETWORK)) {
				Bridge *br = last_bridge_configured();
				if (br == NULL) {
					fprintf(stderr, "Error: no network device configured\n");
					exit(1);
				}

				if (sscanf(argv[i] + 6, "%d", &br->mtu) != 1 || br->mtu < 576 || br->mtu > 9198) {
					fprintf(stderr, "Error: invalid mtu value\n");
					exit(1);
				}
			}
			else
				exit_err_feature("networking");
		}

		else if (strncmp(argv[i], "--ip=", 5) == 0) {
			if (checkcfg(CFG_NETWORK)) {
				Bridge *br = last_bridge_configured();
				if (br == NULL) {
					fprintf(stderr, "Error: no network device configured\n");
					exit(1);
				}
				if (br->arg_ip_none || br->ipsandbox) {
					fprintf(stderr, "Error: cannot configure the IP address twice for the same interface\n");
					exit(1);
				}

				// configure this IP address for the last bridge defined
				if (strcmp(argv[i] + 5, "none") == 0)
					br->arg_ip_none = 1;
				else {
					if (atoip(argv[i] + 5, &br->ipsandbox)) {
						fprintf(stderr, "Error: invalid IP address\n");
						exit(1);
					}
				}
			}
			else
				exit_err_feature("networking");
		}

		else if (strncmp(argv[i], "--ip6=", 6) == 0) {
			if (checkcfg(CFG_NETWORK)) {
				Bridge *br = last_bridge_configured();
				if (br == NULL) {
					fprintf(stderr, "Error: no network device configured\n");
					exit(1);
				}
				if (br->ip6sandbox) {
					fprintf(stderr, "Error: cannot configure the IP address twice for the same interface\n");
					exit(1);
				}

				// configure this IP address for the last bridge defined
				if (check_ip46_address(argv[i] + 6) == 0) {
					fprintf(stderr, "Error: invalid IPv6 address\n");
					exit(1);
				}

				br->ip6sandbox = strdup(argv[i] + 6);
				if (br->ip6sandbox == NULL)
					errExit("strdup");
			}
			else
				exit_err_feature("networking");
		}


		else if (strncmp(argv[i], "--defaultgw=", 12) == 0) {
			if (checkcfg(CFG_NETWORK)) {
				if (atoip(argv[i] + 12, &cfg.defaultgw)) {
					fprintf(stderr, "Error: invalid IP address\n");
					exit(1);
				}
			}
			else
				exit_err_feature("networking");
		}
#endif
		else if (strncmp(argv[i], "--dns=", 6) == 0) {
			if (check_ip46_address(argv[i] + 6) == 0) {
				fprintf(stderr, "Error: invalid DNS server IPv4 or IPv6 address\n");
				exit(1);
			}
			char *dns = strdup(argv[i] + 6);
			if (!dns)
				errExit("strdup");

			if (cfg.dns1 == NULL)
				cfg.dns1 = dns;
			else if (cfg.dns2 == NULL)
				cfg.dns2 = dns;
			else if (cfg.dns3 == NULL)
				cfg.dns3 = dns;
			else if (cfg.dns4 == NULL)
				cfg.dns4 = dns;
			else {
				fprintf(stderr, "Error: up to 4 DNS servers can be specified\n");
				return 1;
			}
		}

		else if (strncmp(argv[i], "--hosts-file=", 13) == 0)
			cfg.hosts_file = fs_check_hosts_file(argv[i] + 13);

#ifdef HAVE_NETWORK
		else if (strcmp(argv[i], "--netfilter") == 0) {
#ifdef HAVE_NETWORK_RESTRICTED
			// compile time restricted networking
			if (getuid() != 0) {
				fprintf(stderr, "Error: --netfilter is only allowed for root\n");
				exit(1);
			}
#endif
			// run time restricted networking
			if (checkcfg(CFG_RESTRICTED_NETWORK) && getuid() != 0) {
				fprintf(stderr, "Error: --netfilter is only allowed for root\n");
				exit(1);
			}
			if (checkcfg(CFG_NETWORK)) {
				arg_netfilter = 1;
			}
			else
				exit_err_feature("networking");
		}

		else if (strncmp(argv[i], "--netfilter=", 12) == 0) {
#ifdef HAVE_NETWORK_RESTRICTED
			// compile time restricted networking
			if (getuid() != 0) {
				fprintf(stderr, "Error: --netfilter is only allowed for root\n");
				exit(1);
			}
#endif
			// run time restricted networking
			if (checkcfg(CFG_RESTRICTED_NETWORK) && getuid() != 0) {
				fprintf(stderr, "Error: --netfilter is only allowed for root\n");
				exit(1);
			}
			if (checkcfg(CFG_NETWORK)) {
				arg_netfilter = 1;
				arg_netfilter_file = argv[i] + 12;
				check_netfilter_file(arg_netfilter_file);
			}
			else
				exit_err_feature("networking");
		}

		else if (strncmp(argv[i], "--netfilter6=", 13) == 0) {
			if (checkcfg(CFG_NETWORK)) {
				arg_netfilter6 = 1;
				arg_netfilter6_file = argv[i] + 13;
				check_netfilter_file(arg_netfilter6_file);
			}
			else
				exit_err_feature("networking");
		}

		else if (strncmp(argv[i], "--netns=", 8) == 0) {
			if (checkcfg(CFG_NETWORK)) {
				arg_netns = argv[i] + 8;
				check_netns(arg_netns);
			}
			else
				exit_err_feature("networking");
		}
#endif
		//*************************************
		// command
		//*************************************
		else if (strncmp(argv[i], "--timeout=", 10) == 0)
			cfg.timeout = extract_timeout(argv[i] + 10);
		else if (strcmp(argv[i], "--appimage") == 0)
			arg_appimage = 1;
		else if (strcmp(argv[i], "--shell=none") == 0) {
			arg_shell_none = 1;
			if (cfg.shell) {
				fprintf(stderr, "Error: a shell was already specified\n");
				return 1;
			}
		}
		else if (strncmp(argv[i], "--shell=", 8) == 0) {
			if (arg_shell_none) {
				fprintf(stderr, "Error: --shell=none was already specified.\n");
				return 1;
			}
			invalid_filename(argv[i] + 8, 0); // no globbing

			if (cfg.shell) {
				fprintf(stderr, "Error: only one user shell can be specified\n");
				return 1;
			}
			cfg.shell = argv[i] + 8;

			if (is_dir(cfg.shell) || strstr(cfg.shell, "..")) {
				fprintf(stderr, "Error: invalid shell\n");
				exit(1);
			}

			// access call checks as real UID/GID, not as effective UID/GID
			if (access(cfg.shell, R_OK)) {
				fprintf(stderr, "Error: cannot access shell file\n");
				exit(1);
			}
		}
		else if (strcmp(argv[i], "-c") == 0) {
			arg_command = 1;
			if (i == (argc -  1)) {
				fprintf(stderr, "Error: option -c requires an argument\n");
				return 1;
			}
		}

		else if (strncmp(argv[i], "--join-or-start=", 16) == 0) {
			// NOTE: this is second part of option handler,
			//		 atempt to find and join sandbox is done in other one

			// set sandbox name and start normally
			cfg.name = argv[i] + 16;
			if (strlen(cfg.name) == 0) {
				fprintf(stderr, "Error: please provide a name for sandbox\n");
				return 1;
			}
		}
		else if (strcmp(argv[i], "--git-install") == 0 ||
			strcmp(argv[i], "--git-uninstall") == 0) {
			fprintf(stderr, "This feature is not enabled in the current build\n");
			exit(1);
		}

		else if (strcmp(argv[i], "--") == 0) {
			// double dash - positional params to follow
			arg_doubledash = 1;
			i++;
			if (i  >= argc) {
				fprintf(stderr, "Error: program name not found\n");
				exit(1);
			}
			extract_command_name(i, argv);
			prog_index = i;
			break;
		}
		else {
			// is this an invalid option?
			if (*argv[i] == '-') {
				fprintf(stderr, "Error: invalid %s command line option\n", argv[i]);
				return 1;
			}

			// we have a program name coming
			if (arg_appimage) {
				cfg.command_name = strdup(argv[i]);
				if (!cfg.command_name)
					errExit("strdup");

				// disable shell=* for appimages
				arg_shell_none = 0;
			}
			else
				extract_command_name(i, argv);
			prog_index = i;
			break;
		}
	}
	EUID_ASSERT();

	// prog_index could still be -1 if no program was specified
	if (prog_index == -1 && arg_shell_none) {
		fprintf(stderr, "Error: shell=none configured, but no program specified\n");
		exit(1);
	}

	// enable seccomp if only seccomp.block-secondary was specified
	if (arg_seccomp_block_secondary)
		arg_seccomp = 1;

	// log command
	logargs(argc, argv);
	if (fullargc) {
		char *msg;
		if (asprintf(&msg, "user %s entering restricted shell", cfg.username) == -1)
			errExit("asprintf");
		logmsg(msg);
		free(msg);
	}

	// guess shell if unspecified
	if (!arg_shell_none && !cfg.shell) {
		cfg.shell = guess_shell();
		if (!cfg.shell) {
			fprintf(stderr, "Error: unable to guess your shell, please set explicitly by using --shell option.\n");
			exit(1);
		}
		if (arg_debug)
			printf("Autoselecting %s as shell\n", cfg.shell);
	}

	// build the sandbox command
	if (prog_index == -1 && cfg.shell) {
		cfg.command_line = cfg.shell;
		cfg.window_title = cfg.shell;
		cfg.command_name = cfg.shell;
	}
	else if (arg_appimage) {
		if (arg_debug)
			printf("Configuring appimage environment\n");
		appimage_set(cfg.command_name);
		build_appimage_cmdline(&cfg.command_line, &cfg.window_title, argc, argv, prog_index, cfg.command_line);
	}
	else {
		build_cmdline(&cfg.command_line, &cfg.window_title, argc, argv, prog_index);
	}
/*	else {
		fprintf(stderr, "Error: command must be specified when --shell=none used.\n");
		exit(1);
	}*/

	assert(cfg.command_name);
	if (arg_debug)
		printf("Command name #%s#\n", cfg.command_name);


	// load the profile
	if (!arg_noprofile) {
		if (!custom_profile) {
			// look for a profile in ~/.config/firejail directory
			char *usercfgdir;
			if (asprintf(&usercfgdir, "%s/.config/firejail", cfg.homedir) == -1)
				errExit("asprintf");
			int rv = profile_find(cfg.command_name, usercfgdir);
			free(usercfgdir);
			custom_profile = rv;
		}
		if (!custom_profile) {
			// look for a user profile in /etc/firejail directory
			int rv = profile_find(cfg.command_name, SYSCONFDIR);
			custom_profile = rv;
		}
	}

	// use default.profile as the default
	if (!custom_profile && !arg_noprofile) {
		// try to load a default profile
		char *profile_name = DEFAULT_USER_PROFILE;
		if (getuid() == 0)
			profile_name = DEFAULT_ROOT_PROFILE;
		if (arg_debug)
			printf("Attempting to find %s.profile...\n", profile_name);

		// look for the profile in ~/.config/firejail directory
		char *usercfgdir;
		if (asprintf(&usercfgdir, "%s/.config/firejail", cfg.homedir) == -1)
			errExit("asprintf");
		custom_profile = profile_find(profile_name, usercfgdir);
		free(usercfgdir);

		if (!custom_profile)
			// look for the profile in /etc/firejail directory
			custom_profile = profile_find(profile_name, SYSCONFDIR);

		if (!custom_profile) {
			fprintf(stderr, "Error: no default.profile installed\n");
			exit(1);
		}

		if (custom_profile)
			fmessage("\n** Note: you can use --noprofile to disable %s.profile **\n\n", profile_name);
	}
	EUID_ASSERT();

	// check network configuration options - it will exit if anything went wrong
	net_check_cfg();

	// check and assign an IP address - for macvlan it will be done again in the sandbox!
	if (any_bridge_configured()) {
		EUID_ROOT();
		lockfd_network = open(RUN_NETWORK_LOCK_FILE, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
		if (lockfd_network != -1) {
			int rv = fchown(lockfd_network, 0, 0);
			(void) rv;
			flock(lockfd_network, LOCK_EX);
		}

		check_network(&cfg.bridge0);
		check_network(&cfg.bridge1);
		check_network(&cfg.bridge2);
		check_network(&cfg.bridge3);

		// save network mapping in shared memory
		network_set_run_file(sandbox_pid);
		EUID_USER();
	}
	EUID_ASSERT();

 	// create the parent-child communication pipe
 	if (pipe(parent_to_child_fds) < 0)
 		errExit("pipe");
 	if (pipe(child_to_parent_fds) < 0)
		errExit("pipe");

	// set name
	EUID_ROOT();
	lockfd_directory = open(RUN_DIRECTORY_LOCK_FILE, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
	if (lockfd_directory != -1) {
		int rv = fchown(lockfd_directory, 0, 0);
		(void) rv;
		flock(lockfd_directory, LOCK_EX);
	}
	if (cfg.name)
		set_name_run_file(sandbox_pid);
	flock(lockfd_directory, LOCK_UN);
	close(lockfd_directory);
	EUID_USER();

	// clone environment
	int flags = CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS | SIGCHLD;

	// in root mode also enable CLONE_NEWIPC
	// in user mode CLONE_NEWIPC will break MIT Shared Memory Extension (MIT-SHM)
	if (getuid() == 0 || arg_ipc) {
		flags |= CLONE_NEWIPC;
		if (arg_debug)
			printf("Enabling IPC namespace\n");
	}

	if (any_bridge_configured() || any_interface_configured() || arg_nonetwork) {
		flags |= CLONE_NEWNET;
	}
	else if (arg_debug)
		printf("Using the local network stack\n");

	EUID_ASSERT();
	EUID_ROOT();
	child = clone(sandbox,
		child_stack + STACK_SIZE,
		flags,
		NULL);
	if (child == -1)
		errExit("clone");
	EUID_USER();

	if (!arg_command && !arg_quiet) {
		fmessage("Parent pid %u, child pid %u\n", sandbox_pid, child);
		// print the path of the new log directory
		if (getuid() == 0) // only for root
			printf("The new log directory is /proc/%d/root/var/log\n", child);
	}

	if (!arg_nonetwork) {
		EUID_ROOT();
		pid_t net_child = fork();
		if (net_child < 0)
			errExit("fork");
		if (net_child == 0) {
			// elevate privileges in order to get grsecurity working
			if (setreuid(0, 0))
				errExit("setreuid");
			if (setregid(0, 0))
				errExit("setregid");
			network_main(child);
			if (arg_debug)
				printf("Host network configured\n");
#ifdef HAVE_GCOV
			__gcov_flush();
#endif
			_exit(0);
		}

		// wait for the child to finish
		waitpid(net_child, NULL, 0);
		EUID_USER();
	}
	EUID_ASSERT();

 	// close each end of the unused pipes
 	close(parent_to_child_fds[0]);
 	close(child_to_parent_fds[1]);

	// notify child that base setup is complete
 	notify_other(parent_to_child_fds[1]);

 	// wait for child to create new user namespace with CLONE_NEWUSER
 	wait_for_other(child_to_parent_fds[0]);
 	close(child_to_parent_fds[0]);

 	if (arg_noroot) {
	 	// update the UID and GID maps in the new child user namespace
		// uid
	 	char *map_path;
	 	if (asprintf(&map_path, "/proc/%d/uid_map", child) == -1)
	 		errExit("asprintf");

	 	char *map;
	 	uid_t uid = getuid();
	 	if (asprintf(&map, "%d %d 1", uid, uid) == -1)
	 		errExit("asprintf");
 		EUID_ROOT();
	 	update_map(map, map_path);
	 	EUID_USER();
	 	free(map);
	 	free(map_path);

	 	// gid file
		if (asprintf(&map_path, "/proc/%d/gid_map", child) == -1)
			errExit("asprintf");
	 	char gidmap[1024];
	 	char *ptr = gidmap;
	 	*ptr = '\0';

	 	// add user group
	 	gid_t gid = getgid();
	 	sprintf(ptr, "%d %d 1\n", gid, gid);
	 	ptr += strlen(ptr);

	 	if (!arg_nogroups) {
		 	//  add tty group
		 	gid_t g = get_group_id("tty");
		 	if (g) {
		 		sprintf(ptr, "%d %d 1\n", g, g);
		 		ptr += strlen(ptr);
		 	}

		 	//  add audio group
		 	g = get_group_id("audio");
		 	if (g) {
		 		sprintf(ptr, "%d %d 1\n", g, g);
		 		ptr += strlen(ptr);
		 	}

		 	//  add video group
		 	g = get_group_id("video");
		 	if (g) {
		 		sprintf(ptr, "%d %d 1\n", g, g);
		 		ptr += strlen(ptr);
		 	}

		 	//  add games group
		 	g = get_group_id("games");
		 	if (g) {
		 		sprintf(ptr, "%d %d 1\n", g, g);
		 	}
		 }

 		EUID_ROOT();
	 	update_map(gidmap, map_path);
	 	EUID_USER();
	 	free(map_path);
 	}
	EUID_ASSERT();

 	// notify child that UID/GID mapping is complete
 	notify_other(parent_to_child_fds[1]);
 	close(parent_to_child_fds[1]);

 	EUID_ROOT();
	if (lockfd_network != -1) {
		flock(lockfd_network, LOCK_UN);
		close(lockfd_network);
	}

	// handle CTRL-C in parent
	signal (SIGINT, my_handler);
	signal (SIGTERM, my_handler);

	// wait for the child to finish
	EUID_USER();
	int status = 0;
	waitpid(child, &status, 0);

	// free globals
	if (cfg.profile) {
		ProfileEntry *prf = cfg.profile;
		while (prf != NULL) {
			ProfileEntry *next = prf->next;
			free(prf->data);
			free(prf->link);
			free(prf);
			prf = next;
		}
	}

	if (WIFEXITED(status)){
		myexit(WEXITSTATUS(status));
	} else if (WIFSIGNALED(status)) {
		myexit(WTERMSIG(status));
	} else {
		myexit(0);
	}

	return 0;
}
