#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fcntl.h"

#define NUM_CPU_INTENSIVE 3
#define NUM_IO_INTENSIVE 3
#define NUM_FORK_BOMB 3  // 减少递归深度，避免警告
#define TEST_DURATION 100  // 测试运行时间（ticks）

// CPU密集型进程（模拟长时间计算）
void cpu_intensive(int id) {
    int start = uptime();
    printf("CPU密集型进程 %d 开始运行\n", id);
    
    int count = 0;
    for (int i = 0; i < 10000000; i++) {
        count += i * i;
        if (i % 1000000 == 0) {
            printf("进程 %d: 已完成 %.1f%% 计算\n", id, (float)i / 10000000.0 * 100);
        }
    }
    
    int end = uptime();
    printf("CPU密集型进程 %d 完成，结果: %d (耗时: %d ticks)\n", id, count, end - start);
    exit(0);
}

// I/O密集型进程（模拟频繁I/O操作）
void io_intensive(int id) {
    int start = uptime();
    printf("I/O密集型进程 %d 开始运行\n", id);
    
    for (int i = 0; i < 10; i++) {
        printf("进程 %d: 开始I/O操作 %d\n", id, i);
        
        // 模拟I/O等待
        sleep(5);
        
        printf("进程 %d: 完成I/O操作 %d\n", id, i);
    }
    
    int end = uptime();
    printf("I/O密集型进程 %d 完成 (耗时: %d ticks)\n", id, end - start);
    exit(0);
}

// 混合型进程（交替计算和I/O）
void mixed_process(int id) {
    int start = uptime();
    printf("混合型进程 %d 开始运行\n", id);
    
    for (int round = 0; round < 5; round++) {
        printf("进程 %d: 第 %d 轮 - 计算阶段\n", id, round);
        
        // 短时间计算
        int sum = 0;
        for (int i = 0; i < 1000000; i++) {
            sum += i;
        }
        
        printf("进程 %d: 第 %d 轮 - I/O阶段\n", id, round);
        
        // I/O操作
        sleep(3);
    }
    
    int end = uptime();
    printf("混合型进程 %d 完成 (耗时: %d ticks)\n", id, end - start);
    exit(0);
}

// 创建进程炸弹（测试调度器对大量进程的处理）- 简化版本
void fork_bomb_simple() {
    int start = uptime();
    printf("进程炸弹测试 - 创建多个独立进程\n");
    
    // 创建多个独立进程，而不是递归创建
    for (int i = 0; i < 8; i++) {
        int pid = fork();
        if (pid == 0) {
            // 子进程
            int child_id = i;
            printf("进程炸弹子进程 %d 开始\n", child_id);
            
            // 每个子进程再创建一些孙子进程
            for (int j = 0; j < 2; j++) {
                int grandchild = fork();
                if (grandchild == 0) {
                    printf("  孙子进程 %d-%d 运行\n", child_id, j);
                    for (int k = 0; k < 3; k++) {
                        printf("  孙子进程 %d-%d: 第 %d 次运行\n", child_id, j, k);
                        sleep(1);
                    }
                    printf("  孙子进程 %d-%d 完成\n", child_id, j);
                    exit(0);
                }
            }
            
            // 父进程等待孙子进程
            for (int j = 0; j < 2; j++) {
                wait(0);
            }
            
            printf("进程炸弹子进程 %d 完成\n", child_id);
            exit(0);
        }
    }
    
    // 等待所有子进程
    for (int i = 0; i < 8; i++) {
        wait(0);
    }
    
    int end = uptime();
    printf("进程炸弹测试完成 (耗时: %d ticks)\n", end - start);
}

// 测试优先级反转情况
void priority_inversion_test() {
    int start = uptime();
    printf("\n=== 测试优先级反转 ===\n");
    
    // 创建高优先级进程（I/O密集型）
    int high_prio = fork();
    if (high_prio == 0) {
        printf("高优先级进程开始\n");
        for (int i = 0; i < 3; i++) {
            printf("高优先级进程执行第 %d 次I/O\n", i);
            sleep(2);
        }
        printf("高优先级进程完成\n");
        exit(0);
    }
    
    sleep(1);  // 让高优先级进程先运行
    
    // 创建低优先级进程（CPU密集型）
    int low_prio = fork();
    if (low_prio == 0) {
        printf("低优先级进程开始（应被抢占）\n");
        for (int i = 0; i < 10000000; i++) {
            // 长时间计算
            volatile int x = i * i;
            (void)x;
            if (i % 2000000 == 0) {
                printf("低优先级进程进度: %.1f%%\n", (float)i / 10000000.0 * 100);
            }
        }
        printf("低优先级进程完成\n");
        exit(0);
    }
    
    wait(0);
    wait(0);
    
    int end = uptime();
    printf("优先级反转测试完成 (耗时: %d ticks)\n", end - start);
}

