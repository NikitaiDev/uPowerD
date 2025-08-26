// SPDX-License-Identifier: MIT
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <linux/module.h>
#include <linux/debugfs.h>
#define DPMD_BUF_MAX 16384

static struct dentry *dpmd_dir;
static struct dentry *status_file;

static char *status_buf;
static size_t status_len;
static DEFINE_MUTEX(status_lock);

static ssize_t status_read(struct file *f, char __user *ubuf, size_t len, loff_t *ppos)
{
	ssize_t ret;
	mutex_lock(&status_lock);
	ret = simple_read_from_buffer(ubuf, len, ppos, status_buf, status_len);
	mutex_unlock(&status_lock);
	return ret;
}

static ssize_t status_write(struct file *f, const char __user *ubuf, size_t len, loff_t *ppos)
{
	ssize_t ret = -EINVAL;
	if (len > DPMD_BUF_MAX)
		len = DPMD_BUF_MAX;
	mutex_lock(&status_lock);
	memset(status_buf, 0, DPMD_BUF_MAX);
	if (copy_from_user(status_buf, ubuf, len)) {
		ret = -EFAULT;
		goto out;
	}
	status_len = len;
	ret = (ssize_t)len;
out:
	mutex_unlock(&status_lock);
	return ret;
}

static const struct file_operations status_fops = {
	.owner = THIS_MODULE,
	.read  = status_read,
	.write = status_write,
	.llseek = default_llseek,
};

static int __init dpmd_init(void)
{
	status_buf = kzalloc(DPMD_BUF_MAX, GFP_KERNEL);
	if (!status_buf)
		return -ENOMEM;

	dpmd_dir = debugfs_create_dir("dpmd", NULL);
	if (IS_ERR_OR_NULL(dpmd_dir)) {
		kfree(status_buf);
		return -ENOMEM;
	}

	status_file = debugfs_create_file("status", 0644, dpmd_dir, NULL, &status_fops);
	if (IS_ERR_OR_NULL(status_file)) {
		debugfs_remove_recursive(dpmd_dir);
		kfree(status_buf);
		return -ENOMEM;
	}

	pr_info("dpmd_debugfs: ready (/sys/kernel/debug/dpmd/status)\n");
	return 0;
}

static void __exit dpmd_exit(void)
{
	debugfs_remove_recursive(dpmd_dir);
	kfree(status_buf);
	pr_info("dpmd_debugfs: unloaded\n");
}

module_init(dpmd_init);
module_exit(dpmd_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("dpmd");
MODULE_DESCRIPTION("DebugFS interface for Dynamic Power Manager daemon");