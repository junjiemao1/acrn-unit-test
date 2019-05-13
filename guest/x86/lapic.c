#include "libcflat.h"
#include "apic.h"
#include "vm.h"
#include "smp.h"
#include "desc.h"
#include "isr.h"
#include "msr.h"
#include "atomic.h"

static u32 x2apic_read(unsigned reg)
{
    unsigned a, d;

    asm volatile ("rdmsr" : "=a"(a), "=d"(d) : "c"(APIC_BASE_MSR + reg/16));
    return a | (u64)d << 32;
}

static void x2apic_write(unsigned reg, u32 val)
{
    asm volatile ("wrmsr" : : "a"(val), "d"(0), "c"(APIC_BASE_MSR + reg/16));
}

static inline void
sync_counter_change(void)
{
	u64 prev_counter, current_counter = x2apic_read(APIC_TMCCT);
	do {
		prev_counter = current_counter;
		current_counter = x2apic_read(APIC_TMCCT);
	} while (current_counter == prev_counter);
}

int main(void)
{
	u64 last_tsc, cur_tsc, acc_tsc = 0UL, target_tsc;
	u32 prev_counter, current_counter;
	int i;

	printf("Initial IA32_APIC_BASE: %016lx\n", rdmsr(MSR_IA32_APICBASE));
	printf("Initial LVT_TIMER: %08x\n", x2apic_read(APIC_LVTT));

	x2apic_write(APIC_LVTT, APIC_LVT_TIMER_ONESHOT | 0xe0);
	x2apic_write(APIC_TDCR, APIC_TDR_DIV_64);
	printf("Current LVT_TIMER: %08x\n", x2apic_read(APIC_LVTT));

	current_counter = 0x40000000U;
	x2apic_write(APIC_TMICT, current_counter);
	last_tsc = rdtsc();
	for (i = 0; i < 16; ) {
		prev_counter = current_counter;
		current_counter = x2apic_read(APIC_TMCCT);
		if (current_counter != prev_counter) {
			acc_tsc += (rdtsc() - last_tsc);
			last_tsc = rdtsc();
			i ++;
		}
	}
	acc_tsc /= 16;
	printf("16 counters take %ld tsc on average.\n", acc_tsc);

	sync_counter_change();
	sync_counter_change();
	target_tsc = rdtsc() + acc_tsc / 2;
	while (rdtsc() < target_tsc);
	last_tsc = rdtsc();

	x2apic_write(APIC_TDCR, APIC_TDR_DIV_128);
	sync_counter_change();
	cur_tsc = rdtsc();

	printf("Starting from TSC %08lx,\n", target_tsc - acc_tsc / 2);
	printf("the next counter change is expected to happen at TSC %08lx.\n", target_tsc + acc_tsc);
	printf("But after changing TDR at TSC %08lx,\n", last_tsc);
	printf("the next counter change happened at TSC %08lx\n", cur_tsc);

	return report_summary();
}
