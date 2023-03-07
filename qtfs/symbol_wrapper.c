#include <linux/compiler_types.h>
#include <linux/syscalls.h>
#include <linux/trace_events.h>
#include <linux/mount.h>
#include <linux/file.h>
#include <linux/kprobes.h>
#include <asm/unistd.h>

#include "conn.h"
#include "symbol_wrapper.h"

static struct kprobe kp = {
	.symbol_name = "kallsyms_lookup_name"
};
struct pt_regs;

#ifdef __x86_64__
#define WRAPPER_ARGS_TO_REGS1(regs) regs->di = (unsigned long)x1;

#define WRAPPER_ARGS_TO_REGS2(regs) \
	WRAPPER_ARGS_TO_REGS1(regs)\
	regs->si = (unsigned long)x2;

#define WRAPPER_ARGS_TO_REGS3(regs) \
	WRAPPER_ARGS_TO_REGS2(regs)\
	regs->dx = (unsigned long)x3;

#define WRAPPER_ARGS_TO_REGS4(regs) \
	WRAPPER_ARGS_TO_REGS3(regs)\
	regs->r10 = (unsigned long)x4;

#define WRAPPER_ARGS_TO_REGS5(regs) \
	WRAPPER_ARGS_TO_REGS4(regs)\
	regs->r8 = (unsigned long)x5;

#define WRAPPER_ARGS_TO_REGS6(regs) \
	WRAPPER_ARGS_TO_REGS5(regs)\
	regs->r9 = (unsigned long)x6;
#endif
#ifdef __aarch64__
// symbols not finded in sys call table
enum qtfs_sym_a64 {
	A64_NR_UNLINK = 0,
	A64_NR_RMDIR,
	A64_NR_EPOLL_WAIT,
	A64_NR_MAX,
};
unsigned long *symbols_a64[A64_NR_MAX];

#define WRAPPER_ARGS_TO_REGS1(regs) regs->regs[0] = (unsigned long)x1;

#define WRAPPER_ARGS_TO_REGS2(regs)\
	WRAPPER_ARGS_TO_REGS1(regs)\
	regs->regs[1] = (unsigned long)x2;

#define WRAPPER_ARGS_TO_REGS3(regs)\
	WRAPPER_ARGS_TO_REGS2(regs)\
	regs->regs[2] = (unsigned long)x3;

#define WRAPPER_ARGS_TO_REGS4(regs)\
	WRAPPER_ARGS_TO_REGS3(regs)\
	regs->regs[3] = (unsigned long)x4;

#define WRAPPER_ARGS_TO_REGS5(regs)\
	WRAPPER_ARGS_TO_REGS4(regs)\
	regs->regs[4] = (unsigned long)x5;

#define WRAPPER_ARGS_TO_REGS6(regs)\
	WRAPPER_ARGS_TO_REGS5(regs)\
	regs->regs[5] = (unsigned long)x6;
#endif

