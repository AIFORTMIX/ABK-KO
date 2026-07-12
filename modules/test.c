/*
 * mem_drv.c - 内核内存读写驱动模块 (无痕读写)
 * 编译: make -C /lib/modules/$(uname -r)/build M=$(PWD) modules
 * 加载: insmod mem_drv.ko
 * 设备: /dev/memdrv
 * 接口: ioctl (见 mem_drv.h)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/pid.h>
#include <linux/namei.h>
#include <linux/dcache.h>
#include <linux/version.h>

#define DEVICE_NAME "memdrv"
#define CLASS_NAME  "memdrv_class"

/* ---------- 用户态接口定义 (与应用程序共享) ---------- */
#define MEM_DRV_MAGIC 'M'

#define CMD_READ_DWORD   _IOR(MEM_DRV_MAGIC, 1, struct mem_rw_params)
#define CMD_READ_FLOAT   _IOR(MEM_DRV_MAGIC, 2, struct mem_rw_params)
#define CMD_READ_STRING  _IOR(MEM_DRV_MAGIC, 3, struct mem_rw_params)
#define CMD_WRITE_DWORD  _IOW(MEM_DRV_MAGIC, 4, struct mem_rw_params)
#define CMD_WRITE_FLOAT  _IOW(MEM_DRV_MAGIC, 5, struct mem_rw_params)
#define CMD_WRITE_STRING _IOW(MEM_DRV_MAGIC, 6, struct mem_rw_params)
#define CMD_GET_MODULE_BASE _IOR(MEM_DRV_MAGIC, 7, struct mem_module_info)

struct mem_rw_params {
    char process_name[32];          /* 目标进程名 (comm) */
    unsigned long base_offset;      /* 相对于模块基址的偏移 */
    unsigned long offsets[8];       /* 指针链偏移列表，以 0 结束 */
    int offset_count;               /* 实际偏移个数 (0 ~ 7) */
    union {
        uint32_t dword_val;
        float float_val;
        char string_val[256];
    } data;
    size_t data_size;               /* 用于 string 读写时的长度 */
};

struct mem_module_info {
    char process_name[32];
    char module_name[64];
    unsigned long base_address;
};

/* ---------- 全局变量 ---------- */
static int major_num;
static struct class *mem_class = NULL;
static struct device *mem_device = NULL;

/* ---------- 辅助函数 ---------- */

/* 根据进程名 (comm) 获取 task_struct */
static struct task_struct *find_task_by_name(const char *name)
{
    struct task_struct *task;
    rcu_read_lock();
    for_each_process(task) {
        if (task->comm && strcmp(task->comm, name) == 0) {
            get_task_struct(task);
            rcu_read_unlock();
            return task;
        }
    }
    rcu_read_unlock();
    return NULL;
}

/* 获取指定进程加载的模块基址 (通过 VMA 遍历) */
static unsigned long get_module_base(struct task_struct *task, const char *module_name)
{
    struct mm_struct *mm = task->mm;
    struct vm_area_struct *vma;
    unsigned long base = 0;
    char *path = NULL;
    char *buf = NULL;
    struct path root;
    char *full_path;

    if (!mm)
        return 0;

    /* 分配临时缓冲区存储路径 */
    buf = kmalloc(PATH_MAX, GFP_KERNEL);
    if (!buf)
        return 0;

    down_read(&mm->mmap_sem);
    for (vma = mm->mmap; vma; vma = vma->vm_next) {
        if (vma->vm_file && vma->vm_file->f_path.dentry) {
            /* 获取完整路径 */
            full_path = d_path(&vma->vm_file->f_path, buf, PATH_MAX);
            if (!IS_ERR(full_path)) {
                /* 检查路径是否包含指定的模块名 (如 "libxxx.so") */
                if (strstr(full_path, module_name)) {
                    base = vma->vm_start;
                    break;
                }
            }
        }
    }
    up_read(&mm->mmap_sem);
    kfree(buf);
    return base;
}

/* 解析指针链: base + offsets[0] -> *(+offsets[1]) -> ... 返回最终地址 */
static unsigned long resolve_pointer_chain(struct task_struct *task,
                                            unsigned long base,
                                            unsigned long *offsets,
                                            int offset_count)
{
    unsigned long addr = base;
    unsigned long ptr_buf;
    int i;

    for (i = 0; i < offset_count; i++) {
        addr += offsets[i];
        /* 从目标进程读取指针值 */
        if (access_process_vm(task, addr, &ptr_buf, sizeof(ptr_buf), FOLL_FORCE) != sizeof(ptr_buf)) {
            return 0;
        }
        addr = ptr_buf;
    }
    return addr;
}

/* ---------- 核心读写函数 (使用 access_process_vm 实现无痕) ---------- */

static int read_process_memory(struct task_struct *task, unsigned long addr,
                                void *buffer, size_t size)
{
    if (!task || !buffer)
        return -EINVAL;
    /* access_process_vm 返回实际读取字节数 */
    long ret = access_process_vm(task, addr, buffer, size, FOLL_FORCE);
    if (ret == size)
        return 0;
    return -EFAULT;
}

static int write_process_memory(struct task_struct *task, unsigned long addr,
                                 void *buffer, size_t size)
{
    if (!task || !buffer)
        return -EINVAL;
    long ret = access_process_vm(task, addr, buffer, size, FOLL_FORCE | FOLL_WRITE);
    if (ret == size)
        return 0;
    return -EFAULT;
}

/* ---------- 数据类型读写函数 ---------- */

/* 读取 DWORD (32位无符号) */
static uint32_t get_dword(struct task_struct *task, unsigned long addr)
{
    uint32_t val = 0;
    if (read_process_memory(task, addr, &val, sizeof(val)) < 0)
        return 0;
    return val;
}

