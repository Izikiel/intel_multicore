# ** por compatibilidad se omiten tildes **
# ==============================================================================
# TRABAJO PRACTICO 3 - System Programming - ORGANIZACION DE COMPUTADOR II - FCEN
# ==============================================================================

MCOPY=mcopy

KERNEL_SRC=kernel.asm
KERNEL_OBJ=kernel.o
KERNEL_BIN=kernel.bin

DEPEND = Makefile \
	kernel.asm isr.asm \
	imprimir.mac \
	gdt.c gdt.h pic.c pic.h idt.c idt.h isr.h tss.c tss.h mmu.c mmu.h contextManager.h contextManager.c\
	sched.c sched.h screen_mapa.c screen_mapa.h screen_estado.c screen_estado.h screen_utils.c screen_utils.h \
	game.c game.h syscall.h utils.h utils.c asmutils.asm syscall_impl.h syscall_impl.c 

OBJ=$(KERNEL_OBJ) gdt.o pic.o idt.o isr.o mmu.o sched.o tss.o contextManager.o screen_mapa.o screen_estado.o screen_utils.o game.o utils.o asmutils.o syscall_impl.o
DISK_IMG=diskette.img
DISK_IMG_BZ=diskette.img.bz2

CFLAGS=-m32 -g -ggdb -Wall -Werror -O0 \
  -fno-zero-initialized-in-bss -fno-stack-protector -ffreestanding


TASK0=idle.tsk
TASK1=task1.ctsk
TASK2=task2.ctsk
TASK3=task3.ctsk
TASK4=task4.ctsk
TASK5=task5.ctsk
TASK6=task6.ctsk
TASK7=task7.ctsk
TASK8=task8.ctsk

TASKS=tareas.tsk

TASKSBINS=$(TASK0) $(TASK1) $(TASK2) $(TASK3) $(TASK4) $(TASK5) $(TASK6) $(TASK7) $(TASK8)

CODESIZE=60928    # = 65536 - 0x1200 = 64KB - 0x1200
TASKSSIZE=73728   # = 1 * (4096 * 2) + 8 * (4096 * 2) = idle + tareas
KERNELSIZE=134656 # = CODESIZE + TASKSSIZE

TASKSTARTADDR=0x40000000

CC=gcc
NASM=nasm
NASMFLAGS=-felf32
LD=ld
LDTASKFLAGS=-s -static -m elf_i386 --oformat binary -b elf32-i386 -e task -Ttext $(TASKSTARTADDR)
LDFLAGS=-static -m elf_i386 --oformat binary -b elf32-i386 -e start -Ttext 0x1200

QUIET = @

.PHONY=clean all image tasks

all: cleantasks genbin tasks image #clean

genbin: genbin.c
	$(CC) -m32 -g -ggdb -Wall -Werror -o genbin genbin.c

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $^

%.o: %.asm
	$(NASM) $(NASMFLAGS) -o $@ $^

kernel.bin: $(OBJ) tasks
	@echo 'Linkeando kernel...'
	$(LD) $(LDFLAGS) -o $@ $(OBJ)
	@echo ''

	@echo 'Generando imagen del kernel...'
	mv kernel.bin kernel.bin.tmp
	dd if=/dev/zero of=kernel.bin bs=1 count=$(KERNELSIZE) conv=notrunc status=noxfer > /dev/null 2>&1
	dd if=kernel.bin.tmp of=kernel.bin bs=1 count=$(CODESIZE) conv=notrunc status=noxfer > /dev/null 2>&1
	dd if=$(TASKS) of=kernel.bin bs=1 count=$(TASKSSIZE) seek=$(CODESIZE) conv=notrunc status=noxfer > /dev/null 2>&1
	rm kernel.bin.tmp
	@echo ''

image: kernel.bin $(DEPEND) $(DISK_IMG)
	@echo 'Copiando el $(KERNEL_BIN) a la imagen de diskette'
	$(MCOPY) -o -i $(DISK_IMG) $(KERNEL_BIN) ::/
	@echo ''

$(DISK_IMG): $(DISK_IMG_BZ)
	bzip2 -dk $(DISK_IMG_BZ)

