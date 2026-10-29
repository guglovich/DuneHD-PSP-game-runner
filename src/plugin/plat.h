/*
 * Объявления слоя системных вызовов для нативного плагина.
 *
 * Здесь перечислено только то, что плагин вызывает напрямую. Функции с
 * обычными именами (`mmap`, `malloc`, `snprintf` и прочие) объявлены в
 * заголовках musl — специально, чтобы подписи совпадали с реализацией из
 * `plat.c` и с тем, что ожидает `guest_load.c`. Свои подписи здесь
 * заводить нельзя: на этой цели `size_t` — `unsigned int`, и расхождение
 * даёт две несовместимые сущности с одним именем.
 */

#ifndef PLAT_H
#define PLAT_H

#include <stddef.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

long plat_syscall(long n, long a0, long a1, long a2, long a3);
long plat_syscall6(long n, long a0, long a1, long a2, long a3,
                   long a4, long a5);

int   plat_getpid(void);
int   plat_write(int fd, const void *buf, size_t len);
int   plat_read(int fd, void *buf, size_t len);
int   plat_open_read(const char *path);
int   plat_open_write(const char *path);

/* Жив ли поток с данным tid. */
int   plat_thread_alive(int tid);

/* Номер потока текущего. */
int   plat_gettid(void);
long  plat_file_size(int fd);
void  plat_flush_icache(void *addr, size_t len);
void *plat_mmap_file(void *addr, size_t len, int prot, int flags, int fd);

/* Поток, разделяющий адресное пространство с вызывающим. */
long plat_clone(void (*fn)(void *), void *arg, unsigned long stack_top,
                unsigned long flags);

#define PLAT_CLONE_THREAD_FLAGS                                            \
	(0x00000100u /* CLONE_VM      */ | 0x00000200u /* CLONE_FS      */ |  \
	 0x00000400u /* CLONE_FILES   */ | 0x00000800u /* CLONE_SIGHAND */ |  \
	 0x00010000u /* CLONE_THREAD  */ | 0x00040000u /* CLONE_SYSVSEM */)

void plat_sleep_ms(unsigned long ms);

/* Завершает текущий поток сырым `exit`. */
void plat_thread_exit(void);

/* Обработчик сигнала поверх сырого `rt_sigaction`. */
int plat_signal_raw(int sig, void *handler, unsigned long flags);
int plat_signal_reset(int sig);

int plat_vformat(char *buf, size_t cap, const char *fmt,
                 __builtin_va_list ap);

/* --- дополнительные системные вызовы для IoFileMgr ------------------- */

int open(const char *path, int flags, ...);
int close(int fd);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
off_t lseek(int fd, off_t offset, int whence);
int stat(const char *path, struct stat *buf);
int fstat(int fd, struct stat *buf);
int close(int fd);
int unlink(const char *path);
int snprintf(char *buf, size_t size, const char *format, ...);
int vsnprintf(char *buf, size_t size, const char *format, __builtin_va_list ap);

#endif /* PLAT_H */
