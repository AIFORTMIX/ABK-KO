// SPDX-License-Identifier: GPL-2.0
/*
 * ABK 外部 .ko 模块构建示例。
 *
 * 这个文件只依赖稳定的基础内核模块接口，适合作为验证不同 GKI/KMI
 * 版本是否能把同一份 .c 编译成对应 .ko 的最小样例。
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>

static int __init abk_hello_init(void)
{
	pr_info("abk_hello: module loaded\n");
	return 0;
}

static void __exit abk_hello_exit(void)
{
	pr_info("abk_hello: module unloaded\n");
}

module_init(abk_hello_init);
module_exit(abk_hello_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ABK");
MODULE_DESCRIPTION("ABK custom external kernel module build example");
