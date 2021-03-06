#include <linux/types.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/oom.h>
#include <linux/sched.h>
#include <linux/vmstat.h>
#include <linux/sysinfo.h>
#include <linux/swap.h>
#include <linux/cpu.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/cpumask.h>
#include <linux/cred.h>
#include <linux/rcupdate.h>
#include <linux/seq_file.h>
#include <asm/uaccess.h>

#ifdef MALI_SUPPORT
#include <mali_ukk.h>
#endif

#ifdef CONFIG_ZSMALLOC
#include <zsmalloc.h>
#endif

#ifdef CONFIG_ZRAM
#include <zram_drv.h>
#endif

#include "mlog_internal.h"

#define CONFIG_MLOG_BUF_SHIFT   16  /* 64KB */

#define P2K(x) (((u32)x) << (PAGE_SHIFT - 10))
#define B2K(x) (((u32)x) >> (10))


#define MLOG_STR_LEN        16
#define MLOG_BUF_LEN        ((1 << CONFIG_MLOG_BUF_SHIFT) >> 2)
#define MLOG_BUF_MASK       (mlog_buf_len-1)
#define MLOG_BUF(idx)       (mlog_buffer[(idx) & MLOG_BUF_MASK])

#define MLOG_ID             ULONG_MAX

#define AID_ROOT            0       /* traditional unix root user */
#define AID_SYSTEM          1000    /* system server */

#define M_MEMFREE           (1 << 0)
#define M_SWAPFREE          (1 << 1)
#define M_CACHED            (1 << 2)
#define M_GPUUSE            (1 << 3)
#define M_MLOCK             (1 << 4)
#define M_ZRAM              (1 << 5)

#define V_PSWPIN            (1 << 0)
#define V_PSWPOUT           (1 << 1)
#define V_PGFMFAULT         (1 << 2)

#define P_ADJ               (1 << 0)
#define P_RSS               (1 << 1)
#define P_RSWAP             (1 << 2)
#define P_SWPIN             (1 << 3)
#define P_SWPOUT            (1 << 4)
#define P_FMFAULT           (1 << 5)
#define P_MINFAULT          (1 << 6)
#define P_MAJFAULT          (1 << 7)

#define P_FMT_SIZE          (P_RSS | P_RSWAP)
#define P_FMT_COUNT         (P_SWPIN | P_SWPOUT | P_FMFAULT | P_MINFAULT | P_MAJFAULT)

#define M_FILTER_ALL        (M_MEMFREE | M_SWAPFREE | M_CACHED | M_GPUUSE | M_MLOCK | M_ZRAM)
#define V_FILTER_ALL        (V_PSWPIN | V_PSWPOUT | V_PGFMFAULT)
#define P_FILTER_ALL        (P_ADJ | P_RSS | P_RSWAP | P_SWPIN | P_SWPOUT | P_FMFAULT)

#define MLOG_TRIGGER_TIMER  0
#define MLOG_TRIGGER_LMK    1
#define MLOG_TRIGGER_LTK    2

extern void mlog_init_procfs(void);

static uint meminfo_filter = M_FILTER_ALL;
static uint vmstat_filter = V_FILTER_ALL;
static uint proc_filter = P_FILTER_ALL;

static DEFINE_SPINLOCK(mlogbuf_lock);
DECLARE_WAIT_QUEUE_HEAD(mlog_wait);
static long mlog_buffer[MLOG_BUF_LEN];
static int mlog_buf_len = MLOG_BUF_LEN;
static unsigned mlog_start;
static unsigned mlog_end;

static int min_adj = -16;
static int max_adj = 16;
static int limit_pid = -1;

static struct timer_list mlog_timer;
static unsigned long timer_intval = HZ;

static char **strfmt_list = NULL;
static int strfmt_idx = 0;
static int strfmt_len = 0;
static int strfmt_proc = 0;

static char cr_str[] = "%c";
static char type_str[] = "<%ld>";
static char time_sec_str[] = "[%5lu";
static char time_nanosec_str[] = ".%06lu]";
static char mem_size_str[] = " %6lu";
static char acc_count_str[] = " %7lu";
static char pid_str[] = " [%lu]";
static char adj_str[] = " %3ld";

static void mlog_emit_32(long v)
{
	MLOG_BUF(mlog_end) = v;
	mlog_end++;
	if (mlog_end - mlog_start > mlog_buf_len)
		mlog_start = mlog_end - mlog_buf_len;
}

