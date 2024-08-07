/*
 * drivecpu.c - 6502 processor emulation of CBM disk drives.
 *
 * Written by
 *  Ettore Perazzoli <ettore@comm2000.it>
 *  Andreas Boose <viceteam@t-online.de>
 *
 * Patches by
 *  Andre Fachat <a.fachat@physik.tu-chemnitz.de>
 *
 * This file is part of VICE, the Versatile Commodore Emulator.
 * See README for copyright notice.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 *  02111-1307  USA.
 *
 */

#include "vice.h"

#include <stdio.h>
#include <string.h>

#include "6510core.h"
#include "alarm.h"
#include "clkguard.h"
#include "debug.h"
#include "drive.h"
#include "drivecpu.h"
#include "drive-check.h"
#include "drivemem.h"
#include "drivetypes.h"
#include "interrupt.h"
#include "lib.h"
#include "log.h"
#include "machine-drive.h"
#include "machine.h"
#include "mem.h"
#include "monitor.h"
#include "mos6510.h"
#include "rotation.h"
#include "snapshot.h"
#include "vicetypes.h"
#include "via.h"

#include "ViceWrapper.h"

#define DRIVE_CPU

//
// TODO: the c64 debugger has hardcoded drive context 0 only (drive 8, no multiple drives supported yet).
//       look at lines such drive_context[0]->via1d1541->c64d_irq_flagged ...

int _c64d_new_drive_pc = -1;
int _c64d_new_drive_dnr = -1;

//

/* Global clock counters.  */
CLOCK drive_clk[DRIVE_NUM];

static void drive_jam(drive_context_t *drv);

static void drivecpu_set_bank_base(void *context);

static interrupt_cpu_status_t *drivecpu_int_status_ptr[DRIVE_NUM];

void drivecpu_setup_context(struct drive_context_s *drv, int i)
{
    monitor_interface_t *mi;
    drivecpu_context_t *cpu;

    if (i) {
        drv->cpu = lib_calloc(1, sizeof(drivecpu_context_t));
    }
    cpu = drv->cpu;

    if (i) {
        drv->cpud = lib_calloc(1, sizeof(drivecpud_context_t));
        drv->func = lib_malloc(sizeof(drivefunc_context_t));

        cpu->int_status = interrupt_cpu_status_new();
        interrupt_cpu_status_init(cpu->int_status, &(cpu->last_opcode_info));
    }
    drivecpu_int_status_ptr[drv->mynumber] = cpu->int_status;

    cpu->rmw_flag = 0;
    cpu->d_bank_limit = 0;
    cpu->d_bank_start = 0;
    cpu->pageone = NULL;
    if (i) {
        cpu->snap_module_name = lib_msprintf("DRIVECPU%d", drv->mynumber);
        cpu->identification_string = lib_msprintf("DRIVE#%d", drv->mynumber + 8);
        cpu->monitor_interface = monitor_interface_new();
    }
    mi = cpu->monitor_interface;
    mi->context = (void *)drv;
    mi->cpu_regs = &(cpu->cpu_regs);
    mi->cpu_R65C02_regs = NULL;
    mi->cpu_65816_regs = NULL;
    mi->dtv_cpu_regs = NULL;
    mi->z80_cpu_regs = NULL;
    mi->h6809_cpu_regs = NULL;
    mi->int_status = cpu->int_status;
    mi->clk = &(drive_clk[drv->mynumber]);
    mi->current_bank = 0;
    mi->mem_bank_list = NULL;
    mi->mem_bank_from_name = NULL;
    mi->get_line_cycle = NULL;
    mi->mem_bank_read = drivemem_bank_read;
    mi->mem_bank_peek = drivemem_bank_peek;
    mi->mem_bank_write = drivemem_bank_store;
    mi->mem_ioreg_list_get = drivemem_ioreg_list_get;
    mi->toggle_watchpoints_func = drivemem_toggle_watchpoints;
    mi->set_bank_base = drivecpu_set_bank_base;
    cpu->monspace = monitor_diskspace_mem(drv->mynumber);

    if (i) {
        drv->cpu->clk_guard = clk_guard_new(drv->clk_ptr, CLOCK_MAX - CLKGUARD_SUB_MIN);

        drv->cpu->alarm_context = alarm_context_new(drv->cpu->identification_string);
    }
}

/* ------------------------------------------------------------------------- */

#define LOAD(a)           (*drv->cpud->read_func_ptr[(a) >> 8])(drv, (WORD)(a))
#define LOAD_ZERO(a)      (*drv->cpud->read_func_ptr[0])(drv, (WORD)(a))
#define LOAD_ADDR(a)      (LOAD((a)) | (LOAD((a) + 1) << 8))
#define LOAD_ZERO_ADDR(a) (LOAD_ZERO((a)) | (LOAD_ZERO((a) + 1) << 8))
#define STORE(a, b)       (*drv->cpud->store_func_ptr[(a) >> 8])(drv, (WORD)(a), (BYTE)(b))
#define STORE_ZERO(a, b)  (*drv->cpud->store_func_ptr[0])(drv, (WORD)(a), (BYTE)(b))

#define JUMP(addr)                                                         \
    do {                                                                   \
        reg_pc = (unsigned int)(addr);                                     \
        if (reg_pc >= cpu->d_bank_limit || reg_pc < cpu->d_bank_start) {   \
            BYTE *p = drv->cpud->read_base_tab_ptr[reg_pc >> 8];           \
            cpu->d_bank_base = p;                                          \
                                                                           \
            if (p != NULL) {                                               \
                DWORD limits = drv->cpud->read_limit_tab_ptr[reg_pc >> 8]; \
                cpu->d_bank_limit = limits & 0xffff;                       \
                cpu->d_bank_start = limits >> 16;                          \
            } else {                                                       \
                cpu->d_bank_start = 0;                                     \
                cpu->d_bank_limit = 0;                                     \
            }                                                              \
        }                                                                  \
    } while (0)

/* ------------------------------------------------------------------------- */

static void cpu_reset(drive_context_t *drv)
{
    int preserve_monitor;

    preserve_monitor = drv->cpu->int_status->global_pending_int & IK_MONITOR;

    log_message(drv->drive->log, "RESET.");

    interrupt_cpu_status_reset(drv->cpu->int_status);

    *(drv->clk_ptr) = 6;
    rotation_reset(drv->drive);
    machine_drive_reset(drv);

    if (preserve_monitor) {
        interrupt_monitor_trap_on(drv->cpu->int_status);
    }
}

void drivecpu_reset_clk(drive_context_t *drv)
{
    drv->cpu->last_clk = maincpu_clk;
    drv->cpu->last_exc_cycles = 0;
    drv->cpu->stop_clk = 0;
}

void drivecpu_reset(drive_context_t *drv)
{
    int preserve_monitor;

    *(drv->clk_ptr) = 0;
    drivecpu_reset_clk(drv);

    preserve_monitor = drv->cpu->int_status->global_pending_int & IK_MONITOR;

    interrupt_cpu_status_reset(drv->cpu->int_status);

    if (preserve_monitor) {
        interrupt_monitor_trap_on(drv->cpu->int_status);
    }

    /* FIXME -- ugly, should be changed in interrupt.h */
    interrupt_trigger_reset(drv->cpu->int_status, *(drv->clk_ptr));
}

void drivecpu_trigger_reset(unsigned int dnr)
{
    interrupt_trigger_reset(drivecpu_int_status_ptr[dnr], drive_clk[dnr] + 1);
}

void drivecpu_set_overflow(drive_context_t *drv)
{
    drivecpu_context_t *cpu = drv->cpu;
    cpu->cpu_regs.p |= P_OVERFLOW;
}

void drivecpu_shutdown(drive_context_t *drv)
{
    drivecpu_context_t *cpu;

    cpu = drv->cpu;

    if (cpu->alarm_context != NULL) {
        alarm_context_destroy(cpu->alarm_context);
    }
    if (cpu->clk_guard != NULL) {
        clk_guard_destroy(cpu->clk_guard);
    }

    monitor_interface_destroy(cpu->monitor_interface);
    interrupt_cpu_status_destroy(cpu->int_status);

    lib_free(cpu->snap_module_name);
    lib_free(cpu->identification_string);

    machine_drive_shutdown(drv);

    lib_free(drv->func);
    lib_free(drv->cpud);
    lib_free(cpu);
}

void drivecpu_init(drive_context_t *drv, int type)
{
    drivemem_init(drv, type);
    drivecpu_reset(drv);
}

inline void drivecpu_wake_up(drive_context_t *drv)
{
    /* FIXME: this value could break some programs, or be way too high for
       others.  Maybe we should put it into a user-definable resource.  */
    if (maincpu_clk - drv->cpu->last_clk > 0xffffff
        && *(drv->clk_ptr) > 934639) {
        log_message(drv->drive->log, "Skipping cycles.");
        drv->cpu->last_clk = maincpu_clk;
    }
}

inline void drivecpu_sleep(drive_context_t *drv)
{
    /* Currently does nothing.  But we might need this hook some day.  */
}

/* Make sure the drive clock counters never overflow; return nonzero if
   they have been decremented to prevent overflow.  */
CLOCK drivecpu_prevent_clk_overflow(drive_context_t *drv, CLOCK sub)
{
    if (sub != 0) {
        /* First, get in sync with what the main CPU has done.  Notice that
           `clk' has already been decremented at this point.  */
        if (drv->drive->enable) {
            if (drv->cpu->last_clk < sub) {
                /* Hm, this is kludgy.  :-(  */
                drive_cpu_execute_all(maincpu_clk + sub);
            }
            drv->cpu->last_clk -= sub;
        } else {
            drv->cpu->last_clk = maincpu_clk;
        }
    }

    /* Then, check our own clock counters.  */
    return clk_guard_prevent_overflow(drv->cpu->clk_guard);
}

/* Handle a ROM trap. */
inline static DWORD drive_trap_handler(drive_context_t *drv)
{
    if (MOS6510_REGS_GET_PC(&(drv->cpu->cpu_regs)) == (WORD)drv->drive->trap) {
        MOS6510_REGS_SET_PC(&(drv->cpu->cpu_regs), drv->drive->trapcont);
        if (drv->drive->idling_method == DRIVE_IDLE_TRAP_IDLE) {
            CLOCK next_clk;

            next_clk = alarm_context_next_pending_clk(drv->cpu->alarm_context);

            if (next_clk > drv->cpu->stop_clk) {
                next_clk = drv->cpu->stop_clk;
            }

            *(drv->clk_ptr) = next_clk;
        }
        return 0;
    }
    return (DWORD)-1;
}

static void drive_generic_dma(void)
{
    /* Generic DMA hosts can be implemented here.
       Not very likey for disk drives. */
}

/* -------------------------------------------------------------------------- */

/* Return nonzero if a pending NMI should be dispatched now.  This takes
   account for the internal delays of the 6510, but does not actually check
   the status of the NMI line.  */
inline static int interrupt_check_nmi_delay(interrupt_cpu_status_t *cs,
                                            CLOCK cpu_clk)
{
    CLOCK nmi_clk = cs->nmi_clk + INTERRUPT_DELAY;

    /* BRK (0x00) delays the NMI by one opcode.  */
    /* TODO DO_INTERRUPT sets last opcode to 0: can NMI occur right after IRQ? */
    if (OPINFO_NUMBER(*cs->last_opcode_info_ptr) == 0x00) {
        return 0;
    }

    /* Branch instructions delay IRQs and NMI by one cycle if branch
       is taken with no page boundary crossing.  */
    if (OPINFO_DELAYS_INTERRUPT(*cs->last_opcode_info_ptr)) {
        nmi_clk++;
    }

    if (cpu_clk >= nmi_clk) {
        return 1;
    }

    return 0;
}

/* Return nonzero if a pending IRQ should be dispatched now.  This takes
   account for the internal delays of the 6510, but does not actually check
   the status of the IRQ line.  */
inline static int interrupt_check_irq_delay(interrupt_cpu_status_t *cs,
                                            CLOCK cpu_clk)
{
    CLOCK irq_clk = cs->irq_clk + INTERRUPT_DELAY;

    /* Branch instructions delay IRQs and NMI by one cycle if branch
       is taken with no page boundary crossing.  */
    if (OPINFO_DELAYS_INTERRUPT(*cs->last_opcode_info_ptr)) {
        irq_clk++;
    }

    /* If an opcode changes the I flag from 1 to 0, the 6510 needs
       one more opcode before it triggers the IRQ routine.  */
    if (cpu_clk >= irq_clk) {
        if (!OPINFO_ENABLES_IRQ(*cs->last_opcode_info_ptr)) {
            return 1;
        } else {
            cs->global_pending_int |= IK_IRQPEND;
        }
    }
    return 0;
}

/* MPi: For some reason MSVC is generating a compiler fatal error when optimising this function? */
#ifdef _MSC_VER
#pragma optimize("",off)
#endif
/* -------------------------------------------------------------------------- */
/* Execute up to the current main CPU clock value.  This automatically
   calculates the corresponding number of clock ticks in the drive.  */

#define reg_a   (cpu->cpu_regs.a)
#define reg_x   (cpu->cpu_regs.x)
#define reg_y   (cpu->cpu_regs.y)
#define reg_pc  (cpu->cpu_regs.pc)
#define reg_sp  (cpu->cpu_regs.sp)
#define reg_p   (cpu->cpu_regs.p)
#define flag_z  (cpu->cpu_regs.z)
#define flag_n  (cpu->cpu_regs.n)

void c64d_get_drivecpu_regs_internal(drive_context_t *drv, uint8 *a, uint8 *x, uint8 *y, uint8 *p, uint8 *sp, uint16 *pc)
{
	drivecpu_context_t *cpu = drv->cpu;
	
	if (cpu != NULL)
	{
		*a = reg_a;
		*x = reg_x;
		*y = reg_y;
		*p = reg_p;
		*sp = reg_sp;
		*pc = reg_pc;
	}
	else
	{
		*a = 0;
		*x = 0;
		*y = 0;
		*p = 0;
		*sp = 0;
		*pc = 0;
	}
}


void drivecpu_execute(drive_context_t *drv, CLOCK clk_value)
{
    CLOCK cycles;
    int tcycles;
    drivecpu_context_t *cpu;

    cpu = drv->cpu;

    drivecpu_wake_up(drv);

    /* Calculate number of main CPU clocks to emulate */
    if (clk_value > cpu->last_clk) {
        cycles = clk_value - cpu->last_clk;
    } else {
        cycles = 0;
    }

    while (cycles != 0) {
        tcycles = cycles > 10000 ? 10000 : cycles;
        cycles -= tcycles;

        cpu->cycle_accum += drv->cpud->sync_factor * tcycles;
        cpu->stop_clk += cpu->cycle_accum >> 16;
        cpu->cycle_accum &= 0xffff;
    }

    /* Run drive CPU emulation until the stop_clk clock has been reached.
     * There appears to be a nasty 32-bit overflow problem here, so we
     * paper over it by only considering subtractions of 2nd complement
     * integers. */
    while ((int) (*(drv->clk_ptr) - cpu->stop_clk) < 0) {
/* Include the 6502/6510 CPU emulation core.  */

#define CLK (*(drv->clk_ptr))
#define RMW_FLAG (cpu->rmw_flag)
#define PAGE_ONE (cpu->pageone)
#define LAST_OPCODE_INFO (cpu->last_opcode_info)
#define LAST_OPCODE_ADDR (cpu->last_opcode_addr)
#define TRACEFLG (debug.drivecpu_traceflg[drv->mynumber])

#define CPU_INT_STATUS (cpu->int_status)

#define ALARM_CONTEXT (cpu->alarm_context)

#define JAM() drive_jam(drv)

#define ROM_TRAP_ALLOWED() 1

#define ROM_TRAP_HANDLER() drive_trap_handler(drv)

#define CALLER (cpu->monspace)

#define DMA_FUNC drive_generic_dma()

#define DMA_ON_RESET

#define drivecpu_byte_ready_egde_clear()  \
    do {                                  \
        drv->drive->byte_ready_edge = 0;  \
    } while (0)

#define drivecpu_rotate()                 \
    do {                                  \
        rotation_rotate_disk(drv->drive); \
    } while (0)

#define drivecpu_byte_ready() (drv->drive->byte_ready_edge)

#define cpu_reset() (cpu_reset)(drv)
#define bank_limit (cpu->d_bank_limit)
#define bank_start (cpu->d_bank_start)
#define bank_base (cpu->d_bank_base)

		
		
		
//#include "6510core.c"
		
///
///
/// 6510core.c starts here
		
		/*
		 * 6510core.c - MOS6510 emulation core.
		 *
		 * Written by
		 *  Ettore Perazzoli <ettore@comm2000.it>
		 *  Andreas Boose <viceteam@t-online.de>
		 *
		 * DTV sections written by
		 *  M.Kiesel <mayne@users.sourceforge.net>
		 *  Hannu Nuotio <hannu.nuotio@tut.fi>
		 *
		 * This file is part of VICE, the Versatile Commodore Emulator.
		 * See README for copyright notice.
		 *
		 *  This program is free software; you can redistribute it and/or modify
		 *  it under the terms of the GNU General Public License as published by
		 *  the Free Software Foundation; either version 2 of the License, or
		 *  (at your option) any later version.
		 *
		 *  This program is distributed in the hope that it will be useful,
		 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
		 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
		 *  GNU General Public License for more details.
		 *
		 *  You should have received a copy of the GNU General Public License
		 *  along with this program; if not, write to the Free Software
		 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
		 *  02111-1307  USA.
		 *
		 */
		
		/* This file is included by CPU definition files */
		/* (maincpu.c, drivecpu.c, ...) */
		
#ifdef DRIVE_CPU
#define CPU_STR "Drive CPU"
#else
#define CPU_STR "Main CPU"
#endif
		
#include "traps.h"
		
#ifndef C64DTV
		/* The C64DTV can use different shadow registers for accu read/write. */
		/* For standard 6510, this is not the case. */
#define reg_a_write(c) reg_a = (c)
#define reg_a_read  reg_a
#define reg_x_write(c) reg_x = (c)
#define reg_x_read  reg_x
#define reg_y_write(c) reg_y = (c)
#define reg_y_read  reg_y
		
		/* Opcode execution time may vary on the C64DTV. */
#define CLK_RTS 3
#define CLK_RTI 4
#define CLK_BRK 5
#define CLK_ABS_I_STORE2 2
#define CLK_STACK_PUSH 1
#define CLK_STACK_PULL 2
#define CLK_ABS_RMW2 3
#define CLK_ABS_I_RMW2 3
#define CLK_ZERO_I_STORE 2
#define CLK_ZERO_I2 2
#define CLK_ZERO_RMW 3
#define CLK_ZERO_I_RMW 4
#define CLK_IND_X_RMW 3
#define CLK_IND_Y_RMW1 1
#define CLK_IND_Y_RMW2 3
#define CLK_BRANCH2 1
#define CLK_INT_CYCLE 1
#define CLK_JSR_INT_CYCLE 1
#define CLK_IND_Y_W 2
#define CLK_NOOP_ZERO_X 2
		
#define IRQ_CYCLES      7
#define NMI_CYCLES      7
#endif
#define RESET_CYCLES    6
		
		/* ------------------------------------------------------------------------- */
		/* Backup for non-6509 CPUs.  */
		
#ifndef LOAD_IND
#define LOAD_IND(a)     LOAD(a)
#endif
#ifndef STORE_IND
#define STORE_IND(a, b)  STORE(a, b)
#endif
		
		/* ------------------------------------------------------------------------- */
		/* Backup for non-variable cycle CPUs.  */
		
#ifndef CLK_ADD
#define CLK_ADD(clock, amount) clock += (amount)
#endif
		
#ifndef REWIND_FETCH_OPCODE
#define REWIND_FETCH_OPCODE(clock) clock -= 2
#endif
		
		/* ------------------------------------------------------------------------- */
		/* Hook for additional delay.  */
		
#ifndef CPU_DELAY_CLK
#define CPU_DELAY_CLK
#endif
		
#ifndef CPU_REFRESH_CLK
#define CPU_REFRESH_CLK
#endif
		
		/* ------------------------------------------------------------------------- */
		
#ifndef CYCLE_EXACT_ALARM
#define PROCESS_ALARMS                                             \
while (CLK >= alarm_context_next_pending_clk(ALARM_CONTEXT)) { \
alarm_context_dispatch(ALARM_CONTEXT, CLK);                \
CPU_DELAY_CLK                                              \
}
#else
#define PROCESS_ALARMS
#endif
		
		/* ------------------------------------------------------------------------- */
		
#define LOCAL_SET_NZ(val) (flag_z = flag_n = (val))
		
#if defined DRIVE_CPU
#define LOCAL_SET_OVERFLOW(val)               \
do {                                      \
if (val) {                            \
reg_p |= P_OVERFLOW;              \
} else {                              \
drivecpu_rotate();                \
drivecpu_byte_ready_egde_clear(); \
reg_p &= ~P_OVERFLOW;             \
}                                     \
} while (0)
#else
#define LOCAL_SET_OVERFLOW(val)   \
do {                          \
if (val) {                \
reg_p |= P_OVERFLOW;  \
} else {                  \
reg_p &= ~P_OVERFLOW; \
}                         \
} while (0)
#endif
		
#define LOCAL_SET_BREAK(val)   \
do {                       \
if (val) {             \
reg_p |= P_BREAK;  \
} else {               \
reg_p &= ~P_BREAK; \
}                      \
} while (0)
		
#define LOCAL_SET_DECIMAL(val)   \
do {                         \
if (val) {               \
reg_p |= P_DECIMAL;  \
} else {                 \
reg_p &= ~P_DECIMAL; \
}                        \
} while (0)
		
#define LOCAL_SET_INTERRUPT(val)   \
do {                           \
if (val) {                 \
reg_p |= P_INTERRUPT;  \
} else {                   \
reg_p &= ~P_INTERRUPT; \
}                          \
} while (0)
		
#define LOCAL_SET_CARRY(val)   \
do {                       \
if (val) {             \
reg_p |= P_CARRY;  \
} else {               \
reg_p &= ~P_CARRY; \
}                      \
} while (0)
		
#define LOCAL_SET_SIGN(val)      (flag_n = (val) ? 0x80 : 0)
#define LOCAL_SET_ZERO(val)      (flag_z = !(val))
#define LOCAL_SET_STATUS(val)    (reg_p = ((val) & ~(P_ZERO | P_SIGN)), \
LOCAL_SET_ZERO((val) & P_ZERO),       \
flag_n = (val))
		
#define LOCAL_OVERFLOW()         (reg_p & P_OVERFLOW)
#define LOCAL_BREAK()            (reg_p & P_BREAK)
#define LOCAL_DECIMAL()          (reg_p & P_DECIMAL)
#define LOCAL_INTERRUPT()        (reg_p & P_INTERRUPT)
#define LOCAL_CARRY()            (reg_p & P_CARRY)
#define LOCAL_SIGN()             (flag_n & 0x80)
#define LOCAL_ZERO()             (!flag_z)
#define LOCAL_STATUS()           (reg_p | (flag_n & 0x80) | P_UNUSED    \
| (LOCAL_ZERO() ? P_ZERO : 0))
		
