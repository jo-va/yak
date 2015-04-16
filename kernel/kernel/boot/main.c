#include <yak/kernel.h>
#include <yak/config.h>
#include <yak/initcall.h>
#include <yak/lib/string.h>
#include <yak/boot/multiboot.h>
#include <yak/video/vbe.h>
#include <yak/video/terminal.h>
#include <yak/cpu/idt.h>
#include <yak/cpu/mp.h>
#include <yak/mem/mem.h>
#include <yak/arch/acpi.h>
#include <yak/arch/tsc.h>
#include <yak/arch/pit.h>
#include <yak/arch/lapic.h>
#include <yak/dev/keyboard.h>

multiboot_info_t *mbi;
vbe_mode_info_t *mode_info;

INIT_CODE void init_system(u64_t magic, u64_t mboot)
{
    mbi = (multiboot_info_t *)((u64_t)mboot + VIRTUAL_BASE);
    mode_info = (vbe_mode_info_t *)(mbi->vbe_mode_info + VIRTUAL_BASE);

    int padding = 10;
    int term_w = mode_info->res_x - padding * 2;
    int term_h = mode_info->res_y - padding * 2;
    term_init(0, mode_info, padding, padding, term_w, term_h, 0xc0c0c0, 0x000000);

    if (magic != MBOOT_LOADER_MAGIC)
        panic("Bad multiboot magic value\n");

    printk("\33\x0f\xf0YAK is booting \33\x0f\xff[%ux%ux%u]\n", mode_info->res_x, mode_info->res_y, mode_info->bpp);
    
    // we should not allocate memory before mem_init()
    // this means that some PML3...PML1 tables must be set statistically
    // by default, we map more memory than needed, mem_init() will 
    // then do a cleanup
    isr_init();
    idt_init();
    tsc_init();
    mp_init(acpi_init()); // this will call mem_init()
    kbd_init();
}

void func(void *r)
{
    (void)r;
    //if (lapic_id() == 0)
    //    printk(".");
}
#include <yak/cpu/interrupt.h>

void kernel_main(u64_t magic, u64_t mboot)
{
    init_system(magic, mboot);

    reclaim_init_mem();

    //isr_register(80, reclaim);
    //lapic_send_ipi(3, 80);

    isr_register(0x20, func);

    for (;;) {
        if (kbd_lastchar() == 'q')
            kbd_reset_system();
    }
}
