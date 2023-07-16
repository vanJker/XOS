#include <xos/xos.h>
#include <xos/types.h>
#include <xos/io.h>
#include <xos/string.h>
#include <xos/console.h>
#include <xos/printk.h>
#include <xos/assert.h>

char buf[1024];

void kernel_init() {
    console_init();
    panic("Out of Memory");
    return;
}