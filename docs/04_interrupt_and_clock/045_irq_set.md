# 045 外中断控制

## 1. EFLAGS

![](./images/eflags.drawio.svg)

IF 位位于 eflags 的第 9 位。

## 2. 核心代码

> 位于 `include/xos/interrupt.h`

```c
u32 get_irq_state();    // 获取当前的外中断响应状态，即获取 IF 位
void irq_disable();     // 关闭外中断响应，即清除 IF 位
void irq_enable();      // 打开外中断响应，即设置 IF 位
void irq_save();        // 保存当前的外中断状态，并关闭外中断
void irq_restore();     // 将外中断状态恢复为先前的外中断状态

// 外中断响应处于禁止状态，即 IF 位为 0
#define ASSERT_IRQ_DISABLE() assert(get_irq_state() == 0)
```

## 3. 代码分析

> 以下代码均位于 `kernel/interrupt.c`

### 3.1 获取中断状态

根据函数调用约定，通过内联汇编实现该函数。

```c
// 获取当前的外中断响应状态，即获取 IF 位
u32 get_irq_state() {
    asm volatile(
        "pushfl\n"        // 将当前的 eflags 压入栈中
        "popl %eax\n"     // 将压入的 eflags 弹出到 eax
        "shrl $9, %eax\n" // 将 eax 右移 9 位，得到 IF 位
        "andl $1, %eax\n" // 只需要 IF 位
    );
}
```

### 3.2 使能 / 关闭中断

通过设置 IF 位的值即可使能或关闭外中断响应，独立出函数 `set_irq_state()` 是为了方便后面 `irq_save()` 和 `irq_restore()` 的实现。

```c
// 设置 IF 位
static _inline void set_irq_state(u32 state) {
    if (state) asm volatile("sti\n");
    else       asm volatile("cli\n"); 
}

// 关闭外中断响应，即清除 IF 位
void irq_disable() {
    set_irq_state(false);
}

// 打开外中断响应，即设置 IF 位
void irq_enable() {
    set_irq_state(true);
}
```

### 3.3 保存 / 恢复中断状态

`irq_save()` 相对于 `irq_disable()`，多了这个功能：它在关闭中断前，先保存了当前的 IF 位。

`irq_restore()` 则是与 `irq_save()` 搭配使用，它的作用是将中断状态，恢复成之前 `irq_save()` 保存的那个状态。

这两个函数对于后面的锁机制的实现十分重要。参考 xv6，我们的实现如下：

```c
// 禁止外中断响应状态的次数，也即调用 irq_save() 的次数
static size_t irq_off_count = 0;
// 首次调用 irq_save() 时保存的外中断状态，因为只有此时才有可能为使能状态
static u32 first_irq_state;

// 保存当前的外中断状态，并关闭外中断
void irq_save() {
    // 获取当前的外中断状态
    u32 pre_irq_state = get_irq_state();
    
    // 关闭中断，也保证了后续的临界区访问
    irq_disable();

    // 如果是首次调用，需要保存此时的外中断状态
    if (irq_off_count == 0) {
        first_irq_state = pre_irq_state;
    }

    // 更新禁止外中断状态的次数
    irq_off_count++;
}

// 将外中断状态恢复为先前的外中断状态
void irq_restore() {
    // 使用 irq_restore 时必须处于外中断禁止状态
    ASSERT_IRQ_DISABLE();
    
    // 保证禁止外中断的次数不为 0，与 irq_save() 相对应
    assert(irq_off_count > 0);

    // 更新禁止外中断状态的次数
    irq_off_count--;
    
    // 如果该调用对应首次调用 irq_save()，则设置为对应的外中断状态
    if (irq_off_count == 0) {
        set_irq_state(first_irq_state);
    }
}
```

