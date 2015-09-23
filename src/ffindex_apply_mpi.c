/* 
 * FFindex
 * written by Andy Hauser <hauser@genzentrum.lmu.de>,
 * and Milot Mirdita <milot@mirdita.de>
 * Please add your name here if you distribute modified versions.
 *
 * FFindex is provided under the Create Commons license "Attribution-ShareAlike
 * 3.0", which basically captures the spirit of the Gnu Public License (GPL).
 *
 * See:
 * http://creativecommons.org/licenses/by-sa/3.0/
 *
 * ffindex_apply
 * apply a program to each FFindex entry
 */

#define _GNU_SOURCE 1
#define _LARGEFILE64_SOURCE 1
#define _FILE_OFFSET_BITS 64

#include <limits.h> // PIPE_BUF
#include <stdlib.h> // EXIT_*, system, malloc, free
#include <unistd.h> // pipe, fork, close, dup2, execvp, write, read, opt*

#include <sys/mman.h> // munmap
#include <sys/wait.h> // waitpid
#include <fcntl.h>    // fcntl, F_*, O_*
#include <signal.h>   // sigaction, sigemptyset

#include <sys/queue.h> // SLIST_*

#include "ffindex.h"
#include "ffutil.h"
#include "mpq/mpq.h"

// Normalize BSD and GNU getopt
#include "gnu_getopt/gnu_getopt.h"

char read_buffer[400 * 1024 * 1024];

int
ffindex_apply_by_entry(char *data,
                       ffindex_index_t * index, ffindex_entry_t * entry,
                       char *program_name, char **program_argv,
                       FILE * data_file_out, FILE * index_file_out,
                       size_t * offset)
{
	int ret = 0;
	int capture_stdout = (data_file_out != NULL);

	int pipefd_stdin[2];
	ret = pipe(pipefd_stdin);
	if (ret != 0)
	{
		fprintf(stderr, "ERROR in pipe stdin!\n");
		perror(entry->name);
		return errno;
	}

	int pipefd_stdout[2];
	if (capture_stdout)
	{
		ret = pipe(pipefd_stdout);
		if (ret != 0)
		{
			fprintf(stderr, "ERROR in pipe stdout!\n");
			perror(entry->name);
			return errno;
		}
	}

	// Flush so child doesn't copy and also flushes, leading to duplicate
	// output
	fflush(data_file_out);
	fflush(index_file_out);

	pid_t child_pid = fork();

	if (child_pid == 0)			// child
	{
		close(pipefd_stdin[1]);
		if (capture_stdout)
		{
			fclose(data_file_out);
			fclose(index_file_out);
			close(pipefd_stdout[0]);
		}
		// Make pipe from parent our new stdin
		int newfd_in = dup2(pipefd_stdin[0], fileno(stdin));
		if (newfd_in < 0)
		{
			fprintf(stderr, "ERROR in dup2 in %d %d\n", pipefd_stdin[0], newfd_in);
			perror(entry->name);
		}
		close(pipefd_stdin[0]);

		if (capture_stdout)
		{
			int newfd_out = dup2(pipefd_stdout[1], fileno(stdout));
			if (newfd_out < 0)
			{
				fprintf(stderr, "ERROR in dup2 out %d %d\n", pipefd_stdout[1], newfd_out);
				perror(entry->name);
			}
			close(pipefd_stdout[1]);
		}
		// exec program with the pipe as stdin
		ret = execvp(program_name, program_argv);

		// never reached on success of execvp
		if (ret == -1)
		{
			perror(program_name);
			return errno;
		}
	}
	else if (child_pid > 0)		// parent
	{
		// parent writes to and possible reads from child

		int flags = 0;

		// Read end is for child only
		close(pipefd_stdin[0]);

		if (capture_stdout)
		{
			close(pipefd_stdout[1]);

			flags = fcntl(pipefd_stdout[0], F_GETFL, 0);
			fcntl(pipefd_stdout[0], F_SETFL, flags | O_NONBLOCK);
		}

		char *filedata = ffindex_get_data_by_entry(data, entry);

		// Write file data to child's stdin.
		ssize_t written = 0;
		size_t to_write = entry->length - 1;	// Don't write ffindex
		// trailing '\0'
		char *b = read_buffer;
		while (written < to_write)
		{
			size_t rest = to_write - written;
			int batch_size = PIPE_BUF;
			if (rest < PIPE_BUF)
			{
				batch_size = rest;
			}

			ssize_t w = write(pipefd_stdin[1], filedata + written,
							  batch_size);
			if (w < 0 && errno != EPIPE)
			{
				fprintf(stderr, "ERROR in child!\n");
				perror(entry->name);
				break;
			}
			else
			{
				written += w;
			}

			if (capture_stdout)
			{
				// To avoid blocking try to read some data
				ssize_t r = read(pipefd_stdout[0], b, PIPE_BUF);
				if (r > 0)
				{
					b += r;
				}
			}
		}
		close(pipefd_stdin[1]);	// child gets EOF

		if (capture_stdout)
		{
			// Read rest
			fcntl(pipefd_stdout[0], F_SETFL, flags);	// Remove O_NONBLOCK
			ssize_t r;
			while ((r = read(pipefd_stdout[0], b, PIPE_BUF)) > 0)
			{
				b += r;
			}
			close(pipefd_stdout[0]);

			ffindex_insert_memory(data_file_out, index_file_out,
                                  offset, read_buffer, b - read_buffer, entry->name);

			// make sure the data is actually written so ffindex_build sees the output
			fflush(data_file_out);
			fflush(index_file_out);
		}

		int status;
		waitpid(child_pid, &status, 0);
		if (WIFEXITED(status))
		{
			fprintf(stdout, "%s\t%zd\t%zd\t%i\n",
                    entry->name, entry->offset, entry->length, WEXITSTATUS(status));
		}
	}
    // fork failed
	else
	{
		fprintf(stderr, "ERROR in fork()\n");
		perror(entry->name);
		return errno;
	}
	return EXIT_SUCCESS;
}

