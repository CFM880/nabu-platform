// SPDX-License-Identifier: GPL-2.0-only
/*
 * nabu: dump the ramoops buffer from the running kernel.
 *
 * pstore only shows what survived the previous boot, so an empty
 * /sys/fs/pstore cannot tell "the console never wrote anything" apart from
 * "the reset wiped the DRAM".  This debugfs node maps the physical ramoops
 * region and prints its header and printable strings, which answers both
 * questions from the running system.
 *
 * Diagnostics only: remove once the ramoops behaviour is settled.
 * Details: nabu-linux-overlay/docs/port-6.18.md
 */
#include <linux/debugfs.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/seq_file.h>

static unsigned long nabu_dump_addr = 0x370000000UL;
static unsigned long nabu_dump_size = 0x400000UL;
module_param_named(addr, nabu_dump_addr, ulong, 0644);
module_param_named(size, nabu_dump_size, ulong, 0644);

#define NABU_DUMP_RUN_MIN	8

static int nabu_ramoops_show(struct seq_file *s, void *unused)
{
	const u8 *p, *run = NULL;
	unsigned long i, nonzero = 0;

	p = memremap(nabu_dump_addr, nabu_dump_size, MEMREMAP_WC);
	if (!p)
		return -ENOMEM;

	for (i = 0; i < nabu_dump_size; i++)
		if (p[i])
			nonzero++;

	seq_printf(s, "addr=%#lx size=%#lx nonzero=%lu header=",
		   nabu_dump_addr, nabu_dump_size, nonzero);
	for (i = 0; i < 64 && i < nabu_dump_size; i++)
		seq_printf(s, "%02x", p[i]);
	seq_puts(s, "\n--- strings ---\n");

	for (i = 0; i < nabu_dump_size; i++) {
		bool printable = p[i] >= 0x20 && p[i] < 0x7f;

		if (printable) {
			if (!run)
				run = &p[i];
			continue;
		}

		if (run && &p[i] - run >= NABU_DUMP_RUN_MIN) {
			seq_write(s, run, &p[i] - run);
			seq_putc(s, '\n');
		}
		run = NULL;
	}

	if (run && nabu_dump_size - (unsigned long)(run - p) >= NABU_DUMP_RUN_MIN) {
		seq_write(s, run, nabu_dump_size - (unsigned long)(run - p));
		seq_putc(s, '\n');
	}

	memunmap((void __force *)p);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(nabu_ramoops);

static int __init nabu_ramoops_dump_init(void)
{
	debugfs_create_file("nabu-ramoops", 0400, NULL, NULL, &nabu_ramoops_fops);

	return 0;
}
late_initcall(nabu_ramoops_dump_init);
