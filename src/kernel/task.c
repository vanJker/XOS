#include <xos/task.h>
#include <xos/printk.h>
#include <xos/debug.h>
#include <xos/assert.h>
#include <xos/memory.h>
#include <xos/interrupt.h>
#include <xos/string.h>
#include <xos/syscall.h>
#include <xos/stdlib.h>

extern void task_switch(task_t *next);
extern u32 volatile jiffies;
extern const u32 jiffy;

// 任务队列
static task_t *task_queue[NUM_TASKS];

// 默认的阻塞任务队列
static list_t blocked_queue;

// 睡眠任务队列
static list_t sleeping_queue;

// 空闲任务
static task_t *idle_task;

// 从任务队列获取一个空闲的任务，并分配 TCB
static task_t *get_free_task() {
    for (size_t i = 0; i < NUM_TASKS; i++) {
        if (task_queue[i] == NULL) {
            task_queue[i] = (task_t *)kalloc(1);
            return task_queue[i];
        }
    }
    return NULL;
}

// 在任务队列中查找某种状态的任务（不查找自己）
// 在符合条件的任务中，取优先级最高并且最久没运行的
// 如果没有符合条件的任务，返回 NULL
static task_t *task_search(task_state_t state) {
    // 查找过程保证中断关闭，防止查找有误
    ASSERT_IRQ_DISABLE();

    task_t *result = NULL;
    task_t *current = current_task();

    for (size_t i = 0; i < NUM_TASKS; i++) {
        task_t *task = task_queue[i];

        if (task == NULL || task == current || task->state != state) {
            continue;
        }

        if (result == NULL || task->ticks > result->ticks || task->jiffies < result->jiffies) {
            result = task;
        }
    }

    // 当无法寻找到任一就绪的任务时，返回 idle 这个空闲任务
    if (result == NULL && state == TASK_READY) {
        result = idle_task;
    }

    return result;
}

// 当前任务
task_t *current_task() {
    // (sp - 4) 保证获取到正确的 TCB
    asm volatile(
        "movl %esp, %eax\n"
        "subl $4, %eax\n"
        "andl $0xfffff000, %eax\n"
    );
}

// 任务调度
void schedule() {
    // 在中断门中使用了该函数（系统调用 yield）
    ASSERT_IRQ_DISABLE();

    task_t *current = current_task();       // 获取当前任务
    task_t *next = task_search(TASK_READY); // 查找一个就绪任务

    assert(next != NULL);               // 保证任务非空
    assert(next->magic == XOS_MAGIC);   // 检测栈溢出

    // 如果当前任务状态为运行，则将状态置为就绪
    if (current->state == TASK_RUNNING) {
        current->state = TASK_READY;
    }
    // 重置当前任务的剩余时间片，为下一次调度准备
    if (current->ticks == 0) {
        current->ticks = current->priority;
    }

    next->state = TASK_RUNNING;
    if (next == current) { // 如果下一个任务还是当前任务，则无需进行上下文切换
        return;
    }

    task_switch(next);
}

// 创建一个默认的任务 TCB
static task_t *task_create(target_t target, const char *name, u32 priority, u32 uid) {
    task_t *task = get_free_task();
    memset(task, 0, PAGE_SIZE); // 清空 TCB 所在的页

    u32 stack = (u32)task + PAGE_SIZE;

    stack -= sizeof(task_frame_t);
    task_frame_t *frame = (task_frame_t *)stack;
    frame->ebx = 0x11111111;
    frame->esi = 0x22222222;
    frame->edi = 0x33333333;
    frame->ebp = 0x44444444;
    frame->eip = (void *)target;

    strcpy((char *)task->name, name);
    task->stack = (u32 *)stack;
    task->state = TASK_READY;
    task->priority = priority;
    task->ticks = task->priority; // 默认剩余时间 = 优先级
    task->jiffies = 0;
    task->uid = uid;
    task->page_dir = get_kernel_page_dir();
    task->vmap = get_kernel_vmap();
    task->magic = XOS_MAGIC;

    return task;
}