#ifdef LAST_OPCODE_INFO
		
		/* If requested, gather some info about the last executed opcode for timing
   purposes.  */
		
		/* Remember the number of the last opcode.  By default, the opcode does not
   delay interrupt and does not change the I flag.  */
#define SET_LAST_OPCODE(x) OPINFO_SET(LAST_OPCODE_INFO, (x), 0, 0, 0)
		
		/* Remember that the last opcode delayed a pending IRQ or NMI by one cycle.  */
#define OPCODE_DELAYS_INTERRUPT() OPINFO_SET_DELAYS_INTERRUPT(LAST_OPCODE_INFO, 1)
		
		/* Remember that the last opcode changed the I flag from 0 to 1, so we have
   to dispatch an IRQ even if the I flag is 0 when we check it.  */
#define OPCODE_DISABLES_IRQ() OPINFO_SET_DISABLES_IRQ(LAST_OPCODE_INFO, 1)
		
		/* Remember that the last opcode changed the I flag from 1 to 0, so we must
   not dispatch an IRQ even if the I flag is 1 when we check it.  */
#define OPCODE_ENABLES_IRQ() OPINFO_SET_ENABLES_IRQ(LAST_OPCODE_INFO, 1)
		
#else
		
		/* Info about the last opcode is not needed.  */
#define SET_LAST_OPCODE(x)
#define OPCODE_DELAYS_INTERRUPT()
#define OPCODE_DISABLES_IRQ()
#define OPCODE_ENABLES_IRQ()
		
#endif
		
#ifdef LAST_OPCODE_ADDR
#define SET_LAST_ADDR(x) LAST_OPCODE_ADDR = (x)
#else
#error "please define LAST_OPCODE_ADDR"
#endif
		
#ifndef DRIVE_CPU
		
#ifndef C64DTV
		/* Export the local version of the registers.  */
#define EXPORT_REGISTERS()          \
do {                            \
GLOBAL_REGS.pc = reg_pc;    \
GLOBAL_REGS.a = reg_a_read; \
GLOBAL_REGS.x = reg_x_read; \
GLOBAL_REGS.y = reg_y_read; \
GLOBAL_REGS.sp = reg_sp;    \
GLOBAL_REGS.p = reg_p;      \
GLOBAL_REGS.n = flag_n;     \
GLOBAL_REGS.z = flag_z;     \
} while (0)
		
		/* Import the public version of the registers.  */
#define IMPORT_REGISTERS()                                 \
do {                                                   \
reg_a_write(GLOBAL_REGS.a); /*TODO*/               \
reg_x_write(GLOBAL_REGS.x);                        \
reg_y_write(GLOBAL_REGS.y);                        \
reg_sp = GLOBAL_REGS.sp;                           \
reg_p = GLOBAL_REGS.p;                             \
flag_n = GLOBAL_REGS.n;                            \
flag_z = GLOBAL_REGS.z;                            \
bank_start = bank_limit = 0; /* prevent caching */ \
JUMP(GLOBAL_REGS.pc);                              \
} while (0)
#else  /* C64DTV */
		
		/* Export the local version of the registers.  */
#define EXPORT_REGISTERS()                                           \
do {                                                             \
GLOBAL_REGS.pc = reg_pc;                                     \
GLOBAL_REGS.a = dtv_registers[0];                            \
GLOBAL_REGS.x = dtv_registers[2];                            \
GLOBAL_REGS.y = dtv_registers[1];                            \
GLOBAL_REGS.sp = reg_sp;                                     \
GLOBAL_REGS.p = reg_p;                                       \
GLOBAL_REGS.n = flag_n;                                      \
GLOBAL_REGS.z = flag_z;                                      \
GLOBAL_REGS.r3 = dtv_registers[3];                           \
GLOBAL_REGS.r4 = dtv_registers[4];                           \
GLOBAL_REGS.r5 = dtv_registers[5];                           \
GLOBAL_REGS.r6 = dtv_registers[6];                           \
GLOBAL_REGS.r7 = dtv_registers[7];                           \
GLOBAL_REGS.r8 = dtv_registers[8];                           \
GLOBAL_REGS.r9 = dtv_registers[9];                           \
GLOBAL_REGS.r10 = dtv_registers[10];                         \
GLOBAL_REGS.r11 = dtv_registers[11];                         \
GLOBAL_REGS.r12 = dtv_registers[12];                         \
GLOBAL_REGS.r13 = dtv_registers[13];                         \
GLOBAL_REGS.r14 = dtv_registers[14];                         \
GLOBAL_REGS.r15 = dtv_registers[15];                         \
GLOBAL_REGS.acm = (reg_a_write_idx << 4) | (reg_a_read_idx); \
GLOBAL_REGS.yxm = (reg_y_idx << 4) | (reg_x_idx);            \
} while (0)
		
		/* Import the public version of the registers.  */
#define IMPORT_REGISTERS()                                 \
do {                                                   \
dtv_registers[0] = GLOBAL_REGS.a;                  \
dtv_registers[2] = GLOBAL_REGS.x;                  \
dtv_registers[1] = GLOBAL_REGS.y;                  \
reg_sp = GLOBAL_REGS.sp;                           \
reg_p = GLOBAL_REGS.p;                             \
flag_n = GLOBAL_REGS.n;                            \
flag_z = GLOBAL_REGS.z;                            \
dtv_registers[3] = GLOBAL_REGS.r3;                 \
dtv_registers[4] = GLOBAL_REGS.r4;                 \
dtv_registers[5] = GLOBAL_REGS.r5;                 \
dtv_registers[6] = GLOBAL_REGS.r6;                 \
dtv_registers[7] = GLOBAL_REGS.r7;                 \
dtv_registers[8] = GLOBAL_REGS.r8;                 \
dtv_registers[9] = GLOBAL_REGS.r9;                 \
dtv_registers[10] = GLOBAL_REGS.r10;               \
dtv_registers[11] = GLOBAL_REGS.r11;               \
dtv_registers[12] = GLOBAL_REGS.r12;               \
dtv_registers[13] = GLOBAL_REGS.r13;               \
dtv_registers[14] = GLOBAL_REGS.r14;               \
dtv_registers[15] = GLOBAL_REGS.r15;               \
reg_a_write_idx = GLOBAL_REGS.acm >> 4;            \
reg_a_read_idx = GLOBAL_REGS.acm & 0xf;            \
reg_y_idx = GLOBAL_REGS.yxm >> 4;                  \
reg_x_idx = GLOBAL_REGS.yxm & 0xf;                 \
bank_start = bank_limit = 0; /* prevent caching */ \
JUMP(GLOBAL_REGS.pc);                              \
} while (0)
		
#endif /* C64DTV */
#else  /* DRIVE_CPU */
#define IMPORT_REGISTERS()
#define EXPORT_REGISTERS()
#endif /* !DRIVE_CPU */
		
		/* Stack operations. */
		
#ifndef PUSH
#define PUSH(val) ((PAGE_ONE)[(reg_sp--)] = ((BYTE)(val)))
#endif
#ifndef PULL
#define PULL()    ((PAGE_ONE)[(++reg_sp)])
#endif
		
#ifdef VICE_DEBUG
#define TRACE_NMI(clk)                        \
do {                                      \
if (TRACEFLG) {                       \
debug_nmi(CPU_INT_STATUS, (clk)); \
}                                     \
} while (0)
		
#define TRACE_IRQ(clk)                        \
do {                                      \
if (TRACEFLG) {                       \
debug_irq(CPU_INT_STATUS, (clk)); \
}                                     \
} while (0)
		
#define TRACE_BRK()                \
do {                           \
if (TRACEFLG) {            \
debug_text("*** BRK"); \
}                          \
} while (0)
#else
#define TRACE_NMI(clk)
#define TRACE_IRQ(clk)
#define TRACE_BRK()
#endif
		
		/* Perform the interrupts in `int_kind'.  If we have both NMI and IRQ,
   execute NMI. NMI can take over an in progress IRQ.  */
		/* FIXME: LOCAL_STATUS() should check byte ready first.  */
#define DO_INTERRUPT(int_kind)                                                                 \
do {                                                                                       \
BYTE ik = (int_kind);                                                                  \
\
if (ik & (IK_IRQ | IK_IRQPEND | IK_NMI)) {                                             \
if (((ik & IK_NMI)                                                                 \
&& interrupt_check_nmi_delay(CPU_INT_STATUS, CLK))                            \
|| ((ik & (IK_IRQ | IK_IRQPEND)) && (!LOCAL_INTERRUPT()                        \
|| OPINFO_DISABLES_IRQ(LAST_OPCODE_INFO)) \
&& interrupt_check_irq_delay(CPU_INT_STATUS, CLK))) {                      \
if (monitor_mask[CALLER] & (MI_STEP)) {                                        \
monitor_check_icount_interrupt();                                          \
}                                                                              \
if (NMI_CYCLES == 7) {                                                         \
FETCH_PARAM(reg_pc);   /* dummy reads */                                   \
CLK_ADD(CLK, 1);                                                           \
FETCH_PARAM(reg_pc);                                                       \
CLK_ADD(CLK, 1);                                                           \
}                                                                              \
LOCAL_SET_BREAK(0);                                                            \
PUSH(reg_pc >> 8);                                                             \
PUSH(reg_pc & 0xff);                                                           \
CLK_ADD(CLK, 2);                                                               \
PUSH(LOCAL_STATUS());                                                          \
CLK_ADD(CLK, 1);                                                               \
LOCAL_SET_INTERRUPT(1);                                                        \
CPU_DELAY_CLK; /* process alarms for cartridge freeze */                       \
PROCESS_ALARMS;                                                                \
if ((CPU_INT_STATUS->global_pending_int & IK_NMI)                              \
&& (CLK >= (CPU_INT_STATUS->nmi_clk + INTERRUPT_DELAY))) {                 \
TRACE_NMI(CLK - NMI_CYCLES + 2);                                           \
interrupt_ack_nmi(CPU_INT_STATUS);                                         \
JUMP(LOAD_ADDR(0xfffa));                                                   \
} else {                                                                       \
TRACE_IRQ(CLK - IRQ_CYCLES + 2);                                           \
interrupt_ack_irq(CPU_INT_STATUS);                                         \
JUMP(LOAD_ADDR(0xfffe));                                                   \
}                                                                              \
SET_LAST_OPCODE(0);                                                            \
CLK_ADD(CLK, 2);                                                               \
}                                                                                  \
}                                                                                      \
if (ik & (IK_TRAP | IK_RESET)) {                                                       \
if (ik & IK_TRAP) {                                                                \
EXPORT_REGISTERS();                                                            \
interrupt_do_trap(CPU_INT_STATUS, (WORD)reg_pc);                               \
IMPORT_REGISTERS();                                                            \
if (CPU_INT_STATUS->global_pending_int & IK_RESET) {                           \
ik |= IK_RESET;                                                            \
}                                                                              \
}                                                                                  \
if (ik & IK_RESET) {                                                               \
interrupt_ack_reset(CPU_INT_STATUS);                                           \
cpu_reset();                                                                   \
bank_start = bank_limit = 0; /* prevent caching */                             \
JUMP(LOAD_ADDR(0xfffc));                                                       \
DMA_ON_RESET;                                                                  \
}                                                                                  \
}                                                                                      \
if (ik & (IK_MONITOR | IK_DMA)) {                                                      \
if (ik & IK_MONITOR) {                                                             \
if (monitor_force_import(CALLER)) {                                            \
IMPORT_REGISTERS();                                                        \
}                                                                              \
if (monitor_mask[CALLER]) {                                                    \
EXPORT_REGISTERS();                                                        \
}                                                                              \
if (monitor_mask[CALLER] & (MI_STEP)) {                                        \
monitor_check_icount((WORD)reg_pc);                                        \
IMPORT_REGISTERS();                                                        \
}                                                                              \
if (monitor_mask[CALLER] & (MI_BREAK)) {                                       \
if (monitor_check_breakpoints(CALLER, (WORD)reg_pc)) {                     \
monitor_startup(CALLER);                                               \
IMPORT_REGISTERS();                                                    \
}                                                                          \
}                                                                              \
if (monitor_mask[CALLER] & (MI_WATCH)) {                                       \
monitor_check_watchpoints(LAST_OPCODE_ADDR, (WORD)reg_pc);                 \
IMPORT_REGISTERS();                                                        \
}                                                                              \
}                                                                                  \
if (ik & IK_DMA) {                                                                 \
EXPORT_REGISTERS();                                                            \
DMA_FUNC;                                                                      \
interrupt_ack_dma(CPU_INT_STATUS);                                             \
IMPORT_REGISTERS();                                                            \
}                                                                                  \
}                                                                                      \
} while (0)
		
		/* ------------------------------------------------------------------------- */
		
		/* Addressing modes.  For convenience, page boundary crossing cycles and
   ``idle'' memory reads are handled here as well. */
		
#define FETCH_PARAM(addr) ((((int)(addr)) < bank_limit) ? bank_base[(addr)] : LOAD(addr))
		
#define LOAD_ABS(addr) LOAD(addr)
		
#define LOAD_ABS_X(addr)                                          \
((((addr) & 0xff) + reg_x_read) > 0xff                        \
? (LOAD(((addr) & 0xff00) | (((addr) + reg_x_read) & 0xff)), \
CLK_ADD(CLK, CLK_INT_CYCLE),                              \
LOAD((addr) + reg_x_read))                                \
: LOAD((addr) + reg_x_read))
		
#define LOAD_ABS_X_RMW(addr)                                   \
(LOAD(((addr) & 0xff00) | (((addr) + reg_x_read) & 0xff)), \
CLK_ADD(CLK, CLK_INT_CYCLE),                              \
LOAD((addr) + reg_x_read))
		
#define LOAD_ABS_Y(addr)                                          \
((((addr) & 0xff) + reg_y_read) > 0xff                        \
? (LOAD(((addr) & 0xff00) | (((addr) + reg_y_read) & 0xff)), \
CLK_ADD(CLK, CLK_INT_CYCLE),                              \
LOAD((addr) + reg_y_read))                                \
: LOAD((addr) + reg_y_read))
		
#define LOAD_ABS_Y_RMW(addr)                                   \
(LOAD(((addr) & 0xff00) | (((addr) + reg_y_read) & 0xff)), \
CLK_ADD(CLK, CLK_INT_CYCLE),                              \
LOAD((addr) + reg_y_read))
		
#define LOAD_IND_X(addr) (CLK_ADD(CLK, 3), LOAD(LOAD_ZERO_ADDR((addr) + reg_x_read)))
		
#define LOAD_IND_Y(addr)                                                    \
(CLK_ADD(CLK, 2), ((LOAD_ZERO_ADDR((addr)) & 0xff) + reg_y_read) > 0xff \
? (LOAD((LOAD_ZERO_ADDR((addr)) & 0xff00)                              \
| ((LOAD_ZERO_ADDR((addr)) + reg_y_read) & 0xff)),             \
CLK_ADD(CLK, CLK_INT_CYCLE),                                        \
LOAD(LOAD_ZERO_ADDR((addr)) + reg_y_read))                          \
: LOAD(LOAD_ZERO_ADDR((addr)) + reg_y_read))
		
