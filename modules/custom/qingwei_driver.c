// ============================================================================
// qingwei_v4.2_fixed.c - 修正 FOLL_READ 未定义错误
// 基于 4.1，增加 OP_GET_PID（不依赖 /proc）
// 适用于 Linux 6.1 / Android 14
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
#include <linux/kobject.h>
#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/pagemap.h>
#include <asm/ptrace.h>
#include <asm/barrier.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("qingwei");
MODULE_DESCRIPTION("qingwei driver (base lookup + PID lookup)");
MODULE_VERSION("4.2-fixed");

#define USER_BUF_SIZE 0x1000

enum sm_req_op {
    OP_NULL = 0,
    OP_READ,
    OP_WRITE,
    OP_MEM_ENUM,
    OP_KEXIT,
    OP_GET_MODULE_BASE,
    OP_GET_PID,
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
};

static struct req_obj *g_req = NULL;
static struct page *g_pages = NULL;
static unsigned int g_num_pages = 0;
static unsigned int g_allocated_size = 0;
static struct task_struct *g_dispatch_thread = NULL;
static bool g_exiting = false;

static int major;
static struct class *qingwei_class = NULL;
static struct device *qingwei_device = NULL;
static struct cdev qingwei_cdev;

static void hide_module(void) {
    struct kobject *kobj = &THIS_MODULE->mkobj.kobj;
    if (kobj) kobject_del(kobj);
}

static int mmu_translate_va_to_pa(struct mm_struct *mm, unsigned long va, unsigned long *pa) {
    pgd_t *pgd = pgd_offset(mm, va);
    if (pgd_none(*pgd) || pgd_bad(*pgd)) return -EFAULT;
    p4d_t *p4d = p4d_offset(pgd, va);
    if (p4d_none(*p4d) || p4d_bad(*p4d)) return -EFAULT;
    pud_t *pud = pud_offset(p4d, va);
    if (pud_none(*pud) || pud_bad(*pud)) return -EFAULT;
    pmd_t *pmd = pmd_offset(pud, va);
    if (pmd_none(*pmd) || pmd_bad(*pmd)) return -EFAULT;
    pte_t *pte = pte_offset_kernel(pmd, va);
    if (!pte_present(*pte)) return -EFAULT;
    *pa = (pte_pfn(*pte) << PAGE_SHIFT) | (va & ~PAGE_MASK);
    return 0;
}

static ssize_t read_process_memory(int pid, unsigned long vaddr, void *buf, size_t size) {
    struct task_struct *task;
    struct mm_struct *mm;
    size_t done = 0;
    int ret;
    rcu_read_lock();
    task = find_task_by_vpid(pid);
    if (!task) { rcu_read_unlock(); return -ESRCH; }
    get_task_struct(task);
    rcu_read_unlock();
    mm = get_task_mm(task);
    if (!mm) { put_task_struct(task); return -ESRCH; }
    down_read(&mm->mmap_lock);
    while (done < size) {
        unsigned long pa;
        unsigned long offset = (vaddr + done) & ~PAGE_MASK;
        size_t chunk = min(size - done, PAGE_SIZE - offset);
        ret = mmu_translate_va_to_pa(mm, vaddr + done, &pa);
        if (ret < 0) {
            memset((char*)buf + done, 0, chunk);
        } else {
            void *kaddr = phys_to_virt(pa);
            if (kaddr) memcpy((char*)buf + done, kaddr, chunk);
            else memset((char*)buf + done, 0, chunk);
        }
        done += chunk;
    }
    up_read(&mm->mmap_lock);
    mmput(mm);
    put_task_struct(task);
    return done;
}

static ssize_t write_process_memory(int pid, unsigned long vaddr, const void *buf, size_t size) {
    struct task_struct *task;
    struct mm_struct *mm;
    size_t done = 0;
    int ret;
    rcu_read_lock();
    task = find_task_by_vpid(pid);
    if (!task) { rcu_read_unlock(); return -ESRCH; }
    get_task_struct(task);
    rcu_read_unlock();
    mm = get_task_mm(task);
    if (!mm) { put_task_struct(task); return -ESRCH; }
    down_read(&mm->mmap_lock);
    while (done < size) {
        unsigned long pa;
        unsigned long offset = (vaddr + done) & ~PAGE_MASK;
        size_t chunk = min(size - done, PAGE_SIZE - offset);
        ret = mmu_translate_va_to_pa(mm, vaddr + done, &pa);
        if (ret == 0) {
            void *kaddr = phys_to_virt(pa);
            if (kaddr) memcpy(kaddr, (char*)buf + done, chunk);
        }
        done += chunk;
    }
    up_read(&mm->mmap_lock);
    mmput(mm);
    put_task_struct(task);
    return done;
}

