#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

/*
pipe: execute a sequence of programs connected by pipes, e.g.

    $ ./pipe ls wc

This should execute `ls` and `wc` with a pipe between them, so the output of `ls` is the input of `wc`. The parent process should wait for all children to finish before exiting. The exit status of the parent should be the exit status of the last program in the pipeline, or 0 if all programs exit successfully.
*/

int main(int argc, char *argv[])
{
	if (argc < 2)
	{
		errno = EINVAL;
		perror("pipe");
		return EINVAL;
	}

	int num_progs = argc - 1;
	int num_pipes = num_progs - 1;

	/* Allocate and create all pipes upfront */
	int (*pipes)[2] = NULL;
	if (num_pipes > 0)
	{
		pipes = malloc((size_t)num_pipes * sizeof(*pipes));
		if (!pipes)
		{
			perror("malloc");
			return errno;
		}
		for (int i = 0; i < num_pipes; i++)
		{
			if (pipe(pipes[i]) == -1)
			{
				int err = errno;
				perror("pipe");
				for (int j = 0; j < i; j++)
				{
					close(pipes[j][0]);
					close(pipes[j][1]);
				}
				free(pipes);
				return err;
			}
		}
	}

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
		return err;
	}

	for (int i = 0; i < num_progs; i++)
	{
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
			exit(err);
		}
	}

	/* Parent: close all pipe ends so children see EOF */
	for (int i = 0; i < num_pipes; i++)
	{
		close(pipes[i][0]);
		close(pipes[i][1]);
	}
	free(pipes);

	/* Wait for all children; no orphans */
	int final_status = 0;
	for (int i = 0; i < num_progs; i++)
	{
		int status;
		if (waitpid(pids[i], &status, 0) == -1)
		{
			int err = errno;
			perror("waitpid");
			free(pids);
			return err;
		}
		if (WIFEXITED(status) && WEXITSTATUS(status) != 0 && final_status == 0)
			final_status = WEXITSTATUS(status);
	}

	free(pids);
	return final_status;
}
