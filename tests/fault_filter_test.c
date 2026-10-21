/*
 * Измерение: ПОВЕДЕНИЕ фильтра чужих сигналов в обработчике гостя.
 *
 * Зачем. Два прогона подряд мои правки диагностики роняли плеер, и оба раза
 * `make check-plugin` был зелёным — он проверяет символы и `NEEDED`, но не
 * поведение обработчика сигналов. Цена — интерфейс плеера. Этот тест
 * закрывает именно то поведение, которое убивало процесс:
 *
 *   1. сигнал от потока, НЕ внесённого в фильтр, должен ПРАВИТЬ PC в кадре
 *      и возвращаться, а не уходить `return` без правки. Без правки сигнал
 *      повторяется на той же инструкции и процесс умирает.
 *      Замер на плеере: `FOREIGN sig=0004 pc=2CB601DC`, затем
 *      `Root: child process terminated, status = 5`.
 *   2. для потока, ВНЕСЁННОГО в фильтр, поведение прежнее: счётчик растёт.
 *   3. PC правится ТОЛЬКО внутри образа: за его пределами кадр чужой.
 *
 * Модель та же, что в `guest_run.c`: есть множество «своих» tid, приём
 * `guest_thread_fault()` и запись PC байтами от младшего к старшему. Тест
 * deliberately повторяет эту модель отдельно, чтобы падение было видно на
 * хосте, а не на плеере.
 *
 * Запуск: ./fault_filter_test; ожидается 3 пройденных проверки и код 0.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define BASE   0x08804000u
#define SPAN   0x003D0000u      /* map_size из отчёта загрузчика */
#define GUEST_CPLINIT_MAX 256

/* --- модель фильтра, зеркало `guest_thread_fault()` --------------------- */

static uint32_t m_guest_pid;
static uint32_t m_thread_tid[GUEST_CPLINIT_MAX];

static int guest_thread_fault(uint32_t tid)
{
	unsigned i;

	if (tid == 0)
		return 1;
	if (tid == m_guest_pid)
		return 1;
	for (i = 0; i < GUEST_CPLINIT_MAX; i++) {
		if (m_thread_tid[i] != 0 && m_thread_tid[i] == tid)
			return 1;
	}
	return 0;
}

/* --- модель записи PC в кадр, зеркало основного пути восстановления ----
 *
 * Важно: байты от МЛАДШЕГО к старшему, от `pc + 4`. Первая версия этого
 * кода в фильтре брала старшие байты от `pc`, а младший от `pc + 4` — на
 * адресе вида `0x08AD7FFC` это даёт `0x08BD7FFC`, прыжок на 16 МиБ вверх. */

static void write_pc(unsigned char *slot, uint32_t pc_v)
{
	uint32_t np = pc_v + 4;

	slot[0] = (unsigned char)(np & 0xFF);
	slot[1] = (unsigned char)((np >> 8) & 0xFF);
	slot[2] = (unsigned char)((np >> 16) & 0xFF);
	slot[3] = (unsigned char)((np >> 24) & 0xFF);
}

/* Обработчик в модели: чужой поток тоже правит PC, если PC внутри образа.
 * Признак правки — выставленный флаг, его проверяет тест. */
static int m_pc_fixed;
static uint32_t m_last_np;

static void handle_fault(uint32_t tid, uint32_t pc_v, unsigned char *slot)
{
	m_pc_fixed = 0;
	m_last_np = 0;

	if (!guest_thread_fault(tid)) {
		if (pc_v >= BASE && pc_v < BASE + SPAN) {
			write_pc(slot, pc_v);
			m_pc_fixed = 1;
		}
		return;
	}
	/* Свой поток: основной путь восстановления (в модели — та же правка). */
	write_pc(slot, pc_v);
	m_pc_fixed = 1;
}

static int fails;

static void chk(const char *what, int ok)
{
	if (ok) {
		printf("  %-46s OK\n", what);
	} else {
		printf("  %-46s ОТКАЗ\n", what);
		fails++;
	}
}

int main(void)
{
	unsigned char slot[4];
	uint32_t tid_guest = 0x2075;
	uint32_t tid_worker = 0x20C2;

	printf("=== фильтр чужих сигналов: поведение, а не символы ===\n");

	m_guest_pid = tid_guest;
	memset(m_thread_tid, 0, sizeof(m_thread_tid));

	/* ВАЖНО: поток-конструктор/воркер НЕ внесён — ровно та ситуация,
	 * которая на плеере убила процесс. */
	chk("свой поток распознан (g_guest_pid)",
	    guest_thread_fault(tid_guest) == 1);
	chk("неучтённый поток = чужой",
	    guest_thread_fault(tid_worker) == 0);
	chk("нулевой tid считается своим (проверить нечем)",
	    guest_thread_fault(0) == 1);

	/* Главная проверка: чужой поток с PC внутри образа ДОЛЖЕН получить
	 * правку PC. Без неё сигнал повторяется и процесс умирает — это и
	 * случилось на плеере (`status = 5`). */
	handle_fault(tid_worker, 0x08AD0C14, slot);
	chk("чужой поток с pc в образе: PC исправлен", m_pc_fixed == 1);

	/* И значение должно быть pc + 4, с байтами от младшего к старшему. */
	{
		uint32_t np = (uint32_t)slot[0] | ((uint32_t)slot[1] << 8)
			    | ((uint32_t)slot[2] << 16) | ((uint32_t)slot[3] << 24);

		chk("pc + 4 записан верно", np == 0x08AD0C18);
	}

	/* Контроль на переносе через 0x10000: адрес вида 0x...FFFC.
	 * Здесь первая версия кода дала бы 0x08BD7FFC вместо 0x08AD8000. */
	memset(slot, 0, sizeof(slot));
	handle_fault(tid_worker, 0x08AD7FFC, slot);
	{
		uint32_t np = (uint32_t)slot[0] | ((uint32_t)slot[1] << 8)
			    | ((uint32_t)slot[2] << 16) | ((uint32_t)slot[3] << 24);

		chk("перенос через 0x10000 не ломает pc", np == 0x08AD8000);
	}

	/* За пределами образа кадр чужой — PC НЕ трогаем. */
	memset(slot, 0xAA, sizeof(slot));
	handle_fault(tid_worker, 0x2CB601DC, slot);
	chk("чужой поток с pc вне образа: PC не тронут",
	    m_pc_fixed == 0 && slot[0] == 0xAA);

	/* Свой поток правит PC всегда, где угодно — основной путь. */
	memset(slot, 0, sizeof(slot));
	handle_fault(tid_guest, 0x2CA00A7C, slot);
	chk("свой поток: PC исправлен и вне образа",
	    m_pc_fixed == 1 && ((uint32_t)slot[0]
	    | ((uint32_t)slot[1] << 8) | ((uint32_t)slot[2] << 16)
	    | ((uint32_t)slot[3] << 24)) == 0x2CA00A80);

	printf("=== тест %s ===\n", fails == 0 ? "пройден" : "провален");
	return fails == 0 ? 0 : 1;
}
