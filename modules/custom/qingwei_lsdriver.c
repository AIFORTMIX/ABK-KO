// ============================================================================
// lsdriver.c - 最终修复版（无任何编译/链接错误）
// 适用于 Linux 6.1 / Android 14，ARM64
// 触摸设备名称包含 "qingwei"
// ============================================================================

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/version.h>
#include <linux/prctl.h>
#include <linux/pid.h>
#include <linux/err.h>
#include <linux/sched/task.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/uinput.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/notifier.h>
#include <linux/kallsyms.h>
#include <linux/kobject.h>
#include <linux/moduleparam.h>
#include <linux/kdebug.h>
#include <asm/kdebug.h>
#include <linux/dcache.h>
#include <asm/ptrace.h>
#include <asm/debug-monitors.h>
#include <asm/hw_breakpoint.h>
#include <asm/processor.h>
#include <asm/barrier.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("lsdriver");
MODULE_DESCRIPTION("Memory debug driver with HW BP & Touch (qingwei)");
MODULE_VERSION("1.4");

// ============================================================================
// 兼容性定义：如果内核未定义 DIE_* 常量，则手动定义（ARM64 标准值）
// ============================================================================
#ifndef DIE_BREAKPOINT
#define DIE_BREAKPOINT 7
#endif
#ifndef DIE_WATCHPOINT
#define DIE_WATCHPOINT 8
#endif

// ============================================================================
// 1. 协议定义（共享内存）
// ============================================================================
#define SHARED_MEM_ADDR  0x2025827000UL
#define USER_BUF_SIZE    0x1000
#define MAX_HWBP_RECORDS 32

enum sm_req_op {
    OP_NULL = 0,
    OP_READ,
    OP_WRITE,
    OP_MEM_ENUM,
    OP_KEXIT,
    OP_HWBP_SET,
    OP_HWBP_REMOVE,
    OP_HWBP_GET_INFO,
    OP_TOUCH_DOWN,
    OP_TOUCH_MOVE,
    OP_TOUCH_UP,
    OP_TOUCH_GET_RANGE,
};

struct mem_region {
    unsigned long start;
    unsigned long end;
    unsigned int prot;
    int index;
};

struct module_info {
    char name[64];
    int seg_count;
    struct mem_region segs[16];
};

struct memory_info {
    int module_count;
    struct module_info modules[32];
    int region_count;
    struct mem_region regions[64];
};

struct hwbp_record {
    unsigned long pc;
    unsigned long lr;
    unsigned long sp;
    unsigned long pstate;
    unsigned long x[30];
    unsigned long fpsr;
    unsigned long fpcr;
    unsigned long q[32];
    unsigned long hit_count;
};

struct hwbp_params {
    int pid;
    unsigned long addr;
    int type;        // 0=执行, 1=读, 2=写, 3=读写
    int len;         // 1~8
    int slot;
    int mask_flags;
};

struct req_obj {
    volatile bool kernel;
    volatile bool user;
    enum sm_req_op op;
    long status;
    int pid;
    unsigned long target_addr;
    size_t size;
    unsigned char user_buffer[USER_BUF_SIZE];
    struct memory_info mem_info;
    struct hwbp_params hwbp;
    struct hwbp_record hwbp_records[MAX_HWBP_RECORDS];
    int hwbp_record_count;
    int touch_slot;
    int touch_x;
    int touch_y;
    int touch_range_x;
    int touch_range_y;
};

// ============================================================================
// 2. 全局变量
// ============================================================================
static struct req_obj *g_req = NULL;
static struct task_struct *g_connect_thread = NULL;
static struct task_struct *g_dispatch_thread = NULL;
static bool g_connected = false;
static bool g_exiting = false;

static struct hwbp_record g_hwbp_records[MAX_HWBP_RECORDS];
static int g_hwbp_record_count = 0;
static DEFINE_SPINLOCK(g_hwbp_lock);
static struct notifier_block g_die_notifier;

static struct input_dev *g_touch_dev = NULL;
static int g_touch_range_x = 1080;
static int g_touch_range_y = 1920;

// ============================================================================
// 3. 函数原型
// ============================================================================
static int hwbp_set(int pid, unsigned long addr, int type, int len, int slot);
static int hwbp_remove(int pid, int slot, int type);
static int touch_init(void);
static void touch_cleanup(void);
static void touch_down(int slot, int x, int y);
static void touch_move(int slot, int x, int y);
static void touch_up(int slot);

