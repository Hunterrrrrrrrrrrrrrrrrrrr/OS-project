#include "../../os/ktest/ktest.h"
#include "../lib/user.h"

// Base Checkpoint 1: sigaction, sigkill, and sigreturn

// send SIGUSR0 to a child process, which default action is to terminate it.
void basic1(char* s) {
    int pid = fork();
    if (pid == 0) {
        // child
        sleep(10);
        exit(1);
    } else {
        // parent
        sigkill(pid, SIGUSR0, 0);
        int ret;
        wait(0, &ret);
        assert(ret == -10 - SIGUSR0);
    }
}

// send SIGUSR0 to a child process, but should be ignored.
void basic2(char* s) {
    int pid = fork();
    if (pid == 0) {
        // child
        sigaction_t sa = {
            .sa_sigaction = SIG_IGN,
            .sa_mask      = 0,
            .sa_restorer  = NULL,
        };
        sigaction(SIGUSR0, &sa, 0);
        sleep(10);
        sleep(10);
        sleep(10);
        exit(1);
    } else {
        // parent
        sleep(5);
        sigkill(pid, SIGUSR0, 0);
        int ret;
        wait(0, &ret);
        assert(ret == 1);
    }
}

void handler3(int signo, siginfo_t* info, void* ctx2) {
    assert(signo == SIGUSR0);
    getpid();
    sleep(1);
    exit(103);
}

// set handler for SIGUSR0, which call exits to terminate the process.
//  this handler will not return, so sigreturn should not be called.
void basic3(char* s) {
    int pid = fork();
    if (pid == 0) {
        // child
        sigaction_t sa = {
            .sa_sigaction = handler3,
            .sa_restorer  = sigreturn,
        };
        sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR0, &sa, 0);
        while (1);
        exit(1);
    } else {
        // parent
        sleep(10);
        sigkill(pid, SIGUSR0, 0);
        int ret;
        wait(0, &ret);
        assert_eq(ret, 103);
    }
}

volatile int handler4_flag = 0;
void handler4(int signo, siginfo_t* info, void* ctx2) {
    assert(signo == SIGUSR0);
    sleep(1);
    sleep(1);
    fprintf(1, "handler4 triggered\n");
    handler4_flag = 1;
}

// set handler for SIGUSR0, and return from handler.
void basic4(char* s) {
    int pid = fork();
    if (pid == 0) {
        // child
        sigaction_t sa = {
            .sa_sigaction = handler4,
            .sa_restorer  = sigreturn,
        };
        sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR0, &sa, 0);
        while (handler4_flag == 0);
        exit(104);
    } else {
        // parent
        sleep(10);
        sigkill(pid, SIGUSR0, 0);
        int ret;
        wait(0, &ret);
        assert_eq(ret, 104);
    }
}

static volatile int handler5_cnt = 0;
void handler5(int signo, siginfo_t* info, void* ctx2) {
    assert(signo == SIGUSR0);
    static volatile int nonreentrace = 0;
    assert(!nonreentrace);  // non-reentrance
    nonreentrace = 1;
    sleep(5);
    sleep(5);
    if (handler5_cnt < 5)
        sigkill(getpid(), SIGUSR0, 0);
    sleep(5);
    sleep(5);
    fprintf(1, "handler5 triggered\n");
    nonreentrace = 0;
    handler5_cnt++;
}

// signal handler itself should not be reentrant.
//  when the signal handler is called for SIGUSR0, it should block all SIGUSR0.
//  after the signal handler returns, the signal should be unblocked.
//   then, the signal handler should be called again. (5 times)
// set handler for SIGUSR0, kernel should block it from re-entrance.
void basic5(char* s) {
    int pid = fork();
    if (pid == 0) {
        // child
        sigaction_t sa = {
            .sa_sigaction = handler5,
            .sa_restorer  = sigreturn,
        };
        sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR0, &sa, 0);
        while (handler5_cnt < 5);
        exit(105);
    } else {
        // parent
        sleep(10);
        sigkill(pid, SIGUSR0, 0);
        int ret;
        wait(0, &ret);
        assert_eq(ret, 105);
    }
}

volatile int handler6_flag = 0;
void handler6(int signo, siginfo_t* info, void* ctx2) {
    assert(signo == SIGUSR0);
    handler6_flag = 1;
    fprintf(1, "handler6 triggered due to %d\n", signo);
    sleep(30);
    assert(handler6_flag == 2);
    handler6_flag = 3;
}

void handler6_2(int signo, siginfo_t* info, void* ctx2) {
    assert(signo == SIGUSR1);
    assert(handler6_flag == 1);
    handler6_flag = 2;
    fprintf(1, "handler6_2 triggered due to %d\n", signo);
}

