// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "qingwei: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/ioctl.h>
#include <linux/dcache.h>
#include <linux/version.h>
#include <linux/slab.h>
#include <linux/kmod.h>          // call_usermodehelper
#include <linux/list.h>
#include <asm/pgtable.h>

/* ---------- 外部符号（用于操纵模块链表） ---------- */
extern struct list_head modules;   // 内核模块链表头

#define DEVICE_NAME "qingwei"
#define MODULE_HIDE_NAME "vfat"    // 伪装模块名

/* ---------- 用户态接口定义 ---------- */
typedef struct {
    int pid;                      // 目标PID（≤0时用pkg_name自动查找）
    char pkg_name[64];            // 包名/进程名
    unsigned long addr;
    size_t size;
    unsigned long offsets[8];     // 指针链偏移（最多8级）
    int offset_count;
    unsigned long user_buf;       // 用户态缓冲区指针
} mem_packet_t;

#define MEM_IOCTL_MAGIC 'Q'
#define CMD_GET_BASE       _IOWR(MEM_IOCTL_MAGIC, 1, mem_packet_t)
#define CMD_READ_MEM       _IOWR(MEM_IOCTL_MAGIC, 2, mem_packet_t)
#define CMD_WRITE_MEM      _IOWR(MEM_IOCTL_MAGIC, 3, mem_packet_t)
#define CMD_READ_PTR       _IOWR(MEM_IOCTL_MAGIC, 4, mem_packet_t)
#define CMD_UNLOAD_MODULE  _IO(MEM_IOCTL_MAGIC, 5)   // 触发自卸载