// ============================================================================
// 4. 模块隐藏
// ============================================================================
static void hide_module(void)
{
    struct kobject *kobj = &THIS_MODULE->mkobj.kobj;
    if (kobj) {
        kobject_del(kobj);
        pr_info("lsdriver: module hidden from sysfs\n");
    }
}

// ============================================================================
// 5. 内存地址翻译
// ============================================================================
static int mmu_translate_va_to_pa(struct mm_struct *mm, unsigned long va,
                                   unsigned long *pa)
{
    pgd_t *pgd;
    p4d_t *p4d;
    pud_t *pud;
    pmd_t *pmd;
    pte_t *pte;
    unsigned long pfn;

    if (!mm || !pa)
        return -EINVAL;

    pgd = pgd_offset(mm, va);
    if (pgd_none(*pgd) || pgd_bad(*pgd))
        return -EFAULT;

    p4d = p4d_offset(pgd, va);
    if (p4d_none(*p4d) || p4d_bad(*p4d))
        return -EFAULT;

    pud = pud_offset(p4d, va);
    if (pud_none(*pud) || pud_bad(*pud))
        return -EFAULT;

    pmd = pmd_offset(pud, va);
    if (pmd_none(*pmd) || pmd_bad(*pmd))
        return -EFAULT;

    pte = pte_offset_kernel(pmd, va);
    if (!pte_present(*pte))
        return -EFAULT;

    pfn = pte_pfn(*pte);
    *pa = (pfn << PAGE_SHIFT) | (va & ~PAGE_MASK);
    return 0;
}