/* 读取 Float */
static float get_float(struct task_struct *task, unsigned long addr)
{
    float val = 0.0f;
    if (read_process_memory(task, addr, &val, sizeof(val)) < 0)
        return 0.0f;
    return val;
}

/* 读取字符串 (最多 size-1 字节，自动添加 '\0') */
static int get_string(struct task_struct *task, unsigned long addr, char *buf, size_t size)
{
    if (!buf || size == 0)
        return -EINVAL;
    long ret = access_process_vm(task, addr, buf, size - 1, FOLL_FORCE);
    if (ret < 0)
        return ret;
    buf[ret] = '\0';
    return 0;
}

/* 写入 DWORD */
static int write_dword(struct task_struct *task, unsigned long addr, uint32_t val)
{
    return write_process_memory(task, addr, &val, sizeof(val));
}

/* 写入 Float */
static int write_float(struct task_struct *task, unsigned long addr, float val)
{
    return write_process_memory(task, addr, &val, sizeof(val));
}

/* 写入字符串 (最多 len 字节) */
static int write_string(struct task_struct *task, unsigned long addr, char *buf, size_t len)
{
    return write_process_memory(task, addr, buf, len);
}

/* ---------- ioctl 处理 ---------- */

static long mem_drv_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct mem_rw_params params;
    struct mem_module_info mod_info;
    struct task_struct *task;
    unsigned long target_addr;
    unsigned long base;
    int ret = 0;

    if (copy_from_user(&params, (void __user *)arg, sizeof(params))) {
        return -EFAULT;
    }

    /* 命令 CMD_GET_MODULE_BASE 单独处理 */
    if (cmd == CMD_GET_MODULE_BASE) {
        if (copy_from_user(&mod_info, (void __user *)arg, sizeof(mod_info))) {
            return -EFAULT;
        }
        task = find_task_by_name(mod_info.process_name);
        if (!task) {
            return -ESRCH;
        }
        base = get_module_base(task, mod_info.module_name);
        put_task_struct(task);
        if (!base)
            return -ENOENT;
        mod_info.base_address = base;
        if (copy_to_user((void __user *)arg, &mod_info, sizeof(mod_info))) {
            return -EFAULT;
        }
        return 0;
    }

    /* 其他读写命令需要解析进程 */
    task = find_task_by_name(params.process_name);
    if (!task) {
        return -ESRCH;
    }

    /* 默认使用 "libxxx.so" 作为模块名，实际可扩展为通过额外字段指定，此处固定 */
    base = get_module_base(task, "libxxx.so");
    if (!base) {
        put_task_struct(task);
        return -ENOENT;
    }

    /* 解析指针链 */
    if (params.offset_count > 8 || params.offset_count < 0) {
        put_task_struct(task);
        return -EINVAL;
    }
    target_addr = resolve_pointer_chain(task, base + params.base_offset,
                                        params.offsets, params.offset_count);
    if (!target_addr) {
        put_task_struct(task);
        return -EFAULT;
    }

    /* 根据命令执行读写 */
    switch (cmd) {
        case CMD_READ_DWORD:
            params.data.dword_val = get_dword(task, target_addr);
            if (copy_to_user((void __user *)arg, &params, sizeof(params))) {
                ret = -EFAULT;
            }
            break;

        case CMD_READ_FLOAT:
            params.data.float_val = get_float(task, target_addr);
            if (copy_to_user((void __user *)arg, &params, sizeof(params))) {
                ret = -EFAULT;
            }
            break;

        case CMD_READ_STRING:
            ret = get_string(task, target_addr, params.data.string_val, sizeof(params.data.string_val));
            if (ret == 0 && copy_to_user((void __user *)arg, &params, sizeof(params))) {
                ret = -EFAULT;
            }
            break;

        case CMD_WRITE_DWORD:
            ret = write_dword(task, target_addr, params.data.dword_val);
            break;

        case CMD_WRITE_FLOAT:
            ret = write_float(task, target_addr, params.data.float_val);
            break;

        case CMD_WRITE_STRING:
            ret = write_string(task, target_addr, params.data.string_val, params.data_size);
            break;

        default:
            ret = -EINVAL;
    }

    put_task_struct(task);
    return ret;
}

/* ---------- 文件操作结构 ---------- */
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = mem_drv_ioctl,
};

/* ---------- 模块初始化和退出 ---------- */

static int __init mem_drv_init(void)
{
    major_num = register_chrdev(0, DEVICE_NAME, &fops);
    if (major_num < 0) {
        printk(KERN_ALERT "Failed to register char device\n");
        return major_num;
    }

    mem_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(mem_class)) {
        unregister_chrdev(major_num, DEVICE_NAME);
        return PTR_ERR(mem_class);
    }

    mem_device = device_create(mem_class, NULL, MKDEV(major_num, 0), NULL, DEVICE_NAME);
    if (IS_ERR(mem_device)) {
        class_destroy(mem_class);
        unregister_chrdev(major_num, DEVICE_NAME);
        return PTR_ERR(mem_device);
    }

    printk(KERN_INFO "mem_drv loaded, major=%d, device /dev/%s\n", major_num, DEVICE_NAME);
    return 0;
}

static void __exit mem_drv_exit(void)
{
    device_destroy(mem_class, MKDEV(major_num, 0));
    class_destroy(mem_class);
    unregister_chrdev(major_num, DEVICE_NAME);
    printk(KERN_INFO "mem_drv unloaded\n");
}

module_init(mem_drv_init);
module_exit(mem_drv_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Kernel memory read/write driver with pointer offset support");
MODULE_VERSION("1.0");
