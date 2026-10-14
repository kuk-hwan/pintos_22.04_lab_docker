#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#ifdef VM
#include "vm/vm.h"
#endif


/* States in a thread's life cycle. */
enum thread_status {
	THREAD_RUNNING,     /* Running thread. */
	THREAD_READY,       /* Not running but ready to run. */
	THREAD_BLOCKED,     /* Waiting for an event to trigger. */
	THREAD_DYING        /* About to be destroyed. */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */

/* A kernel thread or user process.
 *
 * Each thread structure is stored in its own 4 kB page.  The
 * thread structure itself sits at the very bottom of the page
 * (at offset 0).  The rest of the page is reserved for the
 * thread's kernel stack, which grows downward from the top of
 * the page (at offset 4 kB).  Here's an illustration:
 *
 *      4 kB +---------------------------------+
 *           |          kernel stack           |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         grows downward          |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * The upshot of this is twofold:
 *
 *    1. First, `struct thread' must not be allowed to grow too
 *       big.  If it does, then there will not be enough room for
 *       the kernel stack.  Our base `struct thread' is only a
 *       few bytes in size.  It probably should stay well under 1
 *       kB.
 *
 *    2. Second, kernel stacks must not be allowed to grow too
 *       large.  If a stack overflows, it will corrupt the thread
 *       state.  Thus, kernel functions should not allocate large
 *       structures or arrays as non-static local variables.  Use
 *       dynamic allocation with malloc() or palloc_get_page()
 *       instead.
 *
 * The first symptom of either of these problems will probably be
 * an assertion failure in thread_current(), which checks that
 * the `magic' member of the running thread's `struct thread' is
 * set to THREAD_MAGIC.  Stack overflow will normally change this
 * value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
 * the run queue (thread.c), or it can be an element in a
 * semaphore wait list (synch.c).  It can be used these two ways
 * only because they are mutually exclusive: only a thread in the
 * ready state is on the run queue, whereas only a thread in the
 * blocked state is on a semaphore wait list. */
/* Forward declaration */
struct lock;

struct thread {
    /* Thread 기본 정보 */
    tid_t tid;
    enum thread_status status;
    char name[16];
    int priority;

    /* Ready list 또는 semaphore 대기 리스트 */
    struct list_elem elem;

    /* Alarm Clock */
    int wakeup_tick;

    /* Priority Donation */
    int base_priority;
    struct lock *waiting_lock;
    struct list donations;
    struct list_elem donation_elem;

    /* MLFQS */
    int nice;
    int recent_cpu;
    struct list_elem all_elem;

#ifdef USERPROG
    /* User process */
    uint64_t *pml4;
#endif

#ifdef VM
    /* Virtual memory */
    struct supplemental_page_table spt;
#endif

    /* Context switching */
    struct intr_frame tf;

    /* Stack overflow detection */
    unsigned magic;
};

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

void do_iret (struct intr_frame *tf);

/* 17.14 fixed-point format (Pintos Appendix B.6).
   1 sign bit, 17 integer bits, 14 fractional bits. */
typedef int fixed_t;

#define FP_SHIFT 14
#define F (1 << FP_SHIFT)

/* 정수 n -> 고정소수점 */
static inline fixed_t int_to_fp (int n) { return n * F; }

/* 고정소수점 x -> 정수 (0 방향 버림) */
static inline int fp_to_int_zero (fixed_t x) { return x / F; }

/* 고정소수점 x -> 정수 (가장 가까운 정수로 반올림) */
static inline int fp_to_int_round (fixed_t x)
{
  return x >= 0 ? (x + F / 2) / F : (x - F / 2) / F;
}

/* 고정소수점 + 고정소수점 */
static inline fixed_t add_fp (fixed_t x, fixed_t y) { return x + y; }

/* 고정소수점 - 고정소수점 */
static inline fixed_t sub_fp (fixed_t x, fixed_t y) { return x - y; }

/* 고정소수점 + 정수 */
static inline fixed_t add_mixed (fixed_t x, int n) { return x + n * F; }

/* 고정소수점 - 정수 */
static inline fixed_t sub_mixed (fixed_t x, int n) { return x - n * F; }

/* 고정소수점 * 고정소수점 (오버플로 방지를 위해 64비트로 계산) */
static inline fixed_t mult_fp (fixed_t x, fixed_t y)
{
  return (fixed_t) (((int64_t) x) * y / F);
}

/* 고정소수점 * 정수 */
static inline fixed_t mult_mixed (fixed_t x, int n) { return x * n; }

/* 고정소수점 / 고정소수점 (오버플로 방지를 위해 64비트로 계산) */
static inline fixed_t div_fp (fixed_t x, fixed_t y)
{
  return (fixed_t) (((int64_t) x) * F / y);
}

/* 고정소수점 / 정수 */
static inline fixed_t div_mixed (fixed_t x, int n) { return x / n; }


#endif /* threads/thread.h */