idle.tsk: idle.asm
	@echo 'Compilando tarea...'
	$(NASM) -fbin -o $@ $^
	@echo ''

	@echo 'Generando imagen de la tarea idle...'
	mv idle.tsk idle.tsk.tmp
	dd if=/dev/zero bs=1 count=8K of=$@ status=noxfer > /dev/null 2>&1
	dd if=$@.tmp bs=1 count=8K seek=0 conv=notrunc of=$@ status=noxfer > /dev/null 2>&1
	rm $@.tmp
	@echo ''

task1.tsko: tarea1.c
	@echo 'Compilando tarea...'
	$(CC) $(CFLAGS) -Dtask1 -c -o $@ $^
	@echo ''

task2.tsko: tarea2.c
	@echo 'Compilando tarea...'
	$(CC) $(CFLAGS) -Dtask2 -c -o $@ $^
	@echo ''

task3.tsko: tarea3.c
	@echo 'Compilando tarea...'
	$(CC) $(CFLAGS) -Dtask3 -c -o $@ $^
	@echo ''

task4.tsko: tarea4.c
	@echo 'Compilando tarea...'
	$(CC) $(CFLAGS) -Dtask4 -c -o $@ $^
	@echo ''

task5.tsko: tarea5.c
	@echo 'Compilando tarea...'
	$(CC) $(CFLAGS) -Dtask5 -c -o $@ $^
	@echo ''

task6.tsko: tarea6.c
	@echo 'Compilando tarea...'
	$(CC) $(CFLAGS) -Dtask6 -c -o $@ $^
	@echo ''

task7.tsko: tarea7.c
	@echo 'Compilando tarea...'
	$(CC) $(CFLAGS) -Dtask7 -c -o $@ $^
	@echo ''

task8.tsko: tarea8.c
	@echo 'Compilando tarea...'
	$(CC) $(CFLAGS) -Dtask8 -c -o $@ $^
	@echo ''

%.ctsk: %.tsko
	@echo 'Linkeando tarea...'
	$(LD) $(LDTASKFLAGS) -o $@.tmp $^
	@echo ''

	@echo 'Generando imagen de la tarea...'
	./genbin $@.tmp $@ 8192 $$(nm $^ | grep bandera | awk '{print $$1}')
	rm $@.tmp
	@echo ''

tasks: $(TASKSBINS)
	@echo 'Generando imagen de las tareas...'
	$(QUIET) dd if=/dev/zero bs=1 count=$(TASKSSIZE) of=$(TASKS) status=noxfer > /dev/null 2>&1

	$(QUIET) dd if=$(TASK1) bs=1 count=8K of=$(TASKS) seek=0K  status=noxfer > /dev/null 2>&1
	$(QUIET) dd if=$(TASK2) bs=1 count=8K of=$(TASKS) seek=8K  status=noxfer > /dev/null 2>&1
	$(QUIET) dd if=$(TASK3) bs=1 count=8K of=$(TASKS) seek=16K status=noxfer > /dev/null 2>&1
	$(QUIET) dd if=$(TASK4) bs=1 count=8K of=$(TASKS) seek=24K status=noxfer > /dev/null 2>&1
	$(QUIET) dd if=$(TASK5) bs=1 count=8K of=$(TASKS) seek=32K status=noxfer > /dev/null 2>&1
	$(QUIET) dd if=$(TASK6) bs=1 count=8K of=$(TASKS) seek=40K status=noxfer > /dev/null 2>&1
	$(QUIET) dd if=$(TASK7) bs=1 count=8K of=$(TASKS) seek=48K status=noxfer > /dev/null 2>&1
	$(QUIET) dd if=$(TASK8) bs=1 count=8K of=$(TASKS) seek=56K status=noxfer > /dev/null 2>&1
	$(QUIET) dd if=$(TASK0) bs=1 count=8K of=$(TASKS) seek=64K status=noxfer > /dev/null 2>&1
	@echo ''

clean:
	@echo 'Limpiando todo...'
	rm -f *.o
	rm -f *.bin
	rm -f *.tmp
	rm -f *.ctsko
	rm -f *.tsko
	rm -f *.tsk
	rm -f $(DISK_IMG)
	rm -f genbin
	@echo ''

cleantasks:
	@echo 'Limpiando tareas...'
	rm -f *.tsk
	rm -f *.tsko
	@echo ''