#define LOAD_ZERO_X(addr) (LOAD_ZERO((addr) + reg_x_read))
		
#define LOAD_ZERO_Y(addr) (LOAD_ZERO((addr) + reg_y_read))
		
#define LOAD_IND_Y_BANK(addr)                                               \
(CLK_ADD(CLK, 2), ((LOAD_ZERO_ADDR((addr)) & 0xff) + reg_y_read) > 0xff \
? (LOAD_IND((LOAD_ZERO_ADDR((addr)) & 0xff00)                          \
| ((LOAD_ZERO_ADDR((addr)) + reg_y_read) & 0xff)),         \
CLK_ADD(CLK, CLK_INT_CYCLE),                                        \
LOAD_IND(LOAD_ZERO_ADDR((addr)) + reg_y_read))                      \
: LOAD_IND(LOAD_ZERO_ADDR((addr)) + reg_y_read))
		
#define STORE_ABS(addr, value, inc) \
do {                            \
CLK_ADD(CLK, (inc));        \
STORE((addr), (value));     \
} while (0)
		
#define STORE_ABS_X(addr, value, inc)                             \
do {                                                          \
CLK_ADD(CLK, (inc) - 2);                                  \
LOAD((((addr) + reg_x_read) & 0xff) | ((addr) & 0xff00)); \
CLK_ADD(CLK, 2);                                          \
STORE((addr) + reg_x_read, (value));                      \
} while (0)
		
#define STORE_ABS_X_RMW(addr, value, inc)    \
do {                                     \
CLK_ADD(CLK, (inc));                 \
STORE((addr) + reg_x_read, (value)); \
} while (0)
		
#define STORE_ABS_SH_X(addr, value, inc)                          \
do {                                                          \
unsigned int tmp2;                                        \
\
CLK_ADD(CLK, (inc) - 2);                                  \
LOAD((((addr) + reg_x_read) & 0xff) | ((addr) & 0xff00)); \
CLK_ADD(CLK, 2);                                          \
tmp2 = (addr) + reg_x_read;                               \
if (((addr) & 0xff) + reg_x_read > 0xff) {                \
tmp2 = (tmp2 & 0xff) | ((value) << 8);                \
}                                                         \
STORE(tmp2, (value));                                     \
} while (0)
		
#define STORE_ABS_Y(addr, value, inc)                             \
do {                                                          \
CLK_ADD(CLK, (inc) - 2);                                  \
LOAD((((addr) + reg_y_read) & 0xff) | ((addr) & 0xff00)); \
CLK_ADD(CLK, 2);                                          \
STORE((addr) + reg_y_read, (value));                      \
} while (0)
		
#define STORE_ABS_Y_RMW(addr, value, inc)    \
do {                                     \
CLK_ADD(CLK, (inc));                 \
STORE((addr) + reg_y_read, (value)); \
} while (0)
		
#define STORE_ABS_SH_Y(addr, value, inc)                          \
do {                                                          \
unsigned int tmp2;                                        \
\
CLK_ADD(CLK, (inc) - 2);                                  \
LOAD((((addr) + reg_y_read) & 0xff) | ((addr) & 0xff00)); \
CLK_ADD(CLK, 2);                                          \
tmp2 = (addr) + reg_y_read;                               \
if (((addr) & 0xff) + reg_y_read > 0xff) {                \
tmp2 = (tmp2 & 0xff) | ((value) << 8);                \
}                                                         \
STORE(tmp2, (value));                                     \
} while (0)
		
#define INC_PC(value)   (reg_pc += (value))
		
		/* ------------------------------------------------------------------------- */
		
		/* Opcodes.  */
		
		/*
   A couple of caveats about PC:
		 
   - the VIC-II emulation requires PC to be incremented before the first
		 write access (this is not (very) important when writing to the zero
		 page);
		 
   - `p0', `p1' and `p2' can only be used *before* incrementing PC: some
		 machines (eg. the C128) might depend on this.
		 */
		
#define ADC(value, clk_inc, pc_inc)                                                                 \
do {                                                                                            \
unsigned int tmp_value;                                                                     \
unsigned int tmp;                                                                           \
\
tmp_value = (value);                                                                        \
CLK_ADD(CLK, (clk_inc));                                                                    \
\
if (LOCAL_DECIMAL()) {                                                                      \
tmp = (reg_a_read & 0xf) + (tmp_value & 0xf) + (reg_p & 0x1);                           \
if (tmp > 0x9) {                                                                        \
tmp += 0x6;                                                                         \
}                                                                                       \
if (tmp <= 0x0f) {                                                                      \
tmp = (tmp & 0xf) + (reg_a_read & 0xf0) + (tmp_value & 0xf0);                       \
} else {                                                                                \
tmp = (tmp & 0xf) + (reg_a_read & 0xf0) + (tmp_value & 0xf0) + 0x10;                \
}                                                                                       \
LOCAL_SET_ZERO(!((reg_a_read + tmp_value + (reg_p & 0x1)) & 0xff));                     \
LOCAL_SET_SIGN(tmp & 0x80);                                                             \
LOCAL_SET_OVERFLOW(((reg_a_read ^ tmp) & 0x80)  && !((reg_a_read ^ tmp_value) & 0x80)); \
if ((tmp & 0x1f0) > 0x90) {                                                             \
tmp += 0x60;                                                                        \
}                                                                                       \
LOCAL_SET_CARRY((tmp & 0xff0) > 0xf0);                                                  \
} else {                                                                                    \
tmp = tmp_value + reg_a_read + (reg_p & P_CARRY);                                       \
LOCAL_SET_NZ(tmp & 0xff);                                                               \
LOCAL_SET_OVERFLOW(!((reg_a_read ^ tmp_value) & 0x80)  && ((reg_a_read ^ tmp) & 0x80)); \
LOCAL_SET_CARRY(tmp > 0xff);                                                            \
}                                                                                           \
reg_a_write(tmp);                                                                           \
INC_PC(pc_inc);                                                                             \
} while (0)
		
#define ANC(value, pc_inc)                       \
do {                                         \
BYTE tmp = (BYTE)(reg_a_read & (value)); \
reg_a_write(tmp);                        \
LOCAL_SET_NZ(tmp);                       \
LOCAL_SET_CARRY(LOCAL_SIGN());           \
INC_PC(pc_inc);                          \
} while (0)
		
#define AND(value, clk_inc, pc_inc)              \
do {                                         \
BYTE tmp = (BYTE)(reg_a_read & (value)); \
reg_a_write(tmp);                        \
LOCAL_SET_NZ(tmp);                       \
CLK_ADD(CLK, (clk_inc));                 \
INC_PC(pc_inc);                          \
} while (0)
		
		/*
		 The result of the ANE opcode is A = ((A | CONST) & X & IMM), with CONST apparently
		 being both chip- and temperature dependent.
		 
		 The commonly used value for CONST in various documents is 0xee, which is however
		 not to be taken for granted (as it is unstable). see here:
		 http://visual6502.org/wiki/index.php?title=6502_Opcode_8B_(XAA,_ANE)
		 
		 as seen in the list, there are several possible values, and its origin is still
		 kinda unknown. instead of the commonly used 0xee we use 0xff here, since this
		 will make the only known occurance of this opcode in actual code work. see here:
		 https://sourceforge.net/tracker/?func=detail&aid=2110948&group_id=223021&atid=1057617
		 
		 FIXME: in the unlikely event that other code surfaces that depends on another
		 CONST value, it probably has to be made configureable somehow if no value can
		 be found that works for both.
		 */
		
#ifndef ANE
#define ANE(value, pc_inc)                                               \
do {                                                                 \
BYTE tmp = ((reg_a_read | 0xff) & reg_x_read & ((BYTE)(value))); \
reg_a_write(tmp);                                                \
LOCAL_SET_NZ(tmp);                                               \
INC_PC(pc_inc);                                                  \
} while (0)
#endif
		
		/* The fanciest opcode ever... ARR! */
#define ARR(value, pc_inc)                                          \
do {                                                            \
unsigned int tmp;                                           \
\
tmp = reg_a_read & (value);                                 \
if (LOCAL_DECIMAL()) {                                      \
int tmp_2 = tmp;                                        \
tmp_2 |= (reg_p & P_CARRY) << 8;                        \
tmp_2 >>= 1;                                            \
LOCAL_SET_SIGN(LOCAL_CARRY());                          \
LOCAL_SET_ZERO(!tmp_2);                                 \
LOCAL_SET_OVERFLOW((tmp_2 ^ tmp) & 0x40);               \
if (((tmp & 0xf) + (tmp & 0x1)) > 0x5) {                \
tmp_2 = (tmp_2 & 0xf0) | ((tmp_2 + 0x6) & 0xf);     \
}                                                       \
if (((tmp & 0xf0) + (tmp & 0x10)) > 0x50) {             \
tmp_2 = (tmp_2 & 0x0f) | ((tmp_2 + 0x60) & 0xf0);   \
LOCAL_SET_CARRY(1);                                 \
} else {                                                \
LOCAL_SET_CARRY(0);                                 \
}                                                       \
reg_a_write(tmp_2);                                     \
} else {                                                    \
tmp |= (reg_p & P_CARRY) << 8;                          \
tmp >>= 1;                                              \
LOCAL_SET_NZ(tmp);                                      \
LOCAL_SET_CARRY(tmp & 0x40);                            \
LOCAL_SET_OVERFLOW((tmp & 0x40) ^ ((tmp & 0x20) << 1)); \
reg_a_write(tmp);                                       \
}                                                           \
INC_PC(pc_inc);                                             \
} while (0)
		
#define ASL(addr, clk_inc, pc_inc, load_func, store_func) \
do {                                                  \
unsigned int tmp_value, tmp_addr;                 \
\
tmp_addr = (addr);                                \
tmp_value = load_func(tmp_addr);                  \
LOCAL_SET_CARRY(tmp_value & 0x80);                \
tmp_value = (tmp_value << 1) & 0xff;              \
LOCAL_SET_NZ(tmp_value);                          \
RMW_FLAG = 1;                                     \
INC_PC(pc_inc);                                   \
store_func(tmp_addr, tmp_value, clk_inc);         \
RMW_FLAG = 0;                                     \
} while (0)
		
#define ASL_A()                      \
do {                             \
BYTE tmp = reg_a_read;       \
LOCAL_SET_CARRY(tmp & 0x80); \
tmp <<= 1;                   \
reg_a_write(tmp);            \
LOCAL_SET_NZ(tmp);           \
INC_PC(1);                   \
} while (0)
		
#define ASR(value, pc_inc)                       \
do {                                         \
unsigned int tmp = reg_a_read & (value); \
LOCAL_SET_CARRY(tmp & 0x01);             \
tmp >>= 1;                               \
reg_a_write(tmp);                        \
LOCAL_SET_NZ(tmp);                       \
INC_PC(pc_inc);                          \
} while (0)
		
#define BIT(value, pc_inc)                   \
do {                                     \
unsigned int tmp;                    \
\
tmp = (value);                       \
CLK_ADD(CLK, 1);                     \
LOCAL_SET_SIGN(tmp & 0x80);          \
LOCAL_SET_OVERFLOW(tmp & 0x40);      \
LOCAL_SET_ZERO(!(tmp & reg_a_read)); \
INC_PC(pc_inc);                      \
} while (0)
		
#ifndef C64DTV
#define BRANCH(cond, value)                                         \
do {                                                            \
INC_PC(2);                                                  \
\
if (cond) {                                                 \
unsigned int dest_addr = reg_pc + (signed char)(value); \
\
FETCH_PARAM(reg_pc);                                    \
CLK_ADD(CLK, CLK_BRANCH2);                              \
if ((reg_pc ^ dest_addr) & 0xff00) {                    \
LOAD((reg_pc & 0xff00) | (dest_addr & 0xff));       \
CLK_ADD(CLK, CLK_BRANCH2);                          \
} else {                                                \
OPCODE_DELAYS_INTERRUPT();                          \
}                                                       \
JUMP(dest_addr & 0xffff);                               \
}                                                           \
} while (0)
#endif
		
#define BRK()                                                                                     \
do {                                                                                          \
EXPORT_REGISTERS();                                                                       \
INC_PC(2);                                                                                \
LOCAL_SET_BREAK(1);                                                                       \
PUSH(reg_pc >> 8);                                                                        \
PUSH(reg_pc & 0xff);                                                                      \
CLK_ADD(CLK, CLK_BRK - 3);                                                                \
PUSH(LOCAL_STATUS());                                                                     \
CLK_ADD(CLK, 1);                                                                          \
CPU_DELAY_CLK  /* process alarms for cartridge freeze */                                  \
PROCESS_ALARMS                                                                            \
if ((CPU_INT_STATUS->global_pending_int & IK_NMI)                                         \
&& (CLK >= (CPU_INT_STATUS->nmi_clk + INTERRUPT_DELAY))) {                            \
LOCAL_SET_INTERRUPT(1);                                                               \
TRACE_NMI(CLK - CLK_BRK);                                                             \
if (monitor_mask[CALLER] & (MI_STEP)) {                                               \
monitor_check_icount_interrupt();                                                 \
}                                                                                     \
interrupt_ack_nmi(CPU_INT_STATUS);                                                    \
JUMP(LOAD_ADDR(0xfffa));                                                              \
} else if ((CPU_INT_STATUS->global_pending_int & (IK_IRQ | IK_IRQPEND))                   \
&& !LOCAL_INTERRUPT() && (CLK >= (CPU_INT_STATUS->irq_clk + INTERRUPT_DELAY))) { \
LOCAL_SET_INTERRUPT(1);                                                               \
TRACE_IRQ(CLK - CLK_BRK);                                                             \
if (monitor_mask[CALLER] & (MI_STEP)) {                                               \
monitor_check_icount_interrupt();                                                 \
}                                                                                     \
interrupt_ack_irq(CPU_INT_STATUS);                                                    \
JUMP(LOAD_ADDR(0xfffe));                                                              \
} else {                                                                                  \
TRACE_BRK();                                                                          \
LOCAL_SET_INTERRUPT(1);                                                               \
JUMP(LOAD_ADDR(0xfffe));                                                              \
}                                                                                         \
CLK_ADD(CLK, 2);                                                                          \
} while (0)
		
#define CLC()               \
do {                    \
INC_PC(1);          \
LOCAL_SET_CARRY(0); \
} while (0)
		
#define CLD()                 \
do {                      \
INC_PC(1);            \
LOCAL_SET_DECIMAL(0); \
} while (0)
		
#define CLI()                     \
do {                          \
INC_PC(1);                \
if (LOCAL_INTERRUPT()) {  \
OPCODE_ENABLES_IRQ(); \
}                         \
LOCAL_SET_INTERRUPT(0);   \
} while (0)
		
#define CLV()                  \
do {                       \
INC_PC(1);             \
LOCAL_SET_OVERFLOW(0); \
} while (0)
		
#define CMP(value, clk_inc, pc_inc)   \
do {                              \
unsigned int tmp;             \
\
tmp = reg_a_read - (value);   \
LOCAL_SET_CARRY(tmp < 0x100); \
LOCAL_SET_NZ(tmp & 0xff);     \
CLK_ADD(CLK, (clk_inc));      \
INC_PC(pc_inc);               \
} while (0)
		
#define CPX(value, clk_inc, pc_inc)   \
do {                              \
unsigned int tmp;             \
\
tmp = reg_x_read - (value);   \
LOCAL_SET_CARRY(tmp < 0x100); \
LOCAL_SET_NZ(tmp & 0xff);     \
CLK_ADD(CLK, (clk_inc));      \
INC_PC(pc_inc);               \
} while (0)
		
#define CPY(value, clk_inc, pc_inc)   \
do {                              \
unsigned int tmp;             \
\
tmp = reg_y_read - (value);   \
LOCAL_SET_CARRY(tmp < 0x100); \
LOCAL_SET_NZ(tmp & 0xff);     \
CLK_ADD(CLK, (clk_inc));      \
INC_PC(pc_inc);               \
} while (0)
		
#define DCP(addr, clk_inc1, clk_inc2, pc_inc, load_func, store_func) \
do {                                                             \
unsigned int tmp, tmp_addr;                                  \
\
tmp_addr = (addr);                                           \
CLK_ADD(CLK, (clk_inc1));                                    \
tmp = load_func(tmp_addr);                                   \
tmp = (tmp - 1) & 0xff;                                      \
LOCAL_SET_CARRY(reg_a_read >= tmp);                          \
LOCAL_SET_NZ((reg_a_read - tmp));                            \
RMW_FLAG = 1;                                                \
INC_PC(pc_inc);                                              \
store_func(tmp_addr, tmp, (clk_inc2));                       \
RMW_FLAG = 0;                                                \
} while (0)
		
#define DCP_IND_Y(addr)                                               \
do {                                                              \
unsigned int tmp;                                             \
unsigned int tmp_addr = LOAD_ZERO_ADDR(addr);                 \
\
CLK_ADD(CLK, 2);                                              \
LOAD((tmp_addr & 0xff00) | ((tmp_addr + reg_y_read) & 0xff)); \
CLK_ADD(CLK, CLK_IND_Y_RMW1);                                 \
tmp_addr += reg_y_read;                                       \
tmp = LOAD(tmp_addr);                                         \
tmp = (tmp - 1) & 0xff;                                       \
LOCAL_SET_CARRY(reg_a_read >= tmp);                           \
LOCAL_SET_NZ((reg_a_read - tmp));                             \
RMW_FLAG = 1;                                                 \
INC_PC(2);                                                    \
STORE_ABS(tmp_addr, tmp, CLK_IND_Y_RMW2);                     \
RMW_FLAG = 0;                                                 \
} while (0)
		
#define DEC(addr, clk_inc, pc_inc, load_func, store_func) \
do {                                                  \
unsigned int tmp, tmp_addr;                       \
\
tmp_addr = (addr);                                \
tmp = load_func(tmp_addr);                        \
tmp = (tmp - 1) & 0xff;                           \
LOCAL_SET_NZ(tmp);                                \
RMW_FLAG = 1;                                     \
INC_PC(pc_inc);                                   \
store_func(tmp_addr, tmp, (clk_inc));             \
RMW_FLAG = 0;                                     \
} while (0)
		
#define DEX()                        \
do {                             \
reg_x_write(reg_x_read - 1); \
LOCAL_SET_NZ(reg_x_read);    \
INC_PC(1);                   \
} while (0)
		
#define DEY()                        \
do {                             \
reg_y_write(reg_y_read - 1); \
LOCAL_SET_NZ(reg_y_read);    \
INC_PC(1);                   \
} while (0)
		
