#include <linux/spinlock.h>

#include <devices.h>
#include <epoch.h>

void epoch_count_mount(const struct mountinfo *minfo) {
	rcu_read_lock();

	struct object_data *data = get_device_data(minfo);
	if(data == NULL) {
		rcu_read_unlock();
		return;
	}

	unsigned long flags; //cpu-saved flags
	spin_lock_irqsave(&data->lock, flags);

	data->e.n_currently_mounted++;

	if(data->e.n_currently_mounted == 1) {
		//record current date
	}

	spin_unlock_irqrestore(&data->lock, flags);
	rcu_read_unlock();
}

void epoch_count_umount(const struct mountinfo *minfo) {
	rcu_read_lock();

	struct object_data *data = get_device_data(minfo);
	if(data == NULL) {
		rcu_read_unlock();
		return;
	}

	unsigned long flags; //cpu-saved flags
	spin_lock_irqsave(&data->lock, flags);

	data->e.n_currently_mounted--;

	if(data->e.n_currently_mounted < 0) {
		data->e.n_currently_mounted = 0;
	} else if(data->e.n_currently_mounted == 0) {
		epoch_destroy_cached_blocks_lru(data->e);
		epoch_destroy_d_snapdir_dentry(data->e);
	}

	spin_unlock_irqrestore(&data->lock, flags);
	rcu_read_unlock();
}
