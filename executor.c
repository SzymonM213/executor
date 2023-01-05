#include <pthread.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <signal.h>

#include "utils.h"
#include "err.h"

#define MAX_INSTRUCTION_LENGTH 511
#define MAX_OUTPUT_LENGTH 1022
#define MAX_N_TASKS 4096
#define ENDING_MSG_SIZE 50
#define DEBUG false

char msgQueue[ENDING_MSG_SIZE * MAX_N_TASKS];
int queueSize = 0;
bool isHandling = false;
// pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex;

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
    pthread_mutex_t mutex;
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

read_data read_data_init(char* buff, pthread_mutex_t mutex, int fd) {
    read_data result;
    result.buff = buff;
    result.mutex = mutex;
    result.fd = fd;
    return result;
}

void push_queue(char* element) {
    for (int i = 0; i < strlen(element); i++) {
        msgQueue[queueSize++] = element[i];
    }
}

void* read_output(void* data) {
    read_data* rd_data = data;
    char tmp[MAX_INSTRUCTION_LENGTH];
    FILE *rd_file = fdopen(rd_data->fd, "r");

    while (read_line(tmp, MAX_INSTRUCTION_LENGTH, rd_file)) {
    // while (fgets(tmp, MAX_INSTRUCTION_LENGTH, rd_file) != NULL) {
        ASSERT_SYS_OK(pthread_mutex_lock(&rd_data->mutex));
        tmp[strlen(tmp) - 1] = '\0';
        strcpy(rd_data->buff, tmp);
        if(DEBUG) printf("%s\n", rd_data->buff);
        ASSERT_SYS_OK(pthread_mutex_unlock(&rd_data->mutex));
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
        // ta szmata co puszcza program tylko
        ASSERT_SYS_OK(close(outDsc[0]));
        ASSERT_SYS_OK(close(errDsc[0]));

        printf("Task %d started: pid %d.\n", myTask->id, getpid());

        ASSERT_SYS_OK(dup2(outDsc[1], STDOUT_FILENO));
        ASSERT_SYS_OK(dup2(errDsc[1], STDERR_FILENO));

        ASSERT_SYS_OK(close(outDsc[1]));
        ASSERT_SYS_OK(close(errDsc[1]));
        
        // teraz wypisuje output do ojca pewniaczek
    
        // printf("%d\n", strcmp("./chuj\0", myTask->argv[0]));
        // printf("%c %d\n", myTask->argv[0][6], myTask->argv[0][6]);
        ASSERT_SYS_OK(execvp(myTask->argv[0], myTask->argv));
    } else {
        myTask->pid = pid;
        ASSERT_SYS_OK(close(outDsc[1]));
        ASSERT_SYS_OK(close(errDsc[1]));

        read_data rdOut = read_data_init(myTask->out, myTask->outSemaphore, outDsc[0]);
        read_data rdErr = read_data_init(myTask->err, myTask->errSemaphore, errDsc[0]);

        pthread_t output_handlers[2];
        ASSERT_SYS_OK(pthread_create(&output_handlers[0], NULL, read_output, &rdOut));
        ASSERT_SYS_OK(pthread_create(&output_handlers[1], NULL, read_output, &rdErr));

        int status;
        char msg[ENDING_MSG_SIZE];
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            sprintf(msg, "Task %d ended: status %d.\n", myTask->id, status);
        } else {
            sprintf(msg, "Task %d ended: signalled.\n", myTask->id);
        }
        int chuj;
        // pthread_mutex_getprioceiling(&mutex, &chuj);
        // printf("mutex: %d\n", pthread_mutex_trylock(&mutex));
        // printf("kto czeka: %d\n", chuj);
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
    char line[MAX_INSTRUCTION_LENGTH];
    int tasksCount = 0;
    while (read_line(line, MAX_INSTRUCTION_LENGTH, stdin) && line[0] != 'q') {
        line[strlen(line) - 1] = '\0';
        // char cep[2] = {10, '\0'};
        // strtok(line, cep);
        ASSERT_ZERO(pthread_mutex_lock(&mutex));
        isHandling = true;
        ASSERT_ZERO(pthread_mutex_unlock(&mutex));
        if (line[0] == 'r') {
            
            // char *argv[MAX_INSTRUCTION_LENGTH];
            // split_string(line, argv);
            char **argv = split_string2(line);

            // char* arfv[MAX_INSTRUCTION_LENGTH];
            // split_string2(line, argv);

            tasks[tasksCount] = task_init(&(argv[1]), tasksCount);
            // printf("%c %d\n", argv[2][0], argv[2][0]);
            ASSERT_SYS_OK(pthread_create(&(tasks[tasksCount].thread), NULL, start_task, &(tasks[tasksCount])));
            tasksCount++;
        }
        else if (line[0] == 'o') {
            char *split_line[MAX_INSTRUCTION_LENGTH];
            split_string(line, split_line);
            int task_number = split_line[1][0] - '0';
            pthread_mutex_lock(&tasks[task_number].outSemaphore);
            printf("Task %d stdout: '%s'.\n", task_number, tasks[task_number].out);
            pthread_mutex_unlock(&tasks[task_number].outSemaphore);
        }
        else if (line[0] == 'e') {
            char *split_line[MAX_INSTRUCTION_LENGTH]; 
            split_string(line, split_line);
            int task_number = split_line[1][0] - '0';
            pthread_mutex_lock(&tasks[task_number].errSemaphore);
            // printf("%c", tasks[task_number].err[0]);
            printf("Task %d stderr: '%s'.\n", task_number, tasks[task_number].err);
            pthread_mutex_unlock(&tasks[task_number].errSemaphore);
        }
        else if (line[0] == 'k') {
            char *split_line[MAX_INSTRUCTION_LENGTH]; 
            split_string(line, split_line);
            int task_number = split_line[1][0] - '0';
            printf("%d\n", tasks[task_number].pid);
            kill(tasks[task_number].pid, SIGINT);
        }
        else if (line[0] == 's') {
            char **argv = split_string2(line);
            int length = atoi(argv[1]);
            usleep(10 * length);
        }
        ASSERT_ZERO(pthread_mutex_lock(&mutex));
        isHandling = false;
        if(queueSize > 0) printf(msgQueue);
        queueSize = 0;
        ASSERT_ZERO(pthread_mutex_unlock(&mutex));
    }

    for (int i = 0; i < tasksCount; i++) {
        pthread_join(tasks[i].thread, NULL);
    }
    ASSERT_ZERO(pthread_mutex_destroy(&mutex));
}