#define EOR(value, clk_inc, pc_inc)              \
do {                                         \
BYTE tmp = (BYTE)(reg_a_read ^ (value)); \
reg_a_write(tmp);                        \
LOCAL_SET_NZ(tmp);                       \
CLK_ADD(CLK, (clk_inc));                 \
INC_PC(pc_inc);                          \
} while (0)
		
#define INC(addr, clk_inc, pc_inc, load_func, store_func) \
do {                                                  \
unsigned int tmp, tmp_addr;                       \
\
tmp_addr = (addr);                                \
tmp = (load_func(tmp_addr) + 1) & 0xff;           \
LOCAL_SET_NZ(tmp);                                \
RMW_FLAG = 1;                                     \
INC_PC(pc_inc);                                   \
store_func(tmp_addr, tmp, (clk_inc));             \
RMW_FLAG = 0;                                     \
} while (0)
		
#define INX()                        \
do {                             \
reg_x_write(reg_x_read + 1); \
LOCAL_SET_NZ(reg_x_read);    \
INC_PC(1);                   \
} while (0)
		
#define INY()                        \
do {                             \
reg_y_write(reg_y_read + 1); \
LOCAL_SET_NZ(reg_y_read);    \
INC_PC(1);                   \
} while (0)
		
#define ISB(addr, clk_inc1, clk_inc2, pc_inc, load_func, store_func) \
do {                                                             \
BYTE my_src;                                                 \
int my_addr = (addr);                                        \
\
CLK_ADD(CLK, (clk_inc1));                                    \
my_src = load_func(my_addr);                                 \
my_src = (my_src + 1) & 0xff;                                \
SBC(my_src, 0, 0);                                           \
RMW_FLAG = 1;                                                \
INC_PC(pc_inc);                                              \
store_func(my_addr, my_src, clk_inc2);                       \
RMW_FLAG = 0;                                                \
} while (0)
		
#define ISB_IND_Y(addr)                                             \
do {                                                            \
BYTE my_src;                                                \
int my_addr = LOAD_ZERO_ADDR(addr);                         \
\
CLK_ADD(CLK, 2);                                            \
LOAD((my_addr & 0xff00) | ((my_addr + reg_y_read) & 0xff)); \
CLK_ADD(CLK, CLK_IND_Y_RMW1);                               \
my_addr += reg_y_read;                                      \
my_src = LOAD(my_addr);                                     \
my_src = (my_src + 1) & 0xff;                               \
SBC(my_src, 0, 0);                                          \
RMW_FLAG = 1;                                               \
INC_PC(2);                                                  \
STORE_ABS(my_addr, my_src, CLK_IND_Y_RMW2);                 \
RMW_FLAG = 0;                                               \
} while (0)
		
		/* The 0x02 JAM opcode is also used to patch the ROM.  The function trap_handler()
   returns nonzero if this is not a patch, but a `real' JAM instruction. */
		
#define JAM_02()                                                                      \
do {                                                                              \
DWORD trap_result;                                                            \
EXPORT_REGISTERS();                                                           \
if (!ROM_TRAP_ALLOWED() || (trap_result = ROM_TRAP_HANDLER()) == (DWORD)-1) { \
REWIND_FETCH_OPCODE(CLK);                                                 \
JAM();                                                                    \
} else {                                                                      \
if (trap_result) {                                                        \
REWIND_FETCH_OPCODE(CLK);                                             \
SET_OPCODE(trap_result);                                              \
IMPORT_REGISTERS();                                                   \
goto trap_skipped;                                                    \
} else {                                                                  \
IMPORT_REGISTERS();                                                   \
}                                                                         \
}                                                                             \
} while (0)
		
#define JMP(addr)   \
do {            \
JUMP(addr); \
} while (0)
		
#define JMP_IND()                                                    \
do {                                                             \
WORD dest_addr;                                              \
dest_addr = LOAD(p2);                                        \
CLK_ADD(CLK, 1);                                             \
dest_addr |= (LOAD((p2 & 0xff00) | ((p2 + 1) & 0xff)) << 8); \
CLK_ADD(CLK, 1);                                             \
JUMP(dest_addr);                                             \
} while (0)
		
#define JSR()                                         \
do {                                              \
unsigned int tmp_addr;                        \
\
CLK_ADD(CLK, 1);                              \
INC_PC(2);                                    \
CLK_ADD(CLK, 2);                              \
PUSH(((reg_pc) >> 8) & 0xff);                 \
PUSH((reg_pc) & 0xff);                        \
tmp_addr = (p1 | (FETCH_PARAM(reg_pc) << 8)); \
CLK_ADD(CLK, CLK_JSR_INT_CYCLE);              \
JUMP(tmp_addr);                               \
} while (0)
		
#define LAS(value, clk_inc, pc_inc) \
do {                            \
reg_sp = reg_sp & (value);  \
reg_x_write(reg_sp);        \
reg_a_write(reg_sp);        \
LOCAL_SET_NZ(reg_sp);       \
CLK_ADD(CLK, (clk_inc));    \
INC_PC(pc_inc);             \
} while (0)
		
#define LAX(value, clk_inc, pc_inc) \
do {                            \
BYTE tmp = (value);         \
reg_x_write(tmp);           \
reg_a_write(tmp);           \
LOCAL_SET_NZ(tmp);          \
CLK_ADD(CLK, (clk_inc));    \
INC_PC(pc_inc);             \
} while (0)
		
#define LDA(value, clk_inc, pc_inc) \
do {                            \
BYTE tmp = (BYTE)(value);   \
reg_a_write(tmp);           \
CLK_ADD(CLK, (clk_inc));    \
LOCAL_SET_NZ(tmp);          \
INC_PC(pc_inc);             \
} while (0)
		
#define LDX(value, clk_inc, pc_inc) \
do {                            \
reg_x_write((BYTE)(value)); \
LOCAL_SET_NZ(reg_x_read);   \
CLK_ADD(CLK, (clk_inc));    \
INC_PC(pc_inc);             \
} while (0)
		
#define LDY(value, clk_inc, pc_inc) \
do {                            \
reg_y_write((BYTE)(value)); \
LOCAL_SET_NZ(reg_y_read);   \
CLK_ADD(CLK, (clk_inc));    \
INC_PC(pc_inc);             \
} while (0)
		
#define LSR(addr, clk_inc, pc_inc, load_func, store_func) \
do {                                                  \
unsigned int tmp, tmp_addr;                       \
\
tmp_addr = (addr);                                \
tmp = load_func(tmp_addr);                        \
LOCAL_SET_CARRY(tmp & 0x01);                      \
tmp >>= 1;                                        \
LOCAL_SET_NZ(tmp);                                \
RMW_FLAG = 1;                                     \
INC_PC(pc_inc);                                   \
store_func(tmp_addr, tmp, clk_inc);               \
RMW_FLAG = 0;                                     \
} while (0)
		
#define LSR_A()                        \
do {                               \
unsigned int tmp = reg_a_read; \
LOCAL_SET_CARRY(tmp & 0x01);   \
tmp >>= 1;                     \
reg_a_write(tmp);              \
LOCAL_SET_NZ(tmp);             \
INC_PC(1);                     \
} while (0)
		
		/* Note: this is not always exact, as this opcode can be quite unstable!
   Moreover, the behavior is different from the one described in 64doc. */
#ifndef LXA
#define LXA(value, pc_inc)                                  \
do {                                                    \
BYTE tmp = ((reg_a_read | 0xee) & ((BYTE)(value))); \
reg_x_write(tmp);                                   \
reg_a_write(tmp);                                   \
LOCAL_SET_NZ(tmp);                                  \
INC_PC(pc_inc);                                     \
} while (0)
#endif
		
#define ORA(value, clk_inc, pc_inc)              \
do {                                         \
BYTE tmp = (BYTE)(reg_a_read | (value)); \
reg_a_write(tmp);                        \
LOCAL_SET_NZ(tmp);                       \
CLK_ADD(CLK, (clk_inc));                 \
INC_PC(pc_inc);                          \
} while (0)
		
#define NOOP(clk_inc, pc_inc) (CLK_ADD(CLK, (clk_inc)), INC_PC(pc_inc))
		
#define NOOP_IMM(pc_inc) INC_PC(pc_inc)
		
#define NOOP_ABS()       \
do {                 \
LOAD(p2);        \
CLK_ADD(CLK, 1); \
INC_PC(3);       \
} while (0)
		
#define NOOP_ABS_X()     \
do {                 \
LOAD_ABS_X(p2);  \
CLK_ADD(CLK, 1); \
INC_PC(3);       \
} while (0)
		
#define NOP()  NOOP_IMM(1)
		
#define PHA()                         \
do {                              \
CLK_ADD(CLK, CLK_STACK_PUSH); \
PUSH(reg_a_read);             \
INC_PC(1);                    \
} while (0)
		
#define PHP()                           \
do {                                \
CLK_ADD(CLK, CLK_STACK_PUSH);   \
PUSH(LOCAL_STATUS() | P_BREAK); \
INC_PC(1);                      \
} while (0)
		
#define PLA()                         \
do {                              \
BYTE tmp;                     \
CLK_ADD(CLK, CLK_STACK_PULL); \
tmp = PULL();                 \
reg_a_write(tmp);             \
LOCAL_SET_NZ(tmp);            \
INC_PC(1);                    \
} while (0)
		
		/* FIXME: Rotate disk before executing LOCAL_SET_STATUS().  */
#define PLP()                                                 \
do {                                                      \
BYTE s = PULL();                                      \
\
if (!(s & P_INTERRUPT) && LOCAL_INTERRUPT()) {        \
OPCODE_ENABLES_IRQ();                             \
} else if ((s & P_INTERRUPT) && !LOCAL_INTERRUPT()) { \
OPCODE_DISABLES_IRQ();                            \
}                                                     \
CLK_ADD(CLK, CLK_STACK_PULL);                         \
LOCAL_SET_STATUS(s);                                  \
INC_PC(1);                                            \
} while (0)
		
#define RLA(addr, clk_inc1, clk_inc2, pc_inc, load_func, store_func) \
do {                                                             \
unsigned int tmp, tmp2, tmp_addr;                            \
\
tmp_addr = (addr);                                           \
CLK_ADD(CLK, (clk_inc1));                                    \
tmp = ((load_func(tmp_addr) << 1) | (reg_p & P_CARRY));      \
LOCAL_SET_CARRY(tmp & 0x100);                                \
tmp2 = reg_a_read & tmp;                                     \
reg_a_write(tmp2);                                           \
LOCAL_SET_NZ(tmp2);                                          \
RMW_FLAG = 1;                                                \
INC_PC(pc_inc);                                              \
store_func(tmp_addr, tmp, clk_inc2);                         \
RMW_FLAG = 0;                                                \
} while (0)
		
#define RLA_IND_Y(addr)                                               \
do {                                                              \
unsigned int tmp, tmp2;                                       \
unsigned int tmp_addr = LOAD_ZERO_ADDR(addr);                 \
\
CLK_ADD(CLK, 2);                                              \
LOAD((tmp_addr & 0xff00) | ((tmp_addr + reg_y_read) & 0xff)); \
CLK_ADD(CLK, CLK_IND_Y_RMW1);                                 \
tmp_addr += reg_y_read;                                       \
tmp = ((LOAD(tmp_addr) << 1) | (reg_p & P_CARRY));            \
LOCAL_SET_CARRY(tmp & 0x100);                                 \
tmp2 = reg_a_read & tmp;                                      \
reg_a_write(tmp2);                                            \
LOCAL_SET_NZ(tmp2);                                           \
RMW_FLAG = 1;                                                 \
INC_PC(2);                                                    \
STORE_ABS(tmp_addr, tmp, CLK_IND_Y_RMW2);                     \
RMW_FLAG = 0;                                                 \
} while (0)
		
#define ROL(addr, clk_inc, pc_inc, load_func, store_func) \
do {                                                  \
unsigned int tmp, tmp_addr;                       \
\
tmp_addr = (addr);                                \
tmp = load_func(tmp_addr);                        \
tmp = (tmp << 1) | (reg_p & P_CARRY);             \
LOCAL_SET_CARRY(tmp & 0x100);                     \
LOCAL_SET_NZ(tmp & 0xff);                         \
RMW_FLAG = 1;                                     \
INC_PC(pc_inc);                                   \
store_func(tmp_addr, tmp, clk_inc);               \
RMW_FLAG = 0;                                     \
} while (0)
		
#define ROL_A()                             \
do {                                    \
unsigned int tmp = reg_a_read << 1; \
\
tmp |= (reg_p & P_CARRY);           \
reg_a_write(tmp);                   \
LOCAL_SET_NZ(tmp);                  \
LOCAL_SET_CARRY(tmp & 0x100);       \
INC_PC(1);                          \
} while (0)
		
#define ROR(addr, clk_inc, pc_inc, load_func, store_func) \
do {                                                  \
unsigned int src, tmp_addr;                       \
\
tmp_addr = (addr);                                \
src = load_func(tmp_addr);                        \
if (reg_p & P_CARRY) {                            \
src |= 0x100;                                 \
}                                                 \
LOCAL_SET_CARRY(src & 0x01);                      \
src >>= 1;                                        \
LOCAL_SET_NZ(src);                                \
RMW_FLAG = 1;                                     \
INC_PC(pc_inc);                                   \
store_func(tmp_addr, src, (clk_inc));             \
RMW_FLAG = 0;                                     \
} while (0)
		
#define ROR_A()                              \
do {                                     \
unsigned int tmp = reg_a_read, tmp2; \
tmp2 = (tmp >> 1) | (reg_p << 7);    \
LOCAL_SET_CARRY(tmp & 0x01);         \
reg_a_write(tmp2);                   \
LOCAL_SET_NZ(tmp2);                  \
INC_PC(1);                           \
} while (0)
		
#define RRA(addr, clk_inc1, clk_inc2, pc_inc, load_func, store_func) \
do {                                                             \
BYTE src;                                                    \
unsigned int my_temp, tmp_addr;                              \
\
CLK_ADD(CLK, (clk_inc1));                                    \
tmp_addr = (addr);                                           \
src = load_func(tmp_addr);                                   \
my_temp = src >> 1;                                          \
if (reg_p & P_CARRY) {                                       \
my_temp |= 0x80;                                         \
}                                                            \
LOCAL_SET_CARRY(src & 0x1);                                  \
RMW_FLAG = 1;                                                \
INC_PC(pc_inc);                                              \
ADC(my_temp, 0, 0);                                          \
store_func(tmp_addr, my_temp, clk_inc2);                     \
RMW_FLAG = 0;                                                \
} while (0)
		
#define RRA_IND_Y(addr)                                                     \
do {                                                                    \
BYTE src;                                                           \
unsigned int my_tmp_addr;                                           \
unsigned int my_temp;                                               \
\
CLK_ADD(CLK, 2);                                                    \
my_tmp_addr = LOAD_ZERO_ADDR(addr);                                 \
LOAD((my_tmp_addr & 0xff00) | ((my_tmp_addr + reg_y_read) & 0xff)); \
CLK_ADD(CLK, CLK_IND_Y_RMW1);                                       \
my_tmp_addr += reg_y_read;                                          \
src = LOAD(my_tmp_addr);                                            \
RMW_FLAG = 1;                                                       \
INC_PC(2);                                                          \
my_temp = src >> 1;                                                 \
if (reg_p & P_CARRY) {                                              \
my_temp |= 0x80;                                                \
}                                                                   \
LOCAL_SET_CARRY(src & 0x1);                                         \
ADC(my_temp, 0, 0);                                                 \
STORE_ABS(my_tmp_addr, my_temp, CLK_IND_Y_RMW2);                    \
RMW_FLAG = 0;                                                       \
} while (0)
		
		/* RTI does must not use `OPCODE_ENABLES_IRQ()' even if the I flag changes
   from 1 to 0 because the value of I is set 3 cycles before the end of the
   opcode, and thus the 6510 has enough time to call the interrupt routine as
   soon as the opcode ends, if necessary.  */
		/* FIXME: Rotate disk before executing LOCAL_SET_STATUS().  */
#define RTI()                        \
do {                             \
WORD tmp;                    \
\
CLK_ADD(CLK, CLK_RTI);       \
tmp = (WORD)PULL();          \
LOCAL_SET_STATUS((BYTE)tmp); \
tmp = (WORD)PULL();          \
tmp |= (WORD)PULL() << 8;    \
JUMP(tmp);                   \
} while (0)
		
#define RTS()                        \
do {                             \
WORD tmp;                    \
\
CLK_ADD(CLK, CLK_RTS);       \
tmp = PULL();                \
tmp = tmp | (PULL() << 8);   \
JUMP(tmp);                   \
FETCH_PARAM(reg_pc);         \
CLK_ADD(CLK, CLK_INT_CYCLE); \
INC_PC(1);                   \
} while (0)
		
#define SAX(addr, clk_inc1, clk_inc2, pc_inc) \
do {                                      \
unsigned int tmp;                     \
\
CLK_ADD(CLK, (clk_inc1));             \
tmp = (addr);                         \
CLK_ADD(CLK, (clk_inc2));             \
INC_PC(pc_inc);                       \
STORE(tmp, reg_a_read & reg_x_read);  \
} while (0)
		
#define SAX_ZERO(addr, clk_inc, pc_inc)              \
do {                                             \
CLK_ADD(CLK, (clk_inc));                     \
STORE_ZERO((addr), reg_a_read & reg_x_read); \
INC_PC(pc_inc);                              \
} while (0)
		
#define SBC(value, clk_inc, pc_inc)                                                         \
do {                                                                                    \
WORD src, tmp;                                                                      \
\
src = (WORD)(value);                                                                \
CLK_ADD(CLK, (clk_inc));                                                            \
tmp = reg_a_read - src - ((reg_p & P_CARRY) ? 0 : 1);                               \
if (reg_p & P_DECIMAL) {                                                            \
unsigned int tmp_a;                                                             \
tmp_a = (reg_a_read & 0xf) - (src & 0xf) - ((reg_p & P_CARRY) ? 0 : 1);         \
if (tmp_a & 0x10) {                                                             \
tmp_a = ((tmp_a - 6) & 0xf) | ((reg_a_read & 0xf0) - (src & 0xf0) - 0x10);  \
} else {                                                                        \
tmp_a = (tmp_a & 0xf) | ((reg_a_read & 0xf0) - (src & 0xf0));               \
}                                                                               \
if (tmp_a & 0x100) {                                                            \
tmp_a -= 0x60;                                                              \
}                                                                               \
LOCAL_SET_CARRY(tmp < 0x100);                                                   \
LOCAL_SET_NZ(tmp & 0xff);                                                       \
LOCAL_SET_OVERFLOW(((reg_a_read ^ tmp) & 0x80) && ((reg_a_read ^ src) & 0x80)); \
reg_a_write((BYTE) tmp_a);                                                      \
} else {                                                                            \
LOCAL_SET_NZ(tmp & 0xff);                                                       \
LOCAL_SET_CARRY(tmp < 0x100);                                                   \
LOCAL_SET_OVERFLOW(((reg_a_read ^ tmp) & 0x80) && ((reg_a_read ^ src) & 0x80)); \
reg_a_write((BYTE) tmp);                                                        \
}                                                                                   \
INC_PC(pc_inc);                                                                     \
} while (0)
		
