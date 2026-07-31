// SPDX-License-Identifier: GPL-2.0
/*
 * rknand_shims.c - 6.6 forward-port glue for the precompiled FTL blobs.
 *
 * The closed Rockchip FTL blobs (rk_zftl_arm32.S / rk_ftlv5_arm32.S) call kernel
 * routines by their pre-6.x symbol names. The C-preprocessor renames mainline
 * applies to these APIs never reach the hand-written assembler, so on the box's
 * Armbian 6.6.22 kernel these names are unresolved at module load:
 *
 *   - printk()                 : now a macro; only _printk() is exported.
 *   - usleep_range()           : now a static-inline over usleep_range_state().
 *   - __aeabi_unwind_cpp_pr0() : exported only when CONFIG_ARM_UNWIND=y. The box
 *                                uses CONFIG_UNWINDER_FRAME_POINTER, so it (and
 *                                pr1/pr2) are absent from the kernel.
 *
 * Provide real, module-local symbols with the exact legacy names so the dynamic
 * module linker binds the blob against them. (PDE_DATA() is shimmed the same way
 * over in rk_nand_blk.c, next to its pde_data() user.)
 *
 * Includes are deliberately minimal: pulling <linux/delay.h> would expose the
 * static-inline usleep_range and clash with the extern definition below.
 */
#include <linux/linkage.h>
#include <linux/printk.h>
#include <linux/stdarg.h>

/* Real exported primitives we forward to. */
extern void usleep_range_state(unsigned long min, unsigned long max,
			       unsigned int state);
/*
 * TASK_UNINTERRUPTIBLE (0x0002) hard-coded to avoid including <linux/sched.h>,
 * which transitively drags in <linux/delay.h> (see note above). This task-state
 * value is long-stable ABI.
 */
#define RKNAND_TASK_UNINTERRUPTIBLE 0x00000002U

/* Blob calls printk(); forward to the exported vprintk(). */
#undef printk
int printk(const char *fmt, ...)
{
	va_list args;
	int r;

	va_start(args, fmt);
	r = vprintk(fmt, args);
	va_end(args);
	return r;
}

/* Blob calls usleep_range(); forward to the exported usleep_range_state(). */
void usleep_range(unsigned long min, unsigned long max)
{
	usleep_range_state(min, max, RKNAND_TASK_UNINTERRUPTIBLE);
}

/*
 * ARM EH personality routines referenced by the blob's unwind tables. They are
 * never actually executed on a frame-pointer kernel; empty stubs satisfy the
 * link. Provide the whole pr0/pr1/pr2 set defensively.
 */
void __aeabi_unwind_cpp_pr0(void) { }
void __aeabi_unwind_cpp_pr1(void) { }
void __aeabi_unwind_cpp_pr2(void) { }