- 首次调用 `irq_save()` 时的外中断状态，既可能为禁止状态，也可能为使能状态。而其它调用时的外中断状态，只可能为禁止状态，因为 `irq_save()` 的功能为，保存当前的外中断状态，禁止外中断响应。所以我们只需要保存首次调用 `irq_save()` 时的外中断状态，即 `first_irq_state`。
- 因为其它调用时，外中断状态只可能为禁止，以及 `irq_restore()` 必须要与 `irq_save()` 配对使用，所以我们只需记录调用次数，这样就可以保证 `irq_restore()` 只有在对应 `irq_save()` 首次被调用时，才会恢复为 `first_irq_state` 状态。
- 同上，`irq_restore()` 必须要与 `irq_save()` 配对使用，所以调用 `irq_restore()` 时必须处于外中断禁止状态。同理，`irq_save()` 的调用次数也必须大于 0。
- 这两个函数内部都是在外中断响应禁止状态下进行执行的，保证了函数体后续对临界区的访问操作。

---

但是以上设计不能实现每个进程的外中断状态的各自分离保存，而是以 CPU 为单位来保存外中断响应状态。因为我们是用 `static` 来将这些状态保存的内存当中，任一进程均可访问。

要实现每个进程的外中断响应状态的分离保存，需要将这些状态保存在每个进程独有的地方，那就是 —— 栈！！！因为每个进程的栈都是独有的，别的进程不太可能去访问（因为当前并没有实现内存保护，所以其它进程想访问其它进程的栈，还是可以做到的）。所以我们改造 `irq_disabe()` 如下：

```c
// 关闭外中断响应，即清除 IF 位，并返回关中断之前的状态
u32 irq_disable() {
    u32 pre_irq_state = get_irq_state();
    set_irq_state(false);
    return pre_irq_state;
}
```

那么使用方法如下：

```c
void intr_test() {
    // 保存中断状态，并关闭中断
    u32 irq = irq_disable();

    // do something
    
    // 恢复之前保存的中断状态
    set_irq_state(irq);
}
```

## 4. 功能测试

在 `kernel/main.c` 搭建测试框架：

```c
void kernel_init() {
    ...
    irq_disable();
    LOGK("IRQ state: %d\n", get_irq_state()); // should be 0

    irq_enable();
    LOGK("IRQ state: %d\n", get_irq_state()); // should be 1

    irq_save();     // pre_irq_state should be 1
    LOGK("IRQ state: %d\n", get_irq_state()); // should be 0

    irq_restore();  // pre_irq_state should be 0
    LOGK("IRQ state: %d\n", get_irq_state()); // should be 1
    ...
}
```

使用调试来检查所保存的中断状态，在调用 `irq_save()` 后，`pre_irq_state` 应为 1。其它预期均在注释有说明，按照说明调试即可。

## 5. FAQ

>**`set_interrupt_state(false)` 以后，在恢复 IF 位这个操作之前，如果有外中断信号，这个信号是会被丢弃，还是在恢复IF位以后会触发？**

在系统中，`set_interrupt_state(false)` 表示禁用（关闭）中断，而 `set_interrupt_state(true)` 则表示启用（打开）中断。

如果在禁用中断期间发生了外部中断信号，该信号将被丢弃。因为中断被禁用，处理器不会响应外部中断请求，而是继续执行当前的指令流程。

当你恢复（启用）中断时，如果在禁用期间有未处理的中断信号，启用中断后，处理器会立即响应这些未处理的中断信号。这意味着在恢复 IF 位（中断标志位）之后，如果有等待处理的中断信号，处理器会触发中断处理程序，以响应这些中断请求。

---

>**`irq_save()` 与 `irq_restore()` 在锁机制上的应用？**

目前还没有讲到锁机制，但是在锁机制上的应用如下：

```c
void intr_test() {
    irq_save();     // 保存中断状态，并关闭中断

    // do something

    irq_restore();  // 恢复之前保存的中断状态
}
```

> 以上为参考示意，因为 `irq_save()` 和 `irq_restore()` 没有实现分离保存各个进程的外中断响应状态。实际中使用请参考之前的说明。
