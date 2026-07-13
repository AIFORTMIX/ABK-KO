/*
 * mem_drv.c - 内核内存读写驱动模块
 * 编译: 通过 ABK-KO 的 build-ko-module.yml 编译为 .ko
 * 加载: insmod mem_drv.ko
 * 设备: /dev/memdrv
 */

 #include <linux/module.h>
 #include <linux/kernel.h>
 #include <linux/init.h>
 #include <linux/fs.h>
 #include <linux/device.h>
 #include <linux/sched.h>
 #include <linux/sched/signal.h>
 #include <linux/mm.h>
 #include <linux/slab.h>
 #include <linux/uaccess.h>
 #include <linux/pid.h>
 #include <linux/namei.h>
 #include <linux/dcache.h>
 #include <linux/version.h>
 #include <linux/ioctl.h>
 #include <linux/types.h>
 #include <linux/limits.h>
 #include <linux/string.h>
 
 #define DEVICE_NAME "memdrv"
 #define CLASS_NAME  "memdrv_class"
 
 /* ---------- 用户态接口定义 ---------- */
 #define MEM_DRV_MAGIC 'M'
 
 #define CMD_READ_DWORD      _IOR(MEM_DRV_MAGIC, 1, struct mem_rw_params)
 #define CMD_READ_FLOAT      _IOR(MEM_DRV_MAGIC, 2, struct mem_rw_params)
 #define CMD_READ_STRING     _IOR(MEM_DRV_MAGIC, 3, struct mem_rw_params)
 #define CMD_WRITE_DWORD     _IOW(MEM_DRV_MAGIC, 4, struct mem_rw_params)
 #define CMD_WRITE_FLOAT     _IOW(MEM_DRV_MAGIC, 5, struct mem_rw_params)
 #define CMD_WRITE_STRING    _IOW(MEM_DRV_MAGIC, 6, struct mem_rw_params)
 #define CMD_GET_MODULE_BASE _IOR(MEM_DRV_MAGIC, 7, struct mem_module_info)
 
 struct mem_rw_params {
     char process_name[32];
     unsigned long base_offset;
     unsigned long offsets[8];
     int offset_count;
     union {
         u32 dword_val;
         float float_val;
         char string_val[256];
     } data;
     size_t data_size;
 };
 
 struct mem_module_info {
     char process_name[32];
     char module_name[64];
     unsigned long base_address;
 };
 
 /* ---------- 全局变量 ---------- */
 static int major_num;
 static struct class *mem_class;
 static struct device *mem_device;
 
 /* ---------- 兼容封装 ---------- */
 
 static inline void abk_mmap_read_lock(struct mm_struct *mm)
 {
 #if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
     mmap_read_lock(mm);
 #else
     down_read(&mm->mmap_sem);
 #endif
 }
 
 static inline void abk_mmap_read_unlock(struct mm_struct *mm)
 {
 #if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
     mmap_read_unlock(mm);
 #else
     up_read(&mm->mmap_sem);
 #endif
 }
 
 #if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
 #define ABK_FOR_EACH_VMA(mm, vma) \
     VMA_ITERATOR(vmi, (mm), 0); \
     for_each_vma(vmi, (vma))
 #else
 #define ABK_FOR_EACH_VMA(mm, vma) \
     for ((vma) = (mm)->mmap; (vma); (vma) = (vma)->vm_next)
 #endif
 
 /* 根据进程名 comm 获取 task_struct */
 static struct task_struct *find_task_by_name(const char *name)
 {
     struct task_struct *task;
 
     if (!name || !name[0])
         return NULL;
 
     rcu_read_lock();
     for_each_process(task) {
         if (strncmp(task->comm, name, TASK_COMM_LEN) == 0) {
             get_task_struct(task);
             rcu_read_unlock();
             return task;
         }
     }
     rcu_read_unlock();
     return NULL;
 }
 
 /* 获取指定进程加载的模块基址，通过 VMA 遍历 */
 static unsigned long get_module_base(struct task_struct *task, const char *module_name)
 {
     struct mm_struct *mm;
     struct vm_area_struct *vma;
     unsigned long base = 0;
     char *buf;
     char *full_path;
 
     if (!task || !module_name || !module_name[0])
         return 0;
 
     mm = get_task_mm(task);
     if (!mm)
         return 0;
 
     buf = kmalloc(PATH_MAX, GFP_KERNEL);
     if (!buf) {
         mmput(mm);
         return 0;
     }
 
     abk_mmap_read_lock(mm);
     ABK_FOR_EACH_VMA(mm, vma) {
         if (vma->vm_file && vma->vm_file->f_path.dentry) {
             full_path = d_path(&vma->vm_file->f_path, buf, PATH_MAX);
             if (!IS_ERR(full_path) && strstr(full_path, module_name)) {
                 base = vma->vm_start;
                 break;
             }
         }
     }
     abk_mmap_read_unlock(mm);
 
     kfree(buf);
     mmput(mm);
     return base;
 }
 
 /* 解析指针链: base + offsets[0] -> *(+offsets[1]) -> ... */
 static unsigned long resolve_pointer_chain(struct task_struct *task,
                        unsigned long base,
                        unsigned long *offsets,
                        int offset_count)
 {
     unsigned long addr = base;
     unsigned long ptr_buf;
     int i;
 
     if (offset_count < 0 || offset_count > 8)
         return 0;
 
     for (i = 0; i < offset_count; i++) {
         addr += offsets[i];
         if (access_process_vm(task, addr, &ptr_buf, sizeof(ptr_buf), FOLL_FORCE) != sizeof(ptr_buf))
             return 0;
         addr = ptr_buf;
     }
 
     return addr;
 }
 
 static int read_process_memory(struct task_struct *task, unsigned long addr,
                    void *buffer, size_t size)
 {
     long ret;
 
     if (!task || !buffer || size == 0)
         return -EINVAL;
 
     ret = access_process_vm(task, addr, buffer, size, FOLL_FORCE);
     return ret == size ? 0 : -EFAULT;
 }
 
 static int write_process_memory(struct task_struct *task, unsigned long addr,
                 void *buffer, size_t size)
 {
     long ret;
 
     if (!task || !buffer || size == 0)
         return -EINVAL;
 
     ret = access_process_vm(task, addr, buffer, size, FOLL_FORCE | FOLL_WRITE);
     return ret == size ? 0 : -EFAULT;
 }
 
 static u32 get_dword(struct task_struct *task, unsigned long addr)
 {
     u32 val = 0;
 
     read_process_memory(task, addr, &val, sizeof(val));
     return val;
 }
 
 static float get_float(struct task_struct *task, unsigned long addr)
 {
     float val = 0.0f;
 
     read_process_memory(task, addr, &val, sizeof(val));
     return val;
 }
 
 static int get_string(struct task_struct *task, unsigned long addr, char *buf, size_t size)
 {
     long ret;
 
     if (!buf || size == 0)
         return -EINVAL;
 
     ret = access_process_vm(task, addr, buf, size - 1, FOLL_FORCE);
     if (ret < 0)
         return ret;
 
     buf[ret] = '\0';
     return 0;
 }
 
 static int write_dword(struct task_struct *task, unsigned long addr, u32 val)
 {
     return write_process_memory(task, addr, &val, sizeof(val));
 }
 
 static int write_float(struct task_struct *task, unsigned long addr, float val)
 {
     return write_process_memory(task, addr, &val, sizeof(val));
 }
 
 static int write_string(struct task_struct *task, unsigned long addr, char *buf, size_t len)
 {
     return write_process_memory(task, addr, buf, len);
 }
 
 static long mem_drv_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
 {
     struct mem_rw_params params;
     struct mem_module_info mod_info;
     struct task_struct *task;
     unsigned long target_addr;
     unsigned long base;
     int ret = 0;
 
     (void)filp;
 
     if (cmd == CMD_GET_MODULE_BASE) {
         if (copy_from_user(&mod_info, (void __user *)arg, sizeof(mod_info)))
             return -EFAULT;
 
         task = find_task_by_name(mod_info.process_name);
         if (!task)
             return -ESRCH;
 
         base = get_module_base(task, mod_info.module_name);
         put_task_struct(task);
         if (!base)
             return -ENOENT;
 
         mod_info.base_address = base;
         if (copy_to_user((void __user *)arg, &mod_info, sizeof(mod_info)))
             return -EFAULT;
 
         return 0;
     }
 
     if (copy_from_user(&params, (void __user *)arg, sizeof(params)))
         return -EFAULT;
 
     task = find_task_by_name(params.process_name);
     if (!task)
         return -ESRCH;
 
     base = get_module_base(task, "libxxx.so");
     if (!base) {
         put_task_struct(task);
         return -ENOENT;
     }
 
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
 
     switch (cmd) {
     case CMD_READ_DWORD:
         params.data.dword_val = get_dword(task, target_addr);
         if (copy_to_user((void __user *)arg, &params, sizeof(params)))
             ret = -EFAULT;
         break;
     case CMD_READ_FLOAT:
         params.data.float_val = get_float(task, target_addr);
         if (copy_to_user((void __user *)arg, &params, sizeof(params)))
             ret = -EFAULT;
         break;
     case CMD_READ_STRING:
         ret = get_string(task, target_addr, params.data.string_val,
                  sizeof(params.data.string_val));
         if (ret == 0 && copy_to_user((void __user *)arg, &params, sizeof(params)))
             ret = -EFAULT;
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
 
 static const struct file_operations fops = {
     .owner = THIS_MODULE,
     .unlocked_ioctl = mem_drv_ioctl,
 #ifdef CONFIG_COMPAT
     .compat_ioctl = mem_drv_ioctl,
 #endif
 };
 
 static int __init mem_drv_init(void)
 {
     major_num = register_chrdev(0, DEVICE_NAME, &fops);
     if (major_num < 0) {
         pr_alert("mem_drv: failed to register char device\n");
         return major_num;
     }
 
 #if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
     mem_class = class_create(CLASS_NAME);
 #else
     mem_class = class_create(THIS_MODULE, CLASS_NAME);
 #endif
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
 
     pr_info("mem_drv: loaded, major=%d, device=/dev/%s\n", major_num, DEVICE_NAME);
     return 0;
 }
 
 static void __exit mem_drv_exit(void)
 {
     if (mem_device)
         device_destroy(mem_class, MKDEV(major_num, 0));
     if (mem_class)
         class_destroy(mem_class);
     if (major_num > 0)
         unregister_chrdev(major_num, DEVICE_NAME);
 
     pr_info("mem_drv: unloaded\n");
 }
 
 module_init(mem_drv_init);
 module_exit(mem_drv_exit);
 
 MODULE_LICENSE("GPL");
 MODULE_AUTHOR("Your Name");
 MODULE_DESCRIPTION("Kernel memory read/write driver with pointer offset support");
 MODULE_VERSION("1.0");
 