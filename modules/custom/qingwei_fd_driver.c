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
#include <linux/vmalloc.h>
#include <linux/ktime.h>
#include <linux/kprobes.h>
#include <asm/pgtable.h>

#ifdef CONFIG_HAVE_HW_BREAKPOINT
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>

/* 动态解析未导出的 HWBP 符号 */
typedef struct perf_event *(*register_user_hw_bp_fn)(struct perf_event_attr *attr,
    perf_overflow_handler_t triggered, void *context, struct task_struct *tsk);
typedef void (*unregister_hw_bp_fn)(struct perf_event *bp);

static register_user_hw_bp_fn g_register_user_hw_bp = NULL;
static unregister_hw_bp_fn g_unregister_hw_bp = NULL;

/* 通过 kprobe 解析未导出符号（Android 6.1 唯一可靠方式） */
static unsigned long resolve_symbol(const char *name)
{
    struct kprobe kp;
    unsigned long addr;

    memset(&kp, 0, sizeof(kp));
    kp.symbol_name = name;

    if (register_kprobe(&kp) < 0)
        return 0;

    addr = (unsigned long)kp.addr;
    unregister_kprobe(&kp);
    return addr;
}
#endif

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
    unsigned long long cpu_time_ns;   // 模块累计 CPU 时间（纳秒）
    unsigned long long call_count;    // 总 ioctl 调用次数
    unsigned long mem_bytes;          // 模块常驻内存大小（字节）
} module_info_t;

typedef struct {
    unsigned long long hit_count;    // 断点触发次数
    unsigned long long bp_addr;      // 断点地址
    int snapshot_count;              // 当前快照缓存条目数
    int active;                      // 1=激活, 0=未激活
} hwbp_stats_t;

#define MEM_IOCTL_MAGIC 'Q'
#define CMD_GET_BASE            _IOWR(MEM_IOCTL_MAGIC, 1, mem_packet_t)
#define CMD_READ_MEM            _IOWR(MEM_IOCTL_MAGIC, 2, mem_packet_t)
#define CMD_WRITE_MEM           _IOWR(MEM_IOCTL_MAGIC, 3, mem_packet_t)
#define CMD_READ_PTR            _IOWR(MEM_IOCTL_MAGIC, 4, mem_packet_t)
#define CMD_READ_BATCH          _IOWR(MEM_IOCTL_MAGIC, 5, mem_batch_packet_t)
#define CMD_GET_MODULE_INFO     _IOR(MEM_IOCTL_MAGIC, 6, module_info_t)
#define CMD_SET_HW_BP           _IOW(MEM_IOCTL_MAGIC, 7, mem_packet_t)
#define CMD_SET_BLR_ADDRS       _IOW(MEM_IOCTL_MAGIC, 8, mem_packet_t)
#define CMD_QUERY_SNAPSHOT      _IOWR(MEM_IOCTL_MAGIC, 9, mem_packet_t)
#define CMD_GET_HWBP_STATS      _IOR(MEM_IOCTL_MAGIC, 10, hwbp_stats_t)

#define QW_BATCH_MAX_ITEMS 512
#define QW_BATCH_MAX_SIZE  (2 * 1024 * 1024)

/* ---------- 全局变量 ---------- */
static DEFINE_MUTEX(g_task_cache_lock);
static struct task_struct *g_cached_task;
static int g_cached_pid = -1;
static char g_cached_name[64];

static atomic64_t g_total_cpu_ns = ATOMIC64_INIT(0);
static atomic64_t g_call_count = ATOMIC64_INIT(0);

/* ---------- HW Breakpoint 状态 ---------- */
#ifdef CONFIG_HAVE_HW_BREAKPOINT
static struct perf_event *g_hw_bp_event = NULL;
static struct task_struct *g_hw_bp_target_task = NULL;
static unsigned long g_blr_x8_addr = 0;
static unsigned long g_blr_x9_addr = 0;
static bool g_hw_bp_active = false;
static atomic64_t g_hw_bp_hit_count = ATOMIC64_INIT(0);