static void mlog_emit_32_ex(long v)
{
    spin_lock_bh(&mlogbuf_lock);
    mlog_emit_32(v);
    spin_unlock_bh(&mlogbuf_lock);
}

/*
static void mlog_emit_64(long long v)
{
    mlog_emit_32(v >> BITS_PER_LONG);
    mlog_emit_32(v & ULONG_MAX);
}
*/

static void mlog_reset_format(void)
{
    int len;

    spin_lock_bh(&mlogbuf_lock);
    
    if (meminfo_filter)
        meminfo_filter = M_FILTER_ALL;
    if (vmstat_filter)
        vmstat_filter = V_FILTER_ALL;
    if (proc_filter)
        proc_filter = P_FILTER_ALL;

    /* calc len */
    len = 4; // id, type, sec, nanosec

    len += hweight32 (meminfo_filter);
    len += hweight32 (vmstat_filter);

    if (proc_filter)
    {
        len++; /* PID */        
        len += hweight32(proc_filter);
    }

    if (!strfmt_list || strfmt_len != len)
    {
        if (strfmt_list)
            kfree(strfmt_list);
        strfmt_list = kmalloc(sizeof(char *)*len, GFP_ATOMIC);
        strfmt_len = len;
        BUG_ON(!strfmt_list);
    }

    /* setup str format */
    len = 0;
    strfmt_proc = 0;
    strfmt_list[len++] = cr_str;
    strfmt_list[len++] = type_str;
    strfmt_list[len++] = time_sec_str;
    strfmt_list[len++] = time_nanosec_str;

    if (meminfo_filter)
    {
        int i;
        for (i = 0; i < hweight32(meminfo_filter); ++i)
            strfmt_list[len++] = mem_size_str;
    }

    if (vmstat_filter)
    {
        int i;
        for (i = 0; i < hweight32(vmstat_filter); ++i)
            strfmt_list[len++] = acc_count_str;
    }

    if (proc_filter)
    {
        int i;
        strfmt_proc = len;
        strfmt_list[len++] = pid_str;   /* PID */
        strfmt_list[len++] = adj_str;   /* ADJ */
        for (i = 0; i < hweight32(proc_filter & (P_FMT_SIZE)); ++i)
            strfmt_list[len++] = mem_size_str;
        for (i = 0; i < hweight32(proc_filter & (P_FMT_COUNT)); ++i)
            strfmt_list[len++] = acc_count_str;
    }
    strfmt_idx = 0;

    BUG_ON(len != strfmt_len);
    spin_unlock_bh(&mlogbuf_lock);

    MLOG_PRINTK("[mlog] reset format %d", strfmt_len);
    for (len = 0; len < strfmt_len; ++len)
    {
        MLOG_PRINTK(" %s", strfmt_list[len]);
    }
    MLOG_PRINTK("\n");
}

int mlog_print_fmt(struct seq_file *m)
{
    seq_printf(m, "<type>     [time]");

    if (meminfo_filter & M_MEMFREE)
        seq_printf(m, "  memfr");
    if (meminfo_filter & M_SWAPFREE)
        seq_printf(m, "  swpfr");
    if (meminfo_filter & M_CACHED)
        seq_printf(m, "  cache");
    if (meminfo_filter & M_GPUUSE)
        seq_printf(m, "    gpu");
    if (meminfo_filter & M_MLOCK)
        seq_printf(m, "  mlock");
    if (meminfo_filter & M_ZRAM)
        seq_printf(m, "   zram");
    
    if (vmstat_filter & V_PSWPIN)
        seq_printf(m, "   swpin");
    if (vmstat_filter & V_PSWPOUT)
        seq_printf(m, "  swpout");
    if (vmstat_filter & V_PGFMFAULT)
        seq_printf(m, "   fmflt");

    if (proc_filter) {
        seq_printf(m, " [pid]");
        if (proc_filter & P_ADJ)
            seq_printf(m, " adj");
        if (proc_filter & P_RSS)
            seq_printf(m, "    rss");
        if (proc_filter & P_RSWAP)
            seq_printf(m, "   rswp");
        if (proc_filter & P_SWPIN)
            seq_printf(m, "  pswpin");
        if (proc_filter & P_SWPOUT)
            seq_printf(m, " pswpout");
        if (proc_filter & P_FMFAULT)
            seq_printf(m, "  pfmflt");
    }
    seq_printf(m, "\n");
    return 0;
}