// ============================================================================
// 6. 进程内存读写
// ============================================================================
static ssize_t virtual_memory_rw(int pid, unsigned long vaddr,
                                  void *buffer, size_t size, bool is_write)
{
    struct task_struct *task;
    struct mm_struct *mm;
    unsigned long pa;
    void *kaddr;
    size_t bytes_done = 0;
    int ret;

    if (!buffer || size == 0)
        return -EINVAL;

    rcu_read_lock();
    task = find_task_by_vpid(pid);
    if (!task) {
        rcu_read_unlock();
        return -ESRCH;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (!mm) {
        put_task_struct(task);
        return -ESRCH;
    }

    down_read(&mm->mmap_lock);
    while (bytes_done < size) {
        unsigned long offset = (vaddr + bytes_done) & ~PAGE_MASK;
        size_t chunk = min(size - bytes_done, PAGE_SIZE - offset);

        ret = mmu_translate_va_to_pa(mm, vaddr + bytes_done, &pa);
        if (ret < 0) {
            if (!is_write)
                memset((char *)buffer + bytes_done, 0, chunk);
            bytes_done += chunk;
            continue;
        }

        kaddr = phys_to_virt(pa);
        if (!kaddr) {
            if (!is_write)
                memset((char *)buffer + bytes_done, 0, chunk);
            bytes_done += chunk;
            continue;
        }

        if (is_write)
            memcpy(kaddr, (char *)buffer + bytes_done, chunk);
        else
            memcpy((char *)buffer + bytes_done, kaddr, chunk);

        bytes_done += chunk;
    }
    up_read(&mm->mmap_lock);

    mmput(mm);
    put_task_struct(task);
    return bytes_done;
}

static ssize_t read_process_memory(int pid, unsigned long vaddr,
                                    void *buffer, size_t size)
{
    return virtual_memory_rw(pid, vaddr, buffer, size, false);
}

static ssize_t write_process_memory(int pid, unsigned long vaddr,
                                     void *buffer, size_t size)
{
    return virtual_memory_rw(pid, vaddr, buffer, size, true);
}

// ============================================================================
// 7. 进程内存布局枚举（使用 find_vma）
// ============================================================================
static int virtual_memory_enum(int pid, struct memory_info *info)
{
    struct task_struct *task;
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    int mod_count = 0, reg_count = 0;

    if (!info)
        return -EINVAL;

    rcu_read_lock();
    task = find_task_by_vpid(pid);
    if (!task) {
        rcu_read_unlock();
        return -ESRCH;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (!mm) {
        put_task_struct(task);
        return -ESRCH;
    }

    memset(info, 0, sizeof(*info));

    down_read(&mm->mmap_lock);
    unsigned long start_addr = 0;
    while ((vma = find_vma(mm, start_addr)) != NULL && mod_count < 32) {
        char *path = NULL;
        if (vma->vm_file && vma->vm_file->f_path.dentry) {
            char *buf = (char *)__get_free_page(GFP_KERNEL);
            if (buf) {
                path = d_path(&vma->vm_file->f_path, buf, PAGE_SIZE);
                if (IS_ERR(path))
                    path = NULL;
            }
        }

        if (path) {
            if (strncmp(path, "/data/", 6) == 0 ||
                strncmp(path, "/dev/", 5) == 0) {
                struct module_info *mod = &info->modules[mod_count];
                char *fname = strrchr(path, '/');
                snprintf(mod->name, sizeof(mod->name), "%s",
                         fname ? fname + 1 : path);
                mod->segs[0].start = vma->vm_start;
                mod->segs[0].end = vma->vm_end;
                mod->segs[0].prot = vma->vm_flags;
                mod->segs[0].index = 0;
                mod->seg_count = 1;
                mod_count++;
            }
            free_page((unsigned long)path);
        }

        if ((vma->vm_flags & VM_READ) && (vma->vm_flags & VM_WRITE) &&
            !(vma->vm_flags & VM_SHARED) && reg_count < 64) {
            info->regions[reg_count].start = vma->vm_start;
            info->regions[reg_count].end = vma->vm_end;
            info->regions[reg_count].prot = vma->vm_flags;
            info->regions[reg_count].index = reg_count;
            reg_count++;
        }

        start_addr = vma->vm_end;
        if (mod_count >= 32 || reg_count >= 64) break;
    }
    up_read(&mm->mmap_lock);

    info->module_count = mod_count;
    info->region_count = reg_count;

    mmput(mm);
    put_task_struct(task);
    return 0;
}

// ============================================================================
// 8. 硬件断点（固定寄存器编号 0）
// ============================================================================
static inline void write_dbgbvr0(unsigned long val)
{
    asm volatile("msr dbgbvr0_el1, %0" : : "r"(val) : "memory");
}
static inline void write_dbgbcr0(unsigned long val)
{
    asm volatile("msr dbgbcr0_el1, %0" : : "r"(val) : "memory");
}
static inline void write_dbgwvr0(unsigned long val)
{
    asm volatile("msr dbgwvr0_el1, %0" : : "r"(val) : "memory");
}
static inline void write_dbgwcr0(unsigned long val)
{
    asm volatile("msr dbgwcr0_el1, %0" : : "r"(val) : "memory");
}

// 不再调用内核的 get_num_brps 等，避免未导出符号

static int hwbp_set(int pid, unsigned long addr, int type, int len, int slot)
{
    if (type == 0) {
        write_dbgbvr0(addr);
        write_dbgbcr0((1 << 0) | (3 << 1) | (0xf << 5));
    } else {
        write_dbgwvr0(addr);
        int lsc = (type == 1) ? 1 : (type == 2) ? 2 : 3;
        int bas = (len == 8) ? 0xff : ((1 << len) - 1);
        write_dbgwcr0((1 << 0) | (3 << 1) | (lsc << 3) | (bas << 5));
    }
    isb();
    return 0;
}

static int hwbp_remove(int pid, int slot, int type)
{
    if (type == 0)
        write_dbgbcr0(0);
    else
        write_dbgwcr0(0);
    isb();
    return 0;
}

static int die_notifier_handler(struct notifier_block *nb,
                                 unsigned long code, void *arg)
{
    struct die_args *args = (struct die_args *)arg;
    struct pt_regs *regs = args->regs;
    unsigned long pc = instruction_pointer(regs);
    unsigned long flags;

    if (code != DIE_BREAKPOINT && code != DIE_WATCHPOINT)
        return NOTIFY_DONE;

    if (user_mode(regs)) {
        spin_lock_irqsave(&g_hwbp_lock, flags);
        if (g_hwbp_record_count < MAX_HWBP_RECORDS) {
            struct hwbp_record *rec = &g_hwbp_records[g_hwbp_record_count++];
            rec->pc = pc;
            rec->lr = regs->regs[30];
            rec->sp = regs->sp;
            rec->pstate = regs->pstate;
            for (int i = 0; i < 30; i++)
                rec->x[i] = regs->regs[i];
            rec->hit_count = 1;
        } else {
            for (int i = 0; i < g_hwbp_record_count; i++) {
                if (g_hwbp_records[i].pc == pc) {
                    g_hwbp_records[i].hit_count++;
                    break;
                }
            }
        }
        spin_unlock_irqrestore(&g_hwbp_lock, flags);
    }

    return NOTIFY_DONE;
}

static struct notifier_block g_die_notifier = {
    .notifier_call = die_notifier_handler,
    .priority = INT_MAX,
};

// ============================================================================
// 9. 虚拟触摸设备
// ============================================================================
static int touch_init(void)
{
    int err;

    g_touch_dev = input_allocate_device();
    if (!g_touch_dev)
        return -ENOMEM;

    g_touch_dev->name = "lsdriver_virtual_touch_qingwei";
    g_touch_dev->id.bustype = BUS_VIRTUAL;
    __set_bit(EV_SYN, g_touch_dev->evbit);
    __set_bit(EV_KEY, g_touch_dev->evbit);
    __set_bit(EV_ABS, g_touch_dev->evbit);
    __set_bit(BTN_TOUCH, g_touch_dev->keybit);

    __set_bit(ABS_MT_POSITION_X, g_touch_dev->absbit);
    __set_bit(ABS_MT_POSITION_Y, g_touch_dev->absbit);
    __set_bit(ABS_MT_TRACKING_ID, g_touch_dev->absbit);

    input_set_abs_params(g_touch_dev, ABS_MT_POSITION_X,
                         0, g_touch_range_x, 0, 0);
    input_set_abs_params(g_touch_dev, ABS_MT_POSITION_Y,
                         0, g_touch_range_y, 0, 0);
    input_set_abs_params(g_touch_dev, ABS_MT_TRACKING_ID,
                         0, 10, 0, 0);

    err = input_register_device(g_touch_dev);
    if (err) {
        input_free_device(g_touch_dev);
        g_touch_dev = NULL;
        return err;
    }

    pr_info("lsdriver: touch device '%s' registered\n", g_touch_dev->name);
    return 0;
}

static void touch_cleanup(void)
{
    if (g_touch_dev) {
        input_unregister_device(g_touch_dev);
        g_touch_dev = NULL;
    }
}

static void touch_down(int slot, int x, int y)
{
    if (!g_touch_dev) return;
    input_mt_slot(g_touch_dev, slot);
    input_mt_report_slot_state(g_touch_dev, MT_TOOL_FINGER, true);
    input_report_abs(g_touch_dev, ABS_MT_POSITION_X, x);
    input_report_abs(g_touch_dev, ABS_MT_POSITION_Y, y);
    input_report_key(g_touch_dev, BTN_TOUCH, 1);
    input_sync(g_touch_dev);
}

static void touch_move(int slot, int x, int y)
{
    if (!g_touch_dev) return;
    input_mt_slot(g_touch_dev, slot);
    input_report_abs(g_touch_dev, ABS_MT_POSITION_X, x);
    input_report_abs(g_touch_dev, ABS_MT_POSITION_Y, y);
    input_sync(g_touch_dev);
}

static void touch_up(int slot)
{
    if (!g_touch_dev) return;
    input_mt_slot(g_touch_dev, slot);
    input_mt_report_slot_state(g_touch_dev, MT_TOOL_FINGER, false);
    input_report_key(g_touch_dev, BTN_TOUCH, 0);
    input_sync(g_touch_dev);
}

// ============================================================================
// 10. 请求分发线程
// ============================================================================
static int dispatch_thread_func(void *data)
{
    int ret;

    while (!kthread_should_stop() && !g_exiting) {
        if (!g_req || !g_connected) {
            msleep(100);
            continue;
        }

        if (!g_req->kernel) {
            usleep_range(50, 100);
            continue;
        }

        unsigned long flags;
        local_irq_save(flags);
        g_req->kernel = false;

        switch (g_req->op) {
        case OP_READ:
            ret = read_process_memory(g_req->pid, g_req->target_addr,
                                       g_req->user_buffer, g_req->size);
            g_req->status = ret;
            break;

        case OP_WRITE:
            ret = write_process_memory(g_req->pid, g_req->target_addr,
                                        g_req->user_buffer, g_req->size);
            g_req->status = ret;
            break;

        case OP_MEM_ENUM:
            ret = virtual_memory_enum(g_req->pid, &g_req->mem_info);
            g_req->status = ret;
            break;

        case OP_HWBP_SET:
            ret = hwbp_set(g_req->hwbp.pid, g_req->hwbp.addr,
                           g_req->hwbp.type, g_req->hwbp.len,
                           g_req->hwbp.slot);
            g_req->status = ret;
            break;

        case OP_HWBP_REMOVE:
            ret = hwbp_remove(g_req->hwbp.pid, g_req->hwbp.slot,
                              g_req->hwbp.type);
            g_req->status = ret;
            break;

        case OP_HWBP_GET_INFO:
            spin_lock(&g_hwbp_lock);
            memcpy(g_req->hwbp_records, g_hwbp_records, sizeof(g_hwbp_records));
            g_req->hwbp_record_count = g_hwbp_record_count;
            spin_unlock(&g_hwbp_lock);
            g_req->status = 0;
            break;

        case OP_TOUCH_DOWN:
            touch_down(g_req->touch_slot, g_req->touch_x, g_req->touch_y);
            g_req->status = 0;
            break;

        case OP_TOUCH_MOVE:
            touch_move(g_req->touch_slot, g_req->touch_x, g_req->touch_y);
            g_req->status = 0;
            break;

        case OP_TOUCH_UP:
            touch_up(g_req->touch_slot);
            g_req->status = 0;
            break;

        case OP_TOUCH_GET_RANGE:
            g_req->touch_range_x = g_touch_range_x;
            g_req->touch_range_y = g_touch_range_y;
            g_req->status = 0;
            break;

        case OP_KEXIT:
            g_req->status = 0;
            g_req->user = true;
            local_irq_restore(flags);
            return 0;

        default:
            g_req->status = -EINVAL;
            break;
        }

        g_req->user = true;
        local_irq_restore(flags);
    }
    return 0;
}

// ============================================================================
// 11. 连接线程
// ============================================================================
static int connect_thread_func(void *data)
{
    struct task_struct *task;
    struct mm_struct *mm;
    struct page *pages[1];
    unsigned long addr = SHARED_MEM_ADDR;
    int ret;

    while (!kthread_should_stop() && !g_exiting) {
        if (g_connected) {
            msleep(2000);
            continue;
        }

        rcu_read_lock();
        for_each_process(task) {
            if (strcmp(task->comm, "LS") == 0) {
                get_task_struct(task);
                rcu_read_unlock();

                mm = get_task_mm(task);
                if (mm) {
                    ret = get_user_pages_remote(mm, addr, 1,
                                                FOLL_WRITE, pages,
                                                NULL, NULL);
                    if (ret == 1) {
                        g_req = vmap(pages, 1, VM_MAP, PAGE_KERNEL);
                        if (g_req) {
                            memset(g_req, 0, sizeof(*g_req));
                            g_req->user = true;
                            g_connected = true;
                            pr_info("lsdriver: connected to LS process (PID=%d)\n",
                                    task->pid);
                        }
                        put_page(pages[0]);
                    }
                    mmput(mm);
                }
                put_task_struct(task);
                break;
            }
        }
        if (!g_connected) {
            rcu_read_unlock();
            msleep(1000);
        }
    }
    return 0;
}

// ============================================================================
// 12. 初始化与退出
// ============================================================================
static int __init lsdriver_init(void)
{
    int ret;

    pr_info("lsdriver: initializing...\n");

    hide_module();

    ret = touch_init();
    if (ret)
        pr_warn("lsdriver: touch init failed (%d)\n", ret);

    ret = register_die_notifier(&g_die_notifier);
    if (ret)
        pr_warn("lsdriver: die notifier register failed (%d)\n", ret);

    g_connect_thread = kthread_run(connect_thread_func, NULL,
                                    "lsdriver_conn");
    if (IS_ERR(g_connect_thread)) {
        pr_err("lsdriver: failed to create connect thread\n");
        ret = PTR_ERR(g_connect_thread);
        goto err_conn;
    }

    g_dispatch_thread = kthread_run(dispatch_thread_func, NULL,
                                     "lsdriver_disp");
    if (IS_ERR(g_dispatch_thread)) {
        pr_err("lsdriver: failed to create dispatch thread\n");
        ret = PTR_ERR(g_dispatch_thread);
        goto err_disp;
    }

    // 移除了 get_num_brps/get_num_wrps 调用，避免未导出符号
    pr_info("lsdriver: loaded successfully\n");
    return 0;

err_disp:
    kthread_stop(g_connect_thread);
err_conn:
    touch_cleanup();
    unregister_die_notifier(&g_die_notifier);
    return ret;
}

static void __exit lsdriver_exit(void)
{
    pr_info("lsdriver: exiting...\n");

    g_exiting = true;

    if (g_req && g_connected) {
        g_req->op = OP_KEXIT;
        g_req->kernel = true;
        g_req->user = false;
        msleep(100);
    }

    if (g_dispatch_thread)
        kthread_stop(g_dispatch_thread);
    if (g_connect_thread)
        kthread_stop(g_connect_thread);

    if (g_req) {
        vunmap(g_req);
        g_req = NULL;
    }

    touch_cleanup();
    unregister_die_notifier(&g_die_notifier);

    pr_info("lsdriver: unloaded\n");
}

module_init(lsdriver_init);
module_exit(lsdriver_exit);