#define KSYMS(sym, type) \
			qtfs_kern_syms.sym = (type)qtfs_kallsyms_lookup_name(#sym);\
			qtfs_info("qtfs kallsyms get %s:0x%lx.", #sym, (unsigned long)qtfs_kern_syms.sym);

#define KSYMS_NULL_RETURN(sym)\
		if (sym == NULL) {\
			qtfs_err("symbol:%s not finded", #sym);\
			return;\
		}

struct qtfs_kallsyms qtfs_kern_syms;
kallsyms_lookup_name_t qtfs_kallsyms_lookup_name;
void qtfs_kallsyms_hack_init(void)
{
	register_kprobe(&kp);
	qtfs_kallsyms_lookup_name = (kallsyms_lookup_name_t) kp.addr;
	unregister_kprobe(&kp);

	KSYMS(sys_call_table, unsigned long **);
	KSYMS_NULL_RETURN(qtfs_kern_syms.sys_call_table);

	KSYMS(d_absolute_path, char *(*)(const struct path *, char *, int));
	KSYMS_NULL_RETURN(qtfs_kern_syms.d_absolute_path);
	KSYMS(find_get_task_by_vpid, struct task_struct *(*)(pid_t nr));
	KSYMS_NULL_RETURN(qtfs_kern_syms.find_get_task_by_vpid);
	KSYMS(__close_fd, int (*)(struct files_struct *, int));
	KSYMS_NULL_RETURN(qtfs_kern_syms.__close_fd);

#ifdef __aarch64__
#pragma GCC diagnostic ignored "-Wint-conversion"
	symbols_a64[A64_NR_UNLINK] = (unsigned long)qtfs_kallsyms_lookup_name("__arm64_sys_unlink");
	KSYMS_NULL_RETURN(symbols_a64[A64_NR_UNLINK]);
	symbols_a64[A64_NR_RMDIR] = (unsigned long)qtfs_kallsyms_lookup_name("__arm64_sys_rmdir");
	KSYMS_NULL_RETURN(symbols_a64[A64_NR_RMDIR]);
	symbols_a64[A64_NR_EPOLL_WAIT] = (unsigned long)qtfs_kallsyms_lookup_name("__arm64_sys_epoll_wait");
	KSYMS_NULL_RETURN(symbols_a64[A64_NR_EPOLL_WAIT]);
#pragma GCC diagnostic pop
	qtfs_info("finded __arm64_sys_unlink addr:%lx", (unsigned long)symbols_a64[A64_NR_UNLINK]);
	qtfs_info("finded __arm64_sys_rmdir addr:%lx", (unsigned long)symbols_a64[A64_NR_RMDIR]);
	qtfs_info("finded __arm64_sys_epoll_wait addr:%lx", (unsigned long)symbols_a64[A64_NR_EPOLL_WAIT]);
#endif

	return;
}

#define SYSCALL_TYPE (long (*)(const struct pt_regs *))

#ifdef QTFS_CLIENT
unsigned long *symbols_origin[SYMBOL_MAX_NUM];
#define WRAPPER_DEFINE_CLIENT(nargs, ret, func, nr)\
	noinline ret func\
	{\
		struct pt_regs _regs;\
		struct pt_regs *regs = &_regs;\
		int retval;\
		WRAPPER_ARGS_TO_REGS##nargs(regs);\
		retval = (SYSCALL_TYPE symbols_origin[nr])(regs);\
		return retval;\
	}
WRAPPER_DEFINE_CLIENT(2, long, qtfs_syscall_umount(char __user *x1, int x2), SYMBOL_SYSCALL_UMOUNT);
WRAPPER_DEFINE_CLIENT(5, long, qtfs_syscall_mount(char __user *x1, char __user *x2,
			char __user *x3, unsigned long x4, void __user *x5), SYMBOL_SYSCALL_MOUNT);
WRAPPER_DEFINE_CLIENT(4, long, qtfs_syscall_epoll_ctl(int x1, int x2, int x3, 
			struct epoll_event __user *x4), SYMBOL_SYSCALL_EPOLL_CTL);
#endif

#define CALL_SYSCALL_FN(nr, regs) (SYSCALL_TYPE qtfs_kern_syms.sys_call_table[nr])(regs);\
			qtfs_info("qtfs syscall wrapper:%s ret:%d addr:%lx", __func__, retval, (unsigned long)qtfs_kern_syms.sys_call_table[nr]);

#define WRAPPER_DEFINE(nargs, ret, func, nr)\
	noinline ret func\
	{\
		struct pt_regs _regs;\
		struct pt_regs *regs = &_regs;\
		int retval;\
		WRAPPER_ARGS_TO_REGS##nargs(regs);\
		retval = (SYSCALL_TYPE qtfs_kern_syms.sys_call_table[nr])(regs);\
		return retval;\
	}

#ifdef QTFS_SERVER
WRAPPER_DEFINE(2, long, qtfs_syscall_umount(char __user *x1, int x2), __NR_umount2);
WRAPPER_DEFINE(5, long, qtfs_syscall_mount(char __user *x1, char __user *x2,
		char __user *x3, unsigned long x4, void __user *x5), __NR_mount);
WRAPPER_DEFINE(4, long, qtfs_syscall_epoll_ctl(int x1, int x2, int x3, 
			struct epoll_event __user *x4), __NR_epoll_ctl);
WRAPPER_DEFINE(4, int, qtfs_syscall_readlinkat(int x1, const char __user *x2, 
			char __user *x3, int x4), __NR_readlinkat);
WRAPPER_DEFINE(5, int, qtfs_syscall_renameat2(int x1, const char __user *x2,
			int x3, const char __user *x4, unsigned int x5), __NR_renameat2);
WRAPPER_DEFINE(3, long, qtfs_syscall_mkdirat(int x1, const char __user *x2, 
			umode_t x3), __NR_mkdirat);
WRAPPER_DEFINE(2, int, qtfs_syscall_statfs(const char __user *x1, 
			struct statfs __user *x2), __NR_statfs);
WRAPPER_DEFINE(4, int, qtfs_syscall_openat(int x1, const char __user *x2, int x3,
			umode_t x4), __NR_openat);
WRAPPER_DEFINE(5, int, qtfs_syscall_linkat(int x1, const char __user *x2,
			int x3, const char __user *x4, int x5), __NR_linkat);
WRAPPER_DEFINE(4, long, qtfs_syscall_mknodat(int x1, const char __user *x2, umode_t x3,
			unsigned int x4), __NR_mknodat);
WRAPPER_DEFINE(3, off_t, qtfs_syscall_lseek(unsigned int x1, off_t x2, 
			unsigned int x3), __NR_lseek);

#ifdef __aarch64__
#define WRAPPER_DEFINE_A64(nargs, ret, func, nr)\
	noinline ret func\
	{\
		struct pt_regs _regs;\
		struct pt_regs *regs = &_regs;\
		int retval;\
		WRAPPER_ARGS_TO_REGS##nargs(regs);\
		retval = (SYSCALL_TYPE symbols_a64[nr])(regs);\
		return retval;\
	}

// ARM64 syscall not finded in sys_call_table is defined here
WRAPPER_DEFINE_A64(1, long, qtfs_syscall_unlink(const char __user *x1), A64_NR_UNLINK);
WRAPPER_DEFINE_A64(4, int, qtfs_syscall_epoll_wait(int x1, struct epoll_event __user *x2,
			int x3, int x4), A64_NR_EPOLL_WAIT);
WRAPPER_DEFINE_A64(1, long, qtfs_syscall_rmdir(const char __user *x1), A64_NR_RMDIR);
#else
WRAPPER_DEFINE(1, long, qtfs_syscall_unlink(const char __user *x1), __NR_unlink);
WRAPPER_DEFINE(4, int, qtfs_syscall_epoll_wait(int x1, struct epoll_event __user *x2,
			int x3, int x4), __NR_epoll_wait);
WRAPPER_DEFINE(1, long, qtfs_syscall_rmdir(const char __user *x1), __NR_rmdir);
#endif
#endif


