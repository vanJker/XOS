
CFLAGS := -m32		# 生成 32 位的程序
CFLAGS += -Qn 		# 不生成 gcc 版本信息
CFLAGS += -fno-pic	# 不需要生成 PIC
CFLAGS += -fomit-frame-pointer 				# 不生产栈帧信息
CFLAGS += -mpreferred-stack-boundary=2		# 不需要进行栈对齐
CFLAGS += -fno-asynchronous-unwind-tables 	# 不需要生成 CFI
CFLAGS += -I ../src/include	# 头文件寻找路径
CFLAGS :=$(strip ${CFLAGS})

%.s: %.c
	gcc $(CFLAGS) -S $< -o $@

.PHONY: clean
clean:
	rm -rf *.o
	rm -rf *.out