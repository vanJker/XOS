#include <xos/interrupt.h>
#include <xos/debug.h>
#include <xos/printk.h>
#include <xos/stdlib.h>
#include <xos/io.h>
#include <xos/assert.h>
#include <xos/global.h>
#include <xos/memory.h>
#include <xos/task.h>

#define EXCEPTION_SIZE 0x20 // 异常数量
#define ENTRY_SIZE     0x30 // 中断入口数量

#define PIC_M_CTRL 0x20     // 主片的控制端口
#define PIC_M_DATA 0x21     // 主片的数据端口
#define PIC_S_CTRL 0xa0     // 从片的控制端口
#define PIC_S_DATA 0xa1     // 从片的数据端口
#define PIC_EOI    0x20     // 通知中断控制器中断结束

gate_t idt[IDT_SIZE];       // 中断描述符表
pointer_t idt_ptr;          // 中断描述符表指针

handler_t handler_table[IDT_SIZE];                  // 中断处理函数表
extern handler_t handler_entry_table[ENTRY_SIZE];   // 中断入口函数表
extern void syscall_entry();                        // 系统调用入口

// 中断输出的错误信息表
static char *messages[] = {
    [0x00] "#DE Divide Error",
    [0x01] "#DB RESERVED",
    [0x02] "--  NMI Interrupt",
    [0x03] "#BP Breakpoint",
    [0x04] "#OF Overflow",
    [0x05] "#BR BOUND Range Exceeded",
    [0x06] "#UD Invalid Opcode (Undefined Opcode)",
    [0x07] "#NM Device Not Available (No Math Coprocessor)",
    [0x08] "#DF Double Fault",
    [0x09] "    Coprocessor Segment Overrun (reserved)",
    [0x0a] "#TS Invalid TSS",
    [0x0b] "#NP Segment Not Present",
    [0x0c] "#SS Stack-Segment Fault",
    [0x0d] "#GP General Protection",
    [0x0e] "#PF Page Fault",
    [0x0f] "--  (Intel reserved. Do not use.)",
    [0x10] "#MF x87 FPU Floating-Point Error (Math Fault)",
    [0x11] "#AC Alignment Check",
    [0x12] "#MC Machine Check",
    [0x13] "#XF SIMD Floating-Point Exception",
    [0x14] "#VE Virtualization Exception",
    [0x15] "#CP Control Protection Exception",
};

void exception_handler(
    int vector,
    u32 edi, u32 esi, u32 ebp, u32 esp,
    u32 ebx, u32 edx, u32 ecx, u32 eax,
    u32 gs, u32 fs, u32 es, u32 ds,
    u32 vector0, u32 error, u32 eip, u32 cs, u32 eflags
) {
    char *message = NULL;
    if (vector < 0x16) {
        message = messages[vector];
    } else {
        message = messages[0x0f]; // 输出reversed 信息
    }

    printk("\n");
    printk("EXCEPTION : %s \n",    message);
    printk("   VECTOR : 0x%02X\n", vector);
    printk("    ERROR : 0x%08X\n", error);
    printk("   EFLAGS : 0x%08X\n", eflags);
    printk("       CS : 0x%02X\n", cs);
    printk("      EIP : 0x%08X\n", eip);
    printk("      ESP : 0x%08X\n", esp);

    // 阻塞
    hang();
}

// 缺页异常处理函数
void page_fault_handler(
    int vector,
    u32 edi, u32 esi, u32 ebp, u32 esp,
    u32 ebx, u32 edx, u32 ecx, u32 eax,
    u32 gs, u32 fs, u32 es, u32 ds,
    u32 vector0, u32 error, u32 eip, u32 cs, u32 eflags
) {
    assert(vector == 0xe); // 缺页异常中断向量号

    u32 vaddr = get_cr2(); // 获取触发缺页异常的虚拟地址
    LOGK("Page fault address 0x%p\n", vaddr);

    // 前 8M 为恒等映射，不可能触发缺页异常
    assert(KERNEL_MEMORY_SIZE <= vaddr && vaddr < USER_STACK_TOP);
    
    // 缺页异常错误码
    page_error_code_t *page_error = (page_error_code_t *)&error;

    task_t *current = current_task();

    // 尝试写入用户空间的只读页（存在且只读）时，需要对该页进行 Copy On Write
    if (page_error->present) {
        assert(page_error->write);

        // 获取页表项以及对应页框的索引
        page_entry_t *pgtbl = get_pte(vaddr, false);
        page_entry_t *pte = &pgtbl[PTE_IDX(vaddr)];
        u32 pidx = pte->index;

        assert(memory_map()[pidx] > 0);
        if (memory_map()[pidx] == 1) {
            // 将写入的页对应的页框引用数等于 1，说明原先引用该页框的其它进程都已经对该页进行了 Copy On Write，
            // 所以此时只有当前进程引用了该页框。那么只需将该页框的读写权限重新设置为可写即可
            pte->write = 1;
            LOGK("WRITE page for 0x%p\n", vaddr);
        } else {
            // 如果将写入的页对应的页框引用数大于 1，则需要分配一新物理页并进行拷贝，
            // 同时需要更新当前进程的页表、刷新 TLB，以及页框的引用数。
            u32 paddr = copy_page(PAGE_ADDR(PAGE_IDX(vaddr)));
            page_entry_init(pte, PAGE_IDX(paddr));
            flush_tlb(vaddr);
            memory_map()[pidx]--;
            LOGK("WRITE page for 0x%p\n", vaddr);
        }
        assert(memory_map()[pidx] > 0);
        
        return;
    }

    // 如果缺页异常发生在用户栈或用户堆范围内，则进行 Lazy Allocation
    if (!page_error->present && page_error->user 
        && (vaddr < current->brk || vaddr > USER_STACK_BOOTOM)
    ) {
        u32 vpage = PAGE_ADDR(PAGE_IDX(vaddr));
        link_page(vpage);
        return;
    }

    panic("Page Fault!!!");
}