/* ---------- 核心内存读写函数（手动遍历页表） ---------- */
static int manual_read_memory(struct task_struct *task, unsigned long vaddr,
                              void *kbuf, size_t len)
{
    struct mm_struct *mm = task->mm;
    size_t done = 0;
    int ret = 0;

    if (!mm) {
        pr_err("task has no mm\n");
        return -EINVAL;
    }

    mmap_read_lock(mm);

    while (done < len) {
        unsigned long addr = vaddr + done;
        unsigned long remaining = len - done;
        unsigned long page_offset = addr & ~PAGE_MASK;
        size_t copy_size = min_t(size_t, remaining, PAGE_SIZE - page_offset);
        pgd_t *pgd;
        p4d_t *p4d;
        pud_t *pud;
        pmd_t *pmd;
        pte_t *pte;
        struct page *page;
        void *kmap_addr;
        unsigned long pfn;

        pgd = pgd_offset(mm, addr);
        if (pgd_none(*pgd) || pgd_bad(*pgd)) {
            ret = -EFAULT;
            break;
        }

        p4d = p4d_offset(pgd, addr);
        if (p4d_none(*p4d) || p4d_bad(*p4d)) {
            ret = -EFAULT;
            break;
        }

        pud = pud_offset(p4d, addr);
        if (pud_none(*pud) || pud_bad(*pud)) {
            ret = -EFAULT;
            break;
        }

        /* 1GB 巨页 */
        if (pud_huge(*pud)) {
            pfn = pud_pfn(*pud);
            page = pfn_to_page(pfn);
            if (!page) {
                ret = -EFAULT;
                break;
            }
            kmap_addr = kmap_local_page(page);
            memcpy(kbuf + done, kmap_addr + page_offset, copy_size);
            kunmap_local(kmap_addr);
            done += copy_size;
            continue;
        }

        pmd = pmd_offset(pud, addr);
        if (pmd_none(*pmd) || pmd_bad(*pmd)) {
            ret = -EFAULT;
            break;
        }

        /* 2MB 巨页 (THP) */
        if (pmd_trans_huge(*pmd)) {
            pfn = pmd_pfn(*pmd);
            page = pfn_to_page(pfn);
            if (!page) {
                ret = -EFAULT;
                break;
            }
            kmap_addr = kmap_local_page(page);
            memcpy(kbuf + done, kmap_addr + page_offset, copy_size);
            kunmap_local(kmap_addr);
            done += copy_size;
            continue;
        }

        pte = pte_offset_map(pmd, addr);
        if (!pte) {
            ret = -EFAULT;
            break;
        }
        if (!pte_present(*pte)) {
            pte_unmap(pte);
            ret = -EFAULT;
            break;
        }

        pfn = pte_pfn(*pte);
        if (!pfn_valid(pfn)) {
            pte_unmap(pte);
            ret = -EFAULT;
            break;
        }
        page = pfn_to_page(pfn);
        if (!page) {
            pte_unmap(pte);
            ret = -EFAULT;
            break;
        }

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

    if (!mm) {
        pr_err("task has no mm\n");
        return -EINVAL;
    }

    mmap_read_lock(mm);

    while (done < len) {
        unsigned long addr = vaddr + done;
        unsigned long remaining = len - done;
        unsigned long page_offset = addr & ~PAGE_MASK;
        size_t copy_size = min_t(size_t, remaining, PAGE_SIZE - page_offset);
        pgd_t *pgd;
        p4d_t *p4d;
        pud_t *pud;
        pmd_t *pmd;
        pte_t *pte;
        struct page *page;
        void *kmap_addr;
        unsigned long pfn;

        pgd = pgd_offset(mm, addr);
        if (pgd_none(*pgd) || pgd_bad(*pgd)) {
            ret = -EFAULT;
            break;
        }
        p4d = p4d_offset(pgd, addr);
        if (p4d_none(*p4d) || p4d_bad(*p4d)) {
            ret = -EFAULT;
            break;
        }
        pud = pud_offset(p4d, addr);
        if (pud_none(*pud) || pud_bad(*pud)) {
            ret = -EFAULT;
            break;
        }
        if (pud_huge(*pud)) {
            pfn = pud_pfn(*pud);
            page = pfn_to_page(pfn);
            if (!page) { ret = -EFAULT; break; }
            kmap_addr = kmap_local_page(page);
            memcpy(kmap_addr + page_offset, kbuf + done, copy_size);
            kunmap_local(kmap_addr);
            done += copy_size;
            continue;
        }

        pmd = pmd_offset(pud, addr);
        if (pmd_none(*pmd) || pmd_bad(*pmd)) {
            ret = -EFAULT;
            break;
        }
        if (pmd_trans_huge(*pmd)) {
            pfn = pmd_pfn(*pmd);
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
            pr_debug("page not writable at 0x%lx\n", addr);
            ret = -EPERM;
            break;
        }
        pfn = pte_pfn(*pte);
        if (!pfn_valid(pfn)) {
            pte_unmap(pte);
            ret = -EFAULT;
            break;
        }
        page = pfn_to_page(pfn);
        if (!page) {
            pte_unmap(pte);
            ret = -EFAULT;
            break;
        }

        kmap_addr = kmap_local_page(page);
        memcpy(kmap_addr + page_offset, kbuf + done, copy_size);
        kunmap_local(kmap_addr);
        pte_unmap(pte);

        done += copy_size;
    }

    mmap_read_unlock(mm);
    return ret ? ret : done;
}

/* ---------- 辅助函数：按包名查找PID ---------- */
static int find_pid_by_name(const char *name)
{
    struct task_struct *task;
    int pid = -ESRCH;

    if (!name || !*name)
        return -EINVAL;

    rcu_read_lock();
    for_each_process(task) {
        char *cmd = task->comm;
        if (strlen(cmd) && !strcmp(cmd, name)) {
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

/* ---------- 获取模块基地址 ---------- */
static unsigned long get_module_base(struct task_struct *task, const char *mod_name)
{
    struct mm_struct *mm = task->mm;
    struct vm_area_struct *vma;
    unsigned long base = 0;
    char *pathbuf = kmalloc(PAGE_SIZE, GFP_KERNEL);
    if (!pathbuf)
        return 0;

    mmap_read_lock(mm);
    for (vma = mm->mmap; vma; vma = vma->vm_next) {
        if (!vma->vm_file)
            continue;
        char *path = d_path(&vma->vm_file->f_path, pathbuf, PAGE_SIZE);
        if (IS_ERR(path))
            continue;
        if (strstr(path, mod_name)) {
            base = vma->vm_start;
            break;
        }
    }
    mmap_read_unlock(mm);
    kfree(pathbuf);
    return base;
}

/* ---------- 自卸载触发函数 ---------- */
static int trigger_self_unload(void)
{
    // 1. 重新将本模块加入内核模块链表（使 rmmod 能发现）
    list_add(&THIS_MODULE->list, &modules);

    // 2. 异步执行用户态 rmmod 命令（伪装模块名为 vfat）
    char *envp[] = {
        "HOME=/",
        "PATH=/sbin:/system/sbin:/system/bin:/vendor/bin",
        NULL
    };
    char *argv[] = {
        "/system/bin/sh",
        "-c",
        "rmmod vfat",
        NULL
    };
    // 使用 UMH_NO_WAIT 使调用立即返回，避免 ioctl 阻塞在卸载过程中
    int ret = call_usermodehelper(argv[0], argv, envp, UMH_NO_WAIT);
    if (ret < 0)
        pr_err("Failed to trigger unload: %d\n", ret);
    return ret;
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
        // 直接触发自卸载，无需额外参数
        trigger_self_unload();
        return 0;
    }

    if (copy_from_user(&pkt, (void __user *)arg, sizeof(pkt))) {
        return -EFAULT;
    }

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
        if (!mod_name) {
            ret = -EINVAL;
            break;
        }
        if (copy_from_user(namebuf, (void __user *)mod_name, sizeof(namebuf)-1)) {
            ret = -EFAULT;
            break;
        }
        unsigned long base = get_module_base(task, namebuf);
        if (copy_to_user((void __user *)pkt.user_buf, &base, sizeof(unsigned long))) {
            ret = -EFAULT;
        }
        break;
    }

    case CMD_READ_MEM:
    case CMD_READ_PTR: {
        size_t total = pkt.size;
        if (total == 0 || total > 0x1000000) {
            ret = -EINVAL;
            break;
        }
        kbuf = kmalloc(total, GFP_KERNEL);
        if (!kbuf) {
            ret = -ENOMEM;
            break;
        }

        final_addr = pkt.addr;

        if (cmd == CMD_READ_PTR && pkt.offset_count > 0) {
            int i;
            for (i = 0; i < pkt.offset_count; i++) {
                unsigned long cur_addr = final_addr + (i == 0 ? 0 : pkt.offsets[i-1]);
                ret = manual_read_memory(task, cur_addr, &ptr_val, sizeof(uint64_t));
                if (ret < 0) break;
                if (i == pkt.offset_count - 1) {
                    final_addr = ptr_val + pkt.offsets[i];
                } else {
                    final_addr = ptr_val;
                }
            }
            if (ret < 0) break;
        } else {
            if (pkt.offset_count > 0 && pkt.offsets[0] != 0)
                final_addr += pkt.offsets[0];
        }

        ret = manual_read_memory(task, final_addr, kbuf, total);
        if (ret > 0 && copy_to_user((void __user *)pkt.user_buf, kbuf, total)) {
            ret = -EFAULT;
        } else if (ret > 0) {
            ret = 0;
        }
        break;
    }

    case CMD_WRITE_MEM: {
        size_t total = pkt.size;
        if (total == 0 || total > 0x1000000) {
            ret = -EINVAL;
            break;
        }
        kbuf = kmalloc(total, GFP_KERNEL);
        if (!kbuf) {
            ret = -ENOMEM;
            break;
        }
        if (copy_from_user(kbuf, (void __user *)pkt.user_buf, total)) {
            ret = -EFAULT;
            break;
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

/* ---------- 设备操作回调 ---------- */
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = device_ioctl,
};

static struct miscdevice misc_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = DEVICE_NAME,
    .fops = &fops,
};

/* ---------- 模块初始化与退出 ---------- */
static int __init mem_reader_init(void)
{
    int ret;

    ret = misc_register(&misc_dev);
    if (ret) {
        pr_err("failed to register misc device\n");
        return ret;
    }

    // 将模块名伪装为 vfat（影响 lsmod 显示，但后续会从链表中摘除）
    strcpy((char *)THIS_MODULE->name, MODULE_HIDE_NAME);

    // 从内核模块链表中摘除自身，使 lsmod /proc/modules 不可见
    list_del_init(&THIS_MODULE->list);

    pr_info("device /dev/%s created, module hidden (unload via ioctl CMD_UNLOAD_MODULE)\n",
            DEVICE_NAME);
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
MODULE_DESCRIPTION("Hidden manual page-table walk memory R/W for Android 6.1.138");