static void mlog_reset_buffer(void)
{
    spin_lock_bh(&mlogbuf_lock);
    mlog_end = mlog_start = 0;
    spin_unlock_bh(&mlogbuf_lock);

    MLOG_PRINTK("[mlog] reset buffer\n");
}

static void mlog_meminfo(void)
{
    unsigned long memfree;
    unsigned long swapfree;
    unsigned long cached;
    unsigned long gpuuse;
    unsigned long mlock;
    unsigned long zram;

    memfree = P2K(global_page_state(NR_FREE_PAGES));
    swapfree = P2K(nr_swap_pages);
	cached = P2K(global_page_state(NR_FILE_PAGES) -
    			 total_swapcache_pages);
#ifdef MALI_SUPPORT
    gpuuse = B2K(_mali_ukk_report_memory_usage());
#else
    gpuuse = 0;
#endif
    mlock = P2K(global_page_state(NR_MLOCK));
#if defined(CONFIG_ZRAM) & defined(CONFIG_ZSMALLOC)
    zram = (zram_devices && zram_devices->init_done) ?
        B2K(zs_get_total_size_bytes(zram_devices->mem_pool)) :
        0;
#else
    zram = 0;
#endif

    spin_lock_bh(&mlogbuf_lock);
    mlog_emit_32(memfree);
    mlog_emit_32(swapfree);
    mlog_emit_32(cached);
    mlog_emit_32(gpuuse);
    mlog_emit_32(mlock);
    mlog_emit_32(zram);
    spin_unlock_bh(&mlogbuf_lock);
}

static void mlog_vmstat(void)
{
    int cpu;
	unsigned long v[NR_VM_EVENT_ITEMS];

    memset(v, 0, NR_VM_EVENT_ITEMS * sizeof(unsigned long));

	for_each_online_cpu(cpu) {
		struct vm_event_state *this = &per_cpu(vm_event_states, cpu);
        v[PSWPIN] += this->event[PSWPIN];
        v[PSWPOUT] += this->event[PSWPOUT];
        v[PGFMFAULT] += this->event[PGFMFAULT];
	}

    spin_lock_bh(&mlogbuf_lock);
    mlog_emit_32(v[PSWPIN]);
    mlog_emit_32(v[PSWPOUT]);
    mlog_emit_32(v[PGFMFAULT]);
    spin_unlock_bh(&mlogbuf_lock);
}

struct task_struct *find_trylock_task_mm(struct task_struct *t)
{
	if (spin_trylock(&t->alloc_lock))
    {
    	if (likely(t->mm))
    		return t;
    	task_unlock(t);
    }
	return NULL;
}

static void mlog_procinfo(void)
{
    struct task_struct *tsk;

    rcu_read_lock();
    for_each_process(tsk) {
        int oom_score_adj;
        const struct cred *cred;
        struct task_struct *real_parent;
        struct task_struct *p;
        pid_t ppid;
        struct task_struct *t;
        unsigned long swap_in, swap_out, fm_flt, min_flt, maj_flt;
        unsigned long rss;
        unsigned long rswap;

        if (tsk->flags & PF_KTHREAD)
        	continue;

		p = find_trylock_task_mm(tsk);
		if (!p)
			continue;

        if (!p->signal)
            goto unlock_continue;

        oom_score_adj = p->signal->oom_adj;

        if (max_adj < oom_score_adj || oom_score_adj < min_adj)
            goto unlock_continue;

        if (limit_pid != -1 && p->pid != limit_pid)
            goto unlock_continue;

        cred = get_task_cred(p);
        if (!cred)
            goto unlock_continue;

        /* skip root user */
        if (cred->uid == AID_ROOT)
            goto unlock_continue;

        real_parent = rcu_dereference(p->real_parent);
        if (!real_parent)
            goto unlock_continue;

        ppid = real_parent->pid;
        /* skip non java proc (parent is init) */
        if (ppid == 1)
            goto unlock_continue;

        if (oom_score_adj == -16)
        {
            /* only keep system server */
            if (cred->uid != AID_SYSTEM)
                goto unlock_continue;
        }

        /* reset data */
        swap_in = swap_out = fm_flt = min_flt = maj_flt = 0;

        /* all threads */
        t = p;
		do {
			//min_flt += t->min_flt;
			//maj_flt += t->maj_flt;
#ifdef CONFIG_ZRAM
			fm_flt += t->fm_flt;
			swap_in += t->swap_in;
			swap_out += t->swap_out;
#endif
			t = next_thread(t);
		} while (t != p);

        /* emit log */
        rss = P2K(get_mm_rss(p->mm));
        rswap = P2K(get_mm_counter(p->mm, MM_SWAPENTS));

        spin_lock_bh(&mlogbuf_lock);
        mlog_emit_32(p->pid);
        mlog_emit_32(oom_score_adj);
        mlog_emit_32(rss);
        mlog_emit_32(rswap);
        mlog_emit_32(swap_in);
        mlog_emit_32(swap_out);
        mlog_emit_32(fm_flt);
        //mlog_emit_32(min_flt);
        //mlog_emit_32(maj_flt);
        spin_unlock_bh(&mlogbuf_lock);

unlock_continue:
        task_unlock(p);
    }
    rcu_read_unlock();

}