#define SBX(value, pc_inc)                     \
do {                                       \
unsigned int tmp;                      \
\
tmp = (value);                         \
INC_PC(pc_inc);                        \
tmp = (reg_a_read & reg_x_read) - tmp; \
LOCAL_SET_CARRY(tmp < 0x100);          \
reg_x_write(tmp & 0xff);               \
LOCAL_SET_NZ(reg_x_read);              \
} while (0)
		
#undef SEC    /* defined in time.h on SunOS. */
#define SEC()               \
do {                    \
LOCAL_SET_CARRY(1); \
INC_PC(1);          \
} while (0)
		
#define SED()                 \
do {                      \
LOCAL_SET_DECIMAL(1); \
INC_PC(1);            \
} while (0)
		
#define SEI()                      \
do {                           \
if (!LOCAL_INTERRUPT()) {  \
OPCODE_DISABLES_IRQ(); \
}                          \
LOCAL_SET_INTERRUPT(1);    \
INC_PC(1);                 \
} while (0)
		
#define SHA_ABS_Y(addr)                                                                    \
do {                                                                                   \
unsigned int tmp;                                                                  \
\
tmp = (addr);                                                                      \
INC_PC(3);                                                                         \
STORE_ABS_SH_Y(tmp, reg_a_read & reg_x_read & ((tmp >> 8) + 1), CLK_ABS_I_STORE2); \
} while (0)
		
#define SHA_IND_Y(addr)                                     \
do {                                                    \
unsigned int tmp;                                   \
BYTE val;                                           \
\
CLK_ADD(CLK, 2);                                    \
tmp = LOAD_ZERO_ADDR(addr);                         \
LOAD((tmp & 0xff00) | ((tmp + reg_y_read) & 0xff)); \
CLK_ADD(CLK, CLK_IND_Y_W);                          \
val = reg_a_read & reg_x_read & ((tmp >> 8) + 1);   \
if ((tmp & 0xff) + reg_y_read > 0xff) {             \
tmp = ((tmp + reg_y_read) & 0xff) | (val << 8); \
} else {                                            \
tmp += reg_y_read;                              \
}                                                   \
INC_PC(2);                                          \
STORE(tmp, val);                                    \
} while (0)
		
#define SHX_ABS_Y(addr)                                                       \
do {                                                                      \
unsigned int tmp;                                                     \
\
tmp = (addr);                                                         \
INC_PC(3);                                                            \
STORE_ABS_SH_Y(tmp, reg_x_read & ((tmp >> 8) + 1), CLK_ABS_I_STORE2); \
} while (0)
		
#define SHY_ABS_X(addr)                                                       \
do {                                                                      \
unsigned int tmp;                                                     \
\
tmp = (addr);                                                         \
INC_PC(3);                                                            \
STORE_ABS_SH_X(tmp, reg_y_read & ((tmp >> 8) + 1), CLK_ABS_I_STORE2); \
} while (0)
		
#define SHS_ABS_Y(addr)                                                                    \
do {                                                                                   \
int tmp = (addr);                                                                  \
\
INC_PC(3);                                                                         \
STORE_ABS_SH_Y(tmp, reg_a_read & reg_x_read & ((tmp >> 8) + 1), CLK_ABS_I_STORE2); \
reg_sp = reg_a_read & reg_x_read;                                                  \
} while (0)
		
#define SLO(addr, clk_inc1, clk_inc2, pc_inc, load_func, store_func) \
do {                                                             \
BYTE tmp, tmp2;                                              \
int tmp_addr;                                                \
\
CLK_ADD(CLK, (clk_inc1));                                    \
tmp_addr = (addr);                                           \
tmp = load_func(tmp_addr);                                   \
LOCAL_SET_CARRY(tmp & 0x80);                                 \
tmp <<= 1;                                                   \
tmp2 = reg_a_read | tmp;                                     \
reg_a_write(tmp2);                                           \
LOCAL_SET_NZ(tmp2);                                          \
RMW_FLAG = 1;                                                \
INC_PC(pc_inc);                                              \
store_func(tmp_addr, tmp, clk_inc2);                         \
RMW_FLAG = 0;                                                \
} while (0)
		
#define SLO_IND_Y(addr)                                               \
do {                                                              \
BYTE tmp, tmp2;                                               \
unsigned int tmp_addr;                                        \
\
CLK_ADD(CLK, 2);                                              \
tmp_addr = LOAD_ZERO_ADDR(addr);                              \
LOAD((tmp_addr & 0xff00) | ((tmp_addr + reg_y_read) & 0xff)); \
CLK_ADD(CLK, CLK_IND_Y_RMW1);                                 \
tmp_addr += reg_y_read;                                       \
tmp = LOAD(tmp_addr);                                         \
LOCAL_SET_CARRY(tmp & 0x80);                                  \
tmp <<= 1;                                                    \
tmp2 = reg_a_read | tmp;                                      \
reg_a_write(tmp2);                                            \
LOCAL_SET_NZ(tmp2);                                           \
RMW_FLAG = 1;                                                 \
INC_PC(2);                                                    \
STORE_ABS(tmp_addr, tmp, CLK_IND_Y_RMW2);                     \
RMW_FLAG = 0;                                                 \
} while (0)
		
#define SRE(addr, clk_inc1, clk_inc2, pc_inc, load_func, store_func) \
do {                                                             \
unsigned int tmp, tmp2;                                      \
unsigned int tmp_addr;                                       \
\
CLK_ADD(CLK, (clk_inc1));                                    \
tmp_addr = (addr);                                           \
tmp = load_func(tmp_addr);                                   \
LOCAL_SET_CARRY(tmp & 0x01);                                 \
tmp >>= 1;                                                   \
tmp2 = reg_a_read ^ tmp;                                     \
reg_a_write(tmp2);                                           \
LOCAL_SET_NZ(tmp2);                                          \
RMW_FLAG = 1;                                                \
INC_PC(pc_inc);                                              \
store_func(tmp_addr, tmp, clk_inc2);                         \
RMW_FLAG = 0;                                                \
} while (0)
		
#define SRE_IND_Y(addr)                                               \
do {                                                              \
unsigned int tmp, tmp2;                                       \
unsigned int tmp_addr = LOAD_ZERO_ADDR(addr);                 \
\
CLK_ADD(CLK, 2);                                              \
LOAD((tmp_addr & 0xff00) | ((tmp_addr + reg_y_read) & 0xff)); \
CLK_ADD(CLK, CLK_IND_Y_RMW1);                                 \
tmp_addr += reg_y_read;                                       \
tmp = LOAD(tmp_addr);                                         \
LOCAL_SET_CARRY(tmp & 0x01);                                  \
tmp >>= 1;                                                    \
tmp2 = reg_a_read ^ tmp;                                      \
reg_a_write(tmp2);                                            \
LOCAL_SET_NZ(tmp2);                                           \
RMW_FLAG = 1;                                                 \
INC_PC(2);                                                    \
STORE_ABS(tmp_addr, tmp, CLK_IND_Y_RMW2);                     \
RMW_FLAG = 0;                                                 \
} while (0)
		
#define STA(addr, clk_inc1, clk_inc2, pc_inc, store_func) \
do {                                                  \
unsigned int tmp;                                 \
\
CLK_ADD(CLK, (clk_inc1));                         \
tmp = (addr);                                     \
INC_PC(pc_inc);                                   \
store_func(tmp, reg_a_read, clk_inc2);            \
} while (0)
		
#define STA_ZERO(addr, clk_inc, pc_inc) \
do {                                \
CLK_ADD(CLK, (clk_inc));        \
STORE_ZERO((addr), reg_a_read); \
INC_PC(pc_inc);                 \
} while (0)
		
#define STA_IND_Y(addr)                                         \
do {                                                        \
unsigned int tmp;                                       \
\
CLK_ADD(CLK, 2);                                        \
tmp = LOAD_ZERO_ADDR(addr);                             \
LOAD_IND((tmp & 0xff00) | ((tmp + reg_y_read) & 0xff)); \
CLK_ADD(CLK, CLK_IND_Y_W);                              \
INC_PC(2);                                              \
STORE_IND(tmp + reg_y_read, reg_a_read);                \
} while (0)
		
#define STX(addr, clk_inc, pc_inc) \
do {                           \
unsigned int tmp;          \
\
tmp = (addr);              \
CLK_ADD(CLK, (clk_inc));   \
INC_PC(pc_inc);            \
STORE(tmp, reg_x_read);    \
} while (0)
		
#define STX_ZERO(addr, clk_inc, pc_inc) \
do {                                \
CLK_ADD(CLK, (clk_inc));        \
STORE_ZERO((addr), reg_x_read); \
INC_PC(pc_inc);                 \
} while (0)
		
#define STY(addr, clk_inc, pc_inc) \
do {                           \
unsigned int tmp;          \
\
tmp = (addr);              \
CLK_ADD(CLK, (clk_inc));   \
INC_PC(pc_inc);            \
STORE(tmp, reg_y_read);    \
} while (0)
		
#define STY_ZERO(addr, clk_inc, pc_inc) \
do {                                \
CLK_ADD(CLK, (clk_inc));        \
STORE_ZERO((addr), reg_y_read); \
INC_PC(pc_inc);                 \
} while (0)
		
#define TAX()                     \
do {                          \
reg_x_write(reg_a_read);  \
LOCAL_SET_NZ(reg_x_read); \
INC_PC(1);                \
} while (0)
		
#define TAY()                     \
do {                          \
reg_y_write(reg_a_read);  \
LOCAL_SET_NZ(reg_y_read); \
INC_PC(1);                \
} while (0)
		
#define TSX()                 \
do {                      \
reg_x_write(reg_sp);  \
LOCAL_SET_NZ(reg_sp); \
INC_PC(1);            \
} while (0)
		
#define TXA()                     \
do {                          \
reg_a_write(reg_x_read);  \
LOCAL_SET_NZ(reg_x_read); \
INC_PC(1);                \
} while (0)
		
#define TXS()                \
do {                     \
reg_sp = reg_x_read; \
INC_PC(1);           \
} while (0)
		
#define TYA()                     \
do {                          \
reg_a_write(reg_y_read);  \
LOCAL_SET_NZ(reg_y_read); \
INC_PC(1);                \
} while (0)
		
		
		/* ------------------------------------------------------------------------- */
		
		static const BYTE fetch_tab[] = {
			/* 0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F */
			/* $00 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, /* $00 */
			/* $10 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, /* $10 */
			/* $20 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, /* $20 */
			/* $30 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, /* $30 */
			/* $40 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, /* $40 */
			/* $50 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, /* $50 */
			/* $60 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, /* $60 */
			/* $70 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, /* $70 */
			/* $80 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, /* $80 */
			/* $90 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, /* $90 */
			/* $A0 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, /* $A0 */
			/* $B0 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, /* $B0 */
			/* $C0 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, /* $C0 */
			/* $D0 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, /* $D0 */
			/* $E0 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, /* $E0 */
			/* $F0 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1  /* $F0 */
		};
		
#ifndef C64DTV  /* C64DTV opcode_t & fetch are defined in c64dtvcpu.c */
		
#ifdef CPU_8502  /* 8502 specific opcode fetch */
		
		static const BYTE rewind_fetch_tab[] = {
			/* 0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F */
			/* $00 */  1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* $00 */
			/* $10 */  0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* $10 */
			/* $20 */  0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* $20 */
			/* $30 */  0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* $30 */
			/* $40 */  0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* $40 */
			/* $50 */  0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* $50 */
			/* $60 */  0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* $60 */
			/* $70 */  0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* $70 */
			/* $80 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* $80 */
			/* $90 */  0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* $90 */
			/* $A0 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* $A0 */
			/* $B0 */  0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* $B0 */
			/* $C0 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* $C0 */
			/* $D0 */  0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* $D0 */
			/* $E0 */  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* $E0 */
			/* $F0 */  0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* $F0 */
		};
		
#if !defined WORDS_BIGENDIAN && defined ALLOW_UNALIGNED_ACCESS
		
#define opcode_t DWORD
		
#define FETCH_OPCODE(o)                                                \
do {                                                               \
if (((int)reg_pc) < bank_limit) {                              \
o = (*((DWORD *)(bank_base + reg_pc)) & 0xffffff);         \
if (rewind_fetch_tab[o & 0xff]) {                          \
opcode_cycle[0] = vicii_check_memory_refresh(CLK);     \
CLK_ADD(CLK, 1);                                       \
opcode_cycle[1] = vicii_check_memory_refresh(CLK);     \
CLK_ADD(CLK, 1);                                       \
} else {                                                   \
opcode_cycle[0] = 0;                                   \
opcode_cycle[1] = 0;                                   \
CLK_ADD(CLK, 2);                                       \
}                                                          \
if (fetch_tab[o & 0xff]) {                                 \
CLK_ADD(CLK, 1);                                       \
}                                                          \
} else {                                                       \
maincpu_stretch = 0;                                       \
o = LOAD(reg_pc);                                          \
if (rewind_fetch_tab[o & 0xff]) {                          \
opcode_cycle[0] = maincpu_stretch;                     \
if (opcode_cycle[0] == 0) {                            \
opcode_cycle[0] = vicii_check_memory_refresh(CLK); \
}                                                      \
CLK_ADD(CLK, 1);                                       \
maincpu_stretch = 0;                                   \
o |= LOAD(reg_pc + 1) << 8;                            \
opcode_cycle[1] = maincpu_stretch;                     \
if (opcode_cycle[1] == 0) {                            \
opcode_cycle[1] = vicii_check_memory_refresh(CLK); \
}                                                      \
CLK_ADD(CLK, 1);                                       \
} else {                                                   \
CLK_ADD(CLK, 1);                                       \
o |= LOAD(reg_pc + 1) << 8;                            \
CLK_ADD(CLK, 1);                                       \
}                                                          \
if (fetch_tab[o & 0xff]) {                                 \
o |= (LOAD(reg_pc + 2) << 16);                         \
CLK_ADD(CLK, 1);                                       \
}                                                          \
}                                                              \
} while (0)
		
#define p0 (opcode & 0xff)
#define p1 ((opcode >> 8) & 0xff)
#define p2 (opcode >> 8)
		
#else /* WORDS_BIGENDIAN || !ALLOW_UNALIGNED_ACCESS */
		
#define opcode_t         \
struct {             \
BYTE ins;        \
union {          \
BYTE op8[2]; \
WORD op16;   \
} op;            \
}
		
#define FETCH_OPCODE(o)                                                                   \
do {                                                                                  \
if (((int)reg_pc) < bank_limit) {                                                 \
(o).ins = *(bank_base + reg_pc);                                              \
(o).op.op16 = (*(bank_base + reg_pc + 1) | (*(bank_base + reg_pc + 2) << 8)); \
if (rewind_fetch_tab[(o).ins]) {                                              \
opcode_cycle[0] = vicii_check_memory_refresh(CLK);                        \
CLK_ADD(CLK, 1);                                                          \
opcode_cycle[1] = vicii_check_memory_refresh(CLK);                        \
CLK_ADD(CLK, 1);                                                          \
} else {                                                                      \
opcode_cycle[0] = 0;                                                      \
opcode_cycle[1] = 0;                                                      \
CLK_ADD(CLK, 2);                                                          \
}                                                                             \
if (fetch_tab[(o).ins]) {                                                     \
CLK_ADD(CLK, 1);                                                          \
}                                                                             \
} else {                                                                          \
maincpu_stretch = 0;                                                          \
(o).ins = LOAD(reg_pc);                                                       \
if (rewind_fetch_tab[(o).ins]) {                                              \
opcode_cycle[0] = maincpu_stretch;                                        \
if (opcode_cycle[0] == 0) {                                               \
opcode_cycle[0] = vicii_check_memory_refresh(CLK);                    \
}                                                                         \
CLK_ADD(CLK, 1);                                                          \
maincpu_stretch = 0;                                                      \
(o).op.op16 = LOAD(reg_pc + 1);                                           \
opcode_cycle[1] = maincpu_stretch;                                        \
if (opcode_cycle[1] == 0) {                                               \
opcode_cycle[1] = vicii_check_memory_refresh(CLK);                    \
}                                                                         \
CLK_ADD(CLK, 1);                                                          \
} else {                                                                      \
CLK_ADD(CLK, 1);                                                          \
(o).op.op16 = LOAD(reg_pc + 1);                                           \
CLK_ADD(CLK, 1);                                                          \
}                                                                             \
if (fetch_tab[(o).ins]) {                                                     \
(o).op.op16 |= (LOAD(reg_pc + 2) << 8);                                   \
CLK_ADD(CLK, 1);                                                          \
}                                                                             \
}                                                                                 \
} while (0)
		
#define p0 (opcode.ins)
#define p2 (opcode.op.op16)
		
#ifdef WORDS_BIGENDIAN
#  define p1 (opcode.op.op8[1])
#else
#  define p1 (opcode.op.op8[0])
#endif
		
#endif /* !WORDS_BIGENDIAN */
		
#else /* !CPU_8502 */
		
#if !defined WORDS_BIGENDIAN && defined ALLOW_UNALIGNED_ACCESS
		
#define opcode_t DWORD
		
#define FETCH_OPCODE(o)                                        \
do {                                                       \
if (((int)reg_pc) < bank_limit) {                      \
o = (*((DWORD *)(bank_base + reg_pc)) & 0xffffff); \
CLK_ADD(CLK, 2);                                   \
if (fetch_tab[o & 0xff]) {                         \
CLK_ADD(CLK, 1);                               \
}                                                  \
} else {                                               \
o = LOAD(reg_pc);                                  \
CLK_ADD(CLK, 1);                                   \
o |= LOAD(reg_pc + 1) << 8;                        \
CLK_ADD(CLK, 1);                                   \
if (fetch_tab[o & 0xff]) {                         \
o |= (LOAD(reg_pc + 2) << 16);                 \
CLK_ADD(CLK, 1);                               \
}                                                  \
}                                                      \
} while (0)

#define C64D_FETCH_OPCODE_LOAD(o)                                         \
do {                                                        \
o = LOAD(reg_pc);                                   \
CLK_ADD(CLK,1);                                     \
o |= LOAD(reg_pc + 1) << 8;                         \
CLK_ADD(CLK,1);                                     \
if (fetch_tab[o & 0xff]) {                          \
o |= (LOAD(reg_pc + 2) << 16);                 \
CLK_ADD(CLK,1);                                \
}                                                       \
} while (0)

#define p0 (opcode & 0xff)
#define p1 ((opcode >> 8) & 0xff)
#define p2 (opcode >> 8)
		
#else /* WORDS_BIGENDIAN || !ALLOW_UNALIGNED_ACCESS */
		
#define opcode_t         \
struct {             \
BYTE ins;        \
union {          \
BYTE op8[2]; \
WORD op16;   \
} op;            \
}
		
