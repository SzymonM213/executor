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

#define MAX_INSTRUCTION_LENGTH 511
#define MAX_OUTPUT_LENGTH 1022
#define MAX_N_TASKS 4096
#define ENDING_MSG_SIZE 60

char msgQueue[ENDING_MSG_SIZE * MAX_N_TASKS];
int queueSize = 0;
bool isHandling = false;
pthread_mutex_t mutex;
sem_t ended_handling;

typedef struct task {
    char **argv;
    char out[MAX_OUTPUT_LENGTH];
    char err[MAX_OUTPUT_LENGTH];
    pthread_mutex_t outSemaphore;
    pthread_mutex_t errSemaphore;
    pid_t pid;
    pthread_t thread;
    int id;
} task;

task tasks[MAX_N_TASKS];

typedef struct read_data {
    char *buff;
    pthread_mutex_t *mutex_ptr;
    int fd;
} read_data;

task task_init(char **argv, int id) {
    task result;
    ASSERT_ZERO(pthread_mutex_init(&result.outSemaphore, NULL));
    ASSERT_ZERO(pthread_mutex_init(&result.errSemaphore, NULL));
    result.argv = argv;
    result.id = id;
    result.err[0] = '\0';
    result.out[0] = '\0';
    return result;
}

read_data read_data_init(char* buff, pthread_mutex_t *mutex_ptr, int fd) {
    read_data result;
    result.buff = buff;
    result.mutex_ptr = mutex_ptr;
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
        printf("%c", msgQueue[i]);
    }
}

void* read_output(void* data) {
    read_data* rd_data = data;
    char tmp[MAX_INSTRUCTION_LENGTH];
    FILE *rd_file = fdopen(rd_data->fd, "r");

    while (read_line(tmp, MAX_INSTRUCTION_LENGTH, rd_file)) {
        ASSERT_ZERO(pthread_mutex_lock(rd_data->mutex_ptr));
        strcpy(rd_data->buff, tmp);
        ASSERT_ZERO(pthread_mutex_unlock(rd_data->mutex_ptr));
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
        printf("Task %d started: pid %d.\n", myTask->id, pid);
        ASSERT_SYS_OK(close(outDsc[1]));
        ASSERT_SYS_OK(close(errDsc[1]));

        read_data rdOut = read_data_init(myTask->out, &myTask->outSemaphore, outDsc[0]);
        read_data rdErr = read_data_init(myTask->err, &myTask->errSemaphore, errDsc[0]);

        pthread_t output_handlers[2];
        ASSERT_SYS_OK(pthread_create(&output_handlers[0], NULL, read_output, &rdOut));
        ASSERT_SYS_OK(pthread_create(&output_handlers[1], NULL, read_output, &rdErr));

        ASSERT_SYS_OK(sem_post(&ended_handling));

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
            printf(msg);
        }
        ASSERT_ZERO(pthread_mutex_unlock(&mutex));
        free_split_string(myTask->argv - 1);
        pthread_join(output_handlers[0], NULL);
        pthread_join(output_handlers[1], NULL);
    }

    return 0;
}

int main(void) {
    ASSERT_ZERO(pthread_mutex_init(&mutex, NULL));
    ASSERT_SYS_OK(sem_init(&ended_handling, 0, 0));
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
            ASSERT_SYS_OK(sem_wait(&ended_handling));
        } else {
            if (!strcmp(args[0], "out")) {
                int taskNumber = atoi(args[1]);
                ASSERT_ZERO(pthread_mutex_lock(&tasks[taskNumber].outSemaphore));
                printf("Task %d stdout: '%s'.\n", taskNumber, tasks[taskNumber].out);
                ASSERT_ZERO(pthread_mutex_unlock(&tasks[taskNumber].outSemaphore));
            } else if (!strcmp(args[0], "err")) {
                int taskNumber = atoi(args[1]);
                pthread_mutex_lock(&tasks[taskNumber].errSemaphore);
                printf("Task %d stderr: '%s'.\n", taskNumber, tasks[taskNumber].err);
                pthread_mutex_unlock(&tasks[taskNumber].errSemaphore);
            } else if (!strcmp(args[0], "kill")) {
                int taskNumber = atoi(args[1]);
                kill(tasks[taskNumber].pid, SIGINT);
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
    }
    ASSERT_ZERO(pthread_mutex_destroy(&mutex));
    ASSERT_SYS_OK(sem_destroy(&ended_handling));
}