void mlog(int type)
{
    unsigned long flag;
    unsigned long microsec_rem;
    unsigned long long t = local_clock();
#ifdef PROFILE_MLOG_OVERHEAD
    unsigned long long t1 = t;
#endif
    //MLOG_PRINTK("[mlog] log %d %d %d\n", meminfo_filter, vmstat_filter, proc_filter);

    /* time stamp */
	microsec_rem = do_div(t, 1000000000);

    //spin_lock_irqsave(&mlogbuf_lock, flag);

    spin_lock_bh(&mlogbuf_lock);
    mlog_emit_32(MLOG_ID);      // tag for correct start point
    mlog_emit_32(type);
    mlog_emit_32((unsigned long) t);
    mlog_emit_32(microsec_rem / 1000);
    spin_unlock_bh(&mlogbuf_lock);

    /* memory log */
    if (meminfo_filter)
        mlog_meminfo();
    if (vmstat_filter)
        mlog_vmstat();
    if (proc_filter)
        mlog_procinfo();

    //spin_unlock_irqrestore(&mlogbuf_lock, flag);

    if (waitqueue_active(&mlog_wait))      
        wake_up_interruptible(&mlog_wait);

#ifdef PROFILE_MLOG_OVERHEAD
    MLOG_PRINTK("[mlog] %llu ns\n", local_clock() - t1);
#endif
}
EXPORT_SYMBOL(mlog);

void mlog_doopen()
{
    spin_lock_bh(&mlogbuf_lock);
    strfmt_idx = 0;
    spin_unlock_bh(&mlogbuf_lock);
}

int mlog_unread(void)
{
    return mlog_end - mlog_start;
}

int mlog_doread(char __user *buf, size_t len)
{
    unsigned i;
    int error = -EINVAL;
    char mlog_str[MLOG_STR_LEN];

    if (!buf || len < 0)
        goto out;
    error = 0;
    if (!len)
        goto out;
    if (!access_ok(VERIFY_WRITE, buf, len)) {
        error = -EFAULT;
        goto out;
    }

    //MLOG_PRINTK("[mlog] wait %d %d\n", mlog_start, mlog_end);
    error = wait_event_interruptible(mlog_wait, (mlog_start - mlog_end));
    if (error)
        goto out;
    i = 0;
    spin_lock_bh(&mlogbuf_lock);

    //MLOG_PRINTK("[mlog] doread %d %d\n", mlog_start, mlog_end);
    while (!error && (mlog_start != mlog_end) && i < len - MLOG_STR_LEN) {
        int size;
        int v;

        /* retrieve value */
        v = MLOG_BUF(mlog_start);
        mlog_start++;

        if (unlikely((v == MLOG_ID) ^ (strfmt_idx == 0)))
        {
            /* find first valid log */
            if (strfmt_idx == 0)
            {
                continue;
            }
            strfmt_idx = 0;
        }
        if (strfmt_idx == 0)
        {
            v = '\n';
        }
        //MLOG_PRINTK("[mlog] %d: %s\n", strfmt_idx, strfmt_list[strfmt_idx]);
        size = sprintf(mlog_str, strfmt_list[strfmt_idx++], v);

        if (strfmt_idx >= strfmt_len)
            strfmt_idx = strfmt_proc;

        spin_unlock_bh(&mlogbuf_lock);
        if (__copy_to_user(buf, mlog_str, size))
    	    error = -EFAULT;
        else
        {
            buf += size;
            i += size;
        }

        cond_resched();
        spin_lock_bh(&mlogbuf_lock);
    }

    spin_unlock_bh(&mlogbuf_lock);
    if (!error)
        error = i;
out:
    //MLOG_PRINTK("[mlog] doread end %d\n", error);
    return error;
}


