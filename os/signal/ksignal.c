#include "ksignal.h"

#include <defs.h>
#include <proc.h>
#include <string.h>
#include <trap.h>

#include "signal.h"

void setup_signal_handler(struct proc *p, int signo, sigaction_t *act);

/**
 * @brief init the signal struct inside a PCB.
 *
 * @param p
 * @return int
 */
int siginit(struct proc *p) {
    // memset(&p->signal, 0, sizeof(struct ksignal));
    // // 默认所有信号 handler 为 SIG_DFL
    // for (int i = SIGMIN; i <= SIGMAX; i++) {
    //     p->signal.sa[i].sa_sigaction = SIG_DFL;
    //     p->signal.sa[i].sa_mask      = 0;
    //     p->signal.sa[i].sa_restorer  = 0;
    // }

    for (int signo = SIGMIN; signo <= SIGMAX; signo++) {
        p->signal.sa[signo].sa_sigaction = SIG_DFL;
        sigemptyset(&p->signal.sa[signo].sa_mask);
        p->signal.sa[signo].sa_restorer = NULL;
        
        // 初始化siginfo为0
        memset(&p->signal.siginfos[signo], 0, sizeof(siginfo_t));
        p->signal.siginfos[signo].si_signo = signo; // 设置信号编号
    }

    // 清空信号掩码和待处理信号集
    sigemptyset(&p->signal.sigmask);
    sigemptyset(&p->signal.sigpending);

    return 0;
}

int siginit_fork(struct proc *parent, struct proc *child) {
    // // 复制 handler 和 mask，但 pending 清零
    // memcpy(&child->signal.sa, &parent->signal.sa, sizeof(parent->signal.sa));
    // child->signal.sigmask    = parent->signal.sigmask;
    // child->signal.sigpending = 0;
    // 1. 复制父进程的所有信号处理方式
    for (int signo = SIGMIN; signo <= SIGMAX; signo++) {
        child->signal.sa[signo] = parent->signal.sa[signo];
    }

    // 2. 继承父进程的信号屏蔽字
    child->signal.sigmask = parent->signal.sigmask;

    // 3. 清空子进程的待处理信号集（不继承父进程的pending信号）
    sigemptyset(&child->signal.sigpending);

    return 0;
}

int siginit_exec(struct proc *p) {
    // mask/pending 清零，handler 除 SIG_IGN 外全部设为 SIG_DFL
    for (int signo = SIGMIN; signo <= SIGMAX; signo++) {
        // 强制SIGKILL/SIGSTOP为默认行为
        if (signo == SIGKILL || signo == SIGSTOP) {
            p->signal.sa[signo].sa_sigaction = SIG_DFL;
            sigemptyset(&p->signal.sa[signo].sa_mask);
            p->signal.sa[signo].sa_restorer = NULL;
            continue;
        }

        // 仅保留显式设置为SIG_IGN的信号
        if (p->signal.sa[signo].sa_sigaction != SIG_IGN) {
            p->signal.sa[signo].sa_sigaction = SIG_DFL;
            sigemptyset(&p->signal.sa[signo].sa_mask);
            p->signal.sa[signo].sa_restorer = NULL;
            
            // 清除该信号的siginfo（可选但更安全）
            memset(&p->signal.siginfos[signo], 0, sizeof(siginfo_t));
            p->signal.siginfos[signo].si_signo = signo;
        }
    }

    // 注意：sigmask和sigpending保持不变
    return 0;
}

