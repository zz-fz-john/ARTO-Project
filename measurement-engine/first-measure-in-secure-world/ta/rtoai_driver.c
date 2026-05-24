#include <kernel/interrupt.h>
#include <mm/core_memprot.h>
#include <mm/core_mmu.h>
#include <io.h>
#include <arm.h>
#include <trace.h>
#include <initcall.h>
#include <drivers/serial8250_uart.h>


#define BCM2836_LOCAL_BASE  0x40000000
#define BCM2836_LOCAL_SIZE  0x1000

#define SEC_TIMER_IRQ       0  
#define EXPECTED_RAND_VAL   0x12345678


#define CORE0_TIMER_IRQCNTL 0x40000040
#define CORE0_FIQ_SOURCE    0x40000070


register_phys_mem(MEM_AREA_IO_SEC, BCM2836_LOCAL_BASE, BCM2836_LOCAL_SIZE);

static void *g_monitor_virt_addr = NULL;
static bool g_is_timer_started = false;
static uint64_t g_interval_ticks = 0;
static uint32_t g_last_val_end = 0;
static uint32_t g_last_val_start = 0;
static bool g_is_first_run = true;
static vaddr_t g_qa7_base = 0;


static enum itr_return my_security_check_handler(struct itr_handler *h)
{
    uint32_t current_val_start, current_val_end;

    
    write_cntps_ctl(0);

   
    if (g_interval_ticks == 0) return ITRR_HANDLED;

  
    write_cntps_tval(g_interval_ticks); 

    if (!g_monitor_virt_addr) {
        write_cntps_ctl(1);
        return ITRR_HANDLED;
    }

    
    current_val_start = read32((vaddr_t)g_monitor_virt_addr);
    current_val_end   = read32((vaddr_t)g_monitor_virt_addr + 4);

    if (g_is_first_run) {
        g_last_val_start = current_val_start;
        g_last_val_end = current_val_end;
        g_is_first_run = false;
    } else {
        if (current_val_start == g_last_val_start && current_val_end == g_last_val_end) {
            EMSG("SECURITY VIOLATION! System Stalled. Val: 0x%x", current_val_start);
        } else {
            g_last_val_start = current_val_start;
            g_last_val_end = current_val_end;
        }
    }

  
    write_cntps_ctl(1); 
    return ITRR_HANDLED;
}

static struct itr_handler my_itr = {
    .it = SEC_TIMER_IRQ,
    .handler = my_security_check_handler,
    .flags = ITRF_TRIGGER_LEVEL, 
};


static void rpi3_disable_irq(struct itr_chip *chip __unused, size_t it)
{
    if (it != SEC_TIMER_IRQ || !g_qa7_base) return;
    

}

static void rpi3_enable_irq(struct itr_chip *chip __unused, size_t it)
{
    if (it != SEC_TIMER_IRQ || !g_qa7_base) return;


    vaddr_t va_irq_cntl = g_qa7_base + (CORE0_TIMER_IRQCNTL - BCM2836_LOCAL_BASE);
    vaddr_t va_fiq_src  = g_qa7_base + (CORE0_FIQ_SOURCE - BCM2836_LOCAL_BASE);
    
    write32(1, va_fiq_src);  
    write32(1, va_irq_cntl);
}


static void rpi3_add_irq(struct itr_chip *chip __unused, size_t it, uint32_t flags __unused)
{
    
    rpi3_enable_irq(chip, it);
}


static const struct itr_ops rpi3_itr_ops = {
    .add     = rpi3_add_irq,
    .disable = rpi3_disable_irq,  
    .enable  = rpi3_enable_irq,   

};


static struct itr_chip rpi3_itr_chip = {
    .ops = &rpi3_itr_ops,
};


TEE_Result rtoai_secure_timer_start(uint32_t interval_ms, void *va, size_t len)
{
    paddr_t pa;

    if (g_is_timer_started) {
        EMSG("Timer already started!");
        return TEE_ERROR_ACCESS_DENIED;
    }

   
    pa = virt_to_phys(va);
    if (!pa) return TEE_ERROR_BAD_PARAMETERS;
    g_monitor_virt_addr = phys_to_virt(pa, MEM_AREA_RAM_NSEC);
    if (!g_monitor_virt_addr) return TEE_ERROR_GENERIC;

    write32(EXPECTED_RAND_VAL, (vaddr_t)g_monitor_virt_addr);


    uint32_t freq = read_cntfrq(); 
    if (interval_ms < 1) interval_ms = 10;
    g_interval_ticks = (freq / 1000) * interval_ms;
    
    write_cntps_tval(g_interval_ticks);
    write_cntps_ctl(1); 

 
    itr_enable(my_itr.it);

    g_is_timer_started = true;
    IMSG("RT-OAI: Timer Started.");
    return TEE_SUCCESS;
}


static TEE_Result rtoai_timer_driver_init(void)
{
    IMSG("RT-OAI: Init (Service Level)");
    
  
    g_qa7_base = (vaddr_t)phys_to_virt(BCM2836_LOCAL_BASE, MEM_AREA_IO_SEC);
    if (!g_qa7_base) {
        EMSG("PANIC: Failed to map QA7 Base.");
        return TEE_ERROR_GENERIC;
    }

    itr_init(&rpi3_itr_chip);

    
    itr_add(&my_itr);
    
    IMSG("RT-OAI: Driver & Interrupt Controller Registered.");
    return TEE_SUCCESS;
}

service_init(rtoai_timer_driver_init);