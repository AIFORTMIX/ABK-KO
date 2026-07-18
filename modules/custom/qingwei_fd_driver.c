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
#include <linux/kallsyms.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>      // 确保 struct perf_event 完整
#include <asm/processor.h>
#include <asm/pgtable.h>

#define DEVICE_NAME "qingwei_fd"
#define MODULE_HIDE_NAME "vfat"

/* ---------- 用户态接口 ---------- */
typedef struct {
    int pid;
    char pkg_name[64];
    unsigned long addr;
    size_t size;
    unsigned long offsets[8];
    int offset_count;
    unsigned long user_buf;
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

typedef struct {
    int type;           // 1-exec, 2-read, 3-write, 4-rw
    unsigned long addr;
    int len;
} bp_set_t;

typedef struct {
    int idx;
} bp_clear_t;

typedef struct {
    int pid;
    unsigned long pc;
    unsigned long addr;
    int type;
    int idx;
} bp_hit_t;

#define MEM_IOCTL_MAGIC 'Q'
#define CMD_GET_BASE            _IOWR(MEM_IOCTL_MAGIC, 1, mem_packet_t)
#define CMD_READ_MEM            _IOWR(MEM_IOCTL_MAGIC, 2, mem_packet_t)
#define CMD_WRITE_MEM           _IOWR(MEM_IOCTL_MAGIC, 3, mem_packet_t)
#define CMD_READ_PTR            _IOWR(MEM_IOCTL_MAGIC, 4, mem_packet_t)
#define CMD_READ_BATCH          _IOWR(MEM_IOCTL_MAGIC, 5, mem_batch_packet_t)
#define CMD_SET_BREAKPOINT      _IOWR(MEM_IOCTL_MAGIC, 6, bp_set_t)
#define CMD_CLEAR_BREAKPOINT    _IOW(MEM_IOCTL_MAGIC, 7, int)
#define CMD_GET_BREAKPOINT_HIT  _IOR(MEM_IOCTL_MAGIC, 8, bp_hit_t)

#define QW_BATCH_MAX_ITEMS 256
#define QW_BATCH_MAX_SIZE  (1024 * 1024)
#define QW_MAX_BREAKPOINTS 8

/* ---------- 全局变量 ---------- */
static DEFINE_MUTEX(g_task_cache_lock);
static struct task_struct *g_cached_task;
static int g_cached_pid = -1;
static char g_cached_name[64];

/* ---------- 硬件断点动态函数指针（修正类型） ---------- */
static struct perf_event *(*hw_bp_register)(struct perf_event_attr *attr,
                             void (*triggered)(struct perf_event *,
                                               struct perf_sample_data *,
                                               struct pt_regs *),
                             void *context,
                             struct task_struct *tsk);
static void (*hw_bp_unregister)(struct perf_event *bp);
static bool hw_bp_available = false;

struct qw_breakpoint {
    int id;
    int type;
    unsigned long addr;
    int len;
    bool active;
    struct perf_event *event;
};

static struct qw_breakpoint g_bps[QW_MAX_BREAKPOINTS];
static DEFINE_SPINLOCK(g_bp_lock);
static struct {
    int pid;
    unsigned long pc;
    unsigned long addr;
    int type;
    struct task_struct *task;
} g_hit_info;
static bool g_hit_pending;

/* ---------- ARM64 巨页检测 ---------- */
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

/* ---------- 进程查找：优先 comm ---------- */
static int find_pid_by_name(const char *name)
{
    struct task_struct *task;
    int pid = -ESRCH;
    if (!name || !*name)
        return -EINVAL;

    rcu_read_lock();
    for_each_process(task) {
        if (strstr(name, task->comm) || strstr(task->comm, name)) {
            pid = task->pid;
            break;
        }
        if (task->mm && task->mm->arg_start) {
            char buf[512] = {0};
            size_t len = min_t(size_t, sizeof(buf)-1,
                               task->mm->arg_end - task->mm->arg_start);
            if (len > 0) {
                long ret = strncpy_from_user(buf, (char __user *)task->mm->arg_start, len);
                if (ret > 0) {
                    buf[ret] = '\0';
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

/* ---------- 任务缓存 ---------- */
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

/* ---------- 获取模块基址 ---------- */
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

/* ---------- 硬件断点回调 ---------- */
static void qw_breakpoint_handler(struct perf_event *bp, struct perf_sample_data *data,
                                  struct pt_regs *regs)
{
    struct qw_breakpoint *b = bp->overflow_handler_context;
    unsigned long flags;

    spin_lock_irqsave(&g_bp_lock, flags);
    g_hit_info.pid = current->pid;
    g_hit_info.pc = instruction_pointer(regs);
    g_hit_info.addr = b->addr;
    g_hit_info.type = b->type;
    g_hit_info.task = current;
    get_task_struct(current);
    g_hit_pending = true;
    spin_unlock_irqrestore(&g_bp_lock, flags);

    pr_info("HW Breakpoint hit! PID=%d, PC=0x%lx, type=%d, addr=0x%lx\n",
            current->pid, g_hit_info.pc, b->type, b->addr);
    dump_stack();
}

/* ---------- 硬件断点操作（动态调用） ---------- */
static int qw_set_breakpoint(unsigned long addr, int type, int len, int *idx)
{
    struct perf_event_attr attr;
    struct perf_event *bp;
    struct qw_breakpoint *b = NULL;
    int i, ret = 0;

    if (!hw_bp_available)
        return -ENOSYS;

    spin_lock(&g_bp_lock);
    for (i = 0; i < QW_MAX_BREAKPOINTS; i++) {
        if (!g_bps[i].active) {
            b = &g_bps[i];
            break;
        }
    }
    if (!b) {
        spin_unlock(&g_bp_lock);
        return -ENOSPC;
    }
    spin_unlock(&g_bp_lock);

    memset(&attr, 0, sizeof(attr));
    attr.type = PERF_TYPE_BREAKPOINT;
    attr.size = sizeof(attr);
    attr.bp_addr = addr;
    attr.bp_len = len;

    switch (type) {
    case 1: attr.bp_type = HW_BREAKPOINT_X; break;
    case 2: attr.bp_type = HW_BREAKPOINT_R; break;
    case 3: attr.bp_type = HW_BREAKPOINT_W; break;
    case 4: attr.bp_type = HW_BREAKPOINT_R | HW_BREAKPOINT_W; break;
    default: return -EINVAL;
    }

    bp = hw_bp_register(&attr, qw_breakpoint_handler, b, NULL);
    if (IS_ERR(bp)) {
        ret = PTR_ERR(bp);
        pr_err("register_user_hw_breakpoint failed: %d\n", ret);
        return ret;
    }

    b->id = i;
    b->type = type;
    b->addr = addr;
    b->len = len;
    b->active = true;
    b->event = bp;

    *idx = i;
    return 0;
}

static void qw_clear_breakpoint(int idx)
{
    if (!hw_bp_available)
        return;
    if (idx < 0 || idx >= QW_MAX_BREAKPOINTS)
        return;
    spin_lock(&g_bp_lock);
    if (g_bps[idx].active) {
        hw_bp_unregister(g_bps[idx].event);
        g_bps[idx].active = false;
        g_bps[idx].event = NULL;
    }
    spin_unlock(&g_bp_lock);
}

static int qw_get_hit_info(bp_hit_t *hit)
{
    unsigned long flags;
    if (!g_hit_pending)
        return -ENODATA;
    spin_lock_irqsave(&g_bp_lock, flags);
    hit->pid = g_hit_info.pid;
    hit->pc = g_hit_info.pc;
    hit->addr = g_hit_info.addr;
    hit->type = g_hit_info.type;
    hit->idx = -1;
    for (int i = 0; i < QW_MAX_BREAKPOINTS; i++) {
        if (g_bps[i].active && g_bps[i].addr == g_hit_info.addr) {
            hit->idx = i;
            break;
        }
    }
    if (g_hit_info.task)
        put_task_struct(g_hit_info.task);
    g_hit_pending = false;
    spin_unlock_irqrestore(&g_bp_lock, flags);
    return 0;
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

    if (cmd == CMD_SET_BREAKPOINT) {
        bp_set_t bp;
        if (copy_from_user(&bp, (void __user *)arg, sizeof(bp)))
            return -EFAULT;
        int idx;
        ret = qw_set_breakpoint(bp.addr, bp.type, bp.len, &idx);
        if (ret == 0) {
            if (copy_to_user((void __user *)arg, &idx, sizeof(int)))
                ret = -EFAULT;
        }
        return ret;
    }
    if (cmd == CMD_CLEAR_BREAKPOINT) {
        int idx;
        if (copy_from_user(&idx, (void __user *)arg, sizeof(int)))
            return -EFAULT;
        qw_clear_breakpoint(idx);
        return 0;
    }
    if (cmd == CMD_GET_BREAKPOINT_HIT) {
        bp_hit_t hit;
        ret = qw_get_hit_info(&hit);
        if (ret == 0) {
            if (copy_to_user((void __user *)arg, &hit, sizeof(hit)))
                ret = -EFAULT;
        }
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

    // 动态解析硬件断点函数
    hw_bp_register = (void *)kallsyms_lookup_name("register_user_hw_breakpoint");
    hw_bp_unregister = (void *)kallsyms_lookup_name("unregister_hw_breakpoint");
    if (hw_bp_register && hw_bp_unregister) {
        hw_bp_available = true;
        pr_info("Hardware breakpoint functions loaded successfully\n");
    } else {
        hw_bp_available = false;
        pr_info("Hardware breakpoint functions not available, breakpoint feature disabled\n");
    }

    strcpy((char *)THIS_MODULE->name, MODULE_HIDE_NAME);
    pr_info("device /dev/%s ready (shown as '%s' in lsmod)\n", DEVICE_NAME, MODULE_HIDE_NAME);
    return 0;
}

static void __exit mem_reader_exit(void)
{
    mutex_lock(&g_task_cache_lock);
    clear_task_cache_locked();
    mutex_unlock(&g_task_cache_lock);

    // 清除所有断点（如果可用）
    if (hw_bp_available) {
        for (int i = 0; i < QW_MAX_BREAKPOINTS; i++) {
            if (g_bps[i].active)
                qw_clear_breakpoint(i);
        }
    }
    misc_deregister(&misc_dev);
    pr_info("module unloaded\n");
}

module_init(mem_reader_init);
module_exit(mem_reader_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Security Researcher");
MODULE_DESCRIPTION("ARM64 memory R/W with optional HW breakpoint (dynamically resolved)");
