#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

int main(int argc, char *argv[])
{
	if (argc < 2)
	{
		errno = EINVAL;
		fprintf(stderr, "Usage: %s prog1 [prog2 ...]\n", argv[0]);
		return EINVAL;
	}

	int num_progs = argc - 1;
	int num_pipes = num_progs - 1;

	// allocate and create all pipes, each pipe is an array of 2 ints
	// pipes is an array of arrays
	int (*pipes)[2] = NULL;
	if (num_pipes > 0)
	{
		pipes = malloc((size_t)num_pipes * sizeof(*pipes)); // reserve space for num_pipes arrays of 2 ints
		if (!pipes)
		{
			int err = errno;
			perror("malloc");
			errno = err;
			return err;
		}
		for (int i = 0; i < num_pipes; i++) {
			if (pipe(pipes[i]) == -1) {
				int err = errno;
				perror("pipe");
				for (int j = 0; j < i; j++) {
					close(pipes[j][0]);
					close(pipes[j][1]);
				}
				free(pipes);
				errno = err;
				return err;
			}
		}
	}

	// process ids of children
	pid_t *pids = malloc((size_t)num_progs * sizeof(pid_t));
	if (!pids)
	{
		int err = errno;
		perror("malloc");
		for (int i = 0; i < num_pipes; i++)
		{
			close(pipes[i][0]);
			close(pipes[i][1]);
		}
		free(pipes);
		errno = err;
		return err;
	}

	for (int i = 0; i < num_progs; i++) {
		pids[i] = fork();
		if (pids[i] == -1)
		{
			int err = errno;
			perror("fork");
			for (int j = 0; j < num_pipes; j++)
			{
				close(pipes[j][0]);
				close(pipes[j][1]);
			}
			free(pipes);
			for (int j = 0; j < i; j++)
				waitpid(pids[j], NULL, 0);
			free(pids);
			errno = err;
			return err;
		}

		if (pids[i] == 0)
		{
			/* Child: redirect stdin from previous pipe */
			if (i > 0)
			{
				if (dup2(pipes[i - 1][0], STDIN_FILENO) == -1)
				{
					perror("dup2");
					exit(errno);
				}
			}
			/* Child: redirect stdout to next pipe */
			if (i < num_progs - 1)
			{
				if (dup2(pipes[i][1], STDOUT_FILENO) == -1)
				{
					perror("dup2");
					exit(errno);
				}
			}
			/* Close all pipe fds so nothing hangs */
			for (int j = 0; j < num_pipes; j++)
			{
				close(pipes[j][0]);
				close(pipes[j][1]);
			}
			free(pipes);
			free(pids);

			execlp(argv[i + 1], argv[i + 1], (char *)NULL);
			/* execlp only returns on error */

			int err = errno;
			perror(argv[i + 1]);
			errno = EINVAL;
			exit(EINVAL);
		}
	}

	/* Parent: close all pipe ends so children see EOF */
	for (int i = 0; i < num_pipes; i++) {
		close(pipes[i][0]);
		close(pipes[i][1]);
	}
	free(pipes);

	/* Wait for all children; no orphans */
	int final_status = 0;
	for (int i = 0; i < num_progs; i++) {
		int status;
		if (waitpid(pids[i], &status, 0) == -1)
		{
			int err = errno;
			perror("waitpid");
			free(pids);
			errno = err;
			return err;
		}
		if (i == num_progs - 1 && WIFEXITED(status))
			final_status = WEXITSTATUS(status);
	}

	free(pids);
	return final_status;
}