// signal handler can be nested.
void basic6(char* s) {
    int pid = fork();
    if (pid == 0) {
        // child
        sigaction_t sa = {
            .sa_sigaction = handler6,
            .sa_restorer  = sigreturn,
        };
        sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR0, &sa, 0);
        sigaction_t sa2 = {
            .sa_sigaction = handler6_2,
            .sa_restorer  = sigreturn,
        };
        sigemptyset(&sa2.sa_mask);
        sigaction(SIGUSR1, &sa2, 0);
        while (handler6_flag != 3);
        exit(106);
    } else {
        // parent
        sleep(10);
        sigkill(pid, SIGUSR0, 0);
        sleep(5);
        sigkill(pid, SIGUSR1, 0);
        sleep(5);
        int ret;
        wait(0, &ret);
        assert_eq(ret, 106);
    }
}

volatile int handler7_flag = 0;
void handler7(int signo, siginfo_t* info, void* ctx2) {
    assert(signo == SIGUSR0);
    handler7_flag = 1;
    fprintf(1, "handler7 triggered due to %d\n", signo);
    sleep(30);
    sigset_t pending;
    sigpending(&pending);
    assert_eq(pending, sigmask(SIGUSR1));
    assert(handler7_flag == 1);  // handler7 should not interrupted by SIGUSR1 (handler7_2)
    handler7_flag = 2;
}

void handler7_2(int signo, siginfo_t* info, void* ctx2) {
    assert(signo == SIGUSR1);
    assert(handler7_flag == 2);
    handler7_flag = 3;
    fprintf(1, "handler7_2 triggered due to %d\n", signo);
}

// signal handler can be nested.
void basic7(char* s) {
    int pid = fork();
    if (pid == 0) {
        // child
        sigaction_t sa = {
            .sa_sigaction = handler7,
            .sa_restorer  = sigreturn,
        };
        sigemptyset(&sa.sa_mask);
        sigaddset(&sa.sa_mask, SIGUSR1);  // block SIGUSR1 when handling SIGUSR0
        sigaction(SIGUSR0, &sa, 0);

        sigaction_t sa2 = {
            .sa_sigaction = handler7_2,
            .sa_restorer  = sigreturn,
        };
        sigemptyset(&sa2.sa_mask);
        sigaction(SIGUSR1, &sa2, 0);

        while (handler7_flag != 3);
        exit(107);
    } else {
        // parent
        sleep(10);
        sigkill(pid, SIGUSR0, 0);
        sleep(5);
        sigkill(pid, SIGUSR1, 0);
        sleep(5);
        int ret;
        wait(0, &ret);
        assert_eq(ret, 107);
    }
}

// SIG_IGN and SIG_DFL
void basic8(char* s) {
    int pid = fork();
    if (pid == 0) {
        // child
        sigaction_t sa = {
            .sa_sigaction = SIG_IGN,
            .sa_restorer  = NULL,
        };
        sigaction(SIGUSR0, &sa, 0);
        sigkill(getpid(), SIGUSR0, 0);  // should have no effect

        sigaction_t sa2 = {
            .sa_sigaction = SIG_DFL,
            .sa_restorer  = NULL,
        };
        sigaction(SIGUSR1, &sa2, 0);
        sigkill(getpid(), SIGUSR1, 0);  // should terminate the process

        exit(1);
    } else {
        // parent
        sigkill(pid, SIGUSR0, 0);
        int ret;
        wait(0, &ret);
        assert(ret == -10 - SIGUSR1);  // child terminated by SIGUSR1
    }
}

// Base Checkpoint 2: SIGKILL

void handler10(int signo, siginfo_t* info, void* ctx2) {
    exit(2);
}

// child process is killed by signal: SIGKILL, which cannot be handled, ignored and blocked.
void basic10(char* s) {
    int pid = fork();
    if (pid == 0) {
        // child
        sigaction_t sa = {
            .sa_sigaction = handler10,
            .sa_restorer  = NULL,
        };
        sigaction(SIGKILL, &sa, 0);
        // set handler for SIGKILL, which should not be called
        while (1);
        exit(1);
    } else {
        // parent
        sleep(20);
        sigkill(pid, SIGKILL, 0);
        int ret;
        wait(0, &ret);
        assert(ret == -10 - SIGKILL);
    }
}

// child process is killed by signal: SIGKILL, which cannot be handled, ignored and blocked.
void basic11(char* s) {
    int pid = fork();
    if (pid == 0) {
        // child
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGKILL);
        sigprocmask(SIG_BLOCK, &mask, NULL);
        // set handler for SIGKILL, which should not be called
        while (1);
        exit(1);
    } else {
        // parent
        sleep(20);
        sigkill(pid, SIGKILL, 0);
        int ret;
        wait(0, &ret);
        assert(ret == -10 - SIGKILL);
    }
}

