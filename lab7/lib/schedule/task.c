#include "schedule/task.h"
#include "MiniUart.h"
#include "exception/exception.h"
#include "mmu/mmu.h"
#include "mmu/vma.h"
#include "schedule/context.h"
#include "schedule/schedule.h"
#include "sys/unistd.h"
#include "utils.h"

// symbol from linker script
extern uint64_t kernel_start; // physical
extern uint64_t kernel_end; // virtualized

// since circular queue will waste a space for circulation
TaskStruct *queue_buffer[MAX_TASK_NUM + 1];
Queue running_queue = {.buffer = (void **)queue_buffer,
                       .capacity = MAX_TASK_NUM,
                       .front = 0u,
                       .back = 0u};

TaskStruct ktask_pool[MAX_TASK_NUM] __attribute__((aligned(16u)));
uint8_t kstack_pool[MAX_TASK_NUM][4096] __attribute__((aligned(16u)));

UserTaskStruct utask_pool[MAX_TASK_NUM] __attribute__((aligned(16u)));

const uint64_t kUnUse = 0;
const uint64_t kInUse = 1;
const uint64_t kZombie = 2;
const uint64_t kDefaultStackVirtualAddr = 0xffffffffe000;

void idle(void) {
    while (1) {
        sendStringUART("Enter idle state ...\n");

        // Zombie reaper
        for (uint32_t i = 1; i < MAX_TASK_NUM; ++i) {
            if (ktask_pool[i].status == kZombie) {
                ktask_pool[i].status == kUnUse;
                freePages(ktask_pool[i].pgd);
                ktask_pool[i].pgd = NULL;
            }
        }

        if (isQueueEmpty(&running_queue)) {
            break;
        }
        schedule();
    }
    sendStringUART("\n\nTest finished\n\n");
    while (1)
        ;
}

void initIdleTaskState() {
    ktask_pool[0].id = 0u;
    ktask_pool[0].counter = 0u;
    ktask_pool[0].reschedule_flag = true;
    ktask_pool[0].status = kInUse;
    ktask_pool[0].kernel_context.physical_pgd = 0; // master PGD's page frame
    asm volatile("msr tpidr_el1, %0"
                 : /* output operands */
                 : "r"(&ktask_pool[0]) /* input operands */
                 : /* clobbered register */);
}

int64_t createPrivilegeTask(void (*kernel_task)(), void (*user_task)()) {
    for (uint32_t i = 1; i < MAX_TASK_NUM; ++i) {
        if (ktask_pool[i].status == kUnUse) {
            ktask_pool[i].status = kInUse;
            ktask_pool[i].id = i;
            ktask_pool[i].counter = 10u;
            ktask_pool[i].reschedule_flag = false;

            ktask_pool[i].kernel_context.lr = (uint64_t)kernel_task;
            ktask_pool[i].kernel_context.main = (uint64_t)user_task;

            // +1 since stack grows toward lower address
            ktask_pool[i].kernel_context.fp = (uint64_t)kstack_pool[i + 1];
            ktask_pool[i].kernel_context.sp = (uint64_t)kstack_pool[i + 1];

            ktask_pool[i].user_task = utask_pool + i;
            ktask_pool[i].user_task->id = i;
            ktask_pool[i].user_task->regain_resource_flag = false;

            ktask_pool[i].pgd = allocPage();
            ktask_pool[i].tail_page = ktask_pool[i].pgd;
            ktask_pool[i].kernel_context.physical_pgd = translate(
                (uint64_t)ktask_pool[i].pgd, kPageDescriptorToPhysical);

            pushQueue(&running_queue, ktask_pool + i);
            return i;
        } else if (ktask_pool[i].status == kZombie) {
            ktask_pool[i].status = kUnUse;
            freePages(ktask_pool[i].pgd);
            ktask_pool[i].pgd = NULL;
        }
    }

    sendStringUART("[ERROR] No more tasks can be created!\n");
    return -1;
}

