#include <pthread.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <signal.h>
#include <semaphore.h>

#include "utils.h"
#include "err.h"
#include "safe_printf.h"

#define MAX_INSTRUCTION_LENGTH 512
#define MAX_OUTPUT_LENGTH 1022
#define MAX_N_TASKS 4096
#define ENDING_MSG_SIZE 60

char msgQueue[ENDING_MSG_SIZE * MAX_N_TASKS];
int queueSize = 0;
bool isHandling = false;
pthread_mutex_t mutex;
sem_t endedHandling;

typedef struct task {
    char **argv;
    char out[MAX_OUTPUT_LENGTH]; // buffer to save last stdout line
    char err[MAX_OUTPUT_LENGTH]; // buffer to save last stderr line
    pthread_mutex_t outMtx;
    pthread_mutex_t errMtx;
    pid_t pid;
    int id;
    pthread_t thread;
} task;

task tasks[MAX_N_TASKS];

typedef struct read_data {
    char *buff;
    pthread_mutex_t *buffMtxPtr;
    int fd;
} read_data;

task task_init(char **argv, int id) {
    task result;
    ASSERT_ZERO(pthread_mutex_init(&result.outMtx, NULL));
    ASSERT_ZERO(pthread_mutex_init(&result.errMtx, NULL));
    result.argv = argv;
    result.id = id;
    result.err[0] = '\0';
    result.out[0] = '\0';
    return result;
}

read_data read_data_init(char* buff, pthread_mutex_t *buffMtxPtr, int fd) {
    read_data result;
    result.buff = buff;
    result.buffMtxPtr = buffMtxPtr;
    result.fd = fd;
    return result;
}

void push_queue(char* element) {
    for (int i = 0; i < strlen(element); i++) {
        msgQueue[queueSize++] = element[i];
    }
}

void printf_queue() {
    for (int i = 0; i < queueSize; i++) {
        safe_printf("%c", msgQueue[i]);
    }
}

void* read_output(void* data) {
    read_data* rd_data = data;
    char tmp[MAX_INSTRUCTION_LENGTH];
    FILE *rd_file = fdopen(rd_data->fd, "r");

    while (read_line(tmp, MAX_INSTRUCTION_LENGTH, rd_file)) {
        ASSERT_ZERO(pthread_mutex_lock(rd_data->buffMtxPtr));
        strcpy(rd_data->buff, tmp);
        ASSERT_ZERO(pthread_mutex_unlock(rd_data->buffMtxPtr));
    }
    fclose(rd_file);

    return 0;
}

void* start_task(void* data) {
    task* myTask = data;
    int outDsc[2];
    int errDsc[2];
    ASSERT_SYS_OK(pipe(outDsc));
    ASSERT_SYS_OK(pipe(errDsc));
    set_close_on_exec(outDsc[0], true);
    set_close_on_exec(outDsc[1], true);
    set_close_on_exec(errDsc[0], true);
    set_close_on_exec(errDsc[1], true);
    pid_t pid;
    ASSERT_SYS_OK(pid = fork());
    if (!pid) {
        ASSERT_SYS_OK(close(outDsc[0]));
        ASSERT_SYS_OK(close(errDsc[0]));

        ASSERT_SYS_OK(dup2(outDsc[1], STDOUT_FILENO));
        ASSERT_SYS_OK(dup2(errDsc[1], STDERR_FILENO));

        ASSERT_SYS_OK(close(outDsc[1]));
        ASSERT_SYS_OK(close(errDsc[1]));
        
        ASSERT_SYS_OK(execvp(myTask->argv[0], myTask->argv));
    } else {
        myTask->pid = pid;
        safe_printf("Task %d started: pid %d.\n", myTask->id, pid);
        ASSERT_SYS_OK(close(outDsc[1]));
        ASSERT_SYS_OK(close(errDsc[1]));

        read_data rdOut = read_data_init(myTask->out, &myTask->outMtx, outDsc[0]);
        read_data rdErr = read_data_init(myTask->err, &myTask->errMtx, errDsc[0]);

        pthread_t outReaders[2];
        ASSERT_SYS_OK(pthread_create(&outReaders[0], NULL, read_output, &rdOut));
        ASSERT_SYS_OK(pthread_create(&outReaders[1], NULL, read_output, &rdErr));

        ASSERT_SYS_OK(sem_post(&endedHandling));

        int status;
        char msg[ENDING_MSG_SIZE];
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            sprintf(msg, "Task %d ended: status %d.\n", myTask->id, WEXITSTATUS(status));
        } else {
            sprintf(msg, "Task %d ended: signalled.\n", myTask->id);
        }
        ASSERT_ZERO(pthread_mutex_lock(&mutex)); 
        if (isHandling) {
            push_queue(msg);
        } else {
            safe_printf(msg);
        }
        ASSERT_ZERO(pthread_mutex_unlock(&mutex));
        free_split_string(myTask->argv - 1);
        pthread_join(outReaders[0], NULL);
        pthread_join(outReaders[1], NULL);
    }

    return 0;
}

