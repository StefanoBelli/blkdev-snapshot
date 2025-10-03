#include <linux/blkdev.h>

#include <bdsnap/bdsnap.h>
#include <pr-err-failure.h>

static struct delayed_work gdwork;

static void workcb( struct work_struct *work) {
	pr_warn("%s: testing now...\n", module_name(THIS_MODULE));

	struct file* f_bd = bdev_file_open_by_path("/dev/loop0", BLK_OPEN_READ, NULL, NULL);
	if(IS_ERR(f_bd)) {
		pr_err_failure_with_code("bdev_file_open_by_path", PTR_ERR(f_bd));
		goto __my_hrtimer_callback_finish0;
	}

	struct block_device *bd = file_bdev(f_bd);

	rcu_read_lock();

	unsigned long cpu_flags;
	void *handle = bdsnap_search_device(bd, &cpu_flags);
	if(handle == NULL) {
		pr_warn("%s: device could not be found\n", module_name(THIS_MODULE));
		goto __my_hrtimer_callback_finish1;
	}

	char datablock[] = { 'c','i','a','o' };

	if(bdsnap_make_snapshot(handle, datablock, 0, sizeof(datablock), cpu_flags)) {
		pr_warn("%s: did a snapshot!\n", module_name(THIS_MODULE));
	} else {
		pr_warn("%s: bdsnap_make_snapshot goes brrrr\n", module_name(THIS_MODULE));
	}

__my_hrtimer_callback_finish1:
	rcu_read_unlock();
	bdev_fput(f_bd);
__my_hrtimer_callback_finish0:
	schedule_delayed_work(to_delayed_work(work), msecs_to_jiffies(3000));
}

void setup_test(void) {
	INIT_DELAYED_WORK(&gdwork, workcb);
    schedule_delayed_work(&gdwork, msecs_to_jiffies(5000));
}

void destroy_test(void) {

}
