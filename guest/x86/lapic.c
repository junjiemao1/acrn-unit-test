#include "libcflat.h"
#include "apic.h"
#include "vm.h"
#include "smp.h"
#include "desc.h"
#include "isr.h"
#include "msr.h"
#include "atomic.h"

#define LVTT_ILLEGAL_VECTOR 0x08U
#define LVTT_TEST_VECTOR 0x30U
#define SPURIOUS_VECTOR 0xe0U
#define LARGE_TMICT 0x800000U
#define LARGE_TSCDEADLINE 0xFFFFFFFFFFFFFFUL

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

static volatile int lvtt_counter = 0;

static void lvtt_handler(isr_regs_t *regs)
{
    lvtt_counter++;
    x2apic_write(APIC_EOI, 0);
}

static volatile int spurious_counter = 0;

static void spurious_handler(isr_regs_t *regs)
{
    spurious_counter++;
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

static void
dump_lvtt(u32 lvtt)
{
	printf("[LVTT] timer mode = %x%x, masked = %x pending = %x, vector = %x\n",
	       (lvtt & (1 << 18)) ? 1 : 0,
	       (lvtt & (1 << 17)) ? 1 : 0,
	       (lvtt & APIC_LVT_MASKED) ? 1 : 0,
	       (lvtt & APIC_SEND_PENDING) ? 1 : 0,
	       (lvtt & APIC_VECTOR_MASK));
}

static void
dump_esr(u32 esr)
{
	printf("[ESR] send ill = %x, received ill = %x, ill reg access = %x, redir ipi = %x\n",
	       (esr & APIC_ESR_SENDILL) ? 1 : 0,
	       (esr & APIC_ESR_RECVILL) ? 1 : 0,
	       (esr & APIC_ESR_ILLREGA) ? 1 : 0,
	       (esr & 0x10) ? 1 : 0);
}

static void
check_lvt_timer(void)
{
	u32 lvtt, esr;

	printf("LVTT testing:\n");

	/* Initial value */
	lvtt = x2apic_read(APIC_LVTT);
	report("\tInitial value = 00010000H", lvtt == 0x00010000U);

	/* Set illegal vector */
	x2apic_write(APIC_LVTT, LVTT_ILLEGAL_VECTOR);
	lvtt = x2apic_read(APIC_LVTT);
	x2apic_write(APIC_ESR, 0);
	esr = x2apic_read(APIC_ESR);
	dump_lvtt(lvtt);
	dump_esr(esr);
	report("\tSet illegal vector", lvtt == 0x00010000U);

	/* Set [bit 12] (pending) */
	x2apic_write(APIC_LVTT, APIC_SEND_PENDING | LVTT_TEST_VECTOR);
	lvtt = x2apic_read(APIC_LVTT);
	x2apic_write(APIC_ESR, 0);
	esr = x2apic_read(APIC_ESR);
	dump_lvtt(lvtt);
	dump_esr(esr);
	report("\tSet pending bit", lvtt == LVTT_TEST_VECTOR);

	/* Set reserved timer mode */
	x2apic_write(APIC_LVTT, APIC_LVT_TIMER_MASK | LVTT_TEST_VECTOR);
	lvtt = x2apic_read(APIC_LVTT);
	x2apic_write(APIC_ESR, 0);
	esr = x2apic_read(APIC_ESR);
	dump_lvtt(lvtt);
	dump_esr(esr);
	report("\tSet reserved timer mode", lvtt == (APIC_LVT_TIMER_MASK | LVTT_TEST_VECTOR));

	/* Set illegal vector & other bits */
	x2apic_write(APIC_LVTT, APIC_LVT_TIMER_PERIODIC | LVTT_ILLEGAL_VECTOR);
	lvtt = x2apic_read(APIC_LVTT);
	x2apic_write(APIC_ESR, 0);
	esr = x2apic_read(APIC_ESR);
	dump_lvtt(lvtt);
	dump_esr(esr);
	report("\tSet illegal vector & periodic timer mode", lvtt == (APIC_LVT_TIMER_PERIODIC | LVTT_TEST_VECTOR));
}

static void
check_timer_counters(void)
{
	u32 tmict, tmcct, tdcr;
	u64 tscdeadline;

	printf("Timer counters testing:\n");

	/* Read/write legacy timer counters in legacy timer modes */
	x2apic_write(APIC_LVTT, APIC_LVT_TIMER_PERIODIC | APIC_LVT_MASKED | LVTT_TEST_VECTOR);
	x2apic_write(APIC_TMICT, LARGE_TMICT);
	tmict = x2apic_read(APIC_TMICT);
	tmcct = x2apic_read(APIC_TMCCT);
	report("\tTMICT read & write in periodic timer mode", tmict == LARGE_TMICT);
	report("\tTMCCT read after setting TMICT in periodic timer mode", tmcct > 0 && tmcct < LARGE_TMICT);

	/* Read/write TDCR in legacy timer modes */
	x2apic_write(APIC_TDCR, APIC_TDR_DIV_32);
	tdcr = x2apic_read(APIC_TDCR);
	report("\tTDCR read & write in periodic timer mode", tdcr == APIC_TDR_DIV_32);

	/* Read/write IA32_TSCDEADLINE in legacy timer modes */
	wrmsr(MSR_IA32_TSCDEADLINE, LARGE_TSCDEADLINE);
	tscdeadline = rdmsr(MSR_IA32_TSCDEADLINE);
	report("\tIA32_TSCDEADLINE read in periodic timer mode", tscdeadline == 0);

	/* Read/write legacy timer counters right after switching to TSC deadline mode */
	x2apic_write(APIC_LVTT, APIC_LVT_TIMER_TSCDEADLINE | APIC_LVT_MASKED | LVTT_TEST_VECTOR);
	tmict = x2apic_read(APIC_TMICT);
	tmcct = x2apic_read(APIC_TMCCT);
	report("\tTMICT read after switching to TSC deadline mode", tmict == 0);
	report("\tTMCCT read after switching to TSC deadline mode", tmcct == 0);

	/* Read/write TMICT & TMCCT in TSC deadline mode */
	x2apic_write(APIC_TMICT, LARGE_TMICT);
	tmict = x2apic_read(APIC_TMICT);
	tmcct = x2apic_read(APIC_TMCCT);
	report("\tTMICT read & write in TSC deadline mode", tmict == 0);
	report("\tTMCCT read after setting TMICT", tmcct == 0);

	/* Read/write TDCR in TSC deadline mode */
	x2apic_write(APIC_TDCR, APIC_TDR_DIV_32);
	tdcr = x2apic_read(APIC_TDCR);
	report("\tTDCR read & write in TSC deadline timer mode", tdcr == APIC_TDR_DIV_32);

	/* Read/write IA32_TSCDEADLINE in TSC deadline mode */
	wrmsr(MSR_IA32_TSCDEADLINE, LARGE_TSCDEADLINE);
	tscdeadline = rdmsr(MSR_IA32_TSCDEADLINE);
	report("\tIA32_TSCDEADLINE read in TSC deadline mode", tscdeadline == LARGE_TSCDEADLINE);

	/* Read/write legacy timer counters right after switching to reserved timer mode */
	x2apic_write(APIC_LVTT, APIC_LVT_TIMER_MASK | APIC_LVT_MASKED | LVTT_TEST_VECTOR);
	tmict = x2apic_read(APIC_TMICT);
	tmcct = x2apic_read(APIC_TMCCT);
	report("\tTMICT read after switching to TSC deadline mode", tmict == 0);
	report("\tTMCCT read after switching to TSC deadline mode", tmcct == 0);

	/* Write TMICT & TMCCT in reserved timer mode */
	x2apic_write(APIC_TMICT, LARGE_TMICT);
	tmict = x2apic_read(APIC_TMICT);
	tmcct = x2apic_read(APIC_TMCCT);
	report("\tTMICT read & write in TSC deadline mode", tmict == 0);
	report("\tTMCCT read after setting TMICT", tmcct == 0);

	/* Read/write TDCR in reserved timer mode */
	x2apic_write(APIC_TDCR, APIC_TDR_DIV_32);
	tdcr = x2apic_read(APIC_TDCR);
	report("\tTDCR read & write in reserved timer mode", tdcr == APIC_TDR_DIV_32);

	/* Read/write IA32_TSCDEADLINE in reserved timer mode */
	wrmsr(MSR_IA32_TSCDEADLINE, LARGE_TSCDEADLINE);
	tscdeadline = rdmsr(MSR_IA32_TSCDEADLINE);
	report("\tIA32_TSCDEADLINE read in reserved timer mode", tscdeadline == 0);
}

static void
check_timer_counter_persistence(void)
{
	u32 tmict, tmcct, tdcr;
	u64 tscdeadline;

	printf("Timer counters across-timer-mode persistence testing:\n");

	/* Persistence of legacy timer counters after switching to TSC deadline
	 * timer mode and then back to legacy modes */
	x2apic_write(APIC_LVTT, APIC_LVT_TIMER_PERIODIC | APIC_LVT_MASKED | LVTT_TEST_VECTOR);
	x2apic_write(APIC_TMICT, LARGE_TMICT);
	x2apic_write(APIC_TDCR, APIC_TDR_DIV_64);
	x2apic_write(APIC_LVTT, APIC_LVT_TIMER_TSCDEADLINE | APIC_LVT_MASKED | LVTT_TEST_VECTOR);
	x2apic_write(APIC_LVTT, APIC_LVT_TIMER_PERIODIC | APIC_LVT_MASKED | LVTT_TEST_VECTOR);
	tmict = x2apic_read(APIC_TMICT);
	tmcct = x2apic_read(APIC_TMCCT);
	tdcr = x2apic_read(APIC_TDCR);
	report("\tTMICT after -> TSC deadline -> periodic", tmict == 0);
	report("\tTMCCT after -> TSC deadline -> periodic", tmcct == 0);
	report("\tTDCR after -> TSC deadline -> periodic", tdcr == APIC_TDR_DIV_64);

	/* Persistence of legacy timer counters after switching to reserved
	 * timer mode and then back to legacy modes */
	x2apic_write(APIC_LVTT, APIC_LVT_TIMER_PERIODIC | APIC_LVT_MASKED | LVTT_TEST_VECTOR);
	x2apic_write(APIC_TMICT, LARGE_TMICT);
	x2apic_write(APIC_TDCR, APIC_TDR_DIV_64);
	x2apic_write(APIC_LVTT, APIC_LVT_TIMER_MASK | APIC_LVT_MASKED | LVTT_TEST_VECTOR);
	x2apic_write(APIC_LVTT, APIC_LVT_TIMER_PERIODIC | APIC_LVT_MASKED | LVTT_TEST_VECTOR);
	tmict = x2apic_read(APIC_TMICT);
	tmcct = x2apic_read(APIC_TMCCT);
	tdcr = x2apic_read(APIC_TDCR);
	report("\tTMICT after -> reserved -> periodic", tmict == 0);
	report("\tTMCCT after -> reserved -> periodic", tmcct == 0);
	report("\tTDCR after -> reserved -> periodic", tdcr == APIC_TDR_DIV_64);

	/* Persistence of IA32 TSCDEADLINE after switching to legacy timer
	 * modes and then back to TSC deadline mode */
	x2apic_write(APIC_LVTT, APIC_LVT_TIMER_TSCDEADLINE | APIC_LVT_MASKED | LVTT_TEST_VECTOR);
	wrmsr(MSR_IA32_TSCDEADLINE, LARGE_TSCDEADLINE);
	x2apic_write(APIC_LVTT, APIC_LVT_TIMER_PERIODIC | APIC_LVT_MASKED | LVTT_TEST_VECTOR);
	x2apic_write(APIC_LVTT, APIC_LVT_TIMER_TSCDEADLINE | APIC_LVT_MASKED | LVTT_TEST_VECTOR);
	tscdeadline = rdmsr(MSR_IA32_TSCDEADLINE);
	report("\tIA32_TSCDEADLINE after -> periodic -> TSC deadline", tscdeadline == 0);

	/* Persistence of IA32 TSCDEADLINE after switching to reserved timer
	 * mode and then back to TSC deadline mode */
	x2apic_write(APIC_LVTT, APIC_LVT_TIMER_TSCDEADLINE | APIC_LVT_MASKED | LVTT_TEST_VECTOR);
	wrmsr(MSR_IA32_TSCDEADLINE, LARGE_TSCDEADLINE);
	x2apic_write(APIC_LVTT, APIC_LVT_TIMER_MASK | APIC_LVT_MASKED | LVTT_TEST_VECTOR);
	x2apic_write(APIC_LVTT, APIC_LVT_TIMER_TSCDEADLINE | APIC_LVT_MASKED | LVTT_TEST_VECTOR);
	tscdeadline = rdmsr(MSR_IA32_TSCDEADLINE);
	report("\tIA32_TSCDEADLINE after -> reserved -> TSC deadline", tscdeadline == 0);
}

static void
check_spurious_interrupt(void)
{
	u64 tsc;

	lvtt_counter = 0;

	irq_disable();
	x2apic_write(APIC_LVTT, APIC_LVT_TIMER_TSCDEADLINE | LVTT_TEST_VECTOR);
	tsc = rdtsc();
	wrmsr(MSR_IA32_TSCDEADLINE, tsc + 10000);
	while (rdtsc() < tsc + 20000);
	dump_lvtt(x2apic_read(APIC_LVTT));
	irq_enable();
	report("\tNormal timer interrupt", lvtt_counter == 1);

	irq_disable();
	tsc = rdtsc();
	wrmsr(MSR_IA32_TSCDEADLINE, tsc + 10000);
	while (rdtsc() < tsc + 20000);
	x2apic_write(APIC_TASKPRI, LVTT_TEST_VECTOR + 0x20);
	dump_lvtt(x2apic_read(APIC_LVTT));
	irq_enable();
}

static void
check_tmcct_after_updating_tdcr(void)
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
}

int main(void)
{
	handle_irq(LVTT_TEST_VECTOR, lvtt_handler);
	handle_irq(SPURIOUS_VECTOR, spurious_handler);
	irq_enable();

	check_lvt_timer();
	check_timer_counters();
	check_timer_counter_persistence();
	check_spurious_interrupt();
	check_tmcct_after_updating_tdcr();

	return report_summary();
}
