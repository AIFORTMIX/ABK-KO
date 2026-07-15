// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "qingwei: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/ioctl.h>
#include <linux/version.h>
#include <linux/slab.h>
#include <linux/kmod.h>
#include <linux/list.h>
#include <linux/file.h>          // filp_open
#include <linux/fs.h>            // kernel_read
#include <linux/path.h>
#include <asm/pgtable.h>

extern struct list_head modules;

#define DEVICE_NAME "qingwei"
#define MODULE_HIDE_NAME "vfat"

/* ---------- 用户态接口 ---------- */
typedef struct {
    int pid;                      // 目标PID（≤0时用pkg_name自动查找）
    char pkg_name[64];            // 包名/进程名
    unsigned long addr;           // 目标虚拟地址
    size_t size;                  // 读写大小
    unsigned long offsets[8];     // 指针链偏移
    int offset_count;
    unsigned long user_buf;       // 用户态缓冲区（用于读写数据或传入模块名）
} mem_packet_t;

#define MEM_IOCTL_MAGIC 'Q'
#define CMD_GET_BASE       _IOWR(MEM_IOCTL_MAGIC, 1, mem_packet_t)   // 获取基址
#define CMD_READ_MEM       _IOWR(MEM_IOCTL_MAGIC, 2, mem_packet_t)
#define CMD_WRITE_MEM      _IOWR(MEM_IOCTL_MAGIC, 3, mem_packet_t)
#define CMD_READ_PTR       _IOWR(MEM_IOCTL_MAGIC, 4, mem_packet_t)
#define CMD_UNLOAD_MODULE  _IO(MEM_IOCTL_MAGIC, 5)

/* ---------- ARM64 巨页检测宏 ---------- */
#ifndef pud_leaf
#define pud_leaf(pud)   pud_sect(pud)
#endif
#ifndef pmd_leaf
#define pmd_leaf(pmd)   pmd_sect(pmd)
#endif

/* ---------- 手动页表遍历读/写（同上，无变动） ---------- */
static int manual_read_memory(struct task_struct *task, unsigned long vaddr,
                              void *kbuf, size_t len)
{
    struct mm_struct *mm = task->mm;
    size_t done = 0;
    int ret = 0;
    if (!mm) return -EINVAL;
    mmap_read_lock(mm);
    while (done < len) {
        unsigned long addr = vaddr + done;
        unsigned long remaining = len - done;
        unsigned long page_offset = addr & ~PAGE_MASK;
        size_t copy_size = min_t(size_t, remaining, PAGE_SIZE - page_offset);
        pgd_t *pgd; p4d_t *p4d; pud_t *pud; pmd_t *pmd; pte_t *pte;
        struct page *page; void *kmap_addr; unsigned long pfn;

        pgd = pgd_offset(mm, addr);
        if (pgd_none(*pgd) || pgd_bad(*pgd)) { ret = -EFAULT; break; }
        p4d = p4d_offset(pgd, addr);
        if (p4d_none(*p4d) || p4d_bad(*p4d)) { ret = -EFAULT; break; }
        pud = pud_offset(p4d, addr);
        if (pud_none(*pud) || pud_bad(*pud)) { ret = -EFAULT; break; }

        if (pud_leaf(*pud)) {
            pfn = pud_pfn(*pud);
            if (!pfn_valid(pfn)) { ret = -EFAULT; break; }
            page = pfn_to_page(pfn);
            if (!page) { ret = -EFAULT; break; }
            kmap_addr = kmap_local_page(page);
            memcpy(kbuf + done, kmap_addr + page_offset, copy_size);
            kunmap_local(kmap_addr);
            done += copy_size;
            continue;
        }

        pmd = pmd_offset(pud, addr);
        if (pmd_none(*pmd) || pmd_bad(*pmd)) { ret = -EFAULT; break; }
        if (pmd_leaf(*pmd)) {
            pfn = pmd_pfn(*pmd);
            if (!pfn_valid(pfn)) { ret = -EFAULT; break; }
            page = pfn_to_page(pfn);
            if (!page) { ret = -EFAULT; break; }
            kmap_addr = kmap_local_page(page);
            memcpy(kbuf + done, kmap_addr + page_offset, copy_size);
            kunmap_local(kmap_addr);
            done += copy_size;
            continue;
        }

        pte = pte_offset_map(pmd, addr);
        if (!pte) { ret = -EFAULT; break; }
        if (!pte_present(*pte)) { pte_unmap(pte); ret = -EFAULT; break; }
        pfn = pte_pfn(*pte);
        if (!pfn_valid(pfn)) { pte_unmap(pte); ret = -EFAULT; break; }
        page = pfn_to_page(pfn);
        if (!page) { pte_unmap(pte); ret = -EFAULT; break; }
        kmap_addr = kmap_local_page(page);
        memcpy(kbuf + done, kmap_addr + page_offset, copy_size);
        kunmap_local(kmap_addr);
        pte_unmap(pte);
        done += copy_size;
    }
    mmap_read_unlock(mm);
    return ret ? ret : done;
}

