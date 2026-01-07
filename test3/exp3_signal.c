#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>

int p1, p2; // 全局变量，方便信号处理函数访问

// 信号处理函数：父进程收到 Ctrl+C 后调用
void stop_handler(int signum) {
    printf("\n[Parent] Catching SIGINT (Ctrl+C). Sending signals to children...\n");
    // 发送 SIGUSR1 给子进程1
    kill(p1, SIGUSR1);
    // 发送 SIGUSR2 给子进程2
    kill(p2, SIGUSR2);
}

// 子进程1的结束处理
void child1_stop(int signum) {
    printf("Child process 1 is killed by parent!\n");
    exit(0);
}

// 子进程2的结束处理
void child2_stop(int signum) {
    printf("Child process 2 is killed by parent!\n");
    exit(0);
}

int main() {
    // 父进程先拦截 Ctrl+C
    signal(SIGINT, stop_handler);

    while((p1 = fork()) == -1);
    
    if (p1 == 0) {
        // --- 子进程 1 ---
        signal(SIGINT, SIG_IGN); // 忽略 Ctrl+C (只听父进程的命令)
        signal(SIGUSR1, child1_stop); // 注册处理函数
        while(1) {
            printf("Child 1 is alive...\n");
            sleep(1);
        }
    } else {
        while((p2 = fork()) == -1);
        
        if (p2 == 0) {
            // --- 子进程 2 ---
            signal(SIGINT, SIG_IGN); // 忽略 Ctrl+C
            signal(SIGUSR2, child2_stop); // 注册处理函数
            while(1) {
                printf("Child 2 is alive...\n");
                sleep(1);
            }
        } else {
            // --- 父进程 ---
            printf("Parent process is waiting for Ctrl+C...\n");
            
            // 等待子进程结束
            wait(NULL);
            wait(NULL);
            
            printf("Parent process is killed!\n");
            exit(0);
        }
    }
    return 0;
}