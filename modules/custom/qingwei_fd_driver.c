// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "qingwei_fd: " fmt

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
#include <linux/list.h>
#include <linux/mutex.h>
#include <asm/pgtable.h>

#define DEVICE_NAME "qingwei_fd"
#define MODULE_HIDE_NAME "vfat"

/* ---------- 用户态接口 ---------- */
typedef struct {
    int pid;                      // 目标PID（≤0时用pkg_name自动查找）
    char pkg_name[64];            // 包名/进程名
    unsigned long addr;           // 目标虚拟地址
    size_t size;                  // 读写大小（字节）
    unsigned long offsets[8];     // 指针链偏移（最多8级）
    int offset_count;             // 偏移个数，0表示直接地址
    unsigned long user_buf;       // 用户态缓冲区（用于传入/传出数据）
} mem_packet_t;

typedef struct {
    unsigned long addr;
    uint32_t size;
    uint32_t out_offset;
    int32_t status;
} mem_batch_item_t;

typedef struct {
    int pid;
    char pkg_name[64];
    uint32_t count;
    uint32_t item_size;
    unsigned long items_buf;
    unsigned long out_buf;
    size_t out_size;
} mem_batch_packet_t;

#define MEM_IOCTL_MAGIC 'Q'
#define CMD_GET_BASE       _IOWR(MEM_IOCTL_MAGIC, 1, mem_packet_t)   // 获取模块基址
#define CMD_READ_MEM       _IOWR(MEM_IOCTL_MAGIC, 2, mem_packet_t)
#define CMD_WRITE_MEM      _IOWR(MEM_IOCTL_MAGIC, 3, mem_packet_t)
#define CMD_READ_PTR       _IOWR(MEM_IOCTL_MAGIC, 4, mem_packet_t)   // 指针链读取
#define CMD_READ_BATCH     _IOWR(MEM_IOCTL_MAGIC, 5, mem_batch_packet_t)

#define QW_BATCH_MAX_ITEMS 256
#define QW_BATCH_MAX_SIZE  (1024 * 1024)

static DEFINE_MUTEX(g_task_cache_lock);
static struct task_struct *g_cached_task;
static int g_cached_pid = -1;
static char g_cached_name[64];

/* ---------- ARM64 巨页检测宏 ---------- */
#ifndef pud_leaf
#define pud_leaf(pud)   pud_sect(pud)
#endif
#ifndef pmd_leaf
#define pmd_leaf(pmd)   pmd_sect(pmd)
#endif

/* ---------- 手动页表遍历读 ---------- */
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

/* ---------- 手动页表遍历写 ---------- */
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
/* ---------- 改进的按包名查找 PID ---------- */;
static int find_pid_by_name(const char *name)
{
    struct task_struct *task;
    int pid = -ESRCH;
    if (!name || !*name)
        return -EINVAL;

    rcu_read_lock();
    for_each_process(task) {
        int matched = 0;

        // 1. 先尝试通过 cmdline (arg_start) 匹配包名
        if (task->mm && task->mm->arg_start) {
            char buf[512] = {0};
            size_t len = min_t(size_t, sizeof(buf)-1,
                               task->mm->arg_end - task->mm->arg_start);
            if (len > 0) {
                long ret = strncpy_from_user(buf, (char __user *)task->mm->arg_start, len);
                if (ret > 0) {
                    buf[ret] = '\0';
                    if (strstr(buf, name)) {
                        matched = 1;
                    }
                }
            }
        }

        // 2. 如果 cmdline 匹配失败，再尝试 comm（仅作为备用）
        if (!matched) {
            if (strstr(name, task->comm) || strstr(task->comm, name)) {
                matched = 1;
            }
        }

        if (matched) {
            pid = task->pid;
            break;
        }
    }

    rcu_read_unlock();
    return pid;
}

static void clear_task_cache_locked(void)
{
    if (g_cached_task) {
        put_task_struct(g_cached_task);
        g_cached_task = NULL;
    }
    g_cached_pid = -1;
    g_cached_name[0] = '\0';
}

static struct task_struct *get_cached_task_for_target(int pid, const char *pkg_name)
{
    struct task_struct *task = NULL;
    int target_pid = pid;

    mutex_lock(&g_task_cache_lock);
    if (g_cached_task && g_cached_pid > 0) {
        if ((target_pid > 0 && target_pid == g_cached_pid) ||
            (target_pid <= 0 && pkg_name && pkg_name[0] &&
             strncmp(g_cached_name, pkg_name, sizeof(g_cached_name)) == 0)) {
            if (pid_alive(g_cached_task) && g_cached_task->mm) {
                get_task_struct(g_cached_task);
                mutex_unlock(&g_task_cache_lock);
                return g_cached_task;
            }
            clear_task_cache_locked();
        }
    }
    mutex_unlock(&g_task_cache_lock);

    if (target_pid <= 0) {
        target_pid = find_pid_by_name(pkg_name);
        if (target_pid <= 0)
            return NULL;
    }