// 测试老化机制（防止饥饿）
void aging_test() {
    int start = uptime();
    printf("\n=== 测试老化机制 ===\n");
    
    // 创建大量低优先级进程
    for (int i = 0; i < 8; i++) {
        int pid = fork();
        if (pid == 0) {
            // 子进程：低优先级，长时间运行
            printf("低优先级进程 %d 开始（应在老化后提升优先级）\n", i);
            
            for (int j = 0; j < 50; j++) {
                sleep(10);  // 长时间睡眠，模拟长时间运行
                printf("低优先级进程 %d 仍存活，ticks: %d\n", i, uptime());
            }
            
            printf("低优先级进程 %d 完成\n", i);
            exit(0);
        }
    }
    
    // 创建高优先级短进程
    for (int i = 0; i < 3; i++) {
        int pid = fork();
        if (pid == 0) {
            printf("高优先级短进程 %d 开始并快速结束\n", i);
            sleep(1);
            printf("高优先级短进程 %d 完成\n", i);
            exit(0);
        }
    }
    
    // 等待所有进程
    for (int i = 0; i < 11; i++) {
        wait(0);
    }
    
    int end = uptime();
    printf("老化机制测试完成 (耗时: %d ticks)\n", end - start);
}

// 测试进程退出和直接切换
void exit_direct_switch_test() {
    int start = uptime();
    printf("\n=== 测试进程退出直接切换 ===\n");
    
    // 创建5个子进程，直接等待，不需要存储
    for (int i = 0; i < 5; i++) {
        int pid = fork();
        if (pid == 0) {
            // 子进程：运行一段时间后退出
            printf("子进程 %d (PID: %d) 开始运行\n", i, getpid());
            
            // 每个进程运行不同时间
            for (int j = 0; j < i + 1; j++) {
                sleep(2);
                printf("子进程 %d: 运行中...\n", i);
            }
            
            printf("子进程 %d 退出\n", i);
            exit(i);  // 返回不同的退出状态
        }
    }
    
    // 父进程等待子进程并收集退出状态
    printf("父进程等待子进程退出...\n");
    for (int i = 0; i < 5; i++) {
        int status;
        int pid = wait(&status);
        printf("子进程 (PID: %d) 退出，状态: %d\n", pid, status);
    }
    
    int end = uptime();
    printf("进程退出直接切换测试完成 (耗时: %d ticks)\n", end - start);
}

// 测试主动让出CPU
void cpu_yield_test() {
    int start = uptime();
    printf("\n=== 测试主动让出CPU ===\n");
    
    int pid = fork();
    if (pid == 0) {
        // 子进程：频繁短暂sleep让出CPU
        printf("子进程开始频繁短暂sleep让出CPU\n");
        for (int i = 0; i < 10; i++) {
            printf("子进程第 %d 次执行\n", i);
            sleep(1);
            
            // 主动短暂sleep让出CPU
            sleep(0);
        }
        printf("子进程完成\n");
        exit(0);
    }
    
    // 父进程：与子进程交替运行
    for (int i = 0; i < 10; i++) {
        printf("父进程第 %d 次执行\n", i);
        sleep(1);
        
        // 父进程也短暂sleep让出CPU
        if (i % 2 == 0) {
            sleep(0);
        }
    }
    
    wait(0);
    
    int end = uptime();
    printf("主动让出CPU测试完成 (耗时: %d ticks)\n", end - start);
}

// 测试MLFQ调度器对不同类型进程的响应
void responsiveness_test() {
    int start = uptime();
    printf("\n=== 测试调度器响应性 ===\n");
    
    // 创建CPU密集型进程（长时间运行）
    int cpu_pid = fork();
    if (cpu_pid == 0) {
        printf("CPU密集型进程开始运行\n");
        for (int i = 0; i < 5; i++) {
            printf("CPU密集型进程: 第 %d 轮计算\n", i);
            for (int j = 0; j < 5000000; j++) {
                volatile int x = j * j;
                (void)x;
            }
        }
        printf("CPU密集型进程完成\n");
        exit(0);
    }
    
    // 等待一下，让CPU密集型进程先运行一会儿
    sleep(2);
    
    // 创建I/O密集型进程（应优先响应）
    int io_pid = fork();
    if (io_pid == 0) {
        printf("I/O密集型进程开始运行（应优先响应）\n");
        for (int i = 0; i < 3; i++) {
            printf("I/O密集型进程: 第 %d 次I/O操作\n", i);
            sleep(2);
        }
        printf("I/O密集型进程完成\n");
        exit(0);
    }
    
    wait(0);
    wait(0);
    
    int end = uptime();
    printf("调度器响应性测试完成 (耗时: %d ticks)\n", end - start);
}

