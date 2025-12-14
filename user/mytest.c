// user/mytest.c
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void cpu_intensive()
{
    printf("CPU-intensive process %d started\n", getpid());
    for(volatile int i = 0; i < 100000000; i++) {
        asm volatile("nop");
    }
    printf("CPU-intensive process %d finished\n", getpid());
}

void io_intensive()
{
    printf("I/O-intensive process %d started\n", getpid());
    for(int i = 0; i < 5; i++) {
        sleep(10);
        printf("I/O process %d: iteration %d\n", getpid(), i);
    }
    printf("I/O-intensive process %d finished\n", getpid());
}

int main(int argc, char *argv[])
{
    printf("Starting MLFQ test on single CPU...\n");
    
    int pid1 = fork();
    if(pid1 == 0) {
        cpu_intensive();
        exit(0);
    }
    
    int pid2 = fork();
    if(pid2 == 0) {
        io_intensive();
        exit(0);
    }
    
    wait(0);
    wait(0);
    
    printf("MLFQ test completed successfully!\n");
    exit(0);
}
