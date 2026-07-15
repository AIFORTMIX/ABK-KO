// ============================================================================
// qingwei_basic.c - 精简稳定版（仅内存读写 + 枚举）
// 使用 vmalloc + mmap 共享内存，无断点/触摸
// 适用于 Linux 6.1 / Android 14（已修复所有警告）
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
#include <linux/file.h>
#include <linux/fcntl.h>
#include <linux/namei.h>
#include <asm/ptrace.h>
#include <asm/barrier.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("qingwei");
MODULE_DESCRIPTION("qingwei basic memory driver (no BP/touch)");
MODULE_VERSION("3.4");

// ============================================================================
// 协议定义
// ============================================================================
#define USER_BUF_SIZE    0x1000

enum sm_req_op {
    OP_NULL = 0,
    OP_READ,
    OP_WRITE,
    OP_MEM_ENUM,
    OP_KEXIT,
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
    long status;
    int pid;
    unsigned long target_addr;
    size_t size;
    unsigned char user_buffer[USER_BUF_SIZE];
    struct memory_info mem_info;
};

// ============================================================================
// 全局变量
// ============================================================================
static struct req_obj *g_req = NULL;
static struct task_struct *g_dispatch_thread = NULL;
static bool g_exiting = false;

static int major;
static struct class *qingwei_class = NULL;
static struct device *qingwei_device = NULL;
static struct cdev qingwei_cdev;

// ============================================================================
// 文件日志（使用 kernel_write）
// ============================================================================
static void write_kmsg_log(const char *msg) {
    struct file *filp = NULL;
    loff_t pos = 0;
    char *path = "/data/local/tmp/kmsg.txt";
    int ret;

    filp = filp_open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (IS_ERR(filp)) {
        pr_err("qingwei: failed to open log file\n");
        return;
    }

    pos = vfs_llseek(filp, 0, SEEK_END);
    ret = kernel_write(filp, msg, strlen(msg), &pos);
    if (ret < 0)
        pr_err("qingwei: kernel_write failed %d\n", ret);

    vfs_fsync(filp, 0);
    filp_close(filp, NULL);
}

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
// 进程内存布局枚举
// ============================================================================
static int virtual_memory_enum(int pid, struct memory_info *info) {
    struct task_struct *task;
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    int mod_count = 0, reg_count = 0;

    rcu_read_lock();
    task = find_task_by_vpid(pid);
    if (!task) { rcu_read_unlock(); return -ESRCH; }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (!mm) { put_task_struct(task); return -ESRCH; }

    memset(info, 0, sizeof(*info));
    down_read(&mm->mmap_lock);
    unsigned long start = 0;
    while ((vma = find_vma(mm, start)) != NULL && mod_count < 32) {
        char *path = NULL;
        if (vma->vm_file && vma->vm_file->f_path.dentry) {
            char *buf = (char*)__get_free_page(GFP_KERNEL);
            if (buf) {
                path = d_path(&vma->vm_file->f_path, buf, PAGE_SIZE);
                if (IS_ERR(path)) path = NULL;
            }
        }
        if (path) {
            if (strncmp(path, "/data/", 6) == 0 || strncmp(path, "/dev/", 5) == 0) {
                struct module_info *mod = &info->modules[mod_count];
                char *fname = strrchr(path, '/');
                snprintf(mod->name, sizeof(mod->name), "%s", fname ? fname+1 : path);
                mod->segs[0].start = vma->vm_start;
                mod->segs[0].end = vma->vm_end;
                mod->segs[0].prot = vma->vm_flags;
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
            reg_count++;
        }
        start = vma->vm_end;
    }
    up_read(&mm->mmap_lock);
    info->module_count = mod_count;
    info->region_count = reg_count;
    mmput(mm);
    put_task_struct(task);
    return 0;
}

// ============================================================================
// 请求分发线程
// ============================================================================
static int dispatch_thread_func(void *data) {
    int ret;
    write_kmsg_log("dispatch_thread: started\n");
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
            case OP_MEM_ENUM:
                ret = virtual_memory_enum(g_req->pid, &g_req->mem_info);
                g_req->status = ret;
                break;
            case OP_KEXIT:
                g_req->status = 0;
                g_req->user = true;
                local_irq_restore(flags);
                write_kmsg_log("dispatch_thread: received KEXIT\n");
                return 0;
            default:
                g_req->status = -EINVAL;
        }
        g_req->user = true;
        local_irq_restore(flags);
    }
    write_kmsg_log("dispatch_thread: exiting\n");
    return 0;
}

