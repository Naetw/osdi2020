#include "MiniUart.h"
#include "shell.h"
#include "mailbox.h"
#include "exception/timer.h"
#include "schedule/task.h"
#include "schedule/schedule.h"
#include "mmu/vma.h"
#include "mmu/buddy.h"
#include "mmu/kmalloc.h"
#include "mmu/slab.h"

void kernel_main(void) {
    initUART();
    sendStringUART("Hello World!\n");

    sendStringUART("Board Revision: ");
    sendHexUART(getBoardRevision());
    sendUART('\n');

    sendStringUART("VC Core Base Addr: ");
    sendHexUART(getVCBaseAddress());
    sendUART('\n');

    gBuddy.construct();
    gBuddy.show();

    sendStringUART("\nTesting buddy system...\n\n");

    Page *page1 = gBuddy.allocate(0xe4000);
    Page *page2 = gBuddy.allocate(0xe40);
    gBuddy.deallocate(page1);
    gBuddy.deallocate(page2);
    
    sendStringUART("\nTesting slab system...\n\n");
    gSlab.init();

    uint64_t token1 = gSlab.regist(56);
    void *obj1 = gSlab.allocate(token1);
    void *obj2 = gSlab.allocate(token1);

    uint64_t token2 = gSlab.regist(36);
    void *obj3 = gSlab.allocate(token2);
    void *obj4 = gSlab.allocate(token2);

    gSlab.deallocate(obj2);
    gSlab.deallocate(obj3);
    gSlab.deallocate(obj1);
    gSlab.deallocate(obj4);

    sendStringUART("\nTesting kmalloc system...\n\n");
    kmalloc_init();

    void *obj5 = kmalloc(0xadd);
    void *obj6 = kmalloc(0xadd);
    void *obj7 = kmalloc(0x3ffff);
    void *obj8 = kmalloc(0xddaa);

    kfree(obj5);
    kfree(obj7);
    kfree(obj6);
    kfree(obj8);

    sendStringUART("\nTesting fini of kmalloc and slab...\n\n");

    kmalloc_fini();
    gSlab.fini();

    sendStringUART("Press enter to continue...");
    sendUART(recvUART());

    while (1)
        ;

    initPageFrames();

    initIdleTaskState();

    sendStringUART("Want to see process of page reclaim? [y/n]\n");
    if (recvUART() == 'y') {
        page_counter_flag = 1lu;
        createPrivilegeTask(fooTask, test_command1);
        createPrivilegeTask(fooTask, test_command2);
        createPrivilegeTask(fooTask, test_command3);
    } else {
        createPrivilegeTask(fooTask, test_command1);
        createPrivilegeTask(fooTask, test_command2);
    }

    _enable_core_timer();

    idle();
}