static int manual_write_memory(struct task_struct *task, unsigned long vaddr,
                               void *kbuf, size_t len)
{
    struct mm_struct *mm = task->mm;
    size_t done = 0;
    int ret = 0;
    if (!mm) return -EINVAL;
    mmap_read_lock(mm);
    while (done < len) {
        unsigned long addr = vaddr + done;
        unsigned long remaining = len - done;
        unsigned long page_offset = addr & ~PAGE_MASK;
        size_t copy_size = min_t(size_t, remaining, PAGE_SIZE - page_offset);
        pgd_t *pgd; p4d_t *p4d; pud_t *pud; pmd_t *pmd; pte_t *pte;
        struct page *page; void *kmap_addr; unsigned long pfn;

        pgd = pgd_offset(mm, addr);
        if (pgd_none(*pgd) || pgd_bad(*pgd)) { ret = -EFAULT; break; }
        p4d = p4d_offset(pgd, addr);
        if (p4d_none(*p4d) || p4d_bad(*p4d)) { ret = -EFAULT; break; }
        pud = pud_offset(p4d, addr);
        if (pud_none(*pud) || pud_bad(*pud)) { ret = -EFAULT; break; }

        if (pud_leaf(*pud)) {
            pfn = pud_pfn(*pud);
            if (!pfn_valid(pfn)) { ret = -EFAULT; break; }
            page = pfn_to_page(pfn);
            if (!page) { ret = -EFAULT; break; }
            kmap_addr = kmap_local_page(page);
            memcpy(kmap_addr + page_offset, kbuf + done, copy_size);
            kunmap_local(kmap_addr);
            done += copy_size;
            continue;
        }

        pmd = pmd_offset(pud, addr);
        if (pmd_none(*pmd) || pmd_bad(*pmd)) { ret = -EFAULT; break; }
        if (pmd_leaf(*pmd)) {
            pfn = pmd_pfn(*pmd);
            if (!pfn_valid(pfn)) { ret = -EFAULT; break; }
            page = pfn_to_page(pfn);
            if (!page) { ret = -EFAULT; break; }
            kmap_addr = kmap_local_page(page);
            memcpy(kmap_addr + page_offset, kbuf + done, copy_size);
            kunmap_local(kmap_addr);
            done += copy_size;
            continue;
        }

        pte = pte_offset_map(pmd, addr);
        if (!pte) { ret = -EFAULT; break; }
        if (!pte_present(*pte) || !pte_write(*pte)) {
            pte_unmap(pte);
            ret = -EPERM;
            break;
        }
        pfn = pte_pfn(*pte);
        if (!pfn_valid(pfn)) { pte_unmap(pte); ret = -EFAULT; break; }
        page = pfn_to_page(pfn);
        if (!page) { pte_unmap(pte); ret = -EFAULT; break; }
        kmap_addr = kmap_local_page(page);
        memcpy(kmap_addr + page_offset, kbuf + done, copy_size);
        kunmap_local(kmap_addr);
        pte_unmap(pte);
        done += copy_size;
    }
    mmap_read_unlock(mm);
    return ret ? ret : done;
}