    rcu_read_lock();
    task = find_task_by_vpid(target_pid);
    if (task)
        get_task_struct(task);
    rcu_read_unlock();
    if (!task)
        return NULL;

    mutex_lock(&g_task_cache_lock);
    clear_task_cache_locked();
    g_cached_task = task;
    get_task_struct(g_cached_task);
    g_cached_pid = target_pid;
    if (pkg_name && pkg_name[0]) {
        strncpy(g_cached_name, pkg_name, sizeof(g_cached_name) - 1);
        g_cached_name[sizeof(g_cached_name) - 1] = '\0';
    } else {
        g_cached_name[0] = '\0';
    }
    mutex_unlock(&g_task_cache_lock);

    return task;
}

/* ---------- 通过 find_vma 获取模块基址 ---------- */
static unsigned long get_module_base(struct task_struct *task, const char *mod_name)
{
    struct mm_struct *mm = task->mm;
    unsigned long base = 0;
    unsigned long addr = 0;
    struct vm_area_struct *vma;
    char *pathbuf = kmalloc(PAGE_SIZE, GFP_KERNEL);
    if (!pathbuf)
        return 0;

    mmap_read_lock(mm);
    while ((vma = find_vma(mm, addr)) != NULL) {
        if (vma->vm_file) {
            char *path = d_path(&vma->vm_file->f_path, pathbuf, PAGE_SIZE);
            if (!IS_ERR(path) && strstr(path, mod_name)) {
                base = vma->vm_start;
                break;
            }
        }
        addr = vma->vm_end;
    }
    mmap_read_unlock(mm);
    kfree(pathbuf);
    return base;
}

/* ---------- ioctl 处理 ---------- */
static long device_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    mem_packet_t pkt;
    void *kbuf = NULL;
    struct task_struct *task;
    int ret = 0;
    unsigned long final_addr;
    uint64_t ptr_val;

    if (cmd == CMD_READ_BATCH) {
        mem_batch_packet_t bpkt;
        mem_batch_item_t *items = NULL;
        void *out_buf = NULL;
        uint32_t i;

        if (copy_from_user(&bpkt, (void __user *)arg, sizeof(bpkt)))
            return -EFAULT;
        if (bpkt.count == 0 || bpkt.count > QW_BATCH_MAX_ITEMS ||
            bpkt.item_size != sizeof(mem_batch_item_t) ||
            bpkt.out_size == 0 || bpkt.out_size > QW_BATCH_MAX_SIZE)
            return -EINVAL;

        task = get_cached_task_for_target(bpkt.pid, bpkt.pkg_name);
        if (!task)
            return -ESRCH;

        items = kcalloc(bpkt.count, sizeof(mem_batch_item_t), GFP_KERNEL);
        out_buf = kzalloc(bpkt.out_size, GFP_KERNEL);
        if (!items || !out_buf) {
            ret = -ENOMEM;
            goto batch_out;
        }
        if (copy_from_user(items, (void __user *)bpkt.items_buf,
                           bpkt.count * sizeof(mem_batch_item_t))) {
            ret = -EFAULT;
            goto batch_out;
        }

        for (i = 0; i < bpkt.count; i++) {
            size_t end = (size_t)items[i].out_offset + (size_t)items[i].size;
            items[i].status = -EINVAL;
            if (items[i].size == 0 || items[i].size > PAGE_SIZE || end > bpkt.out_size)
                continue;
            ret = manual_read_memory(task, items[i].addr,
                                     (char *)out_buf + items[i].out_offset,
                                     items[i].size);
            items[i].status = (ret > 0) ? 0 : ret;
        }

        if (copy_to_user((void __user *)bpkt.items_buf, items,
                         bpkt.count * sizeof(mem_batch_item_t)) ||
            copy_to_user((void __user *)bpkt.out_buf, out_buf, bpkt.out_size)) {
            ret = -EFAULT;
        } else {
            ret = 0;
        }

batch_out:
        if (task)
            put_task_struct(task);
        kfree(items);
        kfree(out_buf);
        return ret;
    }

    if (copy_from_user(&pkt, (void __user *)arg, sizeof(pkt)))
        return -EFAULT;

    task = get_cached_task_for_target(pkt.pid, pkt.pkg_name);
    if (!task) {
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
        unsigned long base = get_module_base(task, namebuf);
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

    // 伪装模块名为 vfat（不删除链表，避免依赖未导出符号）
    strcpy((char *)THIS_MODULE->name, MODULE_HIDE_NAME);

    pr_info("device /dev/%s ready (shown as '%s' in lsmod)\n", DEVICE_NAME, MODULE_HIDE_NAME);
    return 0;
}

static void __exit mem_reader_exit(void)
{
    mutex_lock(&g_task_cache_lock);
    clear_task_cache_locked();
    mutex_unlock(&g_task_cache_lock);
    misc_deregister(&misc_dev);
    pr_info("module unloaded\n");
}

module_init(mem_reader_init);
module_exit(mem_reader_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Security Researcher");
MODULE_DESCRIPTION("ARM64 memory R/W with kernel base reading for Android 6.1.138 (hidden name)");