// ------------------------------------------------------------------
// 按包名查找 PID（直接读取进程的 cmdline，不经过 /proc）
// 使用 access_remote_vm，FOLL_READ 用 0 代替
// ------------------------------------------------------------------
static int get_pid_by_package_name(const char *pkg_name) {
    struct task_struct *task;
    struct mm_struct *mm;
    char *cmdline_buf;
    int pid = -ESRCH;
    int ret;

    if (!pkg_name)
        return -EINVAL;

    cmdline_buf = kmalloc(USER_BUF_SIZE, GFP_KERNEL);
    if (!cmdline_buf)
        return -ENOMEM;

    rcu_read_lock();
    for_each_process(task) {
        mm = get_task_mm(task);
        if (!mm)
            continue;

        // 读取进程的 cmdline，FOLL_READ 值实际为 0，直接使用 0 避免未定义
        ret = access_remote_vm(mm, mm->arg_start, cmdline_buf, USER_BUF_SIZE - 1, 0);
        mmput(mm);

        if (ret <= 0)
            continue;

        cmdline_buf[ret] = '\0';
        // 检查第一个参数是否完全等于包名（以 '\0' 或空格结尾）
        int len = strlen(pkg_name);
        if (strlen(cmdline_buf) >= len && strncmp(cmdline_buf, pkg_name, len) == 0) {
            if (cmdline_buf[len] == '\0' || cmdline_buf[len] == ' ') {
                pid = task->pid;
                break;
            }
        }
    }
    rcu_read_unlock();

    kfree(cmdline_buf);
    return pid;
}

// ------------------------------------------------------------------
// 按库名获取基址（同 4.1，保持不变）
// ------------------------------------------------------------------
static unsigned long get_module_base(int pid, const char *name) {
    struct task_struct *task;
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    unsigned long base = 0;
    char *path = NULL;
    char *buf = NULL;
    unsigned long start = 0;
    rcu_read_lock();
    task = find_task_by_vpid(pid);
    if (!task) { rcu_read_unlock(); return 0; }
    get_task_struct(task);
    rcu_read_unlock();
    mm = get_task_mm(task);
    if (!mm) { put_task_struct(task); return 0; }
    if (!down_read_trylock(&mm->mmap_lock)) {
        mmput(mm);
        put_task_struct(task);
        return 0;
    }
    buf = (char*)__get_free_page(GFP_KERNEL);
    if (!buf) {
        up_read(&mm->mmap_lock);
        mmput(mm);
        put_task_struct(task);
        return 0;
    }
    while ((vma = find_vma(mm, start)) != NULL) {
        if (vma->vm_file && vma->vm_file->f_path.dentry) {
            path = d_path(&vma->vm_file->f_path, buf, PAGE_SIZE);
            if (!IS_ERR(path)) {
                if (strstr(path, name)) {
                    base = vma->vm_start;
                    break;
                }
            }
        }
        start = vma->vm_end;
    }
    free_page((unsigned long)buf);
    up_read(&mm->mmap_lock);
    mmput(mm);
    put_task_struct(task);
    return base;
}

// ------------------------------------------------------------------
// 分发线程（新增 OP_GET_PID 处理）
// ------------------------------------------------------------------
static int dispatch_thread_func(void *data) {
    int ret;
    pr_info("qingwei: dispatch_thread started\n");
    while (!kthread_should_stop() && !g_exiting) {
        if (!g_req) { msleep(100); continue; }
        if (!g_req->kernel) { usleep_range(50,100); continue; }
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
            case OP_GET_MODULE_BASE: {
                char *name = (char*)g_req->user_buffer;
                name[USER_BUF_SIZE - 1] = '\0';
                unsigned long base = get_module_base(g_req->pid, name);
                if (base) {
                    g_req->target_addr = base;
                    g_req->status = 0;
                } else {
                    g_req->target_addr = 0;
                    g_req->status = -ENOENT;
                }
                break;
            }
            case OP_GET_PID: {
                char *pkg = (char*)g_req->user_buffer;
                pkg[USER_BUF_SIZE - 1] = '\0';
                int pid = get_pid_by_package_name(pkg);
                if (pid > 0) {
                    g_req->pid = pid;
                    g_req->status = 0;
                } else {
                    g_req->pid = -1;
                    g_req->status = pid; // 负错误码
                }
                break;
            }
            case OP_KEXIT:
                g_req->status = 0;
                g_req->user = true;
                local_irq_restore(flags);
                pr_info("qingwei: dispatch_thread received KEXIT\n");
                return 0;
            default:
                g_req->status = -EINVAL;
        }
        g_req->user = true;
        local_irq_restore(flags);
    }
    pr_info("qingwei: dispatch_thread exiting\n");
    return 0;
}