/* ---------- 按包名查找 PID ---------- */
static int find_pid_by_name(const char *name)
{
    struct task_struct *task;
    int pid = -ESRCH;
    if (!name || !*name) return -EINVAL;
    rcu_read_lock();
    for_each_process(task) {
        if (!strcmp(task->comm, name)) {
            pid = task->pid;
            break;
        }
        if (task->mm && task->mm->arg_start) {
            char buf[128] = {0};
            size_t len = min_t(size_t, 127, task->mm->arg_end - task->mm->arg_start);
            if (len > 0) {
                if (strncpy_from_user(buf, (char __user *)task->mm->arg_start, len) > 0) {
                    if (strstr(buf, name)) {
                        pid = task->pid;
                        break;
                    }
                }
            }
        }
    }
    rcu_read_unlock();
    return pid;
}

/* ---------- 内核读取 /proc/pid/maps 获取模块基址 ---------- */
static unsigned long get_module_base_from_maps(int pid, const char *mod_name)
{
    char path[32];
    struct file *file;
    loff_t pos = 0;
    unsigned long base = 0;
    char *buf;
    int ret;

    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    file = filp_open(path, O_RDONLY, 0);
    if (IS_ERR(file)) {
        pr_err("failed to open %s\n", path);
        return 0;
    }

    buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
    if (!buf) {
        filp_close(file, NULL);
        return 0;
    }

    while ((ret = kernel_read(file, buf, PAGE_SIZE-1, &pos)) > 0) {
        char *line = buf;
        char *end = buf + ret;
        char *next;
        buf[ret] = '\0';
        while (line < end) {
            next = strchr(line, '\n');
            if (!next) break;
            *next = '\0';
            // 格式: "start-end perms offset dev inode pathname"
            if (strstr(line, mod_name)) {
                unsigned long start, end;
                if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
                    base = start;
                    goto out;
                }
            }
            line = next + 1;
        }
        pos = 0; // 重置pos? 实际上kernel_read已经更新pos，我们从头开始读？因为我们在循环中，但读取完一页后，pos已经指向下一页，应该继续，但因为我们解析了所有行，如果没找到，需要继续读下一页，但下一次循环会读取下一段，pos在kernel_read内部会更新，所以我们不需要重置，上面kernel_read会从上次pos继续。
        // 但上面的读取是每次读一页，如果文件超过一页，循环会继续。pos已经更新，没问题。
    }

out:
    kfree(buf);
    filp_close(file, NULL);
    return base;
}

/* ---------- 自卸载触发器 ---------- */
static int trigger_self_unload(void)
{
    list_add(&THIS_MODULE->list, &modules);
    char *envp[] = { "HOME=/", "PATH=/sbin:/system/sbin:/system/bin:/vendor/bin", NULL };
    char *argv[] = { "/system/bin/sh", "-c", "rmmod vfat", NULL };
    return call_usermodehelper(argv[0], argv, envp, UMH_NO_WAIT);
}