// ============================================================================
// mmap 操作：将 vmalloc 内存映射到用户空间
// ============================================================================
static int qingwei_mmap(struct file *filp, struct vm_area_struct *vma) {
    unsigned long size = vma->vm_end - vma->vm_start;
    struct page *page;
    int err;

    if (size > PAGE_SIZE) return -EINVAL;

    page = vmalloc_to_page(g_req);
    if (!page) {
        pr_err("qingwei: vmalloc_to_page failed\n");
        return -ENOMEM;
    }

    err = remap_pfn_range(vma, vma->vm_start, page_to_pfn(page), size, vma->vm_page_prot);
    if (err) {
        pr_err("qingwei: remap_pfn_range failed with %d\n", err);
        return err;
    }

    pr_info("qingwei: mmap success, user addr 0x%lx, kernel addr %p\n", vma->vm_start, g_req);
    return 0;
}

// ============================================================================
// 设备文件操作
// ============================================================================
static struct file_operations qingwei_fops = {
    .owner = THIS_MODULE,
    .mmap  = qingwei_mmap,
};

// ============================================================================
// 初始化与退出
// ============================================================================
static int __init qingwei_init(void) {
    dev_t dev;
    char buf[128];

    hide_module();

    // 分配共享内存
    g_req = vmalloc(sizeof(struct req_obj));
    if (!g_req) {
        pr_err("qingwei: vmalloc failed\n");
        return -ENOMEM;
    }
    memset(g_req, 0, sizeof(struct req_obj));

    snprintf(buf, sizeof(buf), "vmalloc addr: 0x%p\n", g_req);
    write_kmsg_log("=== qingwei_init ===\n");
    write_kmsg_log(buf);

    // 创建字符设备
    if (alloc_chrdev_region(&dev, 0, 1, "qingwei") < 0) {
        pr_err("qingwei: alloc_chrdev_region failed\n");
        goto err_vfree;
    }
    major = MAJOR(dev);
    cdev_init(&qingwei_cdev, &qingwei_fops);
    qingwei_cdev.owner = THIS_MODULE;
    if (cdev_add(&qingwei_cdev, dev, 1) < 0) {
        pr_err("qingwei: cdev_add failed\n");
        unregister_chrdev_region(dev, 1);
        goto err_vfree;
    }
    qingwei_class = class_create(THIS_MODULE, "qingwei_class");
    if (IS_ERR(qingwei_class)) {
        pr_err("qingwei: class_create failed\n");
        cdev_del(&qingwei_cdev);
        unregister_chrdev_region(dev, 1);
        goto err_vfree;
    }
    qingwei_device = device_create(qingwei_class, NULL, dev, NULL, "qingwei");
    if (IS_ERR(qingwei_device)) {
        pr_err("qingwei: device_create failed\n");
        class_destroy(qingwei_class);
        cdev_del(&qingwei_cdev);
        unregister_chrdev_region(dev, 1);
        goto err_vfree;
    }

    // 启动分发线程
    g_dispatch_thread = kthread_run(dispatch_thread_func, NULL, "qingwei_disp");
    if (IS_ERR(g_dispatch_thread)) {
        pr_err("qingwei: dispatch thread failed\n");
        device_destroy(qingwei_class, dev);
        class_destroy(qingwei_class);
        cdev_del(&qingwei_cdev);
        unregister_chrdev_region(dev, 1);
        goto err_vfree;
    }

    pr_info("qingwei: loaded successfully (vmalloc addr=%p)\n", g_req);
    write_kmsg_log("qingwei: loaded successfully\n");
    return 0;

err_vfree:
    vfree(g_req);
    g_req = NULL;
    return -1;
}

static void __exit qingwei_exit(void) {
    dev_t dev = MKDEV(major, 0);

    write_kmsg_log("qingwei: exiting...\n");
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

    if (g_req) {
        vfree(g_req);
        g_req = NULL;
    }

    write_kmsg_log("qingwei: unloaded\n");
    pr_info("qingwei: unloaded\n");
}

module_init(qingwei_init);
module_exit(qingwei_exit);