int do_signal(void) {
    struct proc *p = curr_proc();
    // 1~SIGMAX，优先级从小到大
    for (int signo = SIGMIN; signo <= SIGMAX; signo++) {
        uint64 mask = sigmask(signo);
        // 有pending且未被mask
        if ((p->signal.sigpending & mask) && !(p->signal.sigmask & mask)) {
            sigaction_t *act = &p->signal.sa[signo];
            // SIGKILL/SIGSTOP不能被捕获/忽略
            if (signo == SIGKILL || signo == SIGSTOP) {
                setkilled(p, -10 - signo);
                p->signal.sigpending &= ~mask;
                return 1;
            }
            // 忽略
            if (act->sa_sigaction == SIG_IGN) {
                p->signal.sigpending &= ~mask;
                continue;
            }
            // 默认
            if (act->sa_sigaction == SIG_DFL) {
                setkilled(p, -10 - signo);
                p->signal.sigpending &= ~mask;
                return 1;
            }
            // 用户自定义 handler
            // 1. 保存上下文到用户栈
            // 2. 构造 siginfo_t/ucontext
            // 3. 设置 trapframe 跳转到 handler
            // 4. 更新 mask
            // 5. 清 pending
            setup_signal_handler(p, signo, act);
            p->signal.sigpending &= ~mask;
            return 1;
        }
    }
    return 0;
}

// 保存当前上下文，构造 siginfo_t 和 ucontext_t，设置 trapframe 跳转到 handler
void setup_signal_handler(struct proc *p, int signo, sigaction_t *act) {
    struct trapframe *tf = p->trapframe;
    uint64 sp            = tf->sp;

    // 1. 分配 ucontext
    sp -= sizeof(struct ucontext);
    sp &= ~15ULL;
    uint64 uctx_ptr = sp;

    // 2. 分配 siginfo
    sp -= sizeof(siginfo_t);
    sp &= ~15ULL;
    uint64 siginfo_ptr = sp;

    siginfo_t info;
    memset(&info, 0, sizeof(siginfo_t));
    acquire(&p->mm->lock);
    copy_to_user(p->mm, siginfo_ptr, (char *)&info, sizeof(siginfo_t));
    release(&p->mm->lock);

    struct ucontext uctx;
    memset(&uctx, 0, sizeof(struct ucontext));
    uctx.uc_sigmask      = p->signal.sigmask;
    uctx.uc_mcontext.epc = tf->epc;
    for (int i = 0; i < 31; i++) {
        uctx.uc_mcontext.regs[i] = ((uint64 *)&tf->ra)[i];
    }
    acquire(&p->mm->lock);
    copy_to_user(p->mm, uctx_ptr, (char *)&uctx, sizeof(struct ucontext));
    release(&p->mm->lock);

    // 3. 设置 trapframe 跳转到 handler
    tf->sp  = sp;
    tf->a0  = signo;
    tf->a1  = siginfo_ptr;
    tf->a2  = uctx_ptr;
    tf->epc = (uint64)act->sa_sigaction;
    tf->ra  = (uint64)act->sa_restorer;  // handler返回后执行 sigreturn

    // 4. 更新 mask（阻塞 sa_mask 和当前信号）
    p->signal.sigmask |= act->sa_mask;
    p->signal.sigmask |= sigmask(signo);
}

// syscall handlers:
//  sys_* functions are called by syscall.c

int sys_sigaction(int signo, const sigaction_t __user *act, sigaction_t __user *oldact) {
    struct proc *p = curr_proc();
    if (signo < SIGMIN || signo > SIGMAX)
        return -1;

    sigaction_t *cur = &p->signal.sa[signo];
    sigaction_t tmp;

    // 1. 备份旧的
    if (oldact) {
        memmove(&tmp, cur, sizeof(sigaction_t));
        acquire(&p->mm->lock);
        if (copy_to_user(p->mm, (uint64)oldact, (char *)&tmp, sizeof(sigaction_t)) < 0) {
            release(&p->mm->lock);
            return -1;
        }
        release(&p->mm->lock);
    }

    // 2. 设置新的
    if (act) {
        acquire(&p->mm->lock);
        if (copy_from_user(p->mm, (char *)&tmp, (uint64)act, sizeof(sigaction_t)) < 0) {
            release(&p->mm->lock);
            return -1;
        }
        release(&p->mm->lock);
        // SIGKILL/SIGSTOP 只能设为默认或忽略
        if ((signo == SIGKILL || signo == SIGSTOP) && (tmp.sa_sigaction != SIG_DFL && tmp.sa_sigaction != SIG_IGN))
            return -1;
        memmove(cur, &tmp, sizeof(sigaction_t));
    }
    return 0;
}