void checkRescheduleFlag(void) {
    TaskStruct *cur_task = getCurrentTask();

    if (cur_task->reschedule_flag) {
        schedule();

        // regain the resource
        cur_task->user_task->regain_resource_flag = true;
    }
}

static void copyProgram(TaskStruct *cur_task) {
    uint64_t end = ((uint64_t)&kernel_end) & 0x0000fffffffff000;
    for (uint64_t i = (uint64_t)&kernel_start; i < end; i += PAGE_SIZE) {
        // allocate page frame
        cur_task->tail_page = updatePageFramesForMappingProgram(
            cur_task->pgd, cur_task->tail_page, i);
        // copy raw binary
        uint8_t *frame = (uint8_t *)translate((uint64_t)cur_task->tail_page,
                                              kPageDescriptorToVirtual);
        memcpy(frame, (uint8_t *)translate(i, kPhysicalToVirtual), PAGE_SIZE);
    }
}

static void startMain(void (*main)(void));

void doExec(void (*program)()) {
    TaskStruct *cur_task = getCurrentTask();
    UserTaskStruct *user_task = cur_task->user_task;

    // FIXME: remove 0x0000ffffffffffff when change to use embeded user program
    user_task->user_context.lr = (uint64_t)startMain & 0x0000ffffffffffff;

    user_task->user_context.fp = kDefaultStackVirtualAddr;
    user_task->user_context.sp = kDefaultStackVirtualAddr;
    user_task->user_context.main = (uint64_t)program & 0x0000ffffffffffff;

    cur_task->tail_page =
        updatePageFramesForMappingStack(cur_task->pgd, cur_task->tail_page);

    // FIXME: binary_xxx_end won't be aligned to PAGE_SIZE, need to do manually
    //        when change to use embeded user program
    copyProgram(cur_task);

    initUserTaskandSwitch(&cur_task->kernel_context,
                          &cur_task->user_task->user_context);
}

static void copyUserContext(int64_t id) {
    TaskStruct *cur_task = getCurrentTask();

    UserContext *src_ctx = &cur_task->user_task->user_context;
    UserContext *dst_ctx = &utask_pool[id].user_context;

    // fp, lr should be populated by _kernel_exit
    dst_ctx->sp = src_ctx->sp;
    dst_ctx->elr_el1 = src_ctx->elr_el1;
    dst_ctx->spsr_el1 = src_ctx->spsr_el1;
}

void copyStacks(int64_t id) {
    TaskStruct *cur_task = getCurrentTask();

    // kernel
    uint8_t *src_sp = kstack_pool[cur_task->id];
    uint8_t *dst_sp = kstack_pool[id];

    memcpy(dst_sp, src_sp, 4096);

    // bottom - current
    uint64_t src_size =
        (uint64_t)kstack_pool[cur_task->id + 1] - cur_task->kernel_context.sp;
    ktask_pool[id].kernel_context.sp = (uint64_t)kstack_pool[id + 1] - src_size;

    // user
    src_sp = (uint8_t *)getPhysicalofVirtualAddressFromPGD(
        cur_task->pgd, kDefaultStackVirtualAddr - 0x1000);
    dst_sp = (uint8_t *)getPhysicalofVirtualAddressFromPGD(
        ktask_pool[id].pgd, kDefaultStackVirtualAddr - 0x1000);

    memcpy(dst_sp, src_sp, 4096);
}

static void updateTrapFrame(int64_t id) {
    TaskStruct *child_task = ktask_pool + id;

    uint64_t *trapframe = (uint64_t *)child_task->kernel_context.sp;

    // retval
    trapframe[0] = 0;
}