/* ---------- 坐标快照缓存（环形缓冲区，spinlock 保护） ---------- */
#define SNAPSHOT_CACHE_SIZE 256
struct snapshot_entry {
    unsigned long obj_addr;
    u32 x_raw, y_raw, z_raw;   /* 原始 reg 值（用户态转 float） */
    unsigned long jiffies;
};
static struct snapshot_entry g_snapshots[SNAPSHOT_CACHE_SIZE];
static int g_snapshot_head = 0;
static int g_snapshot_count = 0;
static DEFINE_SPINLOCK(g_snapshot_lock);
#endif

/* ---------- ARM64 巨页检测宏 ---------- */
#ifndef pud_leaf
#define pud_leaf(pud)   pud_sect(pud)
#endif
#ifndef pmd_leaf
#define pmd_leaf(pmd)   pmd_sect(pmd)
#endif

/* ---------- 底层读写（已持锁版本） ---------- */
static int __manual_read_memory_locked(struct mm_struct *mm, unsigned long vaddr,
                                       void *kbuf, size_t len)
{
    size_t done = 0;
    int ret = 0;
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
    return ret ? ret : done;
}

/* ---------- 外层读（自动加锁） ---------- */
static int manual_read_memory(struct task_struct *task, unsigned long vaddr,
                              void *kbuf, size_t len)
{
    struct mm_struct *mm = task->mm;
    int ret;
    if (!mm) return -EINVAL;
    mmap_read_lock(mm);
    ret = __manual_read_memory_locked(mm, vaddr, kbuf, len);
    mmap_read_unlock(mm);
    return ret;
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

#ifdef CONFIG_HAVE_HW_BREAKPOINT
/* ===================================================================
 * 硬件断点 (HW Breakpoint) 功能
 *
 * 原理：
 *   在目标进程中设置 ARM64 执行断点（DBGBVR/DBGBCR），
 *   当断点触发时 overflow handler 读取 pt_regs 中的寄存器值，
 *   将解密后的坐标缓存到内核环形缓冲区，供用户态通过
 *   CMD_QUERY_SNAPSHOT 查询。
 *
 * 寄存器映射（基于 sub_9D68BA0 逆向分析）：
 *   X19 = Pawn 对象地址（缓存 key）
 *   X8  = X 坐标（float，低 32 位）
 *   X9  = Y 坐标（float，低 32 位）
 *   X10 = Z 坐标（float，低 32 位）
 *
 * 注意：overflow handler 运行在中断上下文，只能使用 spinlock
 *       和原子操作，不能睡眠、不能调用 copy_to/from_user。
 * =================================================================== */

static void hw_bp_overflow_handler(struct perf_event *event,
                                    struct perf_sample_data *data,
                                    struct pt_regs *regs)
{
    unsigned long obj_addr;
    u32 raw_x, raw_y, raw_z;

    if (!regs)
        return;

    /* 读取寄存器：X19=Pawn, X8/X9/X10=坐标（低32位） */
    obj_addr = regs->regs[19];
    raw_x = (u32)regs->regs[8];
    raw_y = (u32)regs->regs[9];
    raw_z = (u32)regs->regs[10];

    /* 跳过无效地址 */
    if (obj_addr < 0x100000)
        return;

    /* 跳过全零坐标（内核中禁止 float 运算，用 u32 比较） */
    if (raw_x == 0 && raw_y == 0 && raw_z == 0)
        return;

    spin_lock(&g_snapshot_lock);
    g_snapshots[g_snapshot_head].obj_addr = obj_addr;
    g_snapshots[g_snapshot_head].x_raw = raw_x;
    g_snapshots[g_snapshot_head].y_raw = raw_y;
    g_snapshots[g_snapshot_head].z_raw = raw_z;
    g_snapshots[g_snapshot_head].jiffies = jiffies;
    g_snapshot_head = (g_snapshot_head + 1) % SNAPSHOT_CACHE_SIZE;
    if (g_snapshot_count < SNAPSHOT_CACHE_SIZE)
        g_snapshot_count++;
    spin_unlock(&g_snapshot_lock);

    atomic64_inc(&g_hw_bp_hit_count);
}

static void hw_bp_cleanup(void)
{
    if (g_hw_bp_event) {
        if (g_unregister_hw_bp)
            g_unregister_hw_bp(g_hw_bp_event);
        g_hw_bp_event = NULL;
    }
    if (g_hw_bp_target_task) {
        put_task_struct(g_hw_bp_target_task);
        g_hw_bp_target_task = NULL;
    }
    g_hw_bp_active = false;
    g_blr_x8_addr = 0;
    g_blr_x9_addr = 0;

    spin_lock(&g_snapshot_lock);
    g_snapshot_head = 0;
    g_snapshot_count = 0;
    memset(g_snapshots, 0, sizeof(g_snapshots));
    spin_unlock(&g_snapshot_lock);
}

static int hw_bp_setup(struct task_struct *task, unsigned long bp_addr,
                        unsigned long blr_x8, unsigned long blr_x9)
{
    struct perf_event_attr attr;
    int err;

    if (!g_register_user_hw_bp) {
        pr_err("HWBP: register_user_hw_breakpoint not found via kallsyms\n");
        return -ENOSYS;
    }

    /* 先清理旧断点 */
    hw_bp_cleanup();

    /* 初始化并设置执行断点 */
    hw_breakpoint_init(&attr);
    attr.bp_addr = bp_addr;
    attr.bp_len = HW_BREAKPOINT_LEN_4;   /* A64 指令 = 4 字节 */
    attr.bp_type = HW_BREAKPOINT_X;       /* 执行断点 */

    g_hw_bp_event = g_register_user_hw_bp(&attr,
                         hw_bp_overflow_handler, NULL, task);
    if (IS_ERR(g_hw_bp_event)) {
        err = PTR_ERR(g_hw_bp_event);
        g_hw_bp_event = NULL;
        pr_err("register_user_hw_breakpoint failed: %d\n", err);
        return err;
    }

    g_hw_bp_target_task = task;
    get_task_struct(task);
    g_blr_x8_addr = blr_x8;
    g_blr_x9_addr = blr_x9;
    g_hw_bp_active = true;

    pr_info("HWBP set: bp=0x%lx blr_x8=0x%lx blr_x9=0x%lx pid=%d\n",
            bp_addr, blr_x8, blr_x9, task->pid);
    return 0;
}

static int hw_bp_update_blr(struct task_struct *task,
                              unsigned long blr_x8, unsigned long blr_x9)
{
    if (!g_hw_bp_active || !g_hw_bp_target_task) {
        pr_warn("HWBP update_blr: no active breakpoint\n");
        return -ENODEV;
    }

    /* 验证目标进程是否匹配 */
    if (g_hw_bp_target_task != task) {
        pr_warn("HWBP update_blr: task mismatch (cached=%d new=%d)\n",
                g_hw_bp_target_task->pid, task->pid);
        return -EINVAL;
    }

    g_blr_x8_addr = blr_x8;
    g_blr_x9_addr = blr_x9;
    pr_info("HWBP BLR updated: blr_x8=0x%lx blr_x9=0x%lx\n",
            blr_x8, blr_x9);
    return 0;
}

static int hw_bp_query_snapshot(unsigned long obj_addr,
                                 u32 *x_raw, u32 *y_raw, u32 *z_raw)
{
    int i;
    int found = -2; /* 默认：暂无快照 */

    spin_lock(&g_snapshot_lock);
    /* 从最新条目开始反向搜索 */
    for (i = 0; i < g_snapshot_count; i++) {
        int idx = (g_snapshot_head - 1 - i + SNAPSHOT_CACHE_SIZE) % SNAPSHOT_CACHE_SIZE;
        if (g_snapshots[idx].obj_addr == obj_addr) {
            *x_raw = g_snapshots[idx].x_raw;
            *y_raw = g_snapshots[idx].y_raw;
            *z_raw = g_snapshots[idx].z_raw;
            found = 0; /* 命中 */
            break;
        }
    }
    spin_unlock(&g_snapshot_lock);

    return found;
}
#else
/* CONFIG_HAVE_HW_BREAKPOINT 未启用时的桩函数 */
static void hw_bp_cleanup(void) {}
static int hw_bp_setup(struct task_struct *task, unsigned long bp_addr,
                        unsigned long blr_x8, unsigned long blr_x9) { return -ENOSYS; }
static int hw_bp_update_blr(struct task_struct *task,
                              unsigned long blr_x8, unsigned long blr_x9) { return -ENOSYS; }
static int hw_bp_query_snapshot(unsigned long obj_addr,
                                 u32 *x_raw, u32 *y_raw, u32 *z_raw) { return -ENOSYS; }
#endif /* CONFIG_HAVE_HW_BREAKPOINT */

/* ---------- ioctl 处理 ---------- */
static long device_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    u64 start = ktime_get_ns();
    long ret = 0;

    if (cmd == CMD_GET_MODULE_INFO) {
        module_info_t info;
        info.cpu_time_ns = atomic64_read(&g_total_cpu_ns);
        info.call_count = atomic64_read(&g_call_count);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
        info.mem_bytes = THIS_MODULE->core.size;
#else
        info.mem_bytes = THIS_MODULE->core_layout.size;
#endif
        if (copy_to_user((void __user *)arg, &info, sizeof(info)))
            ret = -EFAULT;
        else
            ret = 0;
        goto out;
    }

    if (cmd == CMD_GET_HWBP_STATS) {
        hwbp_stats_t stats;
        memset(&stats, 0, sizeof(stats));
#ifdef CONFIG_HAVE_HW_BREAKPOINT
        stats.hit_count = atomic64_read(&g_hw_bp_hit_count);
        if (g_hw_bp_event) {
            stats.bp_addr = g_hw_bp_event->attr.bp_addr;
        }
        stats.snapshot_count = g_snapshot_count;
        stats.active = g_hw_bp_active ? 1 : 0;
#else
        stats.active = 0;
#endif
        if (copy_to_user((void __user *)arg, &stats, sizeof(stats)))
            ret = -EFAULT;
        else
            ret = 0;
        goto out;
    }

    // 其他命令需要处理...
    mem_packet_t pkt;
    void *kbuf = NULL;
    struct task_struct *task;
    unsigned long final_addr;
    uint64_t ptr_val;

    if (cmd == CMD_READ_BATCH) {
        mem_batch_packet_t bpkt;
        mem_batch_item_t *items = NULL;
        void *out_buf = NULL;
        uint32_t i;

        if (copy_from_user(&bpkt, (void __user *)arg, sizeof(bpkt))) {
            ret = -EFAULT;
            goto out;
        }
        if (bpkt.count == 0 || bpkt.count > QW_BATCH_MAX_ITEMS ||
            bpkt.item_size != sizeof(mem_batch_item_t) ||
            bpkt.out_size == 0 || bpkt.out_size > QW_BATCH_MAX_SIZE) {
            ret = -EINVAL;
            goto out;
        }

        task = get_cached_task_for_target(bpkt.pid, bpkt.pkg_name);
        if (!task) {
            ret = -ESRCH;
            goto out;
        }

        items = kcalloc(bpkt.count, sizeof(mem_batch_item_t), GFP_KERNEL);
        out_buf = vmalloc(bpkt.out_size);
        if (!items || !out_buf) {
            ret = -ENOMEM;
            goto batch_out;
        }
        if (copy_from_user(items, (void __user *)bpkt.items_buf,
                           bpkt.count * sizeof(mem_batch_item_t))) {
            ret = -EFAULT;
            goto batch_out;
        }

        if (task->mm) {
            mmap_read_lock(task->mm);
            for (i = 0; i < bpkt.count; i++) {
                size_t end = (size_t)items[i].out_offset + (size_t)items[i].size;
                items[i].status = -EINVAL;
                if (items[i].size == 0 || items[i].size > PAGE_SIZE || end > bpkt.out_size)
                    continue;
                int r = __manual_read_memory_locked(task->mm, items[i].addr,
                                                    (char *)out_buf + items[i].out_offset,
                                                    items[i].size);
                items[i].status = (r > 0) ? 0 : r;
            }
            mmap_read_unlock(task->mm);
        } else {
            ret = -EINVAL;
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
        vfree(out_buf);
        goto out;
    }

    if (copy_from_user(&pkt, (void __user *)arg, sizeof(pkt))) {
        ret = -EFAULT;
        goto out;
    }

    task = get_cached_task_for_target(pkt.pid, pkt.pkg_name);
    if (!task) {
        ret = -ESRCH;
        goto out;
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

    /* ====== HWBP 命令 ====== */
    case CMD_SET_HW_BP: {
        unsigned long bp_addr = pkt.addr;
        unsigned long blr_addrs[2] = {0, 0};

        if (pkt.user_buf && copy_from_user(blr_addrs,
                (void __user *)pkt.user_buf, sizeof(blr_addrs))) {
            ret = -EFAULT;
            break;
        }
        ret = hw_bp_setup(task, bp_addr, blr_addrs[0], blr_addrs[1]);
        break;
    }

    case CMD_SET_BLR_ADDRS: {
        unsigned long blr_addrs[2] = {0, 0};

        if (pkt.user_buf && copy_from_user(blr_addrs,
                (void __user *)pkt.user_buf, sizeof(blr_addrs))) {
            ret = -EFAULT;
            break;
        }
        ret = hw_bp_update_blr(task, blr_addrs[0], blr_addrs[1]);
        break;
    }

    case CMD_QUERY_SNAPSHOT: {
        unsigned long obj_addr = pkt.addr;
        u32 result[3] = {0, 0, 0};
        int status;

        status = hw_bp_query_snapshot(obj_addr, &result[0], &result[1], &result[2]);

        if (pkt.user_buf && copy_to_user((void __user *)pkt.user_buf,
                result, sizeof(result))) {
            ret = -14; /* 用户缓冲区写入失败 */
        } else {
            ret = status;
        }
        break;
    }

    default:
        ret = -ENOTTY;
    }

    if (task)
        put_task_struct(task);
    kfree(kbuf);

out:
    {
        u64 delta = ktime_get_ns() - start;
        atomic64_add(delta, &g_total_cpu_ns);
        atomic64_inc(&g_call_count);
    }
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

#ifdef CONFIG_HAVE_HW_BREAKPOINT
    /* Android GKI 6.1 不导出 register_user_hw_breakpoint / unregister_hw_breakpoint，
     * 通过 kprobe 临时注册解析符号地址 */
    g_register_user_hw_bp = (register_user_hw_bp_fn)
        resolve_symbol("register_user_hw_breakpoint");
    g_unregister_hw_bp = (unregister_hw_bp_fn)
        resolve_symbol("unregister_hw_breakpoint");

    if (g_register_user_hw_bp && g_unregister_hw_bp) {
        pr_info("HWBP symbols resolved via kprobe\n");
    } else {
        pr_warn("HWBP symbols not found via kprobe, HWBP disabled\n");
    }
#endif

    strcpy((char *)THIS_MODULE->name, MODULE_HIDE_NAME);
    pr_info("device /dev/%s ready (shown as '%s' in lsmod)"
#ifdef CONFIG_HAVE_HW_BREAKPOINT
            " [HWBP supported]"
#endif
            "\n", DEVICE_NAME, MODULE_HIDE_NAME);
    return 0;
}

static void __exit mem_reader_exit(void)
{
    hw_bp_cleanup();
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
MODULE_DESCRIPTION("Optimized ARM64 memory R/W with HWBP snapshot support");