// 配置开机运行至今的 setup 任务，使得其能进行任务调度
static void task_setup() {
    task_t *current = current_task();
    current->magic = XOS_MAGIC;
    current->ticks = 1;

    // 初始化任务队列
    memset(task_queue, 0, sizeof(task_queue));
}

// 任务主动放弃执行权
void task_yield() {
    // 即主动调度到其它空闲任务执行
    schedule();
}

// 阻塞任务
void task_block(task_t *task, list_t *blocked_list, task_state_t state) {
    // 涉及阻塞队列这个临界区
    ASSERT_IRQ_DISABLE();

    // 任务没有位于任一阻塞队列中
    ASSERT_NODE_FREE(&task->node);

    // 如果加入的阻塞队列为 NULL，则加入默认的阻塞队列
    if (blocked_list == NULL) {
        blocked_list = &blocked_queue;
    }

    // 加入阻塞队列，并将任务状态修改为阻塞
    list_push_back(blocked_list, &task->node);
    ASSERT_BLOCKED_STATE(state);
    task->state = state;

    // 如果阻塞的是当前任务，则立即进行调度
    task_t *current = current_task();
    if (current == task) {
        schedule();
    }
}

// 结束阻塞任务
void task_unblock(task_t *task) {
    // 涉及阻塞队列这个临界区
    ASSERT_IRQ_DISABLE();

    // 在任务所处的阻塞队列进行删除
    list_remove(&task->node);

    // 任务此时没有位于任一阻塞队列当中
    ASSERT_NODE_FREE(&task->node);

    // 任务状态修改为就绪
    task->state = TASK_READY;
}

// 任务睡眠一段时间
void task_sleep(u32 ms) {
    // 涉及睡眠队列这个临界区
    ASSERT_IRQ_DISABLE();

    // 睡眠时间不能为 0
    assert(ms > 0);

    // 睡眠的时间片数量向上取整
    u32 ticks = div_round_up(ms, jiffy);

    // 记录睡眠结束时的全局时间片，因为在那个时刻应该要唤醒任务
    task_t *current = current_task();
    current->ticks = jiffies + ticks;

    // 从睡眠链表中找到第一个比当前任务唤醒时间点更晚的任务，进行插入排序
    list_t *list = &sleeping_queue;

    list_node_t *anchor = &list->tail;
    for (list_node_t *ptr = list->head.next; ptr != &list->tail; ptr = ptr->next) {
        task_t *task = element_entry(task_t, node, ptr);
        
        if (task->ticks > current->ticks) {
            anchor = ptr;
            break;
        }
    }

    // 保证当前任务没有位于任何阻塞 / 睡眠队列当中
    ASSERT_NODE_FREE(&current->node);

    // 插入链表
    list_insert_before(anchor, &current->node);

    // 设置阻塞状态为睡眠
    current->state = TASK_SLEEPING;

    // 调度执行其它任务
    schedule();
}

// 唤醒任务
void task_wakeup() {
    // 涉及睡眠队列这个临界区
    ASSERT_IRQ_DISABLE();

    // 从睡眠队列中找到任务成员 ticks 小于或等于全局当前时间片 jiffies 的任务
    // 结束这些任务的阻塞 / 睡眠，进入就绪状态
    list_t *list = &sleeping_queue;

    for (list_node_t *ptr = list->head.next; ptr != &list->tail;) {
        task_t *task = element_entry(task_t, node, ptr);

        if (task->ticks > jiffies) {
            break;
        }

        // task_unblock() 会清空链表节点的 prev 和 next 指针，所以需要事先保存
        ptr = ptr->next;

        // task->ticks = 0;
        task_unblock(task);
    }
}

extern void idle_thread();
extern void init_thread();
extern void test_thread();

// 初始化任务管理
void task_init() {
    list_init(&blocked_queue);
    list_init(&sleeping_queue);

    task_setup();

    idle_task = task_create((target_t)idle_thread, "idle", 1, KERNEL_TASK);
    task_create((target_t)init_thread, "init", 5, USER_TASK);
    task_create((target_t)test_thread, "test", 5, KERNEL_TASK);
}