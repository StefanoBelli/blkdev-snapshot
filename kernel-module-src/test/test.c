#include <linux/blkdev.h>
#include <bdsnap/bdsnap.h>
#include <pr-err-failure.h>

static struct delayed_work gdwork;
static atomic_t sector_idx = ATOMIC_INIT(0);

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

	int sek = atomic_read(&sector_idx);

	if(bdsnap_make_snapshot(handle, datablock, sek, sizeof(datablock), cpu_flags)) {
		pr_warn("%s: did a snapshot of sector %d\n", module_name(THIS_MODULE), sek);
		atomic_inc(&sector_idx);
	} else {
		pr_warn("%s: bdsnap_make_snapshot goes brrrr\n", module_name(THIS_MODULE));
	}

__my_hrtimer_callback_finish1:
	rcu_read_unlock();
	bdev_fput(f_bd);
__my_hrtimer_callback_finish0:
	schedule_delayed_work(to_delayed_work(work), msecs_to_jiffies(5000));
}

/*
#include <lru-ng.h>


static bool test_lru_ng_fill(void) {
	bool __rv = false;

	struct lru_ng *lru = lru_ng_alloc_and_init();
	for(int i = 0; i < LRU_NG__LRU_MAX_ENTRIES; i++) {
		printk("adding key %d to LRU, res = %d\n", i, lru_ng_add(lru, i));
	}

	for(int i = 0; i < LRU_NG__LRU_MAX_ENTRIES; i++) {
		bool res = lru_ng_lookup(lru, i);
		printk("checking for key %d... %s (expecting present in LRU)\n", i, res ? "ok" :"**FAIL**");
		if(!res) {
			pr_err("stopping now due to test failure...\n");
			goto __test_lru_ng_fill_failure;
		}
	}

	for(int i = LRU_NG__LRU_MAX_ENTRIES; i < 2 * LRU_NG__LRU_MAX_ENTRIES; i++) {
		bool res = lru_ng_lookup(lru, i);
		printk("checking for key %d... %s (expecting *NOT* present in LRU)\n", i, !res ? "ok" :"**FAIL**");
		if(res) {
			pr_err("stopping now due to test failure...\n");
			goto __test_lru_ng_fill_failure;
		}
	}

	__rv = true;

__test_lru_ng_fill_failure:
	lru_ng_cleanup_and_destroy(lru);
	return __rv;
}

static bool test_lru_evict(void) {
	bool __rv = false;

	struct lru_ng *lru = lru_ng_alloc_and_init();
	for(int i = 0; i < LRU_NG__LRU_MAX_ENTRIES; i++) {
		printk("adding key %d to lru, res = %d\n", i, lru_ng_add(lru, i));
	}

	for(int i = LRU_NG__LRU_MAX_ENTRIES; i < 2 * LRU_NG__LRU_MAX_ENTRIES; i++) {
		printk("adding key %d to lru, res = %d\n", i, lru_ng_add(lru, i));
		int lookkey = i - LRU_NG__LRU_MAX_ENTRIES;
		bool res = lru_ng_lookup(lru, lookkey);
		printk("checking for key %d... %s (expecting *NOT* present in LRU)\n", lookkey, !res ? "ok" :"**FAIL**");
		if(res) {
			pr_err("stopping now due to test failure...\n");
			goto __test_lru_evict_failure;
		}
	}

	for(int i = LRU_NG__LRU_MAX_ENTRIES; i < 2 * LRU_NG__LRU_MAX_ENTRIES; i++) {
		bool res = lru_ng_lookup(lru, i);
		printk("checking for key %d... %s (expecting present in LRU)\n", i, res ? "ok" :"**FAIL**");
		if(!res) {
			pr_err("stopping now due to test failure...\n");
			goto __test_lru_evict_failure;
		}
	}

	bool res = lru_ng_lookup(lru, 2 * LRU_NG__LRU_MAX_ENTRIES);
	printk("checking for key %d... %s (expecting *NOT* present in LRU)\n", 2 * LRU_NG__LRU_MAX_ENTRIES, !res ? "ok" :"**FAIL**");
	if(res) {
		pr_err("stopping now due to test failure...\n");
		goto __test_lru_evict_failure;
	}

	__rv = true;

__test_lru_evict_failure:
	lru_ng_cleanup_and_destroy(lru);
	return __rv;
}

static bool test_lru_enforce_size_limit(void) {
	bool __rv = false;

	struct lru_ng *lru = lru_ng_alloc_and_init();
	for(int i = 0; i < LRU_NG__LRU_MAX_ENTRIES; i++) {
		printk("adding key %d to lru, res = %d\n", i, lru_ng_add(lru, i));
	}

	lru_ng_add(lru, LRU_NG__LRU_MAX_ENTRIES);
	lru_ng_add(lru, LRU_NG__LRU_MAX_ENTRIES + 1);

	bool res = lru_ng_lookup(lru, 0);
	printk("checking for key %d... %s (expecting *NOT* present in LRU)\n", 0, !res ? "ok" :"**FAIL**");
	if(res) {
		pr_err("stopping now due to test failure...\n");
		goto __test_lru_evict_failure;
	}

	res = lru_ng_lookup(lru, 1);
	printk("checking for key %d... %s (expecting *NOT* present in LRU)\n", 1, !res ? "ok" :"**FAIL**");
	if(res) {
		pr_err("stopping now due to test failure...\n");
		goto __test_lru_evict_failure;
	}

	__rv = true;

__test_lru_evict_failure:
	lru_ng_cleanup_and_destroy(lru);
	return __rv;
}

static bool test_lru_mru_lru(void) {
	bool __rv = false;

	struct lru_ng *lru = lru_ng_alloc_and_init();
	for(int i = 0; i < LRU_NG__LRU_MAX_ENTRIES; i++) {
		printk("adding key %d to lru, res = %d\n", i, lru_ng_add(lru, i));
	}

	for(int i = 0; i < LRU_NG__LRU_MAX_ENTRIES; i++) {
		printk("testing %d\n", lru_ng_add(lru, i + LRU_NG__LRU_MAX_ENTRIES));
		bool res = lru_ng_lookup(lru, i);
		printk("checking for key %d... %s (expecting *NOT* present in LRU)\n", i, !res ? "ok" :"**FAIL**");
		if(res) {
			pr_err("stopping now due to test failure...\n");
			goto __test_lru_mru_failure;
		}
	}

	__rv = true;

__test_lru_mru_failure:
	lru_ng_cleanup_and_destroy(lru);
	return __rv;
}
*/
void setup_test(void) {
	INIT_DELAYED_WORK(&gdwork, workcb);
    schedule_delayed_work(&gdwork, msecs_to_jiffies(5000));

    /*
    if(!test_lru_mru_lru()) {
    	pr_err("test lru mru lru failed\n");
    	return;
    }

    if(!test_lru_enforce_size_limit()) {
    	pr_err("test lru enforce size limit failed\n");
    	return;
    }

    if(!test_lru_ng_fill()) {
    	pr_err("test lru ng fill failed\n");
    	return;
    }

    if(!test_lru_evict()) {
    	pr_err("test lru evict failed\n");
    	return;
    }*/



    //pr_info("all lru tests passed\n");
}

void destroy_test(void) {

}