#define FETCH_OPCODE(o)                                                                   \
do {                                                                                  \
if (((int)reg_pc) < bank_limit) {                                                 \
(o).ins = *(bank_base + reg_pc);                                              \
(o).op.op16 = (*(bank_base + reg_pc + 1) | (*(bank_base + reg_pc + 2) << 8)); \
CLK_ADD(CLK, 2);                                                              \
if (fetch_tab[(o).ins]) {                                                     \
CLK_ADD(CLK, 1);                                                          \
}                                                                             \
} else {                                                                          \
(o).ins = LOAD(reg_pc);                                                       \
CLK_ADD(CLK, 1);                                                              \
(o).op.op16 = LOAD(reg_pc + 1);                                               \
CLK_ADD(CLK, 1);                                                              \
if (fetch_tab[(o).ins]) {                                                     \
(o).op.op16 |= (LOAD(reg_pc + 2) << 8);                                   \
CLK_ADD(CLK, 1);                                                          \
}                                                                             \
}                                                                                 \
} while (0)

#define C64D_FETCH_OPCODE_LOAD(o)                                \
do {                                                        \
(o).ins = LOAD(reg_pc);                             \
CLK_ADD(CLK,1);                                     \
(o).op.op16 = LOAD(reg_pc + 1);                     \
CLK_ADD(CLK,1);                                     \
if (fetch_tab[(o).ins]) {                           \
(o).op.op16 |= (LOAD(reg_pc + 2) << 8);        \
CLK_ADD(CLK,1);                                \
}                                                       \
} while (0)
		
#define p0 (opcode.ins)
#define p2 (opcode.op.op16)
		
#ifdef WORDS_BIGENDIAN
#  define p1 (opcode.op.op8[1])
#else
#  define p1 (opcode.op.op8[0])
#endif
		
#endif /* !WORDS_BIGENDIAN */
#endif
		
		/*  SET_OPCODE for traps */
#if !defined WORDS_BIGENDIAN && defined ALLOW_UNALIGNED_ACCESS
#define SET_OPCODE(o) (opcode) = o
#else
#if !defined WORDS_BIGENDIAN
#define SET_OPCODE(o)                          \
do {                                       \
opcode.ins = (o) & 0xff;               \
opcode.op.op8[0] = ((o) >> 8) & 0xff;  \
opcode.op.op8[1] = ((o) >> 16) & 0xff; \
} while (0)
#else
#define SET_OPCODE(o)                          \
do {                                       \
opcode.ins = (o) & 0xff;               \
opcode.op.op8[1] = ((o) >> 8) & 0xff;  \
opcode.op.op8[0] = ((o) >> 16) & 0xff; \
} while (0)
#endif
		
#endif
		