/* ---------- ioctl 处理 ---------- */
static long device_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    mem_packet_t pkt;
    void *kbuf = NULL;
    struct task_struct *task;
    int ret = 0;
    int pid;
    unsigned long final_addr;
    uint64_t ptr_val;

    if (cmd == CMD_UNLOAD_MODULE) {
        trigger_self_unload();
        return 0;
    }

    if (copy_from_user(&pkt, (void __user *)arg, sizeof(pkt)))
        return -EFAULT;

    /* 自动查找 PID */
    if (pkt.pid <= 0) {
        pid = find_pid_by_name(pkt.pkg_name);
        if (pid <= 0) {
            pr_err("process '%s' not found\n", pkt.pkg_name);
            return -ESRCH;
        }
        pkt.pid = pid;
    } else {
        pid = pkt.pid;
    }

    rcu_read_lock();
    task = find_task_by_vpid(pid);
    if (task)
        get_task_struct(task);
    rcu_read_unlock();
    if (!task) {
        pr_err("task %d not found\n", pid);
        return -ESRCH;
    }

    switch (cmd) {
    case CMD_GET_BASE: {
        char *mod_name = (char *)pkt.user_buf;
        char namebuf[256] = {0};
        if (!mod_name) { ret = -EINVAL; break; }
        if (copy_from_user(namebuf, (void __user *)mod_name, sizeof(namebuf)-1)) {
            ret = -EFAULT; break;
        }
        unsigned long base = get_module_base_from_maps(pid, namebuf);
        if (copy_to_user((void __user *)pkt.user_buf, &base, sizeof(unsigned long)))
            ret = -EFAULT;
        break;
    }

    case CMD_READ_MEM:
    case CMD_READ_PTR: {
        size_t total = pkt.size;
        if (total == 0 || total > 0x1000000) { ret = -EINVAL; break; }
        kbuf = kmalloc(total, GFP_KERNEL);
        if (!kbuf) { ret = -ENOMEM; break; }

        final_addr = pkt.addr;
        if (cmd == CMD_READ_PTR && pkt.offset_count > 0) {
            int i;
            for (i = 0; i < pkt.offset_count; i++) {
                unsigned long cur_addr = final_addr + (i == 0 ? 0 : pkt.offsets[i-1]);
                ret = manual_read_memory(task, cur_addr, &ptr_val, sizeof(uint64_t));
                if (ret < 0) break;
                if (i == pkt.offset_count - 1)
                    final_addr = ptr_val + pkt.offsets[i];
                else
                    final_addr = ptr_val;
            }
            if (ret < 0) break;
        } else {
            if (pkt.offset_count > 0 && pkt.offsets[0] != 0)
                final_addr += pkt.offsets[0];
        }

        ret = manual_read_memory(task, final_addr, kbuf, total);
        if (ret > 0 && copy_to_user((void __user *)pkt.user_buf, kbuf, total))
            ret = -EFAULT;
        else if (ret > 0)
            ret = 0;
        break;
    }

    case CMD_WRITE_MEM: {
        size_t total = pkt.size;
        if (total == 0 || total > 0x1000000) { ret = -EINVAL; break; }
        kbuf = kmalloc(total, GFP_KERNEL);
        if (!kbuf) { ret = -ENOMEM; break; }
        if (copy_from_user(kbuf, (void __user *)pkt.user_buf, total)) {
            ret = -EFAULT; break;
        }
        final_addr = pkt.addr;
        if (pkt.offset_count > 0 && pkt.offsets[0] != 0)
            final_addr += pkt.offsets[0];
        ret = manual_write_memory(task, final_addr, kbuf, total);
        if (ret > 0) ret = 0;
        break;
    }

    default:
        ret = -ENOTTY;
    }

    if (task)
        put_task_struct(task);
    kfree(kbuf);
    return ret;
}

/* ---------- 设备操作 ---------- */
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = device_ioctl,
};

static struct miscdevice misc_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = DEVICE_NAME,
    .fops = &fops,
};

/* ---------- 模块初始/退出 ---------- */
static int __init mem_reader_init(void)
{
    int ret = misc_register(&misc_dev);
    if (ret) {
        pr_err("misc_register failed\n");
        return ret;
    }
    strcpy((char *)THIS_MODULE->name, MODULE_HIDE_NAME);
    list_del_init(&THIS_MODULE->list);
    pr_info("device /dev/%s ready (hidden)\n", DEVICE_NAME);
    return 0;
}

static void __exit mem_reader_exit(void)
{
    misc_deregister(&misc_dev);
    pr_info("module unloaded\n");
}

module_init(mem_reader_init);
module_exit(mem_reader_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Security Researcher");
MODULE_DESCRIPTION("Hidden ARM64 memory R/W with kernel-read maps for base");