// ------------------------------------------------------------------
// mmap 操作（与 4.1 相同）
// ------------------------------------------------------------------
static int qingwei_mmap(struct file *filp, struct vm_area_struct *vma) {
    unsigned long size = vma->vm_end - vma->vm_start;
    unsigned long pfn;
    struct page *page;
    unsigned long addr;
    int err;
    int i;
    if (size > g_allocated_size) return -EINVAL;
    unsigned int num_pages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
    if (num_pages > g_num_pages) return -EINVAL;
    for (i = 0; i < num_pages; i++) {
        page = nth_page(g_pages, i);
        pfn = page_to_pfn(page);
        addr = vma->vm_start + i * PAGE_SIZE;
        err = remap_pfn_range(vma, addr, pfn, PAGE_SIZE, vma->vm_page_prot);
        if (err) return err;
    }
    return 0;
}

static struct file_operations qingwei_fops = {
    .owner = THIS_MODULE,
    .mmap  = qingwei_mmap,
};

// ------------------------------------------------------------------
// 初始化与退出（同 4.1）
// ------------------------------------------------------------------
static int __init qingwei_init(void) {
    dev_t dev;
    int ret;
    unsigned int req_size = sizeof(struct req_obj);
    unsigned int order = get_order(req_size);
    hide_module();
    g_pages = alloc_pages(GFP_KERNEL | __GFP_ZERO, order);
    if (!g_pages) return -ENOMEM;
    g_num_pages = (1 << order);
    g_allocated_size = g_num_pages * PAGE_SIZE;
    g_req = page_address(g_pages);
    if (!g_req) { __free_pages(g_pages, order); return -ENOMEM; }
    pr_info("qingwei: allocated %u pages at %p\n", g_num_pages, g_req);
    if (alloc_chrdev_region(&dev, 0, 1, "qingwei") < 0) goto err;
    major = MAJOR(dev);
    cdev_init(&qingwei_cdev, &qingwei_fops);
    qingwei_cdev.owner = THIS_MODULE;
    ret = cdev_add(&qingwei_cdev, dev, 1);
    if (ret < 0) { unregister_chrdev_region(dev, 1); goto err; }
    qingwei_class = class_create(THIS_MODULE, "qingwei_class");
    if (IS_ERR(qingwei_class)) { cdev_del(&qingwei_cdev); unregister_chrdev_region(dev, 1); goto err; }
    qingwei_device = device_create(qingwei_class, NULL, dev, NULL, "qingwei");
    if (IS_ERR(qingwei_device)) { class_destroy(qingwei_class); cdev_del(&qingwei_cdev); unregister_chrdev_region(dev, 1); goto err; }
    g_dispatch_thread = kthread_run(dispatch_thread_func, NULL, "qingwei_disp");
    if (IS_ERR(g_dispatch_thread)) { device_destroy(qingwei_class, dev); class_destroy(qingwei_class); cdev_del(&qingwei_cdev); unregister_chrdev_region(dev, 1); goto err; }
    pr_info("qingwei: loaded successfully\n");
    return 0;
err:
    if (g_pages) { __free_pages(g_pages, order); g_pages = NULL; g_req = NULL; }
    return -1;
}

static void __exit qingwei_exit(void) {
    dev_t dev = MKDEV(major, 0);
    unsigned int order = get_order(sizeof(struct req_obj));
    g_exiting = true;
    if (g_req) { g_req->op = OP_KEXIT; g_req->kernel = true; g_req->user = false; msleep(100); }
    if (g_dispatch_thread) kthread_stop(g_dispatch_thread);
    device_destroy(qingwei_class, dev);
    class_destroy(qingwei_class);
    cdev_del(&qingwei_cdev);
    unregister_chrdev_region(dev, 1);
    if (g_pages) { __free_pages(g_pages, order); g_pages = NULL; g_req = NULL; }
    pr_info("qingwei: unloaded\n");
}

module_init(qingwei_init);
module_exit(qingwei_exit);
