#include <yak/kernel.h>
#include <yak/lib/string.h>
#include <yak/lib/utils.h>
#include <yak/arch/acpi.h>
#include <yak/arch/ports.h>
#include <yak/arch/pit.h>
#include <yak/arch/pic.h>
#include <yak/arch/lapic.h>
#include <yak/arch/ioapic.h>
#include <yak/mem/vmm.h>
#include <yak/mem/pmm.h>
#include <yak/mem/mem.h>
#include <yak/cpu/idt.h>
#include <yak/cpu/gdt.h>
#include <yak/cpu/percpu.h>
#include <yak/cpu/mp.h>

#define LOG "\33\x0a\xf0<smp>\33r"

typedef struct
{
    uint32_t lapic_address;
    uint32_t flags;
} __attribute__((packed)) acpi_madt_t;

typedef struct
{
    uint8_t entry_type;
    uint8_t record_length;
} __attribute__((packed)) madt_record_t;

typedef struct
{
    uint8_t acpi_proc_id;
    uint8_t lapic_id;
    uint32_t flags;
} __attribute__((packed)) acpi_lapic_t;

typedef struct
{
    uint8_t id;
    uint8_t reserved;
    uint32_t address;
    uint32_t global_system_interrupt_base;
} __attribute__((packed)) acpi_ioapic_t;

typedef struct
{
    uint8_t bus_source;
    uint8_t irq_source;
    uint32_t global_system_interrupt;
    uint16_t flags;
} __attribute__((packed)) acpi_int_t;

enum { 
    ACPI_LOCAL_APIC = 0,
    ACPI_IO_APIC = 1,
    ACPI_INT_SRC_OVERRIDE = 2,
    ACPI_PLATFORM_INT_SRC = 4
};

struct mp_params
{
    uintptr_t cr3;
    gdt_ptr_t gdt_ptr;
    idt_ptr_t idt_ptr;
    uintptr_t stack_ptr;
    uintptr_t percpu_ptr;
    unsigned int id;
} __attribute__((packed));

static unsigned int total_cores = 0;
static unsigned int enabled_cores = 0;
static unsigned int cores_alive = 1;
static unsigned int next_proc_id = 1;

void ap_main(unsigned int id, uintptr_t percpu_base)
{
    percpu_init(id, percpu_base);
    lapic_enable(0);
    *(unsigned char *)(VMM_P2V(AP_STATUS_FLAG)) = AP_READY;

    uintptr_t frame = alloc_frame();
    printk("frame %08x%08x\n", frame >> 32, frame);
    print_mem_stat_local();

    //printk("lapic id %u\n", lapic_id());

    local_irq_enable();

    for (;;)
        cpu_relax();
}

#define CMOS_ADDRESS 0x70
#define CMOS_DATA 0x71
#define LAPIC_VER_NEW 0x10

void start_ap(unsigned int proc_id, unsigned int lapic_id, uintptr_t addr)
{
    printk(LOG " starting core %u (lapic id %d)\n", proc_id, lapic_id);

    // The BSP must initialize CMOS shutdown code to 0x0a...
    outb(CMOS_ADDRESS, 0xf); // offset 0xf is shutdown code
    outb(CMOS_DATA, 0xa);
    
    // and the warn reset vector (dword based at 40:67) to point
    // to the AP startup code
    *((volatile unsigned *)VMM_P2V(0x467)) = (addr & 0xff000) << 12;

    // reset the AP status
    unsigned char *ap_status = (unsigned char *)VMM_P2V(AP_STATUS_FLAG);
    *ap_status = AP_SLEEP;

    lapic_clear_error();

    lapic_send_init_ipi(lapic_id);
    pit_udelay(10000); // wait 10ms

    if (lapic_version() >= LAPIC_VER_NEW) {
        lapic_send_startup_ipi(lapic_id, addr);
        for (int i = 0; i < 10 && *ap_status != AP_STARTED; ++i);
            pit_udelay(100); // wait a total of 1ms

        if (*ap_status != AP_STARTED) {
            lapic_send_startup_ipi(lapic_id, addr);
            for (int i = 0; i < 10000 && *ap_status != AP_STARTED; ++i) {
                pit_udelay(100); // wait a total of 1s
            }
        }
    }
    if (*ap_status != AP_STARTED) {
        *ap_status = AP_READY; // don't block bsp
        printk(LOG "\33\x0f\x40 Unable to start core %u\n", proc_id);
    } else {
        *ap_status = AP_CONTINUE;
        ++cores_alive;
    }

    lapic_clear_error();

    // clean up BIOS reset vector
    outb(CMOS_ADDRESS, 0xf);
    outb(CMOS_DATA, 0);
}