// Base Checkpoint 3: signals under fork & exec

void basic20(char* s) {
    // our modification does not affect our parent process.
    // because `run` method in the testsuite will do fork for us.

    sigaction_t sa = {
        .sa_sigaction = SIG_IGN,
        .sa_restorer  = NULL,
    };
    sigaction(SIGUSR0, &sa, 0);
    // ignore SIGUSR0.

    int pid = fork();
    if (pid == 0) {
        // child
        sigkill(getpid(), SIGUSR0, 0);
        // should have no effect, because parent ignores it.
        exit(1);
    } else {
        // parent
        int ret;
        wait(0, &ret);
        assert(ret == 1);  // child should not be terminated by SIGUSR0
    }
}

volatile int stopcont_flag = 0;

void stopcont_handler(int signo, siginfo_t* info, void* ctx2) {
    if (signo == SIGUSR0) {
        fprintf(1, "Child process received SIGUSR0\n");
        stopcont_flag = 1;
    }
}

// 测试SIGSTOP和SIGCONT基本功能
void basic30(char* s) {
    int pid = fork();
    if (pid == 0) {
        // child
        // 设置SIGUSR0处理函数
        sigaction_t sa = {
            .sa_sigaction = stopcont_handler,
            .sa_restorer  = sigreturn,
        };
        sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR0, &sa, 0);

        fprintf(1, "Child process started\n");

        // 等待父进程发送SIGUSR0
        while (stopcont_flag == 0) {
            // fprintf(1, "waiting for SIGCONT");
            sleep(1);
        }

        fprintf(1, "Child process continuing after SIGUSR0\n");
        exit(130);
    } else {
        // parent
        sleep(5);

        fprintf(1, "Parent sending SIGSTOP to child\n");
        sigkill(pid, SIGSTOP, 0);

        // 检查子进程确实停止了
        sleep(5);

        // 2. 发送SIGUSR0，子进程应该收不到(因为它被停止了)
        fprintf(1, "Parent sending SIGUSR0 to stopped child\n");
        sigkill(pid, SIGUSR0, 0);
        sleep(2);

        // 3. 恢复子进程
        fprintf(1, "Parent sending SIGCONT to child\n");
        sigkill(pid, SIGCONT, 0);

        // 现在子进程应该能收到SIGUSR0并继续执行
        int ret_status;
        wait(0, &ret_status);
        assert_eq(ret_status, 130);

        fprintf(1, "SIGSTOP/SIGCONT test passed\n");
    }
}

// 用于SIGSEGV测试的全局变量
volatile int sigsegv_handled = 0;
volatile void* fault_address = NULL;

// SIGSEGV处理函数
void sigsegv_handler(int signo, siginfo_t* info, void* ctx) {
    fprintf(1, "SIGSEGV处理函数被调用，错误地址: %p\n", info->addr);

    // 打印更多调试信息
    fprintf(1, "信号编号: %d, 应该是: %d\n", info->si_signo, SIGSEGV);
    fprintf(1, "发送进程ID: %d\n", info->si_pid);
    fprintf(1, "信号代码: %d\n", info->si_code);

    // 验证信号信息
    assert(signo == SIGSEGV);
    assert(info->si_signo == SIGSEGV);

    // 记录错误地址用于后续验证
    fault_address = info->addr;

    // 修复寄存器状态，使程序能够继续执行
    struct ucontext* uc = (struct ucontext*)ctx;

    // 我们将epc向后移动4个字节，跳过引起异常的指令
    uc->uc_mcontext.epc += 4;

    sigsegv_handled = 1;
}

// 引发段错误的函数
void trigger_segfault() {
    int* bad_ptr = (int*)0x1000;  // 一个无效地址
    *bad_ptr     = 42;            // 这会触发SIGSEGV
    fprintf(1, "如果你看到这条消息，说明SIGSEGV处理成功\n");
}

// SIGSEGV测试
void basic40(char* s) {
    // 注册SIGSEGV处理函数
    sigaction_t sa = {
        .sa_sigaction = sigsegv_handler,
        .sa_restorer  = sigreturn,
    };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, 0);

    fprintf(1, "SIGSEGV处理函数注册成功\n");

    // 引发段错误
    fprintf(1, "故意引发段错误...\n");
    sigsegv_handled = 0;
    trigger_segfault();

    // 验证处理函数被调用
    assert(sigsegv_handled == 1);
    assert(fault_address != NULL);

    // 返回0，让测试框架认为测试成功
    exit(0);
}
