// ============================================================================
// qingwei_driver.c - 最终版（支持按库名获取基址）
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
MODULE_DESCRIPTION("qingwei driver with module base lookup");
MODULE_VERSION("4.0");

// ============================================================================
// 协议定义
// ============================================================================
#define USER_BUF_SIZE    0x1000

enum sm_req_op {
    OP_NULL = 0,
    OP_READ,
    OP_WRITE,
    OP_MEM_ENUM,                // 保留但不再使用
    OP_KEXIT,
    OP_GET_MODULE_BASE,         // 新增：获取指定库基址
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

struct req_obj {
    volatile bool kernel;
    volatile bool user;
    enum sm_req_op op;
    long status;                // 返回状态（0成功，负值错误）
    int pid;
    unsigned long target_addr;  // 输出：基址（用于 OP_GET_MODULE_BASE）
    size_t size;
    unsigned char user_buffer[USER_BUF_SIZE]; // 输入：库名字符串
    struct memory_info mem_info;
};

// ============================================================================
// 全局变量
// ============================================================================
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

// ============================================================================
// 模块隐藏
// ============================================================================
static void hide_module(void) {
    struct kobject *kobj = &THIS_MODULE->mkobj.kobj;
    if (kobj) {
        kobject_del(kobj);
        pr_info("qingwei: module hidden\n");
    }
}

// ============================================================================
// 内存地址翻译
// ============================================================================
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

// ============================================================================
// 进程内存读写
// ============================================================================
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

// ============================================================================
// 根据库名获取基址
// ============================================================================
static unsigned long get_module_base(int pid, const char *name) {
    struct task_struct *task;
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    unsigned long base = 0;
    char *path = NULL;
    char *buf = NULL;

    rcu_read_lock();
    task = find_task_by_vpid(pid);
    if (!task) { rcu_read_unlock(); return 0; }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (!mm) { put_task_struct(task); return 0; }

    if (!down_read_trylock(&mm->mmap_lock)) {
        pr_err("qingwei: cannot acquire mmap_lock for pid %d\n", pid);
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

    for (vma = mm->mmap; vma; vma = vma->vm_next) {
        if (!vma->vm_file || !vma->vm_file->f_path.dentry)
            continue;
        path = d_path(&vma->vm_file->f_path, buf, PAGE_SIZE);
        if (IS_ERR(path))
            continue;
        // 检查路径是否包含库名
        if (strstr(path, name)) {
            base = vma->vm_start;
            break;
        }
    }

    free_page((unsigned long)buf);
    up_read(&mm->mmap_lock);
    mmput(mm);
    put_task_struct(task);

    if (base)
        pr_info("qingwei: found %s at 0x%lx\n", name, base);
    else
        pr_info("qingwei: %s not found in pid %d\n", name, pid);
    return base;
}

// ============================================================================
// 请求分发线程
// ============================================================================
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
                // user_buffer 存放库名，以 '\0' 结尾
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
            case OP_MEM_ENUM:
                // 保留但不再使用，可简单返回错误
                g_req->status = -ENOSYS;
                break;
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

// ============================================================================
// mmap 操作
// ============================================================================
static int qingwei_mmap(struct file *filp, struct vm_area_struct *vma) {
    unsigned long size = vma->vm_end - vma->vm_start;
    unsigned long pfn;
    struct page *page;
    unsigned long addr;
    int err;
    int i;

    pr_info("qingwei: mmap called, size=%lu, allocated_size=%u\n", size, g_allocated_size);

    if (size > g_allocated_size) {
        pr_err("qingwei: mmap size too large: %lu > %u\n", size, g_allocated_size);
        return -EINVAL;
    }

    unsigned int num_pages = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
    if (num_pages > g_num_pages) {
        pr_err("qingwei: requested pages %u > allocated %u\n", num_pages, g_num_pages);
        return -EINVAL;
    }

    for (i = 0; i < num_pages; i++) {
        page = nth_page(g_pages, i);
        pfn = page_to_pfn(page);
        addr = vma->vm_start + i * PAGE_SIZE;
        err = remap_pfn_range(vma, addr, pfn, PAGE_SIZE, vma->vm_page_prot);
        if (err) {
            pr_err("qingwei: remap_pfn_range failed at page %d with %d\n", i, err);
            return err;
        }
    }

    pr_info("qingwei: mmap success, %u pages mapped\n", num_pages);
    return 0;
}

static struct file_operations qingwei_fops = {
    .owner = THIS_MODULE,
    .mmap  = qingwei_mmap,
};

// ============================================================================
// 初始化与退出
// ============================================================================
static int __init qingwei_init(void) {
    dev_t dev;
    int ret;
    unsigned int req_size = sizeof(struct req_obj);
    unsigned int order = get_order(req_size);

    hide_module();

    g_pages = alloc_pages(GFP_KERNEL | __GFP_ZERO, order);
    if (!g_pages) {
        pr_err("qingwei: alloc_pages failed\n");
        return -ENOMEM;
    }
    g_num_pages = (1 << order);
    g_allocated_size = g_num_pages * PAGE_SIZE;
    g_req = page_address(g_pages);
    if (!g_req) {
        pr_err("qingwei: page_address failed\n");
        __free_pages(g_pages, order);
        return -ENOMEM;
    }

    pr_info("qingwei: allocated %u pages (%u bytes) at %p\n",
            g_num_pages, g_allocated_size, g_req);

    if (alloc_chrdev_region(&dev, 0, 1, "qingwei") < 0) {
        pr_err("qingwei: alloc_chrdev_region failed\n");
        goto err_free_pages;
    }
    major = MAJOR(dev);
    cdev_init(&qingwei_cdev, &qingwei_fops);
    qingwei_cdev.owner = THIS_MODULE;
    ret = cdev_add(&qingwei_cdev, dev, 1);
    if (ret < 0) {
        pr_err("qingwei: cdev_add failed with %d\n", ret);
        unregister_chrdev_region(dev, 1);
        goto err_free_pages;
    }
    qingwei_class = class_create(THIS_MODULE, "qingwei_class");
    if (IS_ERR(qingwei_class)) {
        pr_err("qingwei: class_create failed\n");
        cdev_del(&qingwei_cdev);
        unregister_chrdev_region(dev, 1);
        goto err_free_pages;
    }
    qingwei_device = device_create(qingwei_class, NULL, dev, NULL, "qingwei");
    if (IS_ERR(qingwei_device)) {
        pr_err("qingwei: device_create failed\n");
        class_destroy(qingwei_class);
        cdev_del(&qingwei_cdev);
        unregister_chrdev_region(dev, 1);
        goto err_free_pages;
    }

    g_dispatch_thread = kthread_run(dispatch_thread_func, NULL, "qingwei_disp");
    if (IS_ERR(g_dispatch_thread)) {
        pr_err("qingwei: dispatch thread failed\n");
        device_destroy(qingwei_class, dev);
        class_destroy(qingwei_class);
        cdev_del(&qingwei_cdev);
        unregister_chrdev_region(dev, 1);
        goto err_free_pages;
    }

    pr_info("qingwei: loaded successfully\n");
    return 0;

err_free_pages:
    if (g_pages) {
        __free_pages(g_pages, order);
        g_pages = NULL;
        g_req = NULL;
    }
    return -1;
}

static void __exit qingwei_exit(void) {
    dev_t dev = MKDEV(major, 0);
    unsigned int order = get_order(sizeof(struct req_obj));

    pr_info("qingwei: exiting...\n");
    g_exiting = true;

    if (g_req) {
        g_req->op = OP_KEXIT;
        g_req->kernel = true;
        g_req->user = false;
        msleep(100);
    }

    if (g_dispatch_thread) kthread_stop(g_dispatch_thread);

    device_destroy(qingwei_class, dev);
    class_destroy(qingwei_class);
    cdev_del(&qingwei_cdev);
    unregister_chrdev_region(dev, 1);

    if (g_pages) {
        __free_pages(g_pages, order);
        g_pages = NULL;
        g_req = NULL;
    }

    pr_info("qingwei: unloaded\n");
}

module_init(qingwei_init);
module_exit(qingwei_exit);