static void mlog_timer_handler(void)
{
    mlog(MLOG_TRIGGER_TIMER);

    mod_timer(&mlog_timer, jiffies + timer_intval);
}

static void mlog_init_logger(void)
{
    spin_lock_init(&mlogbuf_lock);
    mlog_reset_format();
    mlog_reset_buffer();
    
    setup_timer(&mlog_timer, mlog_timer_handler, 0);
    mlog_timer.expires = jiffies + timer_intval;

    add_timer(&mlog_timer);
}

static void mlog_exit_logger(void)
{
    if (strfmt_list)
    {
        kfree(strfmt_list);
        strfmt_list = NULL;
    }
}

static int __init mlog_init(void)
{
	int err, i;
    mlog_init_logger();
    mlog_init_procfs();
    return 0;
}

static void __exit mlog_exit(void)
{
    mlog_exit_logger();
}

module_param(min_adj, int, S_IRUGO | S_IWUSR);
module_param(max_adj, int, S_IRUGO | S_IWUSR);
module_param(limit_pid, int, S_IRUGO | S_IWUSR);

static int do_filter_handler(const char *val, const struct kernel_param *kp)
{
    const int ret = param_set_uint(val, kp);
    mlog_reset_format();
    mlog_reset_buffer();
    return ret;
}

static const struct kernel_param_ops param_ops_change_filter = {
    .set = &do_filter_handler,
    .get = &param_get_uint,
    .free = NULL,
};

static int do_time_intval_handler(const char *val, const struct kernel_param *kp)
{
    const int ret = param_set_uint(val, kp);
    mod_timer(&mlog_timer, jiffies + ret);
    return ret;
}

static const struct kernel_param_ops param_ops_change_time_intval = {
    .set = &do_time_intval_handler,
    .get = &param_get_uint,
    .free = NULL,
};

param_check_uint(meminfo_filter, &meminfo_filter);
module_param_cb(meminfo_filter, &param_ops_change_filter, &meminfo_filter, S_IRUGO | S_IWUSR);
__MODULE_PARM_TYPE(meminfo_filter, uint);

param_check_uint(vmstat_filter, &vmstat_filter);
module_param_cb(vmstat_filter, &param_ops_change_filter, &vmstat_filter, S_IRUGO | S_IWUSR);
__MODULE_PARM_TYPE(vmstat_filter, uint);

param_check_uint(proc_filter, &proc_filter);
module_param_cb(proc_filter, &param_ops_change_filter, &proc_filter, S_IRUGO | S_IWUSR);
__MODULE_PARM_TYPE(proc_filter, uint);

param_check_ulong(timer_intval, &timer_intval);
module_param_cb(timer_intval, &param_ops_change_time_intval, &timer_intval, S_IRUGO | S_IWUSR);
__MODULE_PARM_TYPE(timer_intval, ulong);

static uint do_mlog;

static int do_mlog_handler(const char *val, const struct kernel_param *kp)
{
    const int ret = param_set_uint(val, kp);
    mlog(do_mlog);
    //MLOG_PRINTK("[mlog] do_mlog %d\n", do_mlog);
    return ret;
}

static const struct kernel_param_ops param_ops_do_mlog = {
    .set = &do_mlog_handler,
    .get = &param_get_uint,
    .free = NULL,
};

param_check_uint(do_mlog, &do_mlog);
module_param_cb(do_mlog, &param_ops_do_mlog, &do_mlog, S_IRUGO | S_IWUSR);
__MODULE_PARM_TYPE(do_mlog, uint);

module_init(mlog_init);
module_exit(mlog_exit);

//TODO module license & information

MODULE_DESCRIPTION("MEDIATEK Memory Log Driver");
MODULE_AUTHOR("Jimmy Su<jimmy.su@mediatek.com>");
MODULE_LICENSE("GPL");