void send_eoi(int vector) {
    // 如果中断来自主片，只需要向主片发送 EOI
    if (vector >= 0x20 && vector < 0x28) {
        outb(PIC_M_CTRL, PIC_EOI);
    }
    // 如果中断来自从片，除了向从片发送 EOI 以外，还要再向主片发送 EOI
    if (vector >= 0x28 && vector < 0x30) {
        outb(PIC_M_CTRL, PIC_EOI);
        outb(PIC_S_CTRL, PIC_EOI);
    }
}

void default_handler(int vector) {
    static u32 counter = 0;
    send_eoi(vector);
    LOGK("[%x] default interrupt called %d...\n", vector, counter++);
}

void set_interrupt_handler(u32 irq, handler_t handler) {
    assert(irq >= 0 && irq < 16);
    handler_table[IRQ_MASTER_NR + irq] = handler;
}

void set_interrupt_mask(u32 irq, bool enable) {
    assert(irq >= 0 && irq < 16);
    u16 port;

    // 计算端口和屏蔽字
    if (irq < 8) {  // 位于主片
        port = PIC_M_DATA;
    } else {        // 位于从片
        port = PIC_S_DATA;
        irq -= 8;   // 消除偏移量
    }

    // 开启/屏蔽指定 IRQ
    if (enable) outb(port, inb(port) & ~(1 << irq));
    else        outb(port, inb(port) | (1 << irq));
}

// 获取当前的外中断响应状态，即获取 IF 位
u32 get_irq_state() {
    asm volatile(
        "pushfl\n"        // 将当前的 eflags 压入栈中
        "popl %eax\n"     // 将压入的 eflags 弹出到 eax
        "shrl $9, %eax\n" // 将 eax 右移 9 位，得到 IF 位
        "andl $1, %eax\n" // 只需要 IF 位
    );
}

// 设置外中断响应状态，即设置 IF 位
void set_irq_state(u32 state) {
    if (state) asm volatile("sti\n");
    else       asm volatile("cli\n"); 
}

// 关闭外中断响应，即清除 IF 位，并返回关中断之前的状态
u32 irq_disable() {
    u32 pre_irq_state = get_irq_state();
    set_irq_state(false);
    return pre_irq_state;
}

// 打开外中断响应，即设置 IF 位
void irq_enable() {
    set_irq_state(true);
}

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

// 初始化中断控制器
void pic_init() {
    // 主片 ICW
    outb(PIC_M_CTRL, 0b00010001); // ICW1: 边沿触发, 级联 8259, 需要ICW4.
    outb(PIC_M_DATA, 0x20);       // ICW2: 起始端口/中断向量号 0x20
    outb(PIC_M_DATA, 0b00000100); // ICW3: IR2接从片.
    outb(PIC_M_DATA, 0b00000001); // ICW4: 8086模式, 正常EOI

    // 从片 ICW
    outb(PIC_S_CTRL, 0b00010001); // ICW1: 边沿触发, 级联 8259, 需要ICW4.
    outb(PIC_S_DATA, 0x28);       // ICW2: 起始端口/中断向量号 0x28
    outb(PIC_S_DATA, 2);          // ICW3: 设置从片连接到主片的 IR2 引脚
    outb(PIC_S_DATA, 0b00000001); // ICW4: 8086模式, 正常EOI

    // 主片/从片 OCW1
    outb(PIC_M_DATA, 0b11111111); // OCW1: 关闭所有中断
    outb(PIC_S_DATA, 0b11111111); // OCW1: 关闭所有中断
}

// 初始化中断描述符表，以及中断处理函数表
void idt_init() {
    // 初始化中断描述符表
    for (size_t i = 0; i < ENTRY_SIZE; i++) {
        gate_t *gate = &idt[i];
        handler_t handler = handler_entry_table[i];

        gate->offset_low = (u32)handler & 0xffff;
        gate->offset_high = ((u32)handler >> 16) & 0xffff;
        gate->selector = 1 << 3; // 1 号段为代码段
        gate->reserved = 0;      // 保留不用
        gate->type = 0b1110;     // 中断门
        gate->segment = 0;       // 系统段
        gate->DPL = 0;           // 内核态权级
        gate->present = 1;       // 有效位
    }

    // 初始化异常处理函数表
    for (size_t i = 0; i < EXCEPTION_SIZE; i++) {
        handler_table[i] = exception_handler;
    }
    handler_table[0xe] = page_fault_handler; // 注册缺页异常处理函数

    // 初始化外中断处理函数表
    for (size_t i = 0x20; i < ENTRY_SIZE; i++) {
        handler_table[i] = default_handler;
    }

    // 初始化系统调用
    gate_t *gate = &idt[SYSCALL_VECTOR];
    gate->offset_low = (u32)syscall_entry & 0xffff;
    gate->offset_high = ((u32)syscall_entry >> 16) & 0xffff;
    gate->selector = 1 << 3; // 1 号段为代码段
    gate->reserved = 0;      // 保留不用
    gate->type = 0b1110;     // 中断门
    gate->segment = 0;       // 系统段
    gate->DPL = 3;           // 用户态权级
    gate->present = 1;       // 有效位

    // 加载中断描述符表指针
    idt_ptr.base = (u32)idt;
    idt_ptr.limit = sizeof(idt) - 1;
    asm volatile("lidt idt_ptr");
}

// 中断初始化
void interrupt_init() {
    pic_init();
    idt_init();
}