#endif /* !C64DTV */
		
		/* ------------------------------------------------------------------------ */
		
		/* Here, the CPU is emulated. */
		
		{
//			LOGD("drive pc=%4.4x", reg_pc);

			/* handle 8502 fast mode refresh cycles */
			CPU_REFRESH_CLK
			
			/* handle any extra cpu switches */
#ifdef CHECK_AND_RUN_ALTERNATE_CPU
			CHECK_AND_RUN_ALTERNATE_CPU
#endif
			
			CPU_DELAY_CLK
			
			PROCESS_ALARMS
			
			{
				enum cpu_int pending_interrupt;
				
				if (!(CPU_INT_STATUS->global_pending_int & IK_IRQ)
					&& (CPU_INT_STATUS->global_pending_int & IK_IRQPEND)
					&& CPU_INT_STATUS->irq_pending_clk <= CLK) {
					interrupt_ack_irq(CPU_INT_STATUS);
				}
				
				pending_interrupt = CPU_INT_STATUS->global_pending_int;
				if (pending_interrupt != IK_NONE)
				{
					DO_INTERRUPT(pending_interrupt);
					
					// c64 debugger - check interrupt breakpoints when irq is ack'ed
					if ((pending_interrupt & IK_IRQ) && ((reg_p & P_INTERRUPT) == P_INTERRUPT))
					{
						if (c64d_drive1541_is_checking_irq_breakpoints_enabled() == 1)
						{
							if (drive_context[0]->via1d1541->c64d_irq_flagged == 1)
							{
								drive_context[0]->via1d1541->c64d_irq_flagged = 0;
								c64d_drive1541_check_irqvia1_breakpoint();
							}
							else if (drive_context[0]->via2->c64d_irq_flagged == 1)
							{
								drive_context[0]->via2->c64d_irq_flagged = 0;
								c64d_drive1541_check_irqvia2_breakpoint();
							}
							else
							{
								// must be IEC?  // TODO: confirm this
								c64d_drive1541_check_irqiec_breakpoint();
							}
						}
					}
					//
					
					if (!(CPU_INT_STATUS->global_pending_int & IK_IRQ)
						&& CPU_INT_STATUS->global_pending_int & IK_IRQPEND) {
						CPU_INT_STATUS->global_pending_int &= ~IK_IRQPEND;
					}
					CPU_DELAY_CLK
					
					PROCESS_ALARMS
				}
			}
			
			// c64d
			if (_c64d_new_drive_pc == -1)
			{
				viceCurrentDiskPC[0] = reg_pc;
			}
			else
			{
				viceCurrentDiskPC[0] = _c64d_new_drive_pc;
				cpu->cpu_regs.pc = _c64d_new_drive_pc;
				reg_pc = _c64d_new_drive_pc;
			}
			//
			
			{
				if (c64d_debug_mode != DEBUGGER_MODE_RUN_ONE_INSTRUCTION
					&& c64d_debug_mode != DEBUGGER_MODE_RUN_ONE_CYCLE)
				{
					// c64d check PC breakpoint after IRQ or trap
					c64d_drive1541_check_pc_breakpoint(reg_pc);
					viceCurrentDiskPC[0] = reg_pc;
					c64d_debug_pause_check(0);
				}
			}
			
			{
				opcode_t opcode;
#ifdef VICE_DEBUG
				CLOCK debug_clk;
#ifdef DRIVE_CPU
				debug_clk = CLK;
#else
				debug_clk = maincpu_clk;
#endif
#endif
				
#ifdef FEATURE_CPUMEMHISTORY
#ifndef DRIVE_CPU
				memmap_state |= (MEMMAP_STATE_INSTR | MEMMAP_STATE_OPCODE);
#endif
#endif
				SET_LAST_ADDR(reg_pc);
				
				
				// c64d: TODO: forcing reg_pc does not work with FETCH_OPCODE due to bank_base mismatch. how to fix this?
				
//				if (_c64d_new_drive_pc == -1)
//				{
//					FETCH_OPCODE(opcode);
//				}
//				else
				{
					C64D_FETCH_OPCODE_LOAD(opcode);
					
					_c64d_new_drive_pc = -1;
				}
				
				
				
#ifdef FEATURE_CPUMEMHISTORY
#ifndef DRIVE_CPU
#ifndef C64DTV
				/* HACK to cope with FETCH_OPCODE optimization in x64 */
				if (((int)reg_pc) < bank_limit) {
					memmap_mark_read(reg_pc);
				}
#endif
				if (p0 == 0x20) {
					monitor_cpuhistory_store(reg_pc, p0, p1, LOAD(reg_pc + 2), reg_a_read, reg_x_read, reg_y_read, reg_sp, LOCAL_STATUS());
				} else {
					monitor_cpuhistory_store(reg_pc, p0, p1, p2 >> 8, reg_a_read, reg_x_read, reg_y_read, reg_sp, LOCAL_STATUS());
				}
				memmap_state &= ~(MEMMAP_STATE_INSTR | MEMMAP_STATE_OPCODE);
#endif
#endif
				
#ifdef VICE_DEBUG
#ifdef DRIVE_CPU
				if (TRACEFLG) {
					BYTE op = (BYTE)(p0);
					BYTE lo = (BYTE)(p1);
					BYTE hi = (BYTE)(p2 >> 8);
					
					debug_drive((DWORD)(reg_pc), debug_clk,
								mon_disassemble_to_string(e_disk8_space,
														  reg_pc, op,
														  lo, hi, 0, 1, "6502"),
								reg_a_read, reg_x_read, reg_y_read, reg_sp, drv->mynumber + 8);
				}
#else
				if (TRACEFLG) {
					BYTE op = (BYTE)(p0);
					BYTE lo = (BYTE)(p1);
					BYTE hi = (BYTE)(p2 >> 8);
					
					if (op == 0x20) {
						hi = LOAD(reg_pc + 2);
					}
					
					debug_maincpu((DWORD)(reg_pc), debug_clk,
								  mon_disassemble_to_string(e_comp_space,
															reg_pc, op,
															lo, hi, 0, 1, "6502"),
								  reg_a_read, reg_x_read, reg_y_read, reg_sp);
				}
				if (debug.perform_break_into_monitor) {
					monitor_startup_trap();
					debug.perform_break_into_monitor = 0;
				}
#endif
#endif
				
			trap_skipped:
				SET_LAST_OPCODE(p0);
				
				switch (p0) {
					case 0x00:          /* BRK */
						BRK();
						break;
						
					case 0x01:          /* ORA ($nn,X) */
						ORA(LOAD_IND_X(p1), 1, 2);
						break;
						
					case 0x02:          /* JAM - also used for traps */
						STATIC_ASSERT(TRAP_OPCODE == 0x02);
						JAM_02();
						break;
						
					case 0x22:          /* JAM */
					case 0x52:          /* JAM */
					case 0x62:          /* JAM */
					case 0x72:          /* JAM */
					case 0x92:          /* JAM */
					case 0xb2:          /* JAM */
					case 0xd2:          /* JAM */
					case 0xf2:          /* JAM */
#ifndef C64DTV
					case 0x12:          /* JAM */
					case 0x32:          /* JAM */
					case 0x42:          /* JAM */
#endif
						REWIND_FETCH_OPCODE(CLK);
						JAM();
						break;
						
#ifdef C64DTV
						/* These opcodes are defined in c64/c64dtvcpu.c */
					case 0x12:          /* BRA */
						BRANCH(1, p1);
						break;
						
					case 0x32:          /* SAC */
						SAC(p1);
						break;
						
					case 0x42:          /* SIR */
						SIR(p1);
						break;
#endif
						
					case 0x03:          /* SLO ($nn,X) */
						SLO(LOAD_ZERO_ADDR(p1 + reg_x_read), 3, CLK_IND_X_RMW, 2, LOAD_ABS, STORE_ABS);
						break;
						
					case 0x04:          /* NOOP $nn */
					case 0x44:          /* NOOP $nn */
					case 0x64:          /* NOOP $nn */
						NOOP(1, 2);
						break;
						
					case 0x05:          /* ORA $nn */
						ORA(LOAD_ZERO(p1), 1, 2);
						break;
						
					case 0x06:          /* ASL $nn */
						ASL(p1, CLK_ZERO_RMW, 2, LOAD_ZERO, STORE_ABS);
						break;
						
					case 0x07:          /* SLO $nn */
						SLO(p1, 0, CLK_ZERO_RMW, 2, LOAD_ZERO, STORE_ABS);
						break;
						
					case 0x08:          /* PHP */
#ifdef DRIVE_CPU
						drivecpu_rotate();
						if (drivecpu_byte_ready()) {
							drivecpu_byte_ready_egde_clear();
							LOCAL_SET_OVERFLOW(1);
						}
#endif
						PHP();
						break;
						
					case 0x09:          /* ORA #$nn */
						ORA(p1, 0, 2);
						break;
						
					case 0x0a:          /* ASL A */
						ASL_A();
						break;
						
					case 0x0b:          /* ANC #$nn */
						ANC(p1, 2);
						break;
						
					case 0x0c:          /* NOOP $nnnn */
						NOOP_ABS();
						break;
						
					case 0x0d:          /* ORA $nnnn */
						ORA(LOAD(p2), 1, 3);
						break;
						
					case 0x0e:          /* ASL $nnnn */
						ASL(p2, CLK_ABS_RMW2, 3, LOAD_ABS, STORE_ABS);
						break;
						
					case 0x0f:          /* SLO $nnnn */
						SLO(p2, 0, CLK_ABS_RMW2, 3, LOAD_ABS, STORE_ABS);
						break;
						
					case 0x10:          /* BPL $nnnn */
						BRANCH(!LOCAL_SIGN(), p1);
						break;
						
					case 0x11:          /* ORA ($nn),Y */
						ORA(LOAD_IND_Y(p1), 1, 2);
						break;
						
					case 0x13:          /* SLO ($nn),Y */
						SLO_IND_Y(p1);
						break;
						
					case 0x14:          /* NOOP $nn,X */
					case 0x34:          /* NOOP $nn,X */
					case 0x54:          /* NOOP $nn,X */
					case 0x74:          /* NOOP $nn,X */
					case 0xd4:          /* NOOP $nn,X */
					case 0xf4:          /* NOOP $nn,X */
						NOOP(CLK_NOOP_ZERO_X, 2);
						break;
						
					case 0x15:          /* ORA $nn,X */
						ORA(LOAD_ZERO_X(p1), CLK_ZERO_I2, 2);
						break;
						
					case 0x16:          /* ASL $nn,X */
						ASL((p1 + reg_x_read) & 0xff, CLK_ZERO_I_RMW, 2, LOAD_ZERO, STORE_ABS);
						break;
						
					case 0x17:          /* SLO $nn,X */
						SLO((p1 + reg_x_read) & 0xff, 0, CLK_ZERO_I_RMW, 2, LOAD_ZERO, STORE_ABS);
						break;
						
					case 0x18:          /* CLC */
						CLC();
						break;
						
					case 0x19:          /* ORA $nnnn,Y */
						ORA(LOAD_ABS_Y(p2), 1, 3);
						break;
						
					case 0x1a:          /* NOOP */
					case 0x3a:          /* NOOP */
					case 0x5a:          /* NOOP */
					case 0x7a:          /* NOOP */
					case 0xda:          /* NOOP */
					case 0xfa:          /* NOOP */
						NOOP_IMM(1);
						break;
						
					case 0x1b:          /* SLO $nnnn,Y */
						SLO(p2, 0, CLK_ABS_I_RMW2, 3, LOAD_ABS_Y_RMW, STORE_ABS_Y_RMW);
						break;
						
					case 0x1c:          /* NOOP $nnnn,X */
					case 0x3c:          /* NOOP $nnnn,X */
					case 0x5c:          /* NOOP $nnnn,X */
					case 0x7c:          /* NOOP $nnnn,X */
					case 0xdc:          /* NOOP $nnnn,X */
					case 0xfc:          /* NOOP $nnnn,X */
						NOOP_ABS_X();
						break;
						
					case 0x1d:          /* ORA $nnnn,X */
						ORA(LOAD_ABS_X(p2), 1, 3);
						break;
						
					case 0x1e:          /* ASL $nnnn,X */
						ASL(p2, CLK_ABS_I_RMW2, 3, LOAD_ABS_X_RMW, STORE_ABS_X_RMW);
						break;
						
					case 0x1f:          /* SLO $nnnn,X */
						SLO(p2, 0, CLK_ABS_I_RMW2, 3, LOAD_ABS_X_RMW, STORE_ABS_X_RMW);
						break;
						
					case 0x20:          /* JSR $nnnn */
						JSR();
						break;
						
					case 0x21:          /* AND ($nn,X) */
						AND(LOAD_IND_X(p1), 1, 2);
						break;
						
					case 0x23:          /* RLA ($nn,X) */
						RLA(LOAD_ZERO_ADDR(p1 + reg_x_read), 3, CLK_IND_X_RMW, 2, LOAD_ABS, STORE_ABS);
						break;
						
					case 0x24:          /* BIT $nn */
						BIT(LOAD_ZERO(p1), 2);
						break;
						
					case 0x25:          /* AND $nn */
						AND(LOAD_ZERO(p1), 1, 2);
						break;
						
					case 0x26:          /* ROL $nn */
						ROL(p1, CLK_ZERO_RMW, 2, LOAD_ZERO, STORE_ABS);
						break;
						
					case 0x27:          /* RLA $nn */
						RLA(p1, 0, CLK_ZERO_RMW, 2, LOAD_ZERO, STORE_ABS);
						break;
						
					case 0x28:          /* PLP */
						PLP();
						break;
						
					case 0x29:          /* AND #$nn */
						AND(p1, 0, 2);
						break;
						
					case 0x2a:          /* ROL A */
						ROL_A();
						break;
						
					case 0x2b:          /* ANC #$nn */
						ANC(p1, 2);
						break;
						
					case 0x2c:          /* BIT $nnnn */
						BIT(LOAD(p2), 3);
						break;
						
					case 0x2d:          /* AND $nnnn */
						AND(LOAD(p2), 1, 3);
						break;
						
					case 0x2e:          /* ROL $nnnn */
						ROL(p2, CLK_ABS_RMW2, 3, LOAD_ABS, STORE_ABS);
						break;
						
					case 0x2f:          /* RLA $nnnn */
						RLA(p2, 0, CLK_ABS_RMW2, 3, LOAD_ABS, STORE_ABS);
						break;
						
					case 0x30:          /* BMI $nnnn */
						BRANCH(LOCAL_SIGN(), p1);
						break;
						
					case 0x31:          /* AND ($nn),Y */
						AND(LOAD_IND_Y(p1), 1, 2);
						break;
						
					case 0x33:          /* RLA ($nn),Y */
						RLA_IND_Y(p1);
						break;
						
					case 0x35:          /* AND $nn,X */
						AND(LOAD_ZERO_X(p1), CLK_ZERO_I2, 2);
						break;
						
					case 0x36:          /* ROL $nn,X */
						ROL((p1 + reg_x_read) & 0xff, CLK_ZERO_I_RMW, 2, LOAD_ZERO, STORE_ABS);
						break;
						
					case 0x37:          /* RLA $nn,X */
						RLA((p1 + reg_x_read) & 0xff, 0, CLK_ZERO_I_RMW, 2, LOAD_ZERO, STORE_ABS);
						break;
						
					case 0x38:          /* SEC */
						SEC();
						break;
						
					case 0x39:          /* AND $nnnn,Y */
						AND(LOAD_ABS_Y(p2), 1, 3);
						break;
						
					case 0x3b:          /* RLA $nnnn,Y */
						RLA(p2, 0, CLK_ABS_I_RMW2, 3, LOAD_ABS_Y_RMW, STORE_ABS_Y_RMW);
						break;
						
					case 0x3d:          /* AND $nnnn,X */
						AND(LOAD_ABS_X(p2), 1, 3);
						break;
						
					case 0x3e:          /* ROL $nnnn,X */
						ROL(p2, CLK_ABS_I_RMW2, 3, LOAD_ABS_X_RMW, STORE_ABS_X_RMW);
						break;
						
					case 0x3f:          /* RLA $nnnn,X */
						RLA(p2, 0, CLK_ABS_I_RMW2, 3, LOAD_ABS_X_RMW, STORE_ABS_X_RMW);
						break;
						
					case 0x40:          /* RTI */
						RTI();
						break;
						
					case 0x41:          /* EOR ($nn,X) */
						EOR(LOAD_IND_X(p1), 1, 2);
						break;
						
					case 0x43:          /* SRE ($nn,X) */
						SRE(LOAD_ZERO_ADDR(p1 + reg_x_read), 3, CLK_IND_X_RMW, 2, LOAD_ABS, STORE_ABS);
						break;
						
					case 0x45:          /* EOR $nn */
						EOR(LOAD_ZERO(p1), 1, 2);
						break;
						
					case 0x46:          /* LSR $nn */
						LSR(p1, CLK_ZERO_RMW, 2, LOAD_ZERO, STORE_ABS);
						break;
						
					case 0x47:          /* SRE $nn */
						SRE(p1, 0, CLK_ZERO_RMW, 2, LOAD_ZERO, STORE_ABS);
						break;
						
					case 0x48:          /* PHA */
						PHA();
						break;
						
					case 0x49:          /* EOR #$nn */
						EOR(p1, 0, 2);
						break;
						
					case 0x4a:          /* LSR A */
						LSR_A();
						break;
						
					case 0x4b:          /* ASR #$nn */
						ASR(p1, 2);
						break;
						
					case 0x4c:          /* JMP $nnnn */
						JMP(p2);
						break;
						
					case 0x4d:          /* EOR $nnnn */
						EOR(LOAD(p2), 1, 3);
						break;
						
					case 0x4e:          /* LSR $nnnn */
						LSR(p2, CLK_ABS_RMW2, 3, LOAD_ABS, STORE_ABS);
						break;
						
					case 0x4f:          /* SRE $nnnn */
						SRE(p2, 0, CLK_ABS_RMW2, 3, LOAD_ABS, STORE_ABS);
						break;
						
					case 0x50:          /* BVC $nnnn */
#ifdef DRIVE_CPU
						CLK_ADD(CLK, -1);
						drivecpu_rotate();
						if (drivecpu_byte_ready()) {
							drivecpu_byte_ready_egde_clear();
							LOCAL_SET_OVERFLOW(1);
						}
						CLK_ADD(CLK, 1);
#endif
						BRANCH(!LOCAL_OVERFLOW(), p1);
						break;
						
					case 0x51:          /* EOR ($nn),Y */
						EOR(LOAD_IND_Y(p1), 1, 2);
						break;
						
					case 0x53:          /* SRE ($nn),Y */
						SRE_IND_Y(p1);
						break;
						
					case 0x55:          /* EOR $nn,X */
						EOR(LOAD_ZERO_X(p1), CLK_ZERO_I2, 2);
						break;
						
					case 0x56:          /* LSR $nn,X */
						LSR((p1 + reg_x_read) & 0xff, CLK_ZERO_I_RMW, 2, LOAD_ZERO, STORE_ABS);
						break;
						
					case 0x57:          /* SRE $nn,X */
						SRE((p1 + reg_x_read) & 0xff, 0, CLK_ZERO_I_RMW, 2, LOAD_ZERO, STORE_ABS);
						break;
						
					case 0x58:          /* CLI */
						CLI();
						break;
						
					case 0x59:          /* EOR $nnnn,Y */
						EOR(LOAD_ABS_Y(p2), 1, 3);
						break;
						
					case 0x5b:          /* SRE $nnnn,Y */
						SRE(p2, 0, CLK_ABS_I_RMW2, 3, LOAD_ABS_Y_RMW, STORE_ABS_Y_RMW);
						break;
						
					case 0x5d:          /* EOR $nnnn,X */
						EOR(LOAD_ABS_X(p2), 1, 3);
						break;
						
					case 0x5e:          /* LSR $nnnn,X */
						LSR(p2, CLK_ABS_I_RMW2, 3, LOAD_ABS_X_RMW, STORE_ABS_X_RMW);
						break;
						
					case 0x5f:          /* SRE $nnnn,X */
						SRE(p2, 0, CLK_ABS_I_RMW2, 3, LOAD_ABS_X_RMW, STORE_ABS_X_RMW);
						break;
						
					case 0x60:          /* RTS */
						RTS();
						break;
						
					case 0x61:          /* ADC ($nn,X) */
						ADC(LOAD_IND_X(p1), 1, 2);
						break;
						
					case 0x63:          /* RRA ($nn,X) */
						RRA(LOAD_ZERO_ADDR(p1 + reg_x_read), 3, CLK_IND_X_RMW, 2, LOAD_ABS, STORE_ABS);
						break;
						
					case 0x65:          /* ADC $nn */
						ADC(LOAD_ZERO(p1), 1, 2);
						break;
						
					case 0x66:          /* ROR $nn */
						ROR(p1, CLK_ZERO_RMW, 2, LOAD_ZERO, STORE_ABS);
						break;
						
					case 0x67:          /* RRA $nn */
						RRA(p1, 0, CLK_ZERO_RMW, 2, LOAD_ZERO, STORE_ABS);
						break;
						
					case 0x68:          /* PLA */
						PLA();
						break;
						
					case 0x69:          /* ADC #$nn */
						ADC(p1, 0, 2);
						break;
						
					case 0x6a:          /* ROR A */
						ROR_A();
						break;
						
					case 0x6b:          /* ARR #$nn */
						ARR(p1, 2);
						break;
						
					case 0x6c:          /* JMP ($nnnn) */
						JMP_IND();
						break;
						
					case 0x6d:          /* ADC $nnnn */
						ADC(LOAD(p2), 1, 3);
						break;
						
					case 0x6e:          /* ROR $nnnn */
						ROR(p2, CLK_ABS_RMW2, 3, LOAD_ABS, STORE_ABS);
						break;
						
					case 0x6f:          /* RRA $nnnn */
						RRA(p2, 0, CLK_ABS_RMW2, 3, LOAD_ABS, STORE_ABS);
						break;
						
					case 0x70:          /* BVS $nnnn */
#ifdef DRIVE_CPU
						CLK_ADD(CLK, -1);
						drivecpu_rotate();
						if (drivecpu_byte_ready()) {
							drivecpu_byte_ready_egde_clear();
							LOCAL_SET_OVERFLOW(1);
						}
						CLK_ADD(CLK, 1);
#endif
						BRANCH(LOCAL_OVERFLOW(), p1);
						break;
						
					case 0x71:          /* ADC ($nn),Y */
						ADC(LOAD_IND_Y(p1), 1, 2);
						break;
						
					case 0x73:          /* RRA ($nn),Y */
						RRA_IND_Y(p1);
						break;
						
					case 0x75:          /* ADC $nn,X */
						ADC(LOAD_ZERO_X(p1), CLK_ZERO_I2, 2);
						break;
						
					case 0x76:          /* ROR $nn,X */
						ROR((p1 + reg_x_read) & 0xff, CLK_ZERO_I_RMW, 2, LOAD_ZERO, STORE_ABS);
						break;
						
					case 0x77:          /* RRA $nn,X */
						RRA((p1 + reg_x_read) & 0xff, 0, CLK_ZERO_I_RMW, 2, LOAD_ZERO, STORE_ABS);
						break;
						
					case 0x78:          /* SEI */
						SEI();
						break;
						
					case 0x79:          /* ADC $nnnn,Y */
						ADC(LOAD_ABS_Y(p2), 1, 3);
						break;
						
					case 0x7b:          /* RRA $nnnn,Y */
						RRA(p2, 0, CLK_ABS_I_RMW2, 3, LOAD_ABS_Y_RMW, STORE_ABS_Y_RMW);
						break;
						
					case 0x7d:          /* ADC $nnnn,X */
						ADC(LOAD_ABS_X(p2), 1, 3);
						break;
						
					case 0x7e:          /* ROR $nnnn,X */
						ROR(p2, CLK_ABS_I_RMW2, 3, LOAD_ABS_X_RMW, STORE_ABS_X_RMW);
						break;
						
					case 0x7f:          /* RRA $nnnn,X */
						RRA(p2, 0, CLK_ABS_I_RMW2, 3, LOAD_ABS_X_RMW, STORE_ABS_X_RMW);
						break;
						
					case 0x80:          /* NOOP #$nn */
					case 0x82:          /* NOOP #$nn */
					case 0x89:          /* NOOP #$nn */
					case 0xc2:          /* NOOP #$nn */
					case 0xe2:          /* NOOP #$nn */
						NOOP_IMM(2);
						break;
						
					case 0x81:          /* STA ($nn,X) */
						STA(LOAD_ZERO_ADDR(p1 + reg_x_read), 3, 1, 2, STORE_ABS);
						break;
						
					case 0x83:          /* SAX ($nn,X) */
						SAX(LOAD_ZERO_ADDR(p1 + reg_x_read), 3, 1, 2);
						break;
						
					case 0x84:          /* STY $nn */
						STY_ZERO(p1, 1, 2);
						break;
						
					case 0x85:          /* STA $nn */
						STA_ZERO(p1, 1, 2);
						break;
						
					case 0x86:          /* STX $nn */
						STX_ZERO(p1, 1, 2);
						break;
						
					case 0x87:          /* SAX $nn */
						SAX_ZERO(p1, 1, 2);
						break;
						
					case 0x88:          /* DEY */
						DEY();
						break;
						
					case 0x8a:          /* TXA */
						TXA();
						break;
						
					case 0x8b:          /* ANE #$nn */
						ANE(p1, 2);
						break;
						
					case 0x8c:          /* STY $nnnn */
						STY(p2, 1, 3);
						break;
						
					case 0x8d:          /* STA $nnnn */
						STA(p2, 0, 1, 3, STORE_ABS);
						break;
						
					case 0x8e:          /* STX $nnnn */
						STX(p2, 1, 3);
						break;
						
					case 0x8f:          /* SAX $nnnn */
						SAX(p2, 0, 1, 3);
						break;
						
					case 0x90:          /* BCC $nnnn */
						BRANCH(!LOCAL_CARRY(), p1);
						break;
						
					case 0x91:          /* STA ($nn),Y */
						STA_IND_Y(p1);
						break;
						
					case 0x93:          /* SHA ($nn),Y */
						SHA_IND_Y(p1);
						break;
						
					case 0x94:          /* STY $nn,X */
						STY_ZERO(p1 + reg_x_read, CLK_ZERO_I_STORE, 2);
						break;
						
					case 0x95:          /* STA $nn,X */
						STA_ZERO(p1 + reg_x_read, CLK_ZERO_I_STORE, 2);
						break;
						
					case 0x96:          /* STX $nn,Y */
						STX_ZERO(p1 + reg_y_read, CLK_ZERO_I_STORE, 2);
						break;
						
					case 0x97:          /* SAX $nn,Y */
						SAX((p1 + reg_y_read) & 0xff, 0, CLK_ZERO_I_STORE, 2);
						break;
						
					case 0x98:          /* TYA */
						TYA();
						break;
						
					case 0x99:          /* STA $nnnn,Y */
						STA(p2, 0, CLK_ABS_I_STORE2, 3, STORE_ABS_Y);
						break;
						
					case 0x9a:          /* TXS */
						TXS();
						break;
						
					case 0x9b:          /* SHS $nnnn,Y */
#ifdef C64DTV
						NOOP_ABS_Y();
#else
						SHS_ABS_Y(p2);
#endif
						break;
						
					case 0x9c:          /* SHY $nnnn,X */
						SHY_ABS_X(p2);
						break;
						
					case 0x9d:          /* STA $nnnn,X */
						STA(p2, 0, CLK_ABS_I_STORE2, 3, STORE_ABS_X);
						break;
						
					case 0x9e:          /* SHX $nnnn,Y */
						SHX_ABS_Y(p2);
						break;
						
					case 0x9f:          /* SHA $nnnn,Y */
						SHA_ABS_Y(p2);
						break;
						
					case 0xa0:          /* LDY #$nn */
						LDY(p1, 0, 2);
						break;
						
					case 0xa1:          /* LDA ($nn,X) */
						LDA(LOAD_IND_X(p1), 1, 2);
						break;
						
					case 0xa2:          /* LDX #$nn */
						LDX(p1, 0, 2);
						break;
						
					case 0xa3:          /* LAX ($nn,X) */
						LAX(LOAD_IND_X(p1), 1, 2);
						break;
						
					case 0xa4:          /* LDY $nn */
						LDY(LOAD_ZERO(p1), 1, 2);
						break;
						
					case 0xa5:          /* LDA $nn */
						LDA(LOAD_ZERO(p1), 1, 2);
						break;
						
					case 0xa6:          /* LDX $nn */
						LDX(LOAD_ZERO(p1), 1, 2);
						break;
						
					case 0xa7:          /* LAX $nn */
						LAX(LOAD_ZERO(p1), 1, 2);
						break;
						
					case 0xa8:          /* TAY */
						TAY();
						break;
						
					case 0xa9:          /* LDA #$nn */
						LDA(p1, 0, 2);
						break;
						
					case 0xaa:          /* TAX */
						TAX();
						break;
						
					case 0xab:          /* LXA #$nn */
						LXA(p1, 2);
						break;
						
					case 0xac:          /* LDY $nnnn */
						LDY(LOAD(p2), 1, 3);
						break;
						
					case 0xad:          /* LDA $nnnn */
						LDA(LOAD(p2), 1, 3);
						break;
						
					case 0xae:          /* LDX $nnnn */
						LDX(LOAD(p2), 1, 3);
						break;
						
					case 0xaf:          /* LAX $nnnn */
						LAX(LOAD(p2), 1, 3);
						break;
						
					case 0xb0:          /* BCS $nnnn */
						BRANCH(LOCAL_CARRY(), p1);
						break;
						
					case 0xb1:          /* LDA ($nn),Y */
						LDA(LOAD_IND_Y_BANK(p1), 1, 2);
						break;
						
					case 0xb3:          /* LAX ($nn),Y */
						LAX(LOAD_IND_Y(p1), 1, 2);
						break;
						
					case 0xb4:          /* LDY $nn,X */
						LDY(LOAD_ZERO_X(p1), CLK_ZERO_I2, 2);
						break;
						
					case 0xb5:          /* LDA $nn,X */
						LDA(LOAD_ZERO_X(p1), CLK_ZERO_I2, 2);
						break;
						
					case 0xb6:          /* LDX $nn,Y */
						LDX(LOAD_ZERO_Y(p1), CLK_ZERO_I2, 2);
						break;
						
					case 0xb7:          /* LAX $nn,Y */
						LAX(LOAD_ZERO_Y(p1), CLK_ZERO_I2, 2);
						break;
						
					case 0xb8:          /* CLV */
						CLV();
						break;
						
					case 0xb9:          /* LDA $nnnn,Y */
						LDA(LOAD_ABS_Y(p2), 1, 3);
						break;
						
					case 0xba:          /* TSX */
						TSX();
						break;
						
					case 0xbb:          /* LAS $nnnn,Y */
						LAS(LOAD_ABS_Y(p2), 1, 3);
						break;
						
					case 0xbc:          /* LDY $nnnn,X */
						LDY(LOAD_ABS_X(p2), 1, 3);
						break;
						
					case 0xbd:          /* LDA $nnnn,X */
						LDA(LOAD_ABS_X(p2), 1, 3);
						break;
						
					case 0xbe:          /* LDX $nnnn,Y */
						LDX(LOAD_ABS_Y(p2), 1, 3);
						break;
						
					case 0xbf:          /* LAX $nnnn,Y */
						LAX(LOAD_ABS_Y(p2), 1, 3);
						break;
						
					case 0xc0:          /* CPY #$nn */
						CPY(p1, 0, 2);
						break;
						
					case 0xc1:          /* CMP ($nn,X) */
						CMP(LOAD_IND_X(p1), 1, 2);
						break;
						
					case 0xc3:          /* DCP ($nn,X) */
						DCP(LOAD_ZERO_ADDR(p1 + reg_x_read), 3, CLK_IND_X_RMW, 2, LOAD_ABS, STORE_ABS);
						break;
						
					case 0xc4:          /* CPY $nn */
						CPY(LOAD_ZERO(p1), 1, 2);
						break;
						
					case 0xc5:          /* CMP $nn */
						CMP(LOAD_ZERO(p1), 1, 2);
						break;
						
					case 0xc6:          /* DEC $nn */
						DEC(p1, CLK_ZERO_RMW, 2, LOAD_ZERO, STORE_ABS);
						break;
						
					case 0xc7:          /* DCP $nn */
						DCP(p1, 0, CLK_ZERO_RMW, 2, LOAD_ZERO, STORE_ABS);
						break;
						
					case 0xc8:          /* INY */
						INY();
						break;
						
					case 0xc9:          /* CMP #$nn */
						CMP(p1, 0, 2);
						break;
						
					case 0xca:          /* DEX */
						DEX();
						break;
						
					case 0xcb:          /* SBX #$nn */
						SBX(p1, 2);
						break;
						
					case 0xcc:          /* CPY $nnnn */
						CPY(LOAD(p2), 1, 3);
						break;
						
					case 0xcd:          /* CMP $nnnn */
						CMP(LOAD(p2), 1, 3);
						break;
						
					case 0xce:          /* DEC $nnnn */
						DEC(p2, CLK_ABS_RMW2, 3, LOAD_ABS, STORE_ABS);
						break;
						
					case 0xcf:          /* DCP $nnnn */
						DCP(p2, 0, CLK_ABS_RMW2, 3, LOAD_ABS, STORE_ABS);
						break;
						
					case 0xd0:          /* BNE $nnnn */
						BRANCH(!LOCAL_ZERO(), p1);
						break;
						
					case 0xd1:          /* CMP ($nn),Y */
						CMP(LOAD_IND_Y(p1), 1, 2);
						break;
						
					case 0xd3:          /* DCP ($nn),Y */
						DCP_IND_Y(p1);
						break;
						
					case 0xd5:          /* CMP $nn,X */
						CMP(LOAD_ZERO_X(p1), CLK_ZERO_I2, 2);
						break;
						
					case 0xd6:          /* DEC $nn,X */
						DEC((p1 + reg_x_read) & 0xff, CLK_ZERO_I_RMW, 2, LOAD_ABS, STORE_ABS);
						break;
						
					case 0xd7:          /* DCP $nn,X */
						DCP((p1 + reg_x_read) & 0xff, 0, CLK_ZERO_I_RMW, 2, LOAD_ABS, STORE_ABS);
						break;
						
					case 0xd8:          /* CLD */
						CLD();
						break;
						
					case 0xd9:          /* CMP $nnnn,Y */
						CMP(LOAD_ABS_Y(p2), 1, 3);
						break;
						
					case 0xdb:          /* DCP $nnnn,Y */
						DCP(p2, 0, CLK_ABS_I_RMW2, 3, LOAD_ABS_Y_RMW, STORE_ABS_Y_RMW);
						break;
						
					case 0xdd:          /* CMP $nnnn,X */
						CMP(LOAD_ABS_X(p2), 1, 3);
						break;
						
					case 0xde:          /* DEC $nnnn,X */
						DEC(p2, CLK_ABS_I_RMW2, 3, LOAD_ABS_X_RMW, STORE_ABS_X_RMW);
						break;
						
					case 0xdf:          /* DCP $nnnn,X */
						DCP(p2, 0, CLK_ABS_I_RMW2, 3, LOAD_ABS_X_RMW, STORE_ABS_X_RMW);
						break;
						
					case 0xe0:          /* CPX #$nn */
						CPX(p1, 0, 2);
						break;
						
					case 0xe1:          /* SBC ($nn,X) */
						SBC(LOAD_IND_X(p1), 1, 2);
						break;
						
					case 0xe3:          /* ISB ($nn,X) */
						ISB(LOAD_ZERO_ADDR(p1 + reg_x_read), 3, CLK_IND_X_RMW, 2, LOAD_ABS, STORE_ABS);
						break;
						
					case 0xe4:          /* CPX $nn */
						CPX(LOAD_ZERO(p1), 1, 2);
						break;
						
					case 0xe5:          /* SBC $nn */
						SBC(LOAD_ZERO(p1), 1, 2);
						break;
						
					case 0xe6:          /* INC $nn */
						INC(p1, CLK_ZERO_RMW, 2, LOAD_ZERO, STORE_ABS);
						break;
						
					case 0xe7:          /* ISB $nn */
						ISB(p1, 0, CLK_ZERO_RMW, 2, LOAD_ZERO, STORE_ABS);
						break;
						
					case 0xe8:          /* INX */
						INX();
						break;
						
					case 0xe9:          /* SBC #$nn */
						SBC(p1, 0, 2);
						break;
						
					case 0xea:          /* NOP */
						NOP();
						break;
						
					case 0xeb:          /* USBC #$nn (same as SBC) */
						SBC(p1, 0, 2);
						break;
						
					case 0xec:          /* CPX $nnnn */
						CPX(LOAD(p2), 1, 3);
						break;
						
					case 0xed:          /* SBC $nnnn */
						SBC(LOAD(p2), 1, 3);
						break;
						
					case 0xee:          /* INC $nnnn */
						INC(p2, CLK_ABS_RMW2, 3, LOAD_ABS, STORE_ABS);
						break;
						
					case 0xef:          /* ISB $nnnn */
						ISB(p2, 0, CLK_ABS_RMW2, 3, LOAD_ABS, STORE_ABS);
						break;
						
					case 0xf0:          /* BEQ $nnnn */
						BRANCH(LOCAL_ZERO(), p1);
						break;
						
					case 0xf1:          /* SBC ($nn),Y */
						SBC(LOAD_IND_Y(p1), 1, 2);
						break;
						
					case 0xf3:          /* ISB ($nn),Y */
						ISB_IND_Y(p1);
						break;
						
					case 0xf5:          /* SBC $nn,X */
						SBC(LOAD_ZERO_X(p1), CLK_ZERO_I2, 2);
						break;
						
					case 0xf6:          /* INC $nn,X */
						INC((p1 + reg_x_read) & 0xff, CLK_ZERO_I_RMW, 2, LOAD_ZERO, STORE_ABS);
						break;
						
					case 0xf7:          /* ISB $nn,X */
						ISB((p1 + reg_x_read) & 0xff, 0, CLK_ZERO_I_RMW, 2, LOAD_ZERO, STORE_ABS);
						break;
						
					case 0xf8:          /* SED */
						SED();
						break;
						
					case 0xf9:          /* SBC $nnnn,Y */
						SBC(LOAD_ABS_Y(p2), 1, 3);
						break;
						
					case 0xfb:          /* ISB $nnnn,Y */
						ISB(p2, 0, CLK_ABS_I_RMW2, 3, LOAD_ABS_Y_RMW, STORE_ABS_Y_RMW);
						break;
						
					case 0xfd:          /* SBC $nnnn,X */
						SBC(LOAD_ABS_X(p2), 1, 3);
						break;
						
					case 0xfe:          /* INC $nnnn,X */
						INC(p2, CLK_ABS_I_RMW2, 3, LOAD_ABS_X_RMW, STORE_ABS_X_RMW);
						break;
						
					case 0xff:          /* ISB $nnnn,X */
						ISB(p2, 0, CLK_ABS_I_RMW2, 3, LOAD_ABS_X_RMW, STORE_ABS_X_RMW);
						break;
				}
			}
		}

		
