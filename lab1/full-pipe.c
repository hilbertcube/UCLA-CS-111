#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>

int main(int argc, char *argv[])
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s cmd1 [args...] : cmd2 [args...] : ...\n", argv[0]);
		return EINVAL;
	}

	/* ---------------- Parse commands ---------------- */
	int max_cmds = argc;
	char ***cmds = malloc(max_cmds * sizeof(char **));
	if (!cmds) {
		perror("malloc");
		return errno;
	}

	int cmd_count = 0;
	int i = 1;

	while (i < argc) {
		int start = i;

		while (i < argc && strcmp(argv[i], ":") != 0)
			i++;

		int len = i - start;
		if (len == 0) {
			fprintf(stderr, "Empty command\n");
			free(cmds);
			return EINVAL;
		}

		cmds[cmd_count] = malloc((len + 1) * sizeof(char *));
		if (!cmds[cmd_count]) {
			perror("malloc");
			return errno;
		}

		for (int j = 0; j < len; j++)
			cmds[cmd_count][j] = argv[start + j];

		cmds[cmd_count][len] = NULL; // execvp requires NULL termination
		cmd_count++;

		i++; // skip ":"
	}

	int num_progs = cmd_count;
	int num_pipes = num_progs - 1;

	/* ---------------- Create pipes ---------------- */
	int (*pipes)[2] = NULL;
	if (num_pipes > 0) {
		pipes = malloc(num_pipes * sizeof(*pipes));
		if (!pipes) {
			perror("malloc");
			return errno;
		}

		for (int i = 0; i < num_pipes; i++) {
			if (pipe(pipes[i]) == -1) {
				perror("pipe");
				return errno;
			}
		}
	}

	/* ---------------- Fork processes ---------------- */
	pid_t *pids = malloc(num_progs * sizeof(pid_t));
	if (!pids) {
		perror("malloc");
		return errno;
	}

	for (int i = 0; i < num_progs; i++) {
		pids[i] = fork();

		if (pids[i] == -1) {
			perror("fork");
			return errno;
		}

		if (pids[i] == 0) {
			/* stdin from previous */
			if (i > 0) {
				if (dup2(pipes[i - 1][0], STDIN_FILENO) == -1) {
					perror("dup2");
					_exit(errno);
				}
			}

			/* stdout to next */
			if (i < num_progs - 1) {
				if (dup2(pipes[i][1], STDOUT_FILENO) == -1) {
					perror("dup2");
					_exit(errno);
				}
			}

			/* close all pipes */
			for (int j = 0; j < num_pipes; j++) {
				close(pipes[j][0]);
				close(pipes[j][1]);
			}

			execvp(cmds[i][0], cmds[i]);

			perror(cmds[i][0]);
			_exit(errno);
		}
	}

	/* ---------------- Parent cleanup ---------------- */
	for (int i = 0; i < num_pipes; i++) {
		close(pipes[i][0]);
		close(pipes[i][1]);
	}

	/* ---------------- Wait ---------------- */
	int final_status = 0;

	for (int i = 0; i < num_progs; i++) {
		int status;
		waitpid(pids[i], &status, 0);

		if (i == num_progs - 1 && WIFEXITED(status))
			final_status = WEXITSTATUS(status);
	}

	return final_status;
}