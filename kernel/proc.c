#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];
struct proc proc[NPROC];
struct proc *initproc;
struct proc *idleproc;  // 空闲进程指针
int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);
extern char trampoline[];

// 等待锁
struct spinlock wait_lock;

// MLFQ相关全局变量
struct mlfq_queue mlfq_queues[NMLFQ];
int queue_time_slice[NMLFQ] = {1, 2, 4, 8, 16};
struct spinlock mlfq_lock;

// MLFQ初始化
void mlfq_init(void) {
  initlock(&mlfq_lock, "mlfq");
  for(int j = 0; j < NMLFQ; j++) {
    mlfq_queues[j].front = 0;
    mlfq_queues[j].rear = 0;
    mlfq_queues[j].count = 0;
  }
}

// 进程入队
void mlfq_enqueue(int priority, struct proc* p) {
  if(priority < 0) priority = 0;
  if(priority >= NMLFQ) priority = NMLFQ - 1;
  
  acquire(&mlfq_lock);
  
  // 检查进程状态，ZOMBIE进程不应该入队
  if(p->state != RUNNABLE) {
    release(&mlfq_lock);
    return;
  }
  
  struct mlfq_queue* q = &mlfq_queues[priority];
  if(q->count < NPROC) {
    q->procs[q->rear] = p;
    q->rear = (q->rear + 1) % NPROC;
    q->count++;
    
    p->priority = priority;
    p->ticks_in_queue = 0;
    p->entry_time = ticks;
  }
  
  release(&mlfq_lock);
}

// 进程出队
struct proc* mlfq_dequeue(int priority) {
  acquire(&mlfq_lock);
  
  struct proc* p = 0;
  struct mlfq_queue* q = &mlfq_queues[priority];
  
  if(q->count > 0) {
    p = q->procs[q->front];
    q->front = (q->front + 1) % NPROC;
    q->count--;
  }
  
  release(&mlfq_lock);
  return p;
}

// 从队列中移除进程
void mlfq_remove(struct proc* p) {
  if(p == 0 || p == idleproc) return;
  
  acquire(&mlfq_lock);
  
  struct mlfq_queue* q = &mlfq_queues[p->priority];
  
  // 线性搜索并移除
  for(int i = 0; i < q->count; i++) {
    int idx = (q->front + i) % NPROC;
    if(q->procs[idx] == p) {
      // 将后续元素前移
      for(int j = i; j < q->count - 1; j++) {
        int curr = (q->front + j) % NPROC;
        int next = (q->front + j + 1) % NPROC;
        q->procs[curr] = q->procs[next];
      }
      q->count--;
      q->rear = (q->rear - 1 + NPROC) % NPROC;
      break;
    }
  }
  
  release(&mlfq_lock);
}

// 周期性提升优先级（防止饥饿）
void age_boost(void) {
  acquire(&mlfq_lock);
  
  for(int prio = NMLFQ - 1; prio > 0; prio--) {
    struct mlfq_queue* q = &mlfq_queues[prio];
    
    // 收集需要提升的进程
    struct proc* boost_list[NPROC];
    int boost_count = 0;
    
    for(int i = 0; i < q->count && boost_count < NPROC; i++) {
      int idx = (q->front + i) % NPROC;
      struct proc* p = q->procs[idx];
      if(ticks - p->entry_time > 200) {  // 长时间未运行
        boost_list[boost_count++] = p;
      }
    }
    
    // 提升收集到的进程
    for(int i = 0; i < boost_count; i++) {
      struct proc* p = boost_list[i];
      mlfq_remove(p);
      mlfq_enqueue(prio - 1, p);
    }
  }
  
  release(&mlfq_lock);
}

void schedule(void) {
  struct proc *prev = myproc();  // 当前进程
  struct proc *next = 0;
  
  // 关闭中断，保证原子性
  push_off();
  
  // 只有在进程状态是RUNNING且不是空闲进程时才重新入队
  // ZOMBIE进程不会被重新入队
  if(prev && prev->state == RUNNING && prev != idleproc) {
    prev->state = RUNNABLE;
    mlfq_enqueue(prev->priority, prev);
  }
  
  // 选择下一个进程，跳过ZOMBIE状态
    
    for(int prio = 0; prio < NMLFQ; prio++) {
      next = mlfq_dequeue(prio);
      if(next != 0 && next->state == RUNNABLE) {
        goto found;
      }
      // 如果进程状态不是RUNNABLE，放回队列（如果需要的话）
      if(next != 0) {
        mlfq_enqueue(next->priority, next);
        next = 0;
      }
    
  }
  
found:
  // 如果没有进程可运行，使用空闲进程
  if(next == 0) {
    next = idleproc;
  }
  
  // 设置下一个进程状态
  next->state = RUNNING;
  mycpu()->proc = next;
  
  // 切换上下文
  swtch(&prev->context, &next->context);
  
  // 切换回来后，更新CPU当前进程（可能已经改变）
  mycpu()->proc = myproc();
  
  // 恢复中断
  pop_off();
}