typedef struct worker_splits_s worker_splits_t;

struct worker_splits_s {
    int start;
    int end;
    int status;
    SLIST_ENTRY(worker_splits_s) entries;
};

SLIST_HEAD(worker_splits, worker_splits_s) worker_splits_head;

void ffindex_worker_merge_splits(char* data_filename, char* index_filename, int worker_rank, int remove_temporary)
{
    if (!data_filename)
        return;

    if (!index_filename)
        return;

    char merge_command[FILENAME_MAX * 5];
    char tmp_filename[FILENAME_MAX];

    worker_splits_t* entry;
    SLIST_FOREACH(entry, &worker_splits_head, entries) {
        snprintf(merge_command, FILENAME_MAX,
                 "ffindex_build -as -d %s.%d.%d.%d -i %s.%d.%d.%d %s.%d %s.%d",
                 data_filename, worker_rank, entry->start, entry->end,
                 index_filename, worker_rank, entry->start, entry->end,
                 data_filename, worker_rank,
                 index_filename, worker_rank);

        int exit_status = system(merge_command);
        if (exit_status == 0 && remove_temporary)
        {
            snprintf(tmp_filename, FILENAME_MAX, "%s.%d.%d.%d",
                     index_filename, worker_rank, entry->start, entry->end);
            remove(tmp_filename);
            
            snprintf(tmp_filename, FILENAME_MAX, "%s.%d.%d.%d",
                     data_filename, worker_rank, entry->start, entry->end);
            remove(tmp_filename);
        }
    }
}

