#include <linux/miscdevice.h>
#include <linux/dma-mapping.h>
#include <linux/poll.h>
#include "xiaomi_touch_common.h"
#define KNOCK_RAW_DATA_SIZE     PAGE_SIZE
#define KNOCK_NODE_NAME         "xiaomi-touch-knock"
struct knock_data {
	struct miscdevice misc_dev;
	unsigned int *raw_data;
	int raw_data_length;
	dma_addr_t phy_base;
	int need_frame_count;
	bool has_data_update;
	bool registered;
	bool live;
	unsigned long generation;
	void (*need_frame_count_change_listener)(int);
};
static struct knock_data knock_data;
static DEFINE_MUTEX(knock_lock);
/* This queue outlives old poll fds, including a later registration. */
static DECLARE_WAIT_QUEUE_HEAD(knock_wait);

static bool knock_file_live(struct file *file)
{
	return knock_data.live && file->private_data &&
		*(unsigned long *)file->private_data == knock_data.generation;
}

void knock_data_notify(void)
{
	wake_up_interruptible(&knock_wait);
}
EXPORT_SYMBOL_GPL(knock_data_notify);

void update_knock_data(u8 *buf, int size, int frame_id)
{
	if (!buf || size < 0 || size > KNOCK_RAW_DATA_SIZE)
		return;
	/* No producer is wired in this tree; a future producer must be sleepable. */
	mutex_lock(&knock_lock);
	if (knock_data.live && knock_data.raw_data) {
		memcpy(knock_data.raw_data, buf, size);
		knock_data.raw_data_length = size;
		knock_data.has_data_update = true;
	}
	mutex_unlock(&knock_lock);
}
EXPORT_SYMBOL_GPL(update_knock_data);

void register_frame_count_change_listener(void *listener)
{
	mutex_lock(&knock_lock);
	knock_data.need_frame_count_change_listener = listener;
	mutex_unlock(&knock_lock);
}
EXPORT_SYMBOL_GPL(register_frame_count_change_listener);

static int knock_data_open(struct inode *inode, struct file *file)
{
	unsigned long *generation;
	int ret = 0;

	generation = kzalloc(sizeof(*generation), GFP_KERNEL);
	if (!generation)
		return -ENOMEM;
	mutex_lock(&knock_lock);
	if (!knock_data.live) {
		kfree(generation);
		ret = -ENODEV;
	} else {
		*generation = knock_data.generation;
		file->private_data = generation;
	}
	mutex_unlock(&knock_lock);
	return ret;
}

static int knock_data_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	file->private_data = NULL;
	return 0;
}

static ssize_t knock_data_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
	char temp_buf[16];
	int length;
	ssize_t ret = -ENODEV;

	mutex_lock(&knock_lock);
	if (knock_file_live(file)) {
		length = scnprintf(temp_buf, sizeof(temp_buf), "%d\n", knock_data.need_frame_count);
		ret = simple_read_from_buffer(buf, count, pos, temp_buf, length);
	}
	mutex_unlock(&knock_lock);
	return ret;
}

static ssize_t knock_data_write(struct file *file, const char __user *buf, size_t count, loff_t *pos)
{
	char temp_buf[6] = { 0 };
	int frame_count = 0;
	size_t size = min(count, sizeof(temp_buf) - 1);
	ssize_t ret = -ENODEV;

	mutex_lock(&knock_lock);
	if (!knock_file_live(file))
		goto out;
	/* Preserve the legacy copy-failure contract: publish zero, return count. */
	if (!copy_from_user(temp_buf, buf, size))
		sscanf(temp_buf, "%d", &frame_count);
	knock_data.need_frame_count = frame_count;
	if (knock_data.need_frame_count_change_listener)
		knock_data.need_frame_count_change_listener(frame_count);
	/* Release drains this call before common pdata can be withdrawn. */
	thp_send_cmd_to_hal(THP_KNOCK_FRAME_COUNT, frame_count);
	ret = count;
out:
	mutex_unlock(&knock_lock);
	return ret;
}

static long knock_data_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	long ret = -ENODEV;

	mutex_lock(&knock_lock);
	if (knock_file_live(file)) {
		if (!cmd) {
			knock_data.has_data_update = true;
			knock_data_notify();
		}
		ret = 0;
	}
	mutex_unlock(&knock_lock);
	return ret;
}

static unsigned int knock_data_poll(struct file *file, poll_table *wait)
{
	unsigned int mask = 0;

	poll_wait(file, &knock_wait, wait);
	mutex_lock(&knock_lock);
	if (!knock_file_live(file))
		mask = POLLHUP;
	else if (knock_data.has_data_update) {
		mask = POLLIN | POLLRDNORM;
		knock_data.has_data_update = false;
	}
	mutex_unlock(&knock_lock);
	return mask;
}

static int knock_data_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long size = vma->vm_end - vma->vm_start;
	int ret = -ENODEV;

	/* Exactly one backing page: reject pgoff before any shift/add can wrap. */
	if (vma->vm_pgoff || !size || size > KNOCK_RAW_DATA_SIZE)
		return -EINVAL;
	mutex_lock(&knock_lock);
	if (knock_file_live(file) && knock_data.raw_data) {
		ret = remap_pfn_range(vma, vma->vm_start,
				     knock_data.phy_base >> PAGE_SHIFT, size, PAGE_SHARED);
		if (ret)
			ret = -EAGAIN;
	}
	mutex_unlock(&knock_lock);
	/* VMA backing references (including clones) are the next batch. */
	return ret;
}

static const struct file_operations xiaomi_knock_fops = {
	.owner = THIS_MODULE,
	.open = knock_data_open,
	.release = knock_data_release,
	.poll = knock_data_poll,
	.mmap = knock_data_mmap,
	.read = knock_data_read,
	.write = knock_data_write,
	.unlocked_ioctl = knock_data_ioctl,
};

int knock_node_init(void)
{
	int ret;

	/* Common probe/remove serialize registration. No entry is live yet. */
	knock_data.raw_data = kzalloc(KNOCK_RAW_DATA_SIZE, GFP_KERNEL);
	if (!knock_data.raw_data)
		return -ENOMEM;
	knock_data.phy_base = virt_to_phys(knock_data.raw_data);
	knock_data.misc_dev.minor = MISC_DYNAMIC_MINOR;
	knock_data.misc_dev.name = KNOCK_NODE_NAME;
	knock_data.misc_dev.fops = &xiaomi_knock_fops;
	ret = misc_register(&knock_data.misc_dev);
	if (ret) {
		kfree(knock_data.raw_data);
		knock_data.raw_data = NULL;
		return ret;
	}
	knock_data.registered = true;
	return 0;
}

void knock_node_publish(void)
{
	mutex_lock(&knock_lock);
	knock_data.generation++;
	knock_data.need_frame_count = 0;
	knock_data.raw_data_length = 0;
	knock_data.has_data_update = false;
	knock_data.live = true;
	mutex_unlock(&knock_lock);
}

void knock_node_release(void)
{
	mutex_lock(&knock_lock);
	knock_data.live = false;
	knock_data.need_frame_count_change_listener = NULL;
	knock_data_notify();
	mutex_unlock(&knock_lock);
	/* misc open holds misc_mtx before knock_lock; deregister outside it. */
	if (knock_data.registered) {
		misc_deregister(&knock_data.misc_dev);
		knock_data.registered = false;
	}
	mutex_lock(&knock_lock);
	kfree(knock_data.raw_data);
	knock_data.raw_data = NULL;
	knock_data.phy_base = 0;
	mutex_unlock(&knock_lock);
}