// 压力测试：创建大量简单进程
void stress_test() {
    int start = uptime();
    printf("\n=== 压力测试：创建大量简单进程 ===\n");
    
    // 创建多个简单进程
    for (int i = 0; i < 10; i++) {
        int pid = fork();
        if (pid == 0) {
            printf("压力测试进程 %d 开始\n", i);
            for (int j = 0; j < 3; j++) {
                printf("进程 %d: 运行轮次 %d\n", i, j);
                sleep(1);
            }
            printf("压力测试进程 %d 完成\n", i);
            exit(0);
        }
    }
    
    // 等待所有进程
    for (int i = 0; i < 10; i++) {
        wait(0);
    }
    
    int end = uptime();
    printf("压力测试完成 (耗时: %d ticks)\n", end - start);
}

// 主测试函数
int main(int argc, char *argv[]) {
    int total_start = uptime();
    printf("=== MLFQ调度器综合测试 ===\n");
    printf("测试开始时间: %d ticks\n\n", total_start);
    
    // 记录每个测试的时间
    int test_times[8] = {0};
    int current_test = 0;
    
    // 测试1: 基本调度测试
    printf("--- 测试1: 混合进程类型调度 ---\n");
    int test1_start = uptime();
    
    // 创建CPU密集型进程
    for (int i = 0; i < NUM_CPU_INTENSIVE; i++) {
        int pid = fork();
        if (pid == 0) {
            cpu_intensive(i);
            exit(0);
        }
    }
    
    // 创建I/O密集型进程
    for (int i = 0; i < NUM_IO_INTENSIVE; i++) {
        int pid = fork();
        if (pid == 0) {
            io_intensive(i);
            exit(0);
        }
    }
    
    // 创建混合型进程
    int mixed_pid = fork();
    if (mixed_pid == 0) {
        mixed_process(0);
        exit(0);
    }
    
    // 等待所有进程完成
    printf("\n父进程等待所有子进程完成...\n");
    for (int i = 0; i < NUM_CPU_INTENSIVE + NUM_IO_INTENSIVE + 1; i++) {
        wait(0);
    }
    int test1_end = uptime();
    test_times[current_test++] = test1_end - test1_start;
    printf("测试1完成，耗时: %d ticks\n\n", test1_end - test1_start);
    
    // 测试2: 进程退出和直接切换
    int test2_start = uptime();
    exit_direct_switch_test();
    int test2_end = uptime();
    test_times[current_test++] = test2_end - test2_start;
    printf("\n");
    
    // 测试3: 主动让出CPU测试
    int test3_start = uptime();
    cpu_yield_test();
    int test3_end = uptime();
    test_times[current_test++] = test3_end - test3_start;
    printf("\n");
    
    // 测试4: 调度器响应性测试
    int test4_start = uptime();
    responsiveness_test();
    int test4_end = uptime();
    test_times[current_test++] = test4_end - test4_start;
    printf("\n");
    
    // 测试5: 优先级反转测试
    int test5_start = uptime();
    priority_inversion_test();
    int test5_end = uptime();
    test_times[current_test++] = test5_end - test5_start;
    printf("\n");
    
    // 测试6: 压力测试
    int test6_start = uptime();
    stress_test();
    int test6_end = uptime();
    test_times[current_test++] = test6_end - test6_start;
    printf("\n");
    
    // 测试7: 进程炸弹测试（简化版）
    printf("=== 进程炸弹测试（简化版） ===\n");
    int test7_start = uptime();
    int bomb_pid = fork();
    if (bomb_pid == 0) {
        fork_bomb_simple();
        exit(0);
    }
    
    wait(0);
    int test7_end = uptime();
    test_times[current_test++] = test7_end - test7_start;
    printf("\n");
    
    // 测试8: 老化机制测试（可选，需要较长时间）
    if (argc > 1 && strcmp(argv[1], "-a") == 0) {
        printf("=== 老化机制测试 ===\n");
        int test8_start = uptime();
        aging_test();
        int test8_end = uptime();
        test_times[current_test++] = test8_end - test8_start;
        printf("\n");
    } else {
        printf("老化机制测试跳过（使用 -a 参数运行完整测试）\n\n");
    }
    
    // 打印总测试结果
    int total_end = uptime();
    int total_time = total_end - total_start;
    
    printf("=== 所有测试完成 ===\n");
    printf("测试汇总:\n");
    printf("1. 混合进程类型调度: %d ticks\n", test_times[0]);
    printf("2. 进程退出直接切换: %d ticks\n", test_times[1]);
    printf("3. 主动让出CPU测试: %d ticks\n", test_times[2]);
    printf("4. 调度器响应性测试: %d ticks\n", test_times[3]);
    printf("5. 优先级反转测试: %d ticks\n", test_times[4]);
    printf("6. 压力测试: %d ticks\n", test_times[5]);
    printf("7. 进程炸弹测试: %d ticks\n", test_times[6]);
    
    if (argc > 1 && strcmp(argv[1], "-a") == 0) {
        printf("8. 老化机制测试: %d ticks\n", test_times[7]);
    }
    
    printf("\n总测试时间: %d ticks\n", total_time);
    printf("测试完成时间: %d ticks\n", total_end);
    

    
    exit(0);
}