void ffindex_merge_splits(char* data_filename, char* index_filename, int splits, int remove_temporary)
{
    if (!data_filename)
        return;

    if (!index_filename)
        return;

    char merge_command[FILENAME_MAX * 5];
    char tmp_filename[FILENAME_MAX];

    for (int i = 1; i < splits; i++)
    {
        snprintf(merge_command, FILENAME_MAX,
                 "ffindex_build -as -d %s.%d -i %s.%d %s %s",
                 data_filename, i, index_filename, i, data_filename, index_filename);

        int ret = system(merge_command);
        if (ret == 0 && remove_temporary)
        {
            snprintf(tmp_filename, FILENAME_MAX, "%s.%d", index_filename, i);
            remove(tmp_filename);
            
            snprintf(tmp_filename, FILENAME_MAX, "%s.%d", data_filename, i);
            remove(tmp_filename);
        }
    }
}

typedef struct ffindex_apply_mpi_data_s ffindex_apply_mpi_data_t;
struct ffindex_apply_mpi_data_s {
    ffindex_index_t* index;
    void	*  data;
    char*  data_filename_out;
    char*  index_filename_out;
    char*  program_name;
    char** program_argv;
};

int ffindex_apply_worker_payload (const size_t start, const size_t end, const void* data) {
    ffindex_apply_mpi_data_t* apply_data = (ffindex_apply_mpi_data_t*) data;

    FILE *data_file_out = NULL;
    if (apply_data->data_filename_out != NULL)
    {
        char data_filename_out_rank[FILENAME_MAX];
        snprintf(data_filename_out_rank, FILENAME_MAX, "%s.%d.%zu.%zu",
                 apply_data->data_filename_out, MPQ_rank, start, end);

        data_file_out = fopen(data_filename_out_rank, "w+");
        if (data_file_out == NULL)
        {
            fferror_print(__FILE__, __LINE__, "ffindex_apply_worker_payload", apply_data->data_filename_out);
            return EXIT_FAILURE;
        }
    }

    FILE *index_file_out = NULL;
    if (apply_data->index_filename_out != NULL)
    {
        char index_filename_out_rank[FILENAME_MAX];
        snprintf(index_filename_out_rank, FILENAME_MAX, "%s.%d.%zu.%zu",
                 apply_data->index_filename_out, MPQ_rank, start, end);

        index_file_out = fopen(index_filename_out_rank, "w+");
        if (index_file_out == NULL)
        {
            fferror_print(__FILE__, __LINE__, "ffindex_apply_worker_payload", apply_data->index_filename_out);
            return EXIT_FAILURE;
        }
    }

    int exit_status = EXIT_SUCCESS;
    size_t offset = 0;
    for (size_t i = start; i < end; i++)
    {
        ffindex_entry_t *entry = ffindex_get_entry_by_index(apply_data->index, i);
        if (entry == NULL)
        {
            perror(entry->name);
            exit_status = errno;
            break;
        }

        int error = ffindex_apply_by_entry(apply_data->data, apply_data->index, entry,
                                           apply_data->program_name,
                                           apply_data->program_argv, data_file_out,
                                           index_file_out, &offset);
        if (error != 0)
        {
            perror(entry->name);
            exit_status = errno;
            break;
        }
    }

    if (index_file_out) {
        fclose(index_file_out);
    }
    if (data_file_out) {
        fclose(data_file_out);
    }

    worker_splits_t* entry = malloc(sizeof(worker_splits_t));
    entry->start = start;
    entry->end = end;
    entry->status = exit_status;
    SLIST_INSERT_HEAD(&worker_splits_head, entry, entries);

    return exit_status;
}

void usage()
{
    fprintf(stderr,
            "USAGE: ffindex_apply_mpi -d DATA_FILENAME_OUT -i INDEX_FILENAME_OUT [-p PARTS] DATA_FILENAME INDEX_FILENAME -- PROGRAM [PROGRAM_ARGS]*\n"
            "\nDesigned and implemented by Andy Hauser <hauser@genzentrum.lmu.de>\n"
            "\nand Milot Mirdita <milot@mirdita.de>.\n");
}

