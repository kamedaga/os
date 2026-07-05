// SPDX-License-Identifier: GPL-2.0

#include <linux/init.h>
#include <linux/module.h>

int tty_init(void);
void n_tty_init(void);
int kobox_tty_class_init(void);
int kobox_pty_init(void);
int kobox_devpts_init(void);

static int __init linux_tty_core_island_init(void)
{
	int status;

	pr_info("kobox linux_tty_core: init begin\n");
	pr_info("kobox linux_tty_core: kobox_tty_class_init begin\n");
	status = kobox_tty_class_init();
	if (status) {
		pr_info("kobox linux_tty_core: kobox_tty_class_init failed status=%d\n", status);
		return status;
	}
	pr_info("kobox linux_tty_core: kobox_tty_class_init ready\n");

	pr_info("kobox linux_tty_core: tty_init begin\n");
	status = tty_init();
	if (status) {
		pr_info("kobox linux_tty_core: tty_init failed status=%d\n", status);
		return status;
	}
	pr_info("kobox linux_tty_core: tty_init ready\n");

	pr_info("kobox linux_tty_core: n_tty_init begin\n");
	n_tty_init();
	pr_info("kobox linux_tty_core: n_tty_init ready\n");

	pr_info("kobox linux_tty_core: kobox_devpts_init begin\n");
	status = kobox_devpts_init();
	if (status) {
		pr_info("kobox linux_tty_core: kobox_devpts_init failed status=%d\n", status);
		return status;
	}
	pr_info("kobox linux_tty_core: kobox_devpts_init ready\n");

	pr_info("kobox linux_tty_core: kobox_pty_init begin\n");
	status = kobox_pty_init();
	if (status) {
		pr_info("kobox linux_tty_core: kobox_pty_init failed status=%d\n", status);
		return status;
	}
	pr_info("kobox linux_tty_core: kobox_pty_init ready\n");
	pr_info("kobox linux_tty_core: init ready\n");
	return 0;
}

static void __exit linux_tty_core_island_exit(void)
{
}

module_init(linux_tty_core_island_init);
module_exit(linux_tty_core_island_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("PachaOS");
MODULE_DESCRIPTION("Linux 6.8 TTY core island for kobox");
