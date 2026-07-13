/*
 * mem_drv_fast.c - 高频内存读写驱动
 *
 * 特性:
 * - attach 一次缓存目标进程和模块基址
 * - session_id 快速读写，避免每次 ioctl 都遍历进程和 VMA
 * - 支持 DWORD / FLOAT / U64
 * - 支持指针链
 * - 支持批量读写
 * - 支持显式设备名: insmod mem_drv_fast.ko dev_name=memdrv_fast
 *
 * 不包含隐藏、绕过或自隐藏逻辑。
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
#include <linux/dcache.h>
#include <linux/version.h>
#include <linux/ioctl.h>
#include <linux/types.h>
#include <linux/limits.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/atomic.h>

#define MEMDRV_DEFAULT_DEVICE_NAME "memdrv_fast"
#define MEMDRV_CLASS_NAME "memdrv_fast_class"

#define MEMDRV_MAX_SESSIONS 32
#define MEMDRV_MAX_CHAIN 8
#define MEMDRV_MAX_BATCH 256

#define MEMDRV_TYPE_DWORD 1
#define MEMDRV_TYPE_FLOAT 2
#define MEMDRV_TYPE_U64   3

#define MEMDRV_MAGIC 'F'

struct memdrv_attach_req {
	char process_name[32];
	char module_name[64];
	int session_id;
	unsigned long module_base;
};

struct memdrv_fast_rw {
	int session_id;
	int type;
	unsigned long offset;
	unsigned long offsets[MEMDRV_MAX_CHAIN];
	int offset_count;
	union {
		u32 dword_val;
		float float_val;
		u64 u64_val;
	} data;
};

struct memdrv_batch_item {
	int type;
	int write;
	int status;
	unsigned long offset;
	unsigned long offsets[MEMDRV_MAX_CHAIN];
	int offset_count;
	union {
		u32 dword_val;
		float float_val;
		u64 u64_val;
	} data;
};

struct memdrv_batch_req {
	int session_id;
	int count;
	struct memdrv_batch_item items[MEMDRV_MAX_BATCH];
};

#define MEMDRV_IOC_ATTACH        _IOWR(MEMDRV_MAGIC, 1, struct memdrv_attach_req)
#define MEMDRV_IOC_DETACH        _IOW(MEMDRV_MAGIC, 2, int)
#define MEMDRV_IOC_REFRESH_BASE  _IOWR(MEMDRV_MAGIC, 3, struct memdrv_attach_req)
#define MEMDRV_IOC_READ          _IOWR(MEMDRV_MAGIC, 4, struct memdrv_fast_rw)
#define MEMDRV_IOC_WRITE         _IOW(MEMDRV_MAGIC, 5, struct memdrv_fast_rw)
#define MEMDRV_IOC_BATCH         _IOWR(MEMDRV_MAGIC, 6, struct memdrv_batch_req)

struct memdrv_session {
	bool used;
	int id;
	struct task_struct *task;
	char process_name[32];
	char module_name[64];
	unsigned long module_base;
};

static char *memdrv_dev_name = MEMDRV_DEFAULT_DEVICE_NAME;
module_param_named(dev_name, memdrv_dev_name, charp, 0644);
MODULE_PARM_DESC(dev_name, "显式设备名，例如 memdrv_fast，对应 /dev/<dev_name>");

static int major_num;
static struct class *mem_class;
static struct device *mem_device;
static DEFINE_MUTEX(session_lock);
static struct memdrv_session sessions[MEMDRV_MAX_SESSIONS];
static atomic_t next_session_id = ATOMIC_INIT(1);

static bool valid_device_name(const char *name)
{
	size_t i;
	size_t len;

	if (!name || !name[0])
		return false;

	len = strnlen(name, 64);
	if (len == 0 || len >= 64)
		return false;

	for (i = 0; i < len; i++) {
		char c = name[i];

		if ((c >= 'a' && c <= 'z') ||
		    (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') ||
		    c == '_' || c == '-')
			continue;

		return false;
	}

	return true;
}

static inline void memdrv_mmap_read_lock(struct mm_struct *mm)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
	mmap_read_lock(mm);
#else
	down_read(&mm->mmap_sem);
#endif
}

static inline void memdrv_mmap_read_unlock(struct mm_struct *mm)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
	mmap_read_unlock(mm);
#else
	up_read(&mm->mmap_sem);
#endif
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
#define MEMDRV_FOR_EACH_VMA(mm, vma) \
	VMA_ITERATOR(vmi, (mm), 0); \
	for_each_vma(vmi, (vma))
#else
#define MEMDRV_FOR_EACH_VMA(mm, vma) \
	for ((vma) = (mm)->mmap; (vma); (vma) = (vma)->vm_next)
#endif

static struct task_struct *find_task_by_comm(const char *name)
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

static unsigned long find_module_base(struct task_struct *task, const char *module_name)
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

	memdrv_mmap_read_lock(mm);
	MEMDRV_FOR_EACH_VMA(mm, vma) {
		if (vma->vm_file && vma->vm_file->f_path.dentry) {
			full_path = d_path(&vma->vm_file->f_path, buf, PATH_MAX);
			if (!IS_ERR(full_path) && strstr(full_path, module_name)) {
				base = vma->vm_start;
				break;
			}
		}
	}
	memdrv_mmap_read_unlock(mm);

	kfree(buf);
	mmput(mm);
	return base;
}

static int memdrv_read_task(struct task_struct *task, unsigned long addr, void *buf, size_t size)
{
	long ret;

	if (!task || !buf || size == 0)
		return -EINVAL;

	ret = access_process_vm(task, addr, buf, size, FOLL_FORCE);
	return ret == size ? 0 : -EFAULT;
}

static int memdrv_write_task(struct task_struct *task, unsigned long addr, void *buf, size_t size)
{
	long ret;

	if (!task || !buf || size == 0)
		return -EINVAL;

	ret = access_process_vm(task, addr, buf, size, FOLL_FORCE | FOLL_WRITE);
	return ret == size ? 0 : -EFAULT;
}

static int resolve_addr(struct task_struct *task,
			unsigned long module_base,
			unsigned long offset,
			unsigned long *offsets,
			int offset_count,
			unsigned long *addr_out)
{
	unsigned long addr = module_base + offset;
	unsigned long ptr;
	int i;

	if (!addr_out || offset_count < 0 || offset_count > MEMDRV_MAX_CHAIN)
		return -EINVAL;

	for (i = 0; i < offset_count; i++) {
		addr += offsets[i];
		if (memdrv_read_task(task, addr, &ptr, sizeof(ptr)) != 0)
			return -EFAULT;
		addr = ptr;
	}

	*addr_out = addr;
	return 0;
}

static struct memdrv_session *find_session_locked(int id)
{
	int i;

	for (i = 0; i < MEMDRV_MAX_SESSIONS; i++) {
		if (sessions[i].used && sessions[i].id == id)
			return &sessions[i];
	}

	return NULL;
}

static int session_get_task_and_base(int id, struct task_struct **task_out, unsigned long *base_out)
{
	struct memdrv_session *s;

	if (!task_out || !base_out)
		return -EINVAL;

	mutex_lock(&session_lock);
	s = find_session_locked(id);
	if (!s || !s->task) {
		mutex_unlock(&session_lock);
		return -ENOENT;
	}

	get_task_struct(s->task);
	*task_out = s->task;
	*base_out = s->module_base;
	mutex_unlock(&session_lock);
	return 0;
}

static int do_one_rw(struct task_struct *task,
		     unsigned long module_base,
		     struct memdrv_fast_rw *rw,
		     bool write)
{
	unsigned long addr;
	int ret;

	ret = resolve_addr(task, module_base, rw->offset, rw->offsets, rw->offset_count, &addr);
	if (ret != 0)
		return ret;

	switch (rw->type) {
	case MEMDRV_TYPE_DWORD:
		return write ?
			memdrv_write_task(task, addr, &rw->data.dword_val, sizeof(rw->data.dword_val)) :
			memdrv_read_task(task, addr, &rw->data.dword_val, sizeof(rw->data.dword_val));
	case MEMDRV_TYPE_FLOAT:
		return write ?
			memdrv_write_task(task, addr, &rw->data.float_val, sizeof(rw->data.float_val)) :
			memdrv_read_task(task, addr, &rw->data.float_val, sizeof(rw->data.float_val));
	case MEMDRV_TYPE_U64:
		return write ?
			memdrv_write_task(task, addr, &rw->data.u64_val, sizeof(rw->data.u64_val)) :
			memdrv_read_task(task, addr, &rw->data.u64_val, sizeof(rw->data.u64_val));
	default:
		return -EINVAL;
	}
}

static long memdrv_attach(struct memdrv_attach_req __user *argp)
{
	struct memdrv_attach_req req;
	struct task_struct *task = NULL;
	unsigned long base;
	int slot = -1;
	int id;
	int i;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	if (!req.process_name[0] || !req.module_name[0])
		return -EINVAL;

	task = find_task_by_comm(req.process_name);
	if (!task)
		return -ESRCH;

	base = find_module_base(task, req.module_name);
	if (!base) {
		put_task_struct(task);
		return -ENOENT;
	}

	mutex_lock(&session_lock);
	for (i = 0; i < MEMDRV_MAX_SESSIONS; i++) {
		if (!sessions[i].used) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		mutex_unlock(&session_lock);
		put_task_struct(task);
		return -ENOSPC;
	}

	id = atomic_inc_return(&next_session_id);
	if (id <= 0)
		id = atomic_inc_return(&next_session_id);

	memset(&sessions[slot], 0, sizeof(sessions[slot]));
	sessions[slot].used = true;
	sessions[slot].id = id;
	sessions[slot].task = task;
	sessions[slot].module_base = base;
	strncpy(sessions[slot].process_name, req.process_name, sizeof(sessions[slot].process_name) - 1);
	strncpy(sessions[slot].module_name, req.module_name, sizeof(sessions[slot].module_name) - 1);
	mutex_unlock(&session_lock);

	req.session_id = id;
	req.module_base = base;

	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

static long memdrv_detach(int __user *argp)
{
	int id;
	struct memdrv_session *s;
	struct task_struct *task = NULL;

	if (copy_from_user(&id, argp, sizeof(id)))
		return -EFAULT;

	mutex_lock(&session_lock);
	s = find_session_locked(id);
	if (!s) {
		mutex_unlock(&session_lock);
		return -ENOENT;
	}

	task = s->task;
	memset(s, 0, sizeof(*s));
	mutex_unlock(&session_lock);

	if (task)
		put_task_struct(task);

	return 0;
}

static long memdrv_refresh_base(struct memdrv_attach_req __user *argp)
{
	struct memdrv_attach_req req;
	struct memdrv_session *s;
	struct task_struct *task = NULL;
	unsigned long base;

	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	mutex_lock(&session_lock);
	s = find_session_locked(req.session_id);
	if (!s || !s->task) {
		mutex_unlock(&session_lock);
		return -ENOENT;
	}
	get_task_struct(s->task);
	task = s->task;
	strncpy(req.process_name, s->process_name, sizeof(req.process_name) - 1);
	strncpy(req.module_name, s->module_name, sizeof(req.module_name) - 1);
	mutex_unlock(&session_lock);

	base = find_module_base(task, req.module_name);
	put_task_struct(task);
	if (!base)
		return -ENOENT;

	mutex_lock(&session_lock);
	s = find_session_locked(req.session_id);
	if (!s) {
		mutex_unlock(&session_lock);
		return -ENOENT;
	}
	s->module_base = base;
	req.module_base = base;
	mutex_unlock(&session_lock);

	if (copy_to_user(argp, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

static long memdrv_readwrite(struct memdrv_fast_rw __user *argp, bool write)
{
	struct memdrv_fast_rw rw;
	struct task_struct *task = NULL;
	unsigned long base;
	int ret;

	if (copy_from_user(&rw, argp, sizeof(rw)))
		return -EFAULT;

	ret = session_get_task_and_base(rw.session_id, &task, &base);
	if (ret != 0)
		return ret;

	ret = do_one_rw(task, base, &rw, write);
	put_task_struct(task);
	if (ret != 0)
		return ret;

	if (!write && copy_to_user(argp, &rw, sizeof(rw)))
		return -EFAULT;

	return 0;
}

static long memdrv_batch(struct memdrv_batch_req __user *argp)
{
	struct memdrv_batch_req *req;
	struct task_struct *task = NULL;
	unsigned long base;
	int i;
	int ret;

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	if (copy_from_user(req, argp, sizeof(*req))) {
		kfree(req);
		return -EFAULT;
	}

	if (req->count < 0 || req->count > MEMDRV_MAX_BATCH) {
		kfree(req);
		return -EINVAL;
	}

	ret = session_get_task_and_base(req->session_id, &task, &base);
	if (ret != 0) {
		kfree(req);
		return ret;
	}

	for (i = 0; i < req->count; i++) {
		struct memdrv_fast_rw rw;

		memset(&rw, 0, sizeof(rw));
		rw.session_id = req->session_id;
		rw.type = req->items[i].type;
		rw.offset = req->items[i].offset;
		rw.offset_count = req->items[i].offset_count;
		memcpy(rw.offsets, req->items[i].offsets, sizeof(rw.offsets));
		memcpy(&rw.data, &req->items[i].data, sizeof(rw.data));

		req->items[i].status = do_one_rw(task, base, &rw, req->items[i].write != 0);
		if (req->items[i].status == 0)
			memcpy(&req->items[i].data, &rw.data, sizeof(req->items[i].data));
	}

	put_task_struct(task);

	if (copy_to_user(argp, req, sizeof(*req))) {
		kfree(req);
		return -EFAULT;
	}

	kfree(req);
	return 0;
}

static long memdrv_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	(void)filp;

	switch (cmd) {
	case MEMDRV_IOC_ATTACH:
		return memdrv_attach((struct memdrv_attach_req __user *)arg);
	case MEMDRV_IOC_DETACH:
		return memdrv_detach((int __user *)arg);
	case MEMDRV_IOC_REFRESH_BASE:
		return memdrv_refresh_base((struct memdrv_attach_req __user *)arg);
	case MEMDRV_IOC_READ:
		return memdrv_readwrite((struct memdrv_fast_rw __user *)arg, false);
	case MEMDRV_IOC_WRITE:
		return memdrv_readwrite((struct memdrv_fast_rw __user *)arg, true);
	case MEMDRV_IOC_BATCH:
		return memdrv_batch((struct memdrv_batch_req __user *)arg);
	default:
		return -EINVAL;
	}
}

static const struct file_operations fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = memdrv_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = memdrv_ioctl,
#endif
};

static int __init memdrv_init(void)
{
	if (!valid_device_name(memdrv_dev_name)) {
		pr_alert("memdrv_fast: invalid dev_name=%s\n", memdrv_dev_name ? memdrv_dev_name : "(null)");
		return -EINVAL;
	}

	major_num = register_chrdev(0, memdrv_dev_name, &fops);
	if (major_num < 0) {
		pr_alert("memdrv_fast: failed to register char device\n");
		return major_num;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	mem_class = class_create(MEMDRV_CLASS_NAME);
#else
	mem_class = class_create(THIS_MODULE, MEMDRV_CLASS_NAME);
#endif
	if (IS_ERR(mem_class)) {
		unregister_chrdev(major_num, memdrv_dev_name);
		return PTR_ERR(mem_class);
	}

	mem_device = device_create(mem_class, NULL, MKDEV(major_num, 0), NULL, "%s", memdrv_dev_name);
	if (IS_ERR(mem_device)) {
		class_destroy(mem_class);
		unregister_chrdev(major_num, memdrv_dev_name);
		return PTR_ERR(mem_device);
	}

	pr_info("memdrv_fast: loaded, major=%d, device=/dev/%s, sessions=%d, batch=%d\n",
		major_num, memdrv_dev_name, MEMDRV_MAX_SESSIONS, MEMDRV_MAX_BATCH);
	return 0;
}

static void __exit memdrv_exit(void)
{
	int i;

	mutex_lock(&session_lock);
	for (i = 0; i < MEMDRV_MAX_SESSIONS; i++) {
		if (sessions[i].used && sessions[i].task) {
			put_task_struct(sessions[i].task);
			memset(&sessions[i], 0, sizeof(sessions[i]));
		}
	}
	mutex_unlock(&session_lock);

	if (mem_device)
		device_destroy(mem_class, MKDEV(major_num, 0));
	if (mem_class)
		class_destroy(mem_class);
	if (major_num > 0)
		unregister_chrdev(major_num, memdrv_dev_name);

	pr_info("memdrv_fast: unloaded\n");
}

module_init(memdrv_init);
module_exit(memdrv_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("High frequency visible memory read/write driver with sessions and batch ioctls");
MODULE_VERSION("3.0");