int main(void) {
    ASSERT_ZERO(pthread_mutex_init(&mutex, NULL));
    ASSERT_SYS_OK(sem_init(&endedHandling, 0, 0));
    char line[MAX_INSTRUCTION_LENGTH];
    int tasksCount = 0;
    while (read_line(line, MAX_INSTRUCTION_LENGTH, stdin) && line[0] != 'q') {
        ASSERT_ZERO(pthread_mutex_lock(&mutex));
        isHandling = true;
        ASSERT_ZERO(pthread_mutex_unlock(&mutex));
        char **args = split_string(line);
        if (!strcmp(args[0], "run")) {
            tasks[tasksCount] = task_init(&(args[1]), tasksCount);
            ASSERT_SYS_OK(pthread_create(&(tasks[tasksCount].thread), NULL, start_task, &(tasks[tasksCount])));
            tasksCount++;
            ASSERT_SYS_OK(sem_wait(&endedHandling));
        } else {
            if (!strcmp(args[0], "out")) {
                int taskNum = atoi(args[1]);
                ASSERT_ZERO(pthread_mutex_lock(&tasks[taskNum].outMtx));
                safe_printf("Task %d stdout: '%s'.\n", taskNum, tasks[taskNum].out);
                ASSERT_ZERO(pthread_mutex_unlock(&tasks[taskNum].outMtx));
            } else if (!strcmp(args[0], "err")) {
                int taskNum = atoi(args[1]);
                pthread_mutex_lock(&tasks[taskNum].errMtx);
                safe_printf("Task %d stderr: '%s'.\n", taskNum, tasks[taskNum].err);
                pthread_mutex_unlock(&tasks[taskNum].errMtx);
            } else if (!strcmp(args[0], "kill")) {
                int taskNum = atoi(args[1]);
                kill(tasks[taskNum].pid, SIGINT);
            } else if (!strcmp(args[0], "sleep")) {
                int milliseconds = atoi(args[1]);
                usleep(1000 * milliseconds);
            }
            free_split_string(args);
        }
        ASSERT_ZERO(pthread_mutex_lock(&mutex));
        isHandling = false;
        if (queueSize > 0) printf_queue(msgQueue);
        queueSize = 0;
        ASSERT_ZERO(pthread_mutex_unlock(&mutex));
    }

    for (int i = 0; i < tasksCount; i++) {
        kill(tasks[i].pid, SIGKILL);
    }

    for (int i = 0; i < tasksCount; i++) {
        pthread_join(tasks[i].thread, NULL);
        ASSERT_ZERO(pthread_mutex_destroy(&tasks[i].errMtx));
        ASSERT_ZERO(pthread_mutex_destroy(&tasks[i].outMtx));
    }
    ASSERT_ZERO(pthread_mutex_destroy(&mutex));
    ASSERT_SYS_OK(sem_destroy(&endedHandling));
}