void ignore_signal(int signal)
{
    struct sigaction handler;
    handler.sa_handler = SIG_IGN;
    sigemptyset(&handler.sa_mask);
    handler.sa_flags = 0;
    sigaction(signal, &handler, NULL);
}

int main(int argn, char** argv)
{
	int exit_status = EXIT_SUCCESS;

    MPQ_Init(argn, argv);

    int parts = 10;
	char *data_filename_out = NULL, *index_filename_out = NULL;

	int opt;
	while ((opt = gnu_getopt(argn, argv, "d:i:p::")) != -1)
	{
		switch (opt)
		{
		case 'd':
			data_filename_out = optarg;
			break;
		case 'i':
			index_filename_out = optarg;
			break;
        case 'p':
            parts = optarg;
            break;
		}
	}

	if (argn - optind < 3)
	{
		fprintf(stderr, "Not enough arguments %d.\n", optind - argn);
		usage();
		exit_status = EXIT_FAILURE;
		goto cleanup;
	}

	char *data_filename = argv[optind++];
	FILE *data_file = fopen(data_filename, "r");
	if (data_file == NULL)
	{
		fferror_print(__FILE__, __LINE__, argv[0], data_filename);
		exit_status = EXIT_FAILURE;
		goto cleanup;
	}

	char *index_filename = argv[optind++];
	FILE *index_file = fopen(index_filename, "r");
	if (index_file == NULL)
	{
		fferror_print(__FILE__, __LINE__, argv[0], index_filename);
		exit_status = EXIT_FAILURE;
		goto cleanup_1;
	}

	char *program_name = argv[optind];
	char **program_argv = argv + optind;

	size_t data_size;
	char *data = ffindex_mmap_data(data_file, &data_size);

	ffindex_index_t *index = ffindex_index_parse(index_file, 0);
	if (index == NULL)
	{
		fferror_print(__FILE__, __LINE__, "ffindex_index_parse", index_filename);
		exit_status = EXIT_FAILURE;
		goto cleanup_2;
	}

	// Ignore SIGPIPE
	ignore_signal(SIGPIPE);

    if (MPQ_rank != MPQ_MASTER) {
        SLIST_INIT(&worker_splits_head);
    }

    ffindex_apply_mpi_data_t* payload_data = malloc(sizeof(ffindex_apply_mpi_data_t));
    payload_data->index = index;
    payload_data->data = data;
    payload_data->program_name = program_name;
    payload_data->program_argv = program_argv;
    payload_data->data_filename_out = data_filename_out;
    payload_data->index_filename_out = index_filename_out;

    MPQ_Payload = ffindex_apply_worker_payload;
    MPQ_Payload_Environment = (void*) payload_data;

    // ceil div
    int split_size = ((index->n_entries - 1) / ((MPQ_size - 1) * parts)) + 1;
    MPQ_Main(index->n_entries, (split_size > 1 ? split_size : 1));
    
    free(payload_data);

    if (MPQ_rank != MPQ_MASTER) {
        ffindex_worker_merge_splits(data_filename_out, index_filename_out, MPQ_rank, 1);

        while (!SLIST_EMPTY(&worker_splits_head)) {
            worker_splits_t* entry = SLIST_FIRST(&worker_splits_head);
            SLIST_REMOVE_HEAD(&worker_splits_head, entries);
            free(entry);
        }
    }

    munmap(index->index_data, index->index_data_size);
    free(index);

  cleanup_2:
	munmap(data, data_size);
	fclose(index_file);

  cleanup_1:
	fclose(data_file);

  cleanup:
    MPQ_Finalize();

	if (exit_status == EXIT_SUCCESS && MPQ_rank == MPQ_MASTER)
	{
		ffindex_merge_splits(data_filename_out, index_filename_out, MPQ_size, 1);
	}
	return exit_status;
}