int sys_sigreturn() {
    struct proc *p       = curr_proc();
    struct trapframe *tf = p->trapframe;
    uint64 sp            = tf->sp;

    // sp 指向 siginfo
    sp += sizeof(siginfo_t);
    sp = (sp + 15ULL) & ~15ULL;
    // 现在 sp 指向 ucontext
    struct ucontext uctx;
    acquire(&p->mm->lock);
    if (copy_from_user(p->mm, (char *)&uctx, sp, sizeof(struct ucontext)) < 0) {
        release(&p->mm->lock);
        return -1;
    }
    release(&p->mm->lock);

    tf->epc = uctx.uc_mcontext.epc;
    for (int i = 0; i < 31; i++) {
        ((uint64 *)&tf->ra)[i] = uctx.uc_mcontext.regs[i];
    }
    p->signal.sigmask = uctx.uc_sigmask;

    return 0;
}

int sys_sigprocmask(int how, const sigset_t __user *set, sigset_t __user *oldset) {
    struct proc *p = curr_proc();
    sigset_t tmp;

    // 返回当前 mask
    if (oldset) {
        acquire(&p->mm->lock);
        if (copy_to_user(p->mm, (uint64)oldset, (char *)&p->signal.sigmask, sizeof(sigset_t)) < 0) {
            release(&p->mm->lock);
            return -1;
        }
        release(&p->mm->lock);
    }

    // 设置新 mask
    if (set) {
        acquire(&p->mm->lock);
        if (copy_from_user(p->mm, (char *)&tmp, (uint64)set, sizeof(sigset_t)) < 0) {
            release(&p->mm->lock);
            return -1;
        }
        release(&p->mm->lock);

        switch (how) {
            case SIG_BLOCK:
                p->signal.sigmask |= tmp;
                break;
            case SIG_UNBLOCK:
                p->signal.sigmask &= ~tmp;
                break;
            case SIG_SETMASK:
                p->signal.sigmask = tmp;
                break;
            default:
                return -1;
        }
    }
    return 0;
}

int sys_sigpending(sigset_t __user *set) {
    struct proc *p = curr_proc();
    if (!set)
        return -1;
    acquire(&p->mm->lock);
    int ret = copy_to_user(p->mm, (uint64)set, (char *)&p->signal.sigpending, sizeof(sigset_t));
    release(&p->mm->lock);
    return ret < 0 ? -1 : 0;
}

int sys_sigkill(int pid, int signo, int code) {
    // 1. 参数校验
    if (signo < SIGMIN || signo > SIGMAX) 
        return -1;
    
    // 2. 查找目标进程
    struct proc *p = NULL;
    
    // 正确遍历指针数组的方式
    for (int i = 0; pool[i] != NULL; i++) {  // 假设以NULL结尾
        if (pool[i]->pid == pid && pool[i]->state != UNUSED) {
            p = pool[i];
            break;
        }
    }
    
    if (!p) return -1;  // 未找到进程

    // 3. 处理特殊信号
    if (signo == SIGKILL) {
        setkilled(p, -10 - SIGKILL);
        return 0;
    }

    // 4. 添加信号到pending集
    sigaddset(&p->signal.sigpending, signo);
    
    // 5. 填充siginfo
    p->signal.siginfos[signo].si_signo = signo;
    p->signal.siginfos[signo].si_code = code;
    p->signal.siginfos[signo].si_pid = curr_proc()->pid;

    // 6. 唤醒睡眠进程
    if (p->state == SLEEPING && p->sleep_chan) {
        wakeup(p->sleep_chan);
    }

    return 0;
}
