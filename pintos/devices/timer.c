#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"

/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

/* Number of timer ticks since OS booted. */
static int64_t ticks;


/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;


//함수 선언, 변수 선언 아님, intr_handler_func 가 함수이기 때문
static intr_handler_func timer_interrupt;


static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);


// @잠든 스레드를 관리할 리스트
static struct list sleep_list;

/* Sets up the 8254 Programmable Interval Timer (PIT) to
   interrupt PIT_FREQ times per second, and registers the
   corresponding interrupt. */

//@초기화 함수 타이머 인터럽트가 왔을때 처리할 행동을 등록함
void
timer_init (void) {
	/* 8254 input frequency divided by TIMER_FREQ, rounded to
	   nearest. */
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

	outb (0x43, 0x34);    /* CW: counter 0, LSB then MSB, mode 2, binary. */
	outb (0x40, count & 0xff);
	outb (0x40, count >> 8);
	//잠든 스레드 리스트를 초기화 하는 함수
	list_init (&sleep_list);

	//0x20 (타이머에 대한 인터럽트 벡터 번호), 0x20 신호가 왔을때 timer_interrupt 함수를 실행하라고 등록
	intr_register_ext(0x20, timer_interrupt, "8254 Timer");
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void) {
	unsigned high_bit, test_bit;

	ASSERT (intr_get_level () == INTR_ON);
	printf ("Calibrating timer...  ");

	/* Approximate loops_per_tick as the largest power-of-two
	   still less than one timer tick. */
	loops_per_tick = 1u << 10;
	while (!too_many_loops (loops_per_tick << 1)) {
		loops_per_tick <<= 1;
		ASSERT (loops_per_tick != 0);
	}

	/* Refine the next 8 bits of loops_per_tick. */
	high_bit = loops_per_tick;
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
		if (!too_many_loops (high_bit | test_bit))
			loops_per_tick |= test_bit;

	printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
//@현제 ticks 를 구하는 함수, ticks 를 가져올때 인터럽트가 실행되면 안되므로 인터럽트를 끈 후 ticks 를 가져온다.
int64_t
timer_ticks (void) {
	//인터럽트를 끕니다, 타이머 인터럽트가 끼어들 수 없음, intr_disable은 끄기 전 상태 반환
	//enum intr_level은 INTR_ON과 INTR_OFF 두 값 중 하나를 가지는 타입입니다.
	enum intr_level old_level = intr_disable ();
	//t에 ticks 복사, 인터럽트를 껐으므로 변할 수 없음
	int64_t t = ticks;
	intr_set_level(old_level);
	barrier(); //수정 안하고 계속 쓰는 변수는 레지스터에 추가하기 때문에 베리어로 강제로 메모리를 참조하도록 바꿈
	return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed(int64_t then) {
	return timer_ticks() - then;
}

//@함수를 선언
static list_less_func wakeup_less;

//@우선순위를 결정하는 함수
bool wakeup_less (const struct list_elem *a, const struct list_elem *b, void *aux)
{
	struct thread *ta = list_entry (a, struct thread, elem);
	struct thread *tb = list_entry (b, struct thread, elem);
	return ta->wakeup_tick < tb->wakeup_tick;
}

//@현제 스레드를 특정 틱만큼 기다리도록 설정
/* Suspends execution for approximately TICKS timer ticks. */
void timer_sleep (int64_t ticks) {
	if (ticks == 0 || ticks < 0) return;
	int64_t start = timer_ticks ();					//start 를 현제 틱으로 설정

	enum intr_level old_level = intr_disable ();		//인터럽트 비활성화, 이유는 타이머 핸들러 (timer_interrupt) 에서도 sleep_list 를 사용하기 때문에, 경쟁 상태 방지
	struct thread * cur = thread_current(); 			//현제 스레드 
	cur->wakeup_tick = ticks + start;                    // 언제 깨울지 기록
	list_insert_ordered(&sleep_list, &cur->elem, wakeup_less, NULL); 		// 나를 찾을 수 있게 등록
	thread_block ();                                   // 그 다음에 잠들기
	intr_set_level (old_level);                        // 깨어나면 여기부터 실행

	ASSERT (intr_get_level () == INTR_ON);
	while (timer_elapsed(start) < ticks)
		thread_yield();
}

/* Suspends execution for approximately MS milliseconds. */
void
timer_msleep (int64_t ms) {
	real_time_sleep (ms, 1000);
}

/* Suspends execution for approximately US microseconds. */
void
timer_usleep (int64_t us) {
	real_time_sleep (us, 1000 * 1000);
}

/* Suspends execution for approximately NS nanoseconds. */
void
timer_nsleep (int64_t ns) {
	real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void) {
	printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* Timer interrupt handler. */
static void
timer_interrupt (struct intr_frame *args UNUSED) {
	ticks++;

	/* sleep_list는 wakeup_tick 오름차순이므로 앞에서부터만 확인 */
	while (!list_empty (&sleep_list)) {
		struct thread *t = list_entry (list_front (&sleep_list),struct thread, elem);
		if (t->wakeup_tick > ticks)
			break;                       // 맨 앞이 아직이면 뒤는 볼 필요 없음
		list_pop_front (&sleep_list);    // 먼저 sleep_list에서 빼고
		thread_unblock (t);              // 그다음 ready_list로
	}

	thread_tick ();
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops (unsigned loops) {
	/* Wait for a timer tick. */
	int64_t start = ticks;
	while (ticks == start)
		barrier ();

	/* Run LOOPS loops. */
	start = ticks;
	busy_wait (loops);

	/* If the tick count changed, we iterated too long. */
	barrier ();
	return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait (int64_t loops) {
	while (loops-- > 0)
		barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom) {
	/* Convert NUM/DENOM seconds into timer ticks, rounding down.

	   (NUM / DENOM) s
	   ---------------------- = NUM * TIMER_FREQ / DENOM ticks.
	   1 s / TIMER_FREQ ticks
	   */
	int64_t ticks = num * TIMER_FREQ / denom;

	ASSERT (intr_get_level () == INTR_ON);
	if (ticks > 0) {
		/* We're waiting for at least one full timer tick.  Use
		   timer_sleep() because it will yield the CPU to other
		   processes. */
		timer_sleep(ticks);
	} else {
		/* Otherwise, use a busy-wait loop for more accurate
		   sub-tick timing.  We scale the numerator and denominator
		   down by 1000 to avoid the possibility of overflow. */
		ASSERT (denom % 1000 == 0);
		busy_wait(loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
	}
}