/// 6510core.c ends here
///
///
		
		////
		c64d_mark_drive1541_cell_execute(LAST_OPCODE_ADDR & 0xFFFF, LAST_OPCODE_INFO & 0xFF);
		
		
		if (_c64d_new_drive_pc == -1)
		{
			viceCurrentDiskPC[0] = reg_pc;
		}
		else
		{
			viceCurrentDiskPC[0] = _c64d_new_drive_pc;
			cpu->cpu_regs.pc = _c64d_new_drive_pc;
		}
		
		
		//
		if (c64d_is_debug_on_drive1541())
		{
			if (c64d_debug_mode == DEBUGGER_MODE_RUN_ONE_INSTRUCTION)
			{
				c64d_debug_mode = DEBUGGER_MODE_PAUSED;
			}
			
			//LOGD("reg_pc=%04x", reg_pc);
			c64d_drive1541_check_pc_breakpoint(reg_pc);
			
			viceCurrentDiskPC[0] = reg_pc;
			
			c64d_debug_pause_check(0);
		}
		
		//		if (c64d_debug_mode == C64_DEBUG_SHUTDOWN)
		//		{
		//			//LOGD("c64d_debug_mode=C64_DEBUG_SHUTDOWN!");
		//			return;
		//		}
		
		////////

		

    }

    cpu->last_clk = clk_value;
    drivecpu_sleep(drv);
}

#ifdef _MSC_VER
#pragma optimize("",on)
#endif

/* ------------------------------------------------------------------------- */

void c64d_interrupt_drivecpu_trigger_trap(drive_context_t *drv,
										  void (*trap_func)(WORD, void *data),
										  void *data);

void _c64d_set_drive_pc_trap(WORD addr, void *data)
{
	WORD newpc = _c64d_new_drive_pc;
	
	drive_context[_c64d_new_drive_dnr]->cpu->cpu_regs.pc = newpc;
	
	_c64d_new_drive_pc = -1;
}

void c64d_set_drive_pc(int driveNr, uint16 pc)
{
	viceCurrentDiskPC[driveNr] = pc;
	_c64d_new_drive_pc = pc;
	_c64d_new_drive_dnr = driveNr;
	
	// TODO: we need a trap on drive cpu
	//	interrupt_maincpu_trigger_trap(_c64d_set_drive_pc_trap, NULL);
}

uint8 _c64d_new_drive_register_a, _c64d_new_drive_register_x, _c64d_new_drive_register_y, _c64d_new_drive_register_p, _c64d_new_drive_register_sp;

void _c64d_set_drive_register_a_trap(WORD addr, void *data)
{
	drive_context_t *trapDriveContext = (drive_context_t *)data;
	
	trapDriveContext->cpu->cpu_regs.a = _c64d_new_drive_register_a;

	_c64d_new_drive_register_a = -1;
}

void c64d_set_drive_register_a(int driveNr, uint8 a)
{
	_c64d_new_drive_register_a = a;
	
	c64d_interrupt_drivecpu_trigger_trap(drive_context[driveNr], _c64d_set_drive_register_a_trap, (void*)(drive_context[driveNr]));
}

void _c64d_set_drive_register_x_trap(WORD addr, void *data)
{
	drive_context_t *trapDriveContext = (drive_context_t *)data;
	
	trapDriveContext->cpu->cpu_regs.x = _c64d_new_drive_register_x;
	
	_c64d_new_drive_register_x = -1;
}

void c64d_set_drive_register_x(int driveNr, uint8 x)
{
	_c64d_new_drive_register_x = x;
	
	c64d_interrupt_drivecpu_trigger_trap(drive_context[driveNr], _c64d_set_drive_register_x_trap, (void*)(drive_context[driveNr]));
}

void _c64d_set_drive_register_y_trap(WORD addr, void *data)
{
	drive_context_t *trapDriveContext = (drive_context_t *)data;
	
	trapDriveContext->cpu->cpu_regs.y = _c64d_new_drive_register_y;
	
	_c64d_new_drive_register_y = -1;
}

void c64d_set_drive_register_y(int driveNr, uint8 y)
{
	_c64d_new_drive_register_y = y;
	
	c64d_interrupt_drivecpu_trigger_trap(drive_context[driveNr], _c64d_set_drive_register_y_trap, (void*)(drive_context[driveNr]));
}

void _c64d_set_drive_register_p_trap(WORD addr, void *data)
{
	drive_context_t *trapDriveContext = (drive_context_t *)data;
	
	trapDriveContext->cpu->cpu_regs.p = _c64d_new_drive_register_p;
	
	_c64d_new_drive_register_p = -1;
}

void c64d_set_drive_register_p(int driveNr, uint8 p)
{
	_c64d_new_drive_register_p = p;
	
	c64d_interrupt_drivecpu_trigger_trap(drive_context[driveNr], _c64d_set_drive_register_p_trap, (void*)(drive_context[driveNr]));
}

void _c64d_set_drive_register_sp_trap(WORD addr, void *data)
{
	drive_context_t *trapDriveContext = (drive_context_t *)data;
	
	trapDriveContext->cpu->cpu_regs.sp = _c64d_new_drive_register_sp;
	
	_c64d_new_drive_register_sp = -1;
}

void c64d_set_drive_register_sp(int driveNr, uint8 sp)
{
	_c64d_new_drive_register_sp = sp;
	
	c64d_interrupt_drivecpu_trigger_trap(drive_context[driveNr], _c64d_set_drive_register_sp_trap, (void*)(drive_context[driveNr]));
}


void c64d_set_drivecpu_regs_no_trap(int driveNr, uint8 a, uint8 x, uint8 y, uint8 p, uint8 sp)
{
	drive_context[driveNr]->cpu->cpu_regs.a = a;
	drive_context[driveNr]->cpu->cpu_regs.x = x;
	drive_context[driveNr]->cpu->cpu_regs.y = y;
	drive_context[driveNr]->cpu->cpu_regs.p = p;
	drive_context[driveNr]->cpu->cpu_regs.sp = sp;
}

void c64d_set_drivecpu_pc_no_trap(int driveNr, uint16 pc)
{
	drive_context[driveNr]->cpu->cpu_regs.pc = pc;
}

static void drivecpu_set_bank_base(void *context)
{
    drive_context_t *drv;
    drivecpu_context_t *cpu;

    drv = (drive_context_t *)context;
    cpu = drv->cpu;

    JUMP(reg_pc);
}

/* Inlining this fuction makes no sense and would only bloat the code.  */
static void drive_jam(drive_context_t *drv)
{
    unsigned int tmp;
    char *dname = "  Drive";
    drivecpu_context_t *cpu;

    cpu = drv->cpu;

    switch (drv->drive->type) {
        case DRIVE_TYPE_1540:
            dname = "  1540";
            break;
        case DRIVE_TYPE_1541:
            dname = "  1541";
            break;
        case DRIVE_TYPE_1541II:
            dname = "1541-II";
            break;
        case DRIVE_TYPE_1551:
            dname = "  1551";
            break;
        case DRIVE_TYPE_1570:
            dname = "  1570";
            break;
        case DRIVE_TYPE_1571:
            dname = "  1571";
            break;
        case DRIVE_TYPE_1571CR:
            dname = "  1571CR";
            break;
        case DRIVE_TYPE_1581:
            dname = "  1581";
            break;
        case DRIVE_TYPE_2031:
            dname = "  2031";
            break;
        case DRIVE_TYPE_1001:
            dname = "  1001";
            break;
        case DRIVE_TYPE_2040:
            dname = "  2040";
            break;
        case DRIVE_TYPE_3040:
            dname = "  3040";
            break;
        case DRIVE_TYPE_4040:
            dname = "  4040";
            break;
        case DRIVE_TYPE_8050:
            dname = "  8050";
            break;
        case DRIVE_TYPE_8250:
            dname = "  8250";
            break;
    }

    tmp = machine_jam("%s CPU: JAM at $%04X  ", dname, (int)reg_pc);
    switch (tmp) {
        case JAM_RESET:
            reg_pc = 0xeaa0;
            drivecpu_set_bank_base((void *)drv);
            machine_trigger_reset(MACHINE_RESET_MODE_SOFT);
            break;
        case JAM_HARD_RESET:
            reg_pc = 0xeaa0;
            drivecpu_set_bank_base((void *)drv);
            machine_trigger_reset(MACHINE_RESET_MODE_HARD);
            break;
        case JAM_MONITOR:
            monitor_startup(drv->cpu->monspace);
            break;
        default:
            CLK++;
    }
}

/* ------------------------------------------------------------------------- */

#define SNAP_MAJOR 1
#define SNAP_MINOR 1

int drivecpu_snapshot_write_module(drive_context_t *drv, snapshot_t *s)
{
    snapshot_module_t *m;
    drivecpu_context_t *cpu;

    cpu = drv->cpu;

    m = snapshot_module_create(s, drv->cpu->snap_module_name,
                               ((BYTE)(SNAP_MAJOR)), ((BYTE)(SNAP_MINOR)));
    if (m == NULL) {
        return -1;
    }

    if (0
        || SMW_DW(m, (DWORD) *(drv->clk_ptr)) < 0
        || SMW_B(m, (BYTE)MOS6510_REGS_GET_A(&(cpu->cpu_regs))) < 0
        || SMW_B(m, (BYTE)MOS6510_REGS_GET_X(&(cpu->cpu_regs))) < 0
        || SMW_B(m, (BYTE)MOS6510_REGS_GET_Y(&(cpu->cpu_regs))) < 0
        || SMW_B(m, (BYTE)MOS6510_REGS_GET_SP(&(cpu->cpu_regs))) < 0
        || SMW_W(m, (WORD)MOS6510_REGS_GET_PC(&(cpu->cpu_regs))) < 0
        || SMW_B(m, (BYTE)MOS6510_REGS_GET_STATUS(&(cpu->cpu_regs))) < 0
        || SMW_DW(m, (DWORD)(cpu->last_opcode_info)) < 0
        || SMW_DW(m, (DWORD)(cpu->last_clk)) < 0
        || SMW_DW(m, (DWORD)(cpu->cycle_accum)) < 0
        || SMW_DW(m, (DWORD)(cpu->last_exc_cycles)) < 0
        || SMW_DW(m, (DWORD)(cpu->stop_clk)) < 0
        ) {
        goto fail;
    }

    if (interrupt_write_snapshot(cpu->int_status, m) < 0) {
        goto fail;
    }

    if (drv->drive->type == DRIVE_TYPE_1540
        || drv->drive->type == DRIVE_TYPE_1541
        || drv->drive->type == DRIVE_TYPE_1541II
        || drv->drive->type == DRIVE_TYPE_1551
        || drv->drive->type == DRIVE_TYPE_1570
        || drv->drive->type == DRIVE_TYPE_1571
        || drv->drive->type == DRIVE_TYPE_1571CR
        || drv->drive->type == DRIVE_TYPE_2031) {
        if (SMW_BA(m, drv->drive->drive_ram, 0x800) < 0) {
            goto fail;
        }
    }

    if (drv->drive->type == DRIVE_TYPE_1581
        || drv->drive->type == DRIVE_TYPE_2000
        || drv->drive->type == DRIVE_TYPE_4000) {
        if (SMW_BA(m, drv->drive->drive_ram, 0x2000) < 0) {
            goto fail;
        }
    }
    if (drive_check_old(drv->drive->type)) {
        if (SMW_BA(m, drv->drive->drive_ram, 0x1100) < 0) {
            goto fail;
        }
    }

    if (interrupt_write_new_snapshot(cpu->int_status, m) < 0) {
        goto fail;
    }

    return snapshot_module_close(m);

fail:
    if (m != NULL) {
        snapshot_module_close(m);
    }
    return -1;
}

int drivecpu_snapshot_read_module(drive_context_t *drv, snapshot_t *s)
{
    BYTE major, minor;
    snapshot_module_t *m;
    BYTE a, x, y, sp, status;
    WORD pc;
    drivecpu_context_t *cpu;

    cpu = drv->cpu;

    m = snapshot_module_open(s, drv->cpu->snap_module_name, &major, &minor);
    if (m == NULL) {
        return -1;
    }

    /* Before we start make sure all devices are reset.  */
    drivecpu_reset(drv);

    /* XXX: Assumes `CLOCK' is the same size as a `DWORD'.  */
    if (0
        || SMR_DW(m, drv->clk_ptr) < 0
        || SMR_B(m, &a) < 0
        || SMR_B(m, &x) < 0
        || SMR_B(m, &y) < 0
        || SMR_B(m, &sp) < 0
        || SMR_W(m, &pc) < 0
        || SMR_B(m, &status) < 0
        || SMR_DW_UINT(m, &(cpu->last_opcode_info)) < 0
        || SMR_DW(m, &(cpu->last_clk)) < 0
        || SMR_DW(m, &(cpu->cycle_accum)) < 0
        || SMR_DW(m, &(cpu->last_exc_cycles)) < 0
        || SMR_DW(m, &(cpu->stop_clk)) < 0
        ) {
        goto fail;
    }

    MOS6510_REGS_SET_A(&(cpu->cpu_regs), a);
    MOS6510_REGS_SET_X(&(cpu->cpu_regs), x);
    MOS6510_REGS_SET_Y(&(cpu->cpu_regs), y);
    MOS6510_REGS_SET_SP(&(cpu->cpu_regs), sp);
    MOS6510_REGS_SET_PC(&(cpu->cpu_regs), pc);
    MOS6510_REGS_SET_STATUS(&(cpu->cpu_regs), status);

    log_message(drv->drive->log, "RESET (For undump).");

    interrupt_cpu_status_reset(cpu->int_status);

    machine_drive_reset(drv);

    if (interrupt_read_snapshot(cpu->int_status, m) < 0) {
        goto fail;
    }

    if (drv->drive->type == DRIVE_TYPE_1540
        || drv->drive->type == DRIVE_TYPE_1541
        || drv->drive->type == DRIVE_TYPE_1541II
        || drv->drive->type == DRIVE_TYPE_1551
        || drv->drive->type == DRIVE_TYPE_1570
        || drv->drive->type == DRIVE_TYPE_1571
        || drv->drive->type == DRIVE_TYPE_1571CR
        || drv->drive->type == DRIVE_TYPE_2031) {
        if (SMR_BA(m, drv->drive->drive_ram, 0x800) < 0) {
            goto fail;
        }
    }

    if (drv->drive->type == DRIVE_TYPE_1581
        || drv->drive->type == DRIVE_TYPE_2000
        || drv->drive->type == DRIVE_TYPE_4000) {
        if (SMR_BA(m, drv->drive->drive_ram, 0x2000) < 0) {
            goto fail;
        }
    }

    if (drive_check_old(drv->drive->type)) {
        if (SMR_BA(m, drv->drive->drive_ram, 0x1100) < 0) {
            goto fail;
        }
    }

    /* Update `*bank_base'.  */
    JUMP(reg_pc);

    if (interrupt_read_new_snapshot(drv->cpu->int_status, m) < 0) {
        goto fail;
    }

    return snapshot_module_close(m);

fail:
    if (m != NULL) {
        snapshot_module_close(m);
    }
    return -1;
}
