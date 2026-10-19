// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include <linux/slab.h>
#include <linux/string.h>

static int cmdline_proc_show(struct seq_file *m, void *v)
{
	char *cmdline, *token, *pos;
	bool first = true;

	cmdline = kstrdup(saved_command_line, GFP_KERNEL);
	if (!cmdline) {
		seq_puts(m, saved_command_line);
		seq_putc(m, '\n');
		return 0;
	}

	pos = cmdline;
	while ((token = strsep(&pos, " \t\n")) != NULL) {
		if (!*token)
			continue;

		if (!first)
			seq_putc(m, ' ');
		first = false;

		if (strncmp(token, "androidboot.verifiedbootstate=", 30) == 0) {
			seq_puts(m, "androidboot.verifiedbootstate=green");
		} else if (strncmp(token, "androidboot.flash.locked=", 25) == 0) {
			seq_puts(m, "androidboot.flash.locked=1");
		} else if (strncmp(token, "androidboot.vbmeta.device_state=", 32) == 0) {
			seq_puts(m, "androidboot.vbmeta.device_state=locked");
		} else {
			seq_puts(m, token);
		}
	}

	kfree(cmdline);
	seq_putc(m, '\n');
	return 0;
}

static int __init proc_cmdline_init(void)
{
	proc_create_single("cmdline", 0, NULL, cmdline_proc_show);
	return 0;
}
fs_initcall(proc_cmdline_init);