void doFork(uint64_t *trapframe) {
    TaskStruct *cur_task = getCurrentTask();

    int64_t new_task_id = createPrivilegeTask(_child_return_from_fork, NULL);
    if (new_task_id == -1) {
        // TODO:
        sendStringUART("[ERROR] fail to create privilege task\n");
        return;
    }

    TaskStruct *new_task = &ktask_pool[new_task_id];

    new_task->tail_page =
        updatePageFramesForMappingStack(new_task->pgd, new_task->tail_page);

    copyProgram(new_task);

    trapframe[0] = new_task_id;

    copyUserContext(new_task_id);

    cur_task->kernel_context.sp = (uint64_t)trapframe;
    copyStacks(new_task_id);

    // child's fp, retval should be different from parent's ones
    updateTrapFrame(new_task_id);
}

void doExit(int status) {
    TaskStruct *cur_task = getCurrentTask();

    // TODO: handle status code, where to use???

    cur_task->status = kZombie;
    cur_task->reschedule_flag = true;
}

static void barTask(void);
static void forkTask(void);

// kernel task
void fooTask(void (*user_task)()) {

    // kernel routine
    TaskStruct *cur_task = getCurrentTask();

    sendStringUART("\nHi, I'm ");
    sendHexUART(cur_task->id);
    sendStringUART(" in kernel mode...\n");
    sendStringUART("Doing kernel routine for awhile...\n");

    doExec(user_task);
}

// ------ For User Mode ----------------
static void forkedTask(void) {
    int tmp = 5;
    UserTaskStruct *cur_task = getUserCurrentTask();

    writeStringUART("Task ");
    writeHexUART(cur_task->id);
    writeStringUART(" after exec, tmp address ");
    writeHexUART((uint64_t)&tmp);
    writeStringUART(", tmp value ");
    writeHexUART(tmp);
    writeUART('\n');

    exit(0);
}

static void forkTask(void) {
    int cnt = 1;
    if (fork() == 0) {
        fork();
        delay(100000);
        fork();

        UserTaskStruct *cur_task = getUserCurrentTask();

        while (cnt < 10) {
            writeStringUART("Task id ");
            writeHexUART(cur_task->id);
            writeStringUART(", cnt: ");
            writeHexUART(cnt);
            writeUART('\n');
            delay(100000);
            ++cnt;
        }
        exit(0);

        writeStringUART("Shouldn't reach here\n");
    } else {
        UserTaskStruct *cur_task = getUserCurrentTask();

        writeStringUART("Task ");
        writeHexUART(cur_task->id);
        writeStringUART(" before exec, cnt address ");
        writeHexUART((uint64_t)&cnt);
        writeStringUART(", cnt value ");
        writeHexUART(cnt);
        writeUART('\n');
        exec(forkedTask);
    }
}

static void bazTask(void) {
    UserTaskStruct *cur_task = getUserCurrentTask();

    writeStringUART("\nHi, I'm ");
    writeHexUART(cur_task->id);
    writeStringUART(" in user mode...\n");

    writeStringUART("Doing bazTask() for awhile...\n");

    while (cur_task->regain_resource_flag == false)
        ;

    // reset
    cur_task->regain_resource_flag = false;
}

// main user task
static void barTask(void) {
    writeStringUART("\nHi, I'm ");
    writeHexUART(getTaskId());
    writeStringUART(" in user mode...\n");

    writeStringUART("Doing barTask() for awhile...\n");

    exit(0);
}

void test_command1(void) {
    int cnt = 0;
    if (fork() == 0) {
        fork();
        fork();
        while (cnt < 10) {
            writeStringUART("Task id: ");
            writeHexUART(getTaskId());
            writeStringUART(", sp: ");
            writeHexUART((uint64_t)&cnt);
            writeStringUART(", cnt: ");
            writeHexUART(cnt);
            writeUART('\n');
            delay(100000);
            ++cnt;
        }
        exit(0);
    }
}

void test_command2(void) {
    if (fork() == 0) {
        int *a = 0x0;
        writeHexUART(*a); // trigger page fault
    }
}

void test_command3(void) {
    writeStringUART("Test page reclaiming\n");
}

static void startMain(void (*main)(void)) {
    main();
    exit(0);
}