// 为每个进程分配内核栈页面
void proc_mapstacks(pagetable_t kpgtbl) {
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// 初始化进程表
void procinit(void) {
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  
  // 初始化所有进程
  for(p = proc; p < &proc[NPROC]; p++) {
    initlock(&p->lock, "proc");
    p->state = UNUSED;
    p->kstack = KSTACK((int) (p - proc));
    p->priority = 0;
    p->ticks_in_queue = 0;
    p->entry_time = 0;
  }
  
  // 初始化空闲进程（使用第一个进程槽位）
  idleproc = &proc[0];
  // 这里不需要锁，因为是在单核启动阶段
  idleproc->state = RUNNABLE;
  idleproc->pid = 0;
  idleproc->priority = NMLFQ - 1;  // 最低优先级
  safestrcpy(idleproc->name, "idle", sizeof(idleproc->name));
  
  // 初始化空闲进程的上下文
  memset(&idleproc->context, 0, sizeof(idleproc->context));
  idleproc->context.ra = (uint64)forkret;
  idleproc->context.sp = idleproc->kstack + PGSIZE;
  
  // 空闲进程入队
  mlfq_enqueue(idleproc->priority, idleproc);
  
  mlfq_init();  // 初始化MLFQ队列
}

// CPU ID
int cpuid() {
  int id = r_tp();
  return id;
}

// 获取当前CPU
struct cpu* mycpu(void) {
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// 获取当前进程
struct proc* myproc(void) {
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

// 分配PID
int allocpid() {
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// 分配进程
static struct proc* allocproc(void) {
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p == idleproc) continue;  // 跳过空闲进程
    
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // 分配trapframe页
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // 创建空用户页表
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // 设置新上下文
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  // 初始化MLFQ字段
  p->priority = 0;
  p->ticks_in_queue = 0;
  p->entry_time = ticks;
  
  return p;
}

// 释放进程
static void freeproc(struct proc *p) {
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// 创建进程页表
pagetable_t proc_pagetable(struct proc *p) {
  pagetable_t pagetable;

  // 创建空页表
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // 映射trampoline代码
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // 映射trapframe页
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// 释放进程页表
void proc_freepagetable(pagetable_t pagetable, uint64 sz) {
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// 设置第一个用户进程
void userinit(void) {
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // 分配用户页并复制initcode
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // 准备从内核返回到用户空间
  p->trapframe->epc = 0;      // 用户程序计数器
  p->trapframe->sp = PGSIZE;  // 用户栈指针

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;
  mlfq_enqueue(p->priority, p);
  release(&p->lock);
}

// 增长或缩小用户内存
int growproc(int n) {
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// 创建新进程
int fork(void) {
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // 分配进程
  if((np = allocproc()) == 0){
    return -1;
  }

  // 从父进程复制用户内存到子进程
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // 复制保存的用户寄存器
  *(np->trapframe) = *(p->trapframe);

  // 在子进程中使fork返回0
  np->trapframe->a0 = 0;

  // 增加打开文件描述符的引用计数
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  // 复制MLFQ字段
  np->priority = p->priority;
  np->ticks_in_queue = 0;
  np->entry_time = ticks;
  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  // 这里不需要锁，因为np的状态只在这里设置
  np->state = RUNNABLE;
  mlfq_enqueue(np->priority, np);

  return pid;
}

// 重新分配子进程给init
void reparent(struct proc *p) {
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}
/*
static void exit_schedule(void) {
  struct proc *p = myproc();  // 当前进程，应该是ZOMBIE状态
  struct proc *next = 0;
  
  // 关闭中断，保证原子性
  push_off();
  
  // 选择下一个进程
  for(int prio = 0; prio < NMLFQ; prio++) {
    next = mlfq_dequeue(prio);
    if(next != 0) {
      break;
    }
  }
  
  // 如果没有进程可运行，使用空闲进程
  if(next == 0) {
    next = idleproc;
  }
  
  // 设置下一个进程状态
  next->state = RUNNING;
  mycpu()->proc = next;
  
  // 切换上下文
  swtch(&p->context, &next->context);
  
  // 如果切换回来（不应该发生），进入死循环
  mycpu()->proc = myproc();
  pop_off();
  
  // 不应该执行到这里
  panic("zombie process scheduled again");
}
*/
// 退出当前进程 - 最终版本
void exit(int status) {
  struct proc *p = myproc();
  
  // 从MLFQ队列中移除
  mlfq_remove(p);
  
  if(p == initproc)
    panic("init exiting");

  // 关闭所有打开的文件
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // 将子进程交给init
  reparent(p);

  // 父进程可能在wait()中睡眠
  wakeup(p->parent);
  
  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);
  
// 专门用于退出的调度函数
  schedule();
  
  // 永远不会执行到这里
  panic("zombie exit");
  
}



// 等待子进程退出
int wait(uint64 addr) {
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // 扫描进程表查找退出的子进程
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        acquire(&pp->lock);
        havekids = 1;
        if(pp->state == ZOMBIE){
          // 找到一个
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // 如果没有子进程，则无需等待
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // 等待子进程退出
    sleep(p, &wait_lock);
  }
}

// 调度器 - 简化版本
void scheduler(void) {
  struct cpu *c = mycpu();
  
  // 设置当前CPU的进程为空闲进程
  c->proc = idleproc;
  
  // 空闲进程循环
  for(;;) {
    // 确保空闲进程在队列中
    if(idleproc->state != RUNNABLE) {
      idleproc->state = RUNNABLE;
    }
    
    // 在空闲进程中循环
    intr_on();
    asm volatile("wfi");  // 等待中断，节省功耗
  }
}

// 调度切换
void sched(void) {
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  // 使用直接调度
  schedule();
}

// 放弃CPU
void yield(void) {
  struct proc *p = myproc();
  
  // 如果进程是ZOMBIE状态，不要重新入队
  if(p->state == ZOMBIE) {
    // 直接触发调度，但不会重新入队
    push_off();
    
    struct proc *next = 0;
    for(int prio = 0; prio < NMLFQ; prio++) {
      next = mlfq_dequeue(prio);
      if(next != 0) {
        break;
      }
    }
    
    if(next == 0) {
      next = idleproc;
    }
    
    next->state = RUNNING;
    mycpu()->proc = next;
    
    swtch(&p->context, &next->context);
    
    mycpu()->proc = myproc();
    pop_off();
  } else {
    // 正常进程调用schedule
    schedule();
  }
}

// fork返回
void forkret(void) {
  static int first = 1;

  // 注意：我们不释放锁，因为锁没有被持有

  if (first) {
    // 文件系统初始化必须在常规进程的上下文中运行
    fsinit(ROOTDEV);
    first = 0;
    __sync_synchronize();
  }

  usertrapret();
}

// 睡眠
void sleep(void *chan, struct spinlock *lk) {
  struct proc *p = myproc();
  //acquire(&p->lock);
  // 释放调用者传递的锁
  release(lk);
  
  // 设置睡眠状态
  p->chan = chan;
  p->state = SLEEPING;
  
  // 调用调度函数
  schedule();
  
  // 当被唤醒后，继续执行到这里
  // 清理状态
  p->chan = 0;
  //release(&p->lock);
  // 重新获取调用者传递的锁
  acquire(lk);
}

// 唤醒
void wakeup(void *chan) {
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc() && p != idleproc){
      // 原子比较和交换的方式来避免锁
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
        // 唤醒的进程放入最高优先级（I/O密集型）
        mlfq_enqueue(0, p);
      }
    }
  }
}

// 杀死进程
int kill(int pid) {
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    if(p == idleproc) continue;
    acquire(&p->lock);
    
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        p->state = RUNNABLE;
        mlfq_enqueue(p->priority, p);
    
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

// 设置进程为已杀死
void setkilled(struct proc *p) {
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

// 检查进程是否被杀死
int killed(struct proc *p) {
  return p->killed;
}

// 复制输出
int either_copyout(int user_dst, uint64 dst, void *src, uint64 len) {
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// 复制输入
int either_copyin(void *dst, int user_src, uint64 src, uint64 len) {
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// 进程转储
void procdump(void) {
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}