unsigned int count_cpus(uintptr_t madt_address)
{
    if (madt_address == 0)
        return 1;

    sdt_header_t *madt_header = (sdt_header_t *)map_temp(madt_address);
    acpi_madt_t *madt_data = (acpi_madt_t *)((uint8_t *)madt_header + sizeof(sdt_header_t));
    
    total_cores = 0;
    enabled_cores = 0;
    uint8_t *record = (uint8_t *)madt_data + sizeof(acpi_madt_t);
    while (record < (uint8_t *)madt_header + madt_header->length) {
        if (*record == 0) {
            ++total_cores;
            acpi_lapic_t *lapic_record = (acpi_lapic_t *)(record + sizeof(madt_record_t));
            // make sure the cpu is enabled (bit 0 set)
            if ((lapic_record->flags & 0x1) == 1)
                ++enabled_cores;
        }
        record += *(record + 1);
    }
    return enabled_cores;
}

extern const char trampoline[];
extern const char trampoline_end[];
extern const char kernel_percpu_start[];
extern const char kernel_percpu_end[];

void mp_init(uintptr_t madt_address)
{
    percpu_init(0, (uintptr_t)kernel_percpu_start);

    count_cpus(madt_address);
    printk(LOG " detected %u cpu%s (%u enabled)\n", 
            total_cores, total_cores > 1 ? "s" : "", enabled_cores);

    // create the percpu areas and stacks right after the bsp percpu area
    uintptr_t percpu_areas, stacks;
    percpu_mem_init(enabled_cores - 1, &percpu_areas, &stacks);

    // relocate the boot structures after the percpu areas and stacks
    relocate_structures();

    // free all available mem
    mem_init();

    sdt_header_t *madt_header = (sdt_header_t *)map_temp(madt_address);
    acpi_madt_t *madt_data = (acpi_madt_t *)((uint8_t *)madt_header + sizeof(sdt_header_t));

    if (madt_data->flags & 0x1)
        pic_disable();

    lapic_init(madt_data->lapic_address);

    // remap the madt_header since apic_init uses map_temp too...
    madt_header = (sdt_header_t *)map_temp(madt_address);

    const uint8_t bsp_id = lapic_id();

    // TODO: I can't use memcpy here with -O3 ....
    //memcpy((void *)VMM_P2V(TRAMPOLINE_START), (void *)trampoline, trampoline_end - trampoline);
    unsigned char *src = (unsigned char *)trampoline;
    unsigned char *dst = (unsigned char *)VMM_P2V(TRAMPOLINE_START);
    unsigned char *end = src + (size_t)(trampoline_end - trampoline);
    while (src != end)
        *dst++ = *src++;

    struct mp_params params;
    asm volatile("movq %%cr3, %0" : "=r"(params.cr3));
    asm volatile("sgdt %0" : "=m"(params.gdt_ptr));
    asm volatile("sidt %0" : "=m"(params.idt_ptr));

    const size_t percpu_size = align_up(
            (uintptr_t)kernel_percpu_end - (uintptr_t)kernel_percpu_start, PAGE_SIZE);

    unsigned char *ap_status = (unsigned char *)VMM_P2V(AP_STATUS_FLAG);

    uint8_t *record = (uint8_t *)madt_data + sizeof(acpi_madt_t);
    while (record < (uint8_t *)madt_header + madt_header->length) {
        switch (*record) {
            case 0: ;
                acpi_lapic_t *lapic_record = (acpi_lapic_t *)(record + sizeof(madt_record_t));
                if ((lapic_record->flags & 0x1) == 1 && lapic_record->lapic_id != bsp_id) {

                    params.id = next_proc_id++;
                    params.stack_ptr = stacks + params.id * STACK_SIZE - 8;
                    params.percpu_ptr = percpu_areas + (params.id - 1) * percpu_size;
                    memcpy((void *)TRAMPOLINE_PARAMS, (void *)&params, sizeof(params));

                    start_ap(params.id, lapic_record->lapic_id, TRAMPOLINE_START);
                    while (*ap_status != AP_READY)
                        asm volatile("pause" ::: "memory");
                }
                break;

            case 1: ;
                acpi_ioapic_t *ioapic_record = (acpi_ioapic_t *)(record + sizeof(madt_record_t));
                ioapic_add(ioapic_record->id, ioapic_record->address);
                break;
            case 2:
                printk(LOG " INT OVERRIDE\n");
                break;
            case 4:
                printk(LOG " INT SRC\n");
                break;
            default:
                printk(LOG " \33\x0f\x40Unknown MADT entry type: %u\n", *record);
                break;
        }
        record += *(record + 1);
    }

    ioapic_init();
}
