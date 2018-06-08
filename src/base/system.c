/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <signal.h>

#include "system.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <aes128/aes.h>
#include <md5/md5.h>
#include <zlib/zlib.h>

#if defined(CONF_WEBSOCKETS)
	#include "engine/shared/websockets.h"
#endif

#if defined(CONF_FAMILY_UNIX)
	#include <sys/time.h>
	#include <unistd.h>

	/* unix net includes */
	#include <sys/socket.h>
	#include <sys/ioctl.h>
	#include <errno.h>
	#include <netdb.h>
	#include <netinet/in.h>
	#include <fcntl.h>
	#include <pthread.h>
	#include <arpa/inet.h>

	#include <dirent.h>

#if defined(CONF_PLATFORM_MACOSX)
	// some lock and pthread functions are already defined in headers
	// included from Carbon.h
	// this prevents having duplicate definitions of those
	#define _lock_set_user_
	#define _task_user_

	#include <Carbon/Carbon.h>
	#include <mach/mach_time.h>
#endif

#if defined(__ANDROID__)
	#include <android/log.h>
#endif

#elif defined(CONF_FAMILY_WINDOWS)
	#define WIN32_LEAN_AND_MEAN
	#undef _WIN32_WINNT
	#define _WIN32_WINNT 0x0501 /* required for mingw to get getaddrinfo to work */
	#include <windows.h>
	#include <winsock2.h>
	#include <ws2tcpip.h>
	#include <fcntl.h>
	#include <direct.h>
	#include <errno.h>
	#include <process.h>
	#include <shellapi.h>
	#include <wincrypt.h>
#else
	#error NOT IMPLEMENTED
#endif

#if defined(CONF_PLATFORM_SOLARIS)
	#include <sys/filio.h>
#endif

#if defined(__cplusplus)
extern "C" {
#endif

#ifdef FUZZING
static unsigned char gs_NetData[1024];
static int gs_NetPosition = 0;
static int gs_NetSize = 0;
#endif

IOHANDLE io_stdin() { return (IOHANDLE)stdin; }
IOHANDLE io_stdout() { return (IOHANDLE)stdout; }
IOHANDLE io_stderr() { return (IOHANDLE)stderr; }

static DBG_LOGGER loggers[16];
static int num_loggers = 0;

static NETSTATS network_stats = {0};
static MEMSTATS memory_stats = {0};

static NETSOCKET invalid_socket = {NETTYPE_INVALID, -1, -1};

#define AF_WEBSOCKET_INET (0xee)

static int abort_on_assert = 0;
static void dbg_abort();
void set_abort_on_assert(int enabled)
{
	abort_on_assert = enabled;
}
void dbg_assert_imp(const char *filename, int line, int test, const char *msg)
{
	if(!test)
	{
		dbg_msg("assert", "%s(%d): %s", filename, line, msg);
		if(abort_on_assert)
//			dbg_abort();
//		else
			dbg_break();
	}
}
int dbg_assert_strict_imp(const char *filename, int line, int test, const char *msg)
{
	// strict debugging assertion: only perform the test if this is a debug build. Otherwise return adequately.
	#if defined(CONF_DEBUG)
		dbg_assert_imp(filename, line, test, msg);
	#endif
	return !test;
}

#if !defined(CONF_PLATFORM_MACOSX)
#define QUEUE_SIZE 16
static int dbg_msg_threaded = 0;
typedef struct
{
	char q[QUEUE_SIZE][1024*4];
	int begin;
	int end;
	SEMAPHORE mutex;
	SEMAPHORE notempty;
	SEMAPHORE notfull;
} Queue;
static Queue log_queue;
int queue_empty(Queue *q);
#endif

static void dbg_abort()
{
	//*((volatile unsigned*)0) = 0x0;

	wait_log_queue();
	io_flush(io_stdout());
#if defined(CONF_FAMILY_WINDOWS)
	*((volatile unsigned*)0) = 0x0;
#else
	abort();
#endif
}

void dbg_break()
{
#if defined(CONF_FAMILY_WINDOWS)
	dbg_abort();
#else
	wait_log_queue();
	io_flush(io_stdout());
	#if defined(CONF_DEBUG)
		raise(SIGTRAP);
	#endif
#endif
}

void wait_log_queue()
{
	if(dbg_msg_threaded) // wait for all debug output to be flushed
		while(!queue_empty(&log_queue))
			thread_sleep(20);
}

#if !defined(CONF_PLATFORM_MACOSX)
int queue_empty(Queue *q)
{
	return q->begin == q->end;
}

int queue_full(Queue *q)
{
	return ((q->end+1) % QUEUE_SIZE) == q->begin;
}

void dbg_msg_thread(void *v)
{
	char str[1024*4];
	int i;
	int f;
	int num;
	while(1)
	{
		semaphore_wait(&log_queue.notempty);
		semaphore_wait(&log_queue.mutex);
		f = queue_full(&log_queue);

		str_copy(str, log_queue.q[log_queue.begin], sizeof(str));

		log_queue.begin = (log_queue.begin + 1) % QUEUE_SIZE;

		if(f)
			semaphore_signal(&log_queue.notfull);

		if(!queue_empty(&log_queue))
			semaphore_signal(&log_queue.notempty);

		num = num_loggers;
		semaphore_signal(&log_queue.mutex);

		for(i = 0; i < num; i++)
			loggers[i](str);
	}
}

void dbg_enable_threaded()
{
	Queue *q;
	void *Thread;

	q = &log_queue;
	q->begin = 0;
	q->end = 0;
	semaphore_init(&q->mutex);
	semaphore_init(&q->notempty);
	semaphore_init(&q->notfull);
	semaphore_signal(&q->mutex);
	semaphore_signal(&q->notfull);

	dbg_msg_threaded = 1;

	Thread = thread_init_named(dbg_msg_thread, 0, "dbg_msg worker");
	thread_detach(Thread);
}
#endif

static int s_DbgMsgDisabled = 0;

void set_dbg_msg_enabled(int enabled)
{
	s_DbgMsgDisabled = !enabled;
}

void dbg_msg(const char *sys, const char *fmt, ...)
{
	if(s_DbgMsgDisabled)
		return;

	va_list args;
	char *msg;
	int len;

	//str_format(str, sizeof(str), "[%08x][%s]: ", (int)time(0), sys);
	time_t rawtime;
	struct tm* timeinfo;
	char timestr [80];

	time ( &rawtime );
	timeinfo = localtime ( &rawtime );

	strftime (timestr,sizeof(timestr),"%y-%m-%d %H:%M:%S",timeinfo);

#if !defined(CONF_PLATFORM_MACOSX)
	if(dbg_msg_threaded)
	{
		int e;
		semaphore_wait(&log_queue.notfull);
		semaphore_wait(&log_queue.mutex);
		e = queue_empty(&log_queue);

		str_format(log_queue.q[log_queue.end], sizeof(log_queue.q[log_queue.end]), "[%s][%s]: ", timestr, sys);

		len = strlen(log_queue.q[log_queue.end]);
		msg = (char *)log_queue.q[log_queue.end] + len;

		va_start(args, fmt);
#if defined(CONF_FAMILY_WINDOWS)
		//see http://www.cplusplus.com/articles/2ywTURfi/

		if(str_comp_nocase(sys, "chat") == 0)
			SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), 15); // gamechat: white
		else if(str_comp_nocase(sys, "teamchat") == 0)
			SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), 2); // teamchat: green
		else if(str_comp_nocase(sys, "serv") == 0)
			SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), 6); // serverchat: yellow
		else if(str_find_nocase(sys, "error"))
			SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), 4); // errors: red
		else if(str_comp_nocase(sys, "irc") && str_find_nocase(msg, "chat]"))
			SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), 3); // irc-chat: blue
		else
			SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), 8); // everything else: gray

		_vsnprintf(msg, sizeof(log_queue.q[log_queue.end])-len, fmt, args);
#else
		// colored output
		if(str_comp(sys, "chat") == 0)
			printf("\033[0;%im", 36); // Cyan
		else if(str_comp(sys, "teamchat") == 0)
			printf("\033[0;%im", 32); // Green
		else if(str_comp(sys, "serv") == 0)
			printf("\033[0;%im", 33); // Yellow
		else if(str_find_nocase(sys, "warn"))
			printf("\033[0;%im", 93); // Bright Yellow
		else if(str_find_nocase(sys, "error"))
			printf("\033[0;%im", 91); // Bright Red
		else if(str_find_nocase(sys, ".lua"))
			printf("\033[0;%im", 37); // Bright White
		else
		{
#ifdef CONF_DEBUG
			printf("\033[0;%im", 0);
#else
			printf("\033[0;%im", 90); // Bright Black
#endif
		}

		vsnprintf(msg, sizeof(log_queue.q[log_queue.end])-len, fmt, args);
#endif
		va_end(args);

		log_queue.end = (log_queue.end + 1) % QUEUE_SIZE;

		if(e)
			semaphore_signal(&log_queue.notempty);

		if(!queue_full(&log_queue))
			semaphore_signal(&log_queue.notfull);

		semaphore_signal(&log_queue.mutex);
	}
	else
#endif
	{
		char str[1024*4];
		int i;

		str_format(str, sizeof(str), "[%s][%s]: ", timestr, sys);

		len = strlen(str);
		msg = (char *)str + len;

		va_start(args, fmt);
#if defined(CONF_FAMILY_WINDOWS)
		_vsnprintf(msg, sizeof(str)-len, fmt, args);
#else
		vsnprintf(msg, sizeof(str)-len, fmt, args);
#endif
		va_end(args);

		for(i = 0; i < num_loggers; i++)
			loggers[i](str);
	}
}

#if defined(CONF_FAMILY_WINDOWS)
static void logger_win_console(const char *line)
{
	#define _MAX_LENGTH 1024
	#define _MAX_LENGTH_ERROR (_MAX_LENGTH+32)

	static const int UNICODE_REPLACEMENT_CHAR = 0xfffd;

	static const char *STR_TOO_LONG = "(str too long)";
	static const char *INVALID_UTF8 = "(invalid utf8)";

	wchar_t wline[_MAX_LENGTH_ERROR];
	size_t len = 0;

	const char *read = line;
	const char *error = STR_TOO_LONG;
	while(len < _MAX_LENGTH)
	{
		// Read a character. This also advances the read pointer
		int glyph = str_utf8_decode(&read);
		if(glyph < 0)
		{
			// If there was an error decoding the UTF-8 sequence,
			// emit a replacement character. Since the
			// str_utf8_decode function will not work after such
			// an error, end the string here.
			glyph = UNICODE_REPLACEMENT_CHAR;
			error = INVALID_UTF8;
		}
		else if(glyph == 0)
		{
			// A character code of 0 signals the end of the string.
			error = 0;
			break;
		}
		else if(glyph > 0xffff)
		{
			// Since the windows console does not really support
			// UTF-16, don't mind doing actual UTF-16 encoding,
			// but rather emit a replacement character.
			glyph = UNICODE_REPLACEMENT_CHAR;
		}

		// Again, since the windows console does not really support
		// UTF-16, but rather something along the lines of UCS-2,
		// simply put the character into the output.
		wline[len++] = glyph;
	}

	if(error)
	{
		read = error;
		while(1)
		{
			// Errors are simple ascii, no need for UTF-8
			// decoding
			char character = *read;
			if(character == 0)
				break;

			dbg_assert_legacy(len < _MAX_LENGTH_ERROR, "str too short for error");
			wline[len++] = character;
			read++;
		}
	}

	// Terminate the line
	dbg_assert_legacy(len < _MAX_LENGTH_ERROR, "str too short for \\r");
	wline[len++] = '\r';
	dbg_assert_legacy(len < _MAX_LENGTH_ERROR, "str too short for \\n");
	wline[len++] = '\n';

	// Ignore any error that might occur
	WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), wline, len, 0, 0);

	#undef _MAX_LENGTH
	#undef _MAX_LENGTH_ERROR
}
#endif

static void logger_stdout(const char *line)
{
	printf("%s\n", line);
	fflush(stdout);
#if defined(__ANDROID__)
	__android_log_print(ANDROID_LOG_INFO, "AllTheHaxx", "%s", line);
#endif
}

static void logger_debugger(const char *line)
{
#if defined(CONF_FAMILY_WINDOWS)
	OutputDebugString(line);
	OutputDebugString("\n");
#endif
}


static IOHANDLE logfile = 0;
static void logger_file(const char *line)
{
	io_write(logfile, line, strlen(line));
	io_write_newline(logfile);
	io_flush(logfile);
}

void dbg_logger(DBG_LOGGER logger)
{
#if !defined(CONF_PLATFORM_MACOSX)
	if(dbg_msg_threaded)
		semaphore_wait(&log_queue.mutex);
#endif
	loggers[num_loggers] = logger;
	num_loggers++;
#if !defined(CONF_PLATFORM_MACOSX)
	if(dbg_msg_threaded)
		semaphore_signal(&log_queue.mutex);
#endif
}

void dbg_logger_stdout()
{
#if defined(CONF_FAMILY_WINDOWS)
	if(GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_CHAR)
	{
		dbg_logger(logger_win_console);
		return;
	}
#endif
	dbg_logger(logger_stdout);
}

void dbg_logger_debugger() { dbg_logger(logger_debugger); }
void dbg_logger_file(const char *filename)
{
	logfile = io_open(filename, IOFLAG_WRITE);
	if(logfile)
		dbg_logger(logger_file);
	else
		dbg_msg("dbg/logger", "failed to open '%s' for logging", filename);

}
/* */

#if defined(CONF_DEBUG)
/*typedef struct MEMHEADER
{
	const char *filename;
	int line;
	int size;
	struct MEMHEADER *prev;
	struct MEMHEADER *next;
} MEMHEADER;*/

typedef struct MEMTAIL
{
	int guard;
} MEMTAIL;

static const int MEM_GUARD_VAL = 0xbaadc0de;

#endif

void* mem_alloc_debug(const char *filename, int line, unsigned size, unsigned alignment)
{
	/* TODO: fix alignment */
	/* TODO: add debugging */

#if defined(CONF_DEBUG)
	MEMTAIL *tail;
	MEMHEADER *header = (struct MEMHEADER *)malloc(sizeof(MEMHEADER)+size+sizeof(MEMTAIL));
	dbg_assert_legacy(header != 0, "mem_alloc failure");
	if(!header)
		return NULL;
	tail = (struct MEMTAIL *)(((char*)(header+1))+size);
	header->size = size;
	header->filename = filename;
	header->line = line;
	header->checksum = size+line+(int)filename[0];

	memory_stats.allocated += header->size;
	memory_stats.total_allocations++;
	memory_stats.active_allocations++;

	tail->guard = MEM_GUARD_VAL;

	// put the new header at the start
	header->prev = (MEMHEADER *)0;
	header->next = memory_stats.first;
	if(memory_stats.first)
		memory_stats.first->prev = header;
	memory_stats.first = header;

	/*dbg_msg("mem", "++ %p", header+1); */
	return header+1;
#else
	return malloc(size);
#endif
}

void mem_free(void *p)
{
	if(p)
	{
#if defined(CONF_DEBUG)
		MEMHEADER *header = (MEMHEADER *)p - 1;
		MEMTAIL *tail = (MEMTAIL *)(((char*)(header+1))+header->size);

		if(tail->guard != MEM_GUARD_VAL)
			dbg_msg("mem", "!! dealloc @@ %p[%i] from '%s' INVALID GUARD: 0x%08x{@%p} != 0x%08x", p, header->size, header->filename, tail->guard, tail, MEM_GUARD_VAL);
		if(header->checksum != header->size+header->line+(int)header->filename[0])
			dbg_msg("mem", "!! dealloc INVALID HEADER @@ %p[%i] from '%s' (%i != %i)", p, header->size, header->filename, header->checksum, header->size+header->line+(int)header->filename[0]);

		memory_stats.allocated -= header->size;
		memory_stats.active_allocations--;
		if(memory_stats.active_allocations == 0)
		{
			dbg_assert_legacy(memory_stats.allocated == 0, "Got memory allocated while not having memory allocated??");
			memory_stats.first = 0x0;
		}

		if(header->prev) // this on is in the middle somewhere
			header->prev->next = header->next; // rip it out
		else // this header is at the beginning
		{
			dbg_assert_legacy(!memory_stats.first || memory_stats.first == header, "memory corruption detected");
			memory_stats.first = header->next;
		}
		if(header->next)
			header->next->prev = header->prev;

		// clear it out for debugging purposes
		header->filename = "XXXXXXX\0";
		header->next = 0;
		header->prev = 0;
		header->checksum = 0xBAADC0DE;
		header->line = 0xBAADC0DE;

		free(header);
#else
		free(p);
#endif
	}
}

void mem_debug_dump_legacy(IOHANDLE file)
{
#if defined(CONF_DEBUG)
	char buf[1024];
	MEMHEADER *header = memory_stats.first;
	if(!file)
		file = io_open("memory.txt", IOFLAG_WRITE);

	if(file)
	{
		while(header)
		{
			str_format(buf, sizeof(buf), "%s(%d): %d", header->filename, header->line, header->size);
			io_write(file, buf, (unsigned)str_length(buf));
			io_write_newline(file);
			header = header->next;
		}

		io_close(file);
	}
#endif
}


void mem_copy(void *dest, const void *source, unsigned size)
{
	memcpy(dest, source, size);
}

void mem_move(void *dest, const void *source, unsigned size)
{
	memmove(dest, source, size);
}

void mem_zero(void *block, unsigned size)
{
	memset(block, 0, size);
}

void mem_set(void *block, int value, unsigned size)
{
	memset(block, value, size);
}

int mem_check_imp()
{
#if defined(CONF_DEBUG)
	MEMHEADER *header = memory_stats.first;
	while(header)
	{
		MEMTAIL *tail = (MEMTAIL *)(((char*)(header+1))+header->size);
		if(tail->guard != MEM_GUARD_VAL)
		{
			dbg_msg("mem", "memory check failed at %s(%d): %d", header->filename, header->line, header->size);
			return 0;
		}
		if(header->checksum != header->size+header->line+(int)header->filename[0])
		{
			dbg_msg("mem", "memory check failed: INVALID HEADER @@ %p[%i] from '%s' (%i != %i)", header, header->size, header->filename, header->checksum, header->size+header->line+(int)header->filename[0]);
			return 0;
		}

		header = header->next;
	}
#endif

	return 1;
}

IOHANDLE io_open(const char *filename, int flags)
{
	if(flags == IOFLAG_READ)
		return (IOHANDLE)fopen(filename, "rb");
	if(flags == IOFLAG_WRITE)
		return (IOHANDLE)fopen(filename, "wb");
	if(flags == IOFLAG_APPEND)
		return (IOHANDLE)fopen(filename, "ab");
	return 0x0;
}

IOHANDLE io_open_raw(const char *filename, const char *flags)
{
	return (IOHANDLE)fopen(filename, flags);
}

unsigned io_read(IOHANDLE io, void *buffer, unsigned size)
{
	return fread(buffer, 1, size, (FILE*)io);
}

void io_read_threaded(void *io_data)
{
	struct io_thread_data *d = (struct io_thread_data*)io_data;
	d->ret = fread(d->buffer, 1, d->size, (FILE*)d->io);
}

long io_skip(IOHANDLE io, long size)
{
	fseek((FILE*)io, size, SEEK_CUR);
	return size;
}

int io_seek(IOHANDLE io, long offset, int origin)
{
	int real_origin;

	switch(origin)
	{
	case IOSEEK_START:
		real_origin = SEEK_SET;
		break;
	case IOSEEK_CUR:
		real_origin = SEEK_CUR;
		break;
	case IOSEEK_END:
		real_origin = SEEK_END;
		break;
	default:
		return -1;
	}

	return fseek((FILE*)io, offset, real_origin);
}

long int io_tell(IOHANDLE io)
{
	return ftell((FILE*)io);
}

long int io_length(IOHANDLE io)
{
	long int length;
	io_seek(io, 0, IOSEEK_END);
	length = io_tell(io);
	io_seek(io, 0, IOSEEK_START);
	return length;
}

unsigned io_write(IOHANDLE io, const void *buffer, unsigned size)
{
	return (unsigned int)fwrite(buffer, 1, size, (FILE*)io);
}

unsigned io_write_newline(IOHANDLE io)
{
#if defined(CONF_FAMILY_WINDOWS)
	return fwrite("\r\n", 1, 2, (FILE*)io);
#else
	return (unsigned int)fwrite("\n", 1, 1, (FILE*)io);
#endif
}

int io_close(IOHANDLE io)
{
	return fclose((FILE*)io);
}

int io_flush(IOHANDLE io)
{
	return fflush((FILE*)io);
}

void *thread_init(void (*threadfunc)(void *), void *u)
{
	return thread_init_named(threadfunc, u, 0);
}

void *thread_init_named(void (*threadfunc)(void *), void *u, const char *n)
{
#if defined(CONF_FAMILY_UNIX)
	pthread_t id;
	if(pthread_create(&id, NULL, (void *(*)(void*))threadfunc, u) == 0)
	{
		if(n && n[0])
			pthread_setname_np(id, n);
		return (void *)id;
	}
	else
		return NULL;
#elif defined(CONF_FAMILY_WINDOWS)
	return CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)threadfunc, u, 0, NULL);
#else
	#error not implemented
#endif
}

void thread_wait(void *thread)
{
#if defined(CONF_FAMILY_UNIX)
	pthread_join((pthread_t)thread, NULL);
#elif defined(CONF_FAMILY_WINDOWS)
	WaitForSingleObject((HANDLE)thread, INFINITE);
#else
	#error not implemented
#endif
}

/*void thread_destroy(void *thread)
{
#if defined(CONF_FAMILY_UNIX)
	void *r = 0;
	pthread_join((pthread_t)thread, &r);
#else
	//#error not implemented
#endif
}*/

void thread_yield()
{
#if defined(CONF_FAMILY_UNIX)
	sched_yield();
#elif defined(CONF_FAMILY_WINDOWS)
	Sleep(0);
#else
	#error not implemented
#endif
}

void thread_sleep(unsigned milliseconds)
{
#if defined(CONF_FAMILY_UNIX)
	usleep(milliseconds*1000);
#elif defined(CONF_FAMILY_WINDOWS)
	Sleep(milliseconds);
#else
	#error not implemented
#endif
}

int thread_detach(void *thread)
{
#if defined(CONF_FAMILY_UNIX)
	return pthread_detach((pthread_t)(thread)) == 0;
#elif defined(CONF_FAMILY_WINDOWS)
	return CloseHandle(thread) != 0;
#else
	#error not implemented
#endif
}

void *thread_get_current()
{
#if defined(CONF_FAMILY_UNIX)
	pthread_t ptid = pthread_self();
	uint64_t threadId = 0;
	memcpy(&threadId, &ptid, sizeof(ptid));
	return (void*)threadId;
#elif defined(CONF_FAMILY_WINDOWS)
	return GetCurrentThread();
#else
	#error not implemented
#endif
}




#if defined(CONF_FAMILY_UNIX)
typedef pthread_mutex_t LOCKINTERNAL;
#elif defined(CONF_FAMILY_WINDOWS)
typedef CRITICAL_SECTION LOCKINTERNAL;
#else
	#error not implemented on this platform
#endif

LOCK lock_create()
{
	LOCKINTERNAL *lock = (LOCKINTERNAL*)mem_alloc(sizeof(LOCKINTERNAL), 4);

#if defined(CONF_FAMILY_UNIX)
	pthread_mutex_init(lock, 0x0);
#elif defined(CONF_FAMILY_WINDOWS)
	InitializeCriticalSection((LPCRITICAL_SECTION)lock);
#else
	#error not implemented on this platform
#endif
	return (LOCK)lock;
}

void lock_destroy(LOCK lock)
{
#if defined(CONF_FAMILY_UNIX)
	pthread_mutex_destroy((LOCKINTERNAL *)lock);
#elif defined(CONF_FAMILY_WINDOWS)
	DeleteCriticalSection((LPCRITICAL_SECTION)lock);
#else
	#error not implemented on this platform
#endif
	mem_free(lock);
}

int lock_trylock(LOCK lock)
{
#if defined(CONF_FAMILY_UNIX)
	return pthread_mutex_trylock((LOCKINTERNAL *)lock);
#elif defined(CONF_FAMILY_WINDOWS)
	return !TryEnterCriticalSection((LPCRITICAL_SECTION)lock);
#else
	#error not implemented on this platform
#endif
}

void lock_wait(LOCK lock)
{
#if defined(CONF_FAMILY_UNIX)
	pthread_mutex_lock((LOCKINTERNAL *)lock);
#elif defined(CONF_FAMILY_WINDOWS)
	EnterCriticalSection((LPCRITICAL_SECTION)lock);
#else
	#error not implemented on this platform
#endif
}

void lock_unlock(LOCK lock)
{
#if defined(CONF_FAMILY_UNIX)
	pthread_mutex_unlock((LOCKINTERNAL *)lock);
#elif defined(CONF_FAMILY_WINDOWS)
	LeaveCriticalSection((LPCRITICAL_SECTION)lock);
#else
	#error not implemented on this platform
#endif
}

#if !defined(CONF_PLATFORM_MACOSX)
	#if defined(CONF_FAMILY_UNIX)
	void semaphore_init(SEMAPHORE *sem) { sem_init(sem, 0, 0); }
	void semaphore_wait(SEMAPHORE *sem) { sem_wait(sem); }
	void semaphore_signal(SEMAPHORE *sem) { sem_post(sem); }
	void semaphore_destroy(SEMAPHORE *sem) { sem_destroy(sem); }
	#elif defined(CONF_FAMILY_WINDOWS)
	void semaphore_init(SEMAPHORE *sem) { *sem = CreateSemaphore(0, 0, 10000, 0); }
	void semaphore_wait(SEMAPHORE *sem) { WaitForSingleObject((HANDLE)*sem, INFINITE); }
	void semaphore_signal(SEMAPHORE *sem) { ReleaseSemaphore((HANDLE)*sem, 1, NULL); }
	void semaphore_destroy(SEMAPHORE *sem) { CloseHandle((HANDLE)*sem); }
	#else
		#error not implemented on this platform
	#endif
#endif

static int new_tick = -1;

void set_new_tick()
{
	new_tick = 1;
}

/* -----  time ----- */
int64 time_get()
{
	static int64 last = 0;
	if(new_tick == 0)
		return last;
	if(new_tick != -1)
		new_tick = 0;

	int64 time = time_get_raw();
#if defined(CONF_FAMILY_WINDOWS)
	if(time < last) /* for some reason, QPC can return values in the past */
		return last;
#endif
	last = time;
	return last;
}

int64 time_get_raw()
{
#if defined(CONF_PLATFORM_MACOSX)
	static int got_timebase = 0;
		mach_timebase_info_data_t timebase;
		uint64_t time;
		uint64_t q;
		uint64_t r;
		if(!got_timebase)
		{
			mach_timebase_info(&timebase);
		}
		time = mach_absolute_time();
		q = time / timebase.denom;
		r = time % timebase.denom;
		return q * timebase.numer + r * timebase.numer / timebase.denom;
#elif defined(CONF_FAMILY_UNIX)
	struct timespec spec;
	clock_gettime(CLOCK_MONOTONIC, &spec);
	return (int64)spec.tv_sec*(int64)1000000+(int64)spec.tv_nsec/1000;
#elif defined(CONF_FAMILY_WINDOWS)
	int64 t;
		QueryPerformanceCounter((PLARGE_INTEGER)&t);
		return t;
#else
		#error not implemented
	#endif
}

int64 time_freq()
{
#if defined(CONF_PLATFORM_MACOSX)
	return 1000000000;
#elif defined(CONF_FAMILY_UNIX)
	return 1000000;
#elif defined(CONF_FAMILY_WINDOWS)
	int64 t;
	QueryPerformanceFrequency((PLARGE_INTEGER)&t);
	return t;
#else
	#error not implemented
#endif
}

double time_to_millis(int64 time)
{
	return (double)time/((double)time_freq()/1000);
}
double time_to_nanos(int64 time)
{
	return (double)time/((double)time_freq()/1000000);
}

/* -----  network ----- */
static void netaddr_to_sockaddr_in(const NETADDR *src, struct sockaddr_in *dest)
{
	mem_zero(dest, sizeof(struct sockaddr_in));
	if(src->type != NETTYPE_IPV4 && src->type != NETTYPE_WEBSOCKET_IPV4)
	{
		dbg_msg("system", "couldn't convert NETADDR of type %d to ipv4", src->type);
		return;
	}

	dest->sin_family = AF_INET;
	dest->sin_port = htons(src->port);
	mem_copy(&dest->sin_addr.s_addr, src->ip, 4);
}

static void netaddr_to_sockaddr_in6(const NETADDR *src, struct sockaddr_in6 *dest)
{
	mem_zero(dest, sizeof(struct sockaddr_in6));
	if(src->type != NETTYPE_IPV6)
	{
		dbg_msg("system", "couldn't not convert NETADDR of type %d to ipv6", src->type);
		return;
	}

	dest->sin6_family = AF_INET6;
	dest->sin6_port = htons(src->port);
	mem_copy(&dest->sin6_addr.s6_addr, src->ip, 16);
}

static void sockaddr_to_netaddr(const struct sockaddr *src, NETADDR *dst)
{
	if(src->sa_family == AF_INET)
	{
		mem_zero(dst, sizeof(NETADDR));
		dst->type = NETTYPE_IPV4;
		dst->port = htons(((struct sockaddr_in*)src)->sin_port);
		mem_copy(dst->ip, &((struct sockaddr_in*)src)->sin_addr.s_addr, 4);
	}
	else if(src->sa_family == AF_WEBSOCKET_INET)
	{
		mem_zero(dst, sizeof(NETADDR));
		dst->type = NETTYPE_WEBSOCKET_IPV4;
		dst->port = htons(((struct sockaddr_in*)src)->sin_port);
		mem_copy(dst->ip, &((struct sockaddr_in*)src)->sin_addr.s_addr, 4);
	}
	else if(src->sa_family == AF_INET6)
	{
		mem_zero(dst, sizeof(NETADDR));
		dst->type = NETTYPE_IPV6;
		dst->port = htons(((struct sockaddr_in6*)src)->sin6_port);
		mem_copy(dst->ip, &((struct sockaddr_in6*)src)->sin6_addr.s6_addr, 16);
	}
	else
	{
		mem_zero(dst, sizeof(struct sockaddr));
		dbg_msg("system", "couldn't convert sockaddr of family %d", src->sa_family);
	}
}

int net_addr_comp(const NETADDR *a, const NETADDR *b)
{
	return mem_comp(a, b, sizeof(NETADDR));
}

void net_addr_str(const NETADDR *addr, char *string, int max_length, int add_port)
{
	if(addr->type == NETTYPE_IPV4 || addr->type == NETTYPE_WEBSOCKET_IPV4)
	{
		if(add_port != 0)
			str_format(string, max_length, "%d.%d.%d.%d:%d", addr->ip[0], addr->ip[1], addr->ip[2], addr->ip[3], addr->port);
		else
			str_format(string, max_length, "%d.%d.%d.%d", addr->ip[0], addr->ip[1], addr->ip[2], addr->ip[3]);
	}
	else if(addr->type == NETTYPE_IPV6)
	{
		if(add_port != 0)
			str_format(string, max_length, "[%x:%x:%x:%x:%x:%x:%x:%x]:%d",
				(addr->ip[0]<<8)|addr->ip[1], (addr->ip[2]<<8)|addr->ip[3], (addr->ip[4]<<8)|addr->ip[5], (addr->ip[6]<<8)|addr->ip[7],
				(addr->ip[8]<<8)|addr->ip[9], (addr->ip[10]<<8)|addr->ip[11], (addr->ip[12]<<8)|addr->ip[13], (addr->ip[14]<<8)|addr->ip[15],
				addr->port);
		else
			str_format(string, max_length, "[%x:%x:%x:%x:%x:%x:%x:%x]",
				(addr->ip[0]<<8)|addr->ip[1], (addr->ip[2]<<8)|addr->ip[3], (addr->ip[4]<<8)|addr->ip[5], (addr->ip[6]<<8)|addr->ip[7],
				(addr->ip[8]<<8)|addr->ip[9], (addr->ip[10]<<8)|addr->ip[11], (addr->ip[12]<<8)|addr->ip[13], (addr->ip[14]<<8)|addr->ip[15]);
	}
	else
		str_format(string, max_length, "unknown type %d", addr->type);
}

void net_addr_split(char *pAddr, int max_length)
{
	int i;
	for(i = 0; pAddr[i] != '\0' && i < max_length; i++)
	{
		if(pAddr[i] == ':')
			pAddr[i] = ' ';
	}
}

static int priv_net_extract(const char *hostname, char *host, int max_host, unsigned short *port)
{
	int i;

	*port = 0;
	host[0] = 0;

	if(hostname[0] == '[')
	{
		// ipv6 mode
		for(i = 1; i < max_host && hostname[i] && hostname[i] != ']'; i++)
			host[i-1] = hostname[i];
		host[i-1] = 0;
		if(hostname[i] != ']') // malformatted
			return -1;

		i++;
		if(hostname[i] == ':')
			*port = (unsigned short)atol(hostname + i + 1);
	}
	else
	{
		// generic mode (ipv4, hostname etc)
		for(i = 0; i < max_host-1 && hostname[i] && hostname[i] != ':'; i++)
			host[i] = hostname[i];
		host[i] = 0;

		if(hostname[i] == ':')
			*port = (unsigned short)atol(hostname+i+1);
	}

	return 0;
}

int net_host_lookup(const char *hostname, NETADDR *addr, int types)
{
	struct addrinfo hints;
	struct addrinfo *result = NULL;
	int e;
	char host[256];
	unsigned short port = 0;

	if(priv_net_extract(hostname, host, sizeof(host), &port))
		return -1;

	dbg_msg("host lookup", "host='%s' port=%d %d", host, port, types);

	mem_zero(&hints, sizeof(hints));

	hints.ai_family = AF_UNSPEC;

	if(types == NETTYPE_IPV4)
		hints.ai_family = AF_INET;
	else if(types == NETTYPE_IPV6)
		hints.ai_family = AF_INET6;

	e = getaddrinfo(host, NULL, &hints, &result);

	if(!result)
		return -1;

	if(e != 0)
	{
		freeaddrinfo(result);
		return -1;
	}

	sockaddr_to_netaddr(result->ai_addr, addr);
	addr->port = port;
	freeaddrinfo(result);
	return 0;
}

static int parse_int(int *out, const char **str)
{
	int i = 0;
	*out = 0;
	if(**str < '0' || **str > '9')
		return -1;

	i = **str - '0';
	(*str)++;

	while(1)
	{
		if(**str < '0' || **str > '9')
		{
			*out = i;
			return 0;
		}

		i = (i*10) + (**str - '0');
		(*str)++;
	}

	return 0;
}

static int parse_char(char c, const char **str)
{
	if(**str != c) return -1;
	(*str)++;
	return 0;
}

static int parse_uint8(unsigned char *out, const char **str)
{
	int i;
	if(parse_int(&i, str) != 0) return -1;
	if(i < 0 || i > 0xff) return -1;
	*out = (unsigned char)i;
	return 0;
}

static int parse_uint16(unsigned short *out, const char **str)
{
	int i;
	if(parse_int(&i, str) != 0) return -1;
	if(i < 0 || i > 0xffff) return -1;
	*out = (unsigned short)i;
	return 0;
}

int net_addr_from_str(NETADDR *addr, const char *string)
{
	const char *str = string;
	mem_zero(addr, sizeof(NETADDR));

	if(str[0] == '[')
	{
		/* ipv6 */
		struct sockaddr_in6 sa6;
		char buf[128];
		int i;
		str++;
		for(i = 0; i < 127 && str[i] && str[i] != ']'; i++)
			buf[i] = str[i];
		buf[i] = 0;
		str += i;
#if defined(CONF_FAMILY_WINDOWS)
		{
			int size;
			sa6.sin6_family = AF_INET6;
			size = (int)sizeof(sa6);
			if(WSAStringToAddress(buf, AF_INET6, NULL, (struct sockaddr *)&sa6, &size) != 0)
				return -1;
		}
#else
		sa6.sin6_family = AF_INET6;

		if(inet_pton(AF_INET6, buf, &sa6.sin6_addr) != 1)
			return -1;
#endif
		sockaddr_to_netaddr((struct sockaddr *)&sa6, addr);

		if(*str == ']')
		{
			str++;
			if(*str == ':')
			{
				str++;
				if(parse_uint16(&addr->port, &str))
					return -1;
			}
		}
		else
			return -1;

		return 0;
	}
	else
	{
		/* ipv4 */
		if(parse_uint8(&addr->ip[0], &str)) return -1;
		if(parse_char('.', &str)) return -1;
		if(parse_uint8(&addr->ip[1], &str)) return -1;
		if(parse_char('.', &str)) return -1;
		if(parse_uint8(&addr->ip[2], &str)) return -1;
		if(parse_char('.', &str)) return -1;
		if(parse_uint8(&addr->ip[3], &str)) return -1;
		if(*str == ':')
		{
			str++;
			if(parse_uint16(&addr->port, &str)) return -1;
		}

		addr->type = NETTYPE_IPV4;
	}

	return 0;
}

static void priv_net_close_socket(int sock)
{
#if defined(CONF_FAMILY_WINDOWS)
	closesocket(sock);
#else
	close(sock);
#endif
}

static int priv_net_close_all_sockets(NETSOCKET sock)
{
	/* close down ipv4 */
	if(sock.ipv4sock >= 0)
	{
		priv_net_close_socket(sock.ipv4sock);
		sock.ipv4sock = -1;
		sock.type &= ~NETTYPE_IPV4;
	}

#if defined(CONF_WEBSOCKETS)
	/* close down websocket_ipv4 */
	if(sock.web_ipv4sock >= 0)
	{
		websocket_destroy(sock.web_ipv4sock);
		sock.web_ipv4sock = -1;
		sock.type &= ~NETTYPE_WEBSOCKET_IPV4;
	}
#endif

	/* close down ipv6 */
	if(sock.ipv6sock >= 0)
	{
		priv_net_close_socket(sock.ipv6sock);
		sock.ipv6sock = -1;
		sock.type &= ~NETTYPE_IPV6;
	}
	return 0;
}

static int priv_net_create_socket(int domain, int type, struct sockaddr *addr, int sockaddrlen)
{
	int sock, e;

	/* create socket */
	sock = socket(domain, type, 0);
	if(sock < 0)
	{
#if defined(CONF_FAMILY_WINDOWS)
		char buf[128];
		int error = WSAGetLastError();
		if(FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS, 0, error, 0, buf, sizeof(buf), 0) == 0)
			buf[0] = 0;
		dbg_msg("net", "failed to create socket with domain %d and type %d (%d '%s')", domain, type, error, buf);
#else
		dbg_msg("net", "failed to create socket with domain %d and type %d (%d '%s')", domain, type, errno, strerror(errno));
#endif
		return -1;
	}

#if defined(CONF_FAMILY_UNIX)
	/* on tcp sockets set SO_REUSEADDR
		to fix port rebind on restart */
	if (domain == AF_INET && type == SOCK_STREAM)
	{
		int option = 1;
		setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
	}
#endif

	/* set to IPv6 only if thats what we are creating */
#if defined(IPV6_V6ONLY)	/* windows sdk 6.1 and higher */
	if(domain == AF_INET6)
	{
		int ipv6only = 1;
		setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&ipv6only, sizeof(ipv6only));
	}
#endif

	/* bind the socket */
	e = bind(sock, addr, sockaddrlen);
	if(e != 0)
	{
#if defined(CONF_FAMILY_WINDOWS)
		char buf[128];
		int error = WSAGetLastError();
		if(FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS, 0, error, 0, buf, sizeof(buf), 0) == 0)
			buf[0] = 0;
		dbg_msg("net", "failed to bind socket with domain %d and type %d (%d '%s')", domain, type, error, buf);
#else
		dbg_msg("net", "failed to bind socket with domain %d and type %d (%d '%s')", domain, type, errno, strerror(errno));
#endif
		priv_net_close_socket(sock);
		return -1;
	}

	/* return the newly created socket */
	return sock;
}

NETSOCKET net_udp_create(NETADDR bindaddr)
{
	NETSOCKET sock = invalid_socket;
	NETADDR tmpbindaddr = bindaddr;
	int broadcast = 1;
	int recvsize = 65536;

	if(bindaddr.type&NETTYPE_IPV4)
	{
		struct sockaddr_in addr;
		int socket = -1;

		/* bind, we should check for error */
		tmpbindaddr.type = NETTYPE_IPV4;
		netaddr_to_sockaddr_in(&tmpbindaddr, &addr);
		socket = priv_net_create_socket(AF_INET, SOCK_DGRAM, (struct sockaddr *)&addr, sizeof(addr));
		if(socket >= 0)
		{
			sock.type |= NETTYPE_IPV4;
			sock.ipv4sock = socket;

			/* set boardcast */
			setsockopt(socket, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcast, sizeof(broadcast));

			/* set receive buffer size */
			setsockopt(socket, SOL_SOCKET, SO_RCVBUF, (char*)&recvsize, sizeof(recvsize));

			{
				/* set DSCP/TOS */
				int iptos = 0x10 /* IPTOS_LOWDELAY */;
				//int iptos = 46; /* High Priority */
				setsockopt(socket, IPPROTO_IP, IP_TOS, (char*)&iptos, sizeof(iptos));
			}
		}
	}

#if defined(CONF_WEBSOCKETS)
	if(bindaddr.type&NETTYPE_WEBSOCKET_IPV4)
	{
		int socket = -1;
		char addr_str[NETADDR_MAXSTRSIZE];

		/* bind, we should check for error */
		tmpbindaddr.type = NETTYPE_WEBSOCKET_IPV4;

		net_addr_str(&tmpbindaddr, addr_str, sizeof(addr_str), 0);
		socket = websocket_create(addr_str, tmpbindaddr.port);

		if (socket >= 0) {
			sock.type |= NETTYPE_WEBSOCKET_IPV4;
			sock.web_ipv4sock = socket;
		}
	}
#endif

	if(bindaddr.type&NETTYPE_IPV6)
	{
		struct sockaddr_in6 addr;
		int socket = -1;

		/* bind, we should check for error */
		tmpbindaddr.type = NETTYPE_IPV6;
		netaddr_to_sockaddr_in6(&tmpbindaddr, &addr);
		socket = priv_net_create_socket(AF_INET6, SOCK_DGRAM, (struct sockaddr *)&addr, sizeof(addr));
		if(socket >= 0)
		{
			sock.type |= NETTYPE_IPV6;
			sock.ipv6sock = socket;

			/* set boardcast */
			setsockopt(socket, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcast, sizeof(broadcast));

			/* set receive buffer size */
			setsockopt(socket, SOL_SOCKET, SO_RCVBUF, (char*)&recvsize, sizeof(recvsize));

			{
				/* set DSCP/TOS */
				int iptos = 0x10 /* IPTOS_LOWDELAY */;
				//int iptos = 46; /* High Priority */
				setsockopt(socket, IPPROTO_IP, IP_TOS, (char*)&iptos, sizeof(iptos));
			}
		}
	}

	/* set non-blocking */
	net_set_non_blocking(sock);

#ifdef FUZZING
	IOHANDLE file = io_open("bar.txt", IOFLAG_READ);
	gs_NetPosition = 0;
	gs_NetSize = io_length(file);
	io_read(file, gs_NetData, 1024);
	io_close(file);
#endif /* FUZZING */

	/* return */
	return sock;
}

long net_udp_send(NETSOCKET sock, const NETADDR *addr, const void *data, unsigned int size)
{
#ifndef FUZZING
	long d = -1;

	if(addr->type&NETTYPE_IPV4)
	{
		if(sock.ipv4sock >= 0)
		{
			struct sockaddr_in sa;
			if(addr->type&NETTYPE_LINK_BROADCAST)
			{
				mem_zero(&sa, sizeof(sa));
				sa.sin_port = htons(addr->port);
				sa.sin_family = AF_INET;
				sa.sin_addr.s_addr = INADDR_BROADCAST;
			}
			else
				netaddr_to_sockaddr_in(addr, &sa);

			d = sendto((int)sock.ipv4sock, (const char*)data, size, 0, (struct sockaddr *)&sa, sizeof(sa));
		}
		else
			dbg_msg("net", "can't send ipv4 traffic to this socket");
	}

#if defined(CONF_WEBSOCKETS)
	if(addr->type&NETTYPE_WEBSOCKET_IPV4)
	{
		if(sock.web_ipv4sock >= 0)
			d = websocket_send(sock.web_ipv4sock, (const unsigned char*)data, size, addr->port);
		else
			dbg_msg("net", "can't send websocket_ipv4 traffic to this socket");
	}
#endif

	if(addr->type&NETTYPE_IPV6)
	{
		if(sock.ipv6sock >= 0)
		{
			struct sockaddr_in6 sa;
			if(addr->type&NETTYPE_LINK_BROADCAST)
			{
				mem_zero(&sa, sizeof(sa));
				sa.sin6_port = htons(addr->port);
				sa.sin6_family = AF_INET6;
				sa.sin6_addr.s6_addr[0] = 0xff; /* multicast */
				sa.sin6_addr.s6_addr[1] = 0x02; /* link local scope */
				sa.sin6_addr.s6_addr[15] = 1; /* all nodes */
			}
			else
				netaddr_to_sockaddr_in6(addr, &sa);

			d = sendto((int)sock.ipv6sock, (const char*)data, size, 0, (struct sockaddr *)&sa, sizeof(sa));
		}
		else
			dbg_msg("net", "can't send ipv6 traffic to this socket");
	}
	/*
	else
		dbg_msg("net", "can't send to network of type %d", addr->type);
		*/

	/*if(d < 0)
	{
		char addrstr[256];
		net_addr_str(addr, addrstr, sizeof(addrstr));

		dbg_msg("net", "sendto error (%d '%s')", errno, strerror(errno));
		dbg_msg("net", "\tsock = %d %x", sock, sock);
		dbg_msg("net", "\tsize = %d %x", size, size);
		dbg_msg("net", "\taddr = %s", addrstr);

	}*/
	network_stats.sent_bytes += size;
	network_stats.sent_packets++;
	return d;
#else
	return size;
#endif /* FUZZING */
}

long net_udp_recv(NETSOCKET sock, NETADDR *addr, void *data, unsigned int maxsize)
{
#ifndef FUZZING
	char sockaddrbuf[128];
	socklen_t fromlen;// = sizeof(sockaddrbuf);
	long bytes = 0;

	if(bytes == 0 && sock.ipv4sock >= 0)
	{
		fromlen = sizeof(struct sockaddr_in);
		bytes = recvfrom(sock.ipv4sock, (char*)data, maxsize, 0, (struct sockaddr *)&sockaddrbuf, &fromlen);
	}

	if(bytes <= 0 && sock.ipv6sock >= 0)
	{
		fromlen = sizeof(struct sockaddr_in6);
		bytes = recvfrom(sock.ipv6sock, (char*)data, maxsize, 0, (struct sockaddr *)&sockaddrbuf, &fromlen);
	}

#if defined(CONF_WEBSOCKETS)
	if(bytes <= 0 && sock.web_ipv4sock >= 0)
	{
		fromlen = sizeof(struct sockaddr);
		bytes = websocket_recv(sock.web_ipv4sock, data, maxsize, (struct sockaddr_in *)&sockaddrbuf, fromlen);
		((struct sockaddr_in *)&sockaddrbuf)->sin_family = AF_WEBSOCKET_INET;
	}
#endif

	if(bytes > 0)
	{
		sockaddr_to_netaddr((struct sockaddr *)&sockaddrbuf, addr);
		network_stats.recv_bytes += bytes;
		network_stats.recv_packets++;
		return bytes;
	}
	else if(bytes == 0)
		return 0;
	return -1; /* error */
#else
	addr->type = NETTYPE_IPV4;
	addr->port = 11111;
	addr->ip[0] = 127;
	addr->ip[1] = 0;
	addr->ip[2] = 0;
	addr->ip[3] = 1;

	int CurrentData = 0;
	while (gs_NetPosition < gs_NetSize && CurrentData < maxsize)
	{
		if(gs_NetData[gs_NetPosition] == '\n')
		{
			gs_NetPosition++;
			break;
		}

		((unsigned char*)data)[CurrentData] = gs_NetData[gs_NetPosition];
		CurrentData++;
		gs_NetPosition++;
	}

	if (gs_NetPosition >= gs_NetSize)
		exit(0);

	return CurrentData;
#endif /* FUZZING */
}

int net_udp_close(NETSOCKET sock)
{
	return priv_net_close_all_sockets(sock);
}

NETSOCKET net_tcp_create(NETADDR bindaddr)
{
	NETSOCKET sock = invalid_socket;
	NETADDR tmpbindaddr = bindaddr;

	if(bindaddr.type&NETTYPE_IPV4)
	{
		struct sockaddr_in addr;
		int socket = -1;

		/* bind, we should check for error */
		tmpbindaddr.type = NETTYPE_IPV4;
		netaddr_to_sockaddr_in(&tmpbindaddr, &addr);
		socket = priv_net_create_socket(AF_INET, SOCK_STREAM, (struct sockaddr *)&addr, sizeof(addr));
		if(socket >= 0)
		{
			sock.type |= NETTYPE_IPV4;
			sock.ipv4sock = socket;
		}
	}

	if(bindaddr.type&NETTYPE_IPV6)
	{
		struct sockaddr_in6 addr;
		int socket = -1;

		/* bind, we should check for error */
		tmpbindaddr.type = NETTYPE_IPV6;
		netaddr_to_sockaddr_in6(&tmpbindaddr, &addr);
		socket = priv_net_create_socket(AF_INET6, SOCK_STREAM, (struct sockaddr *)&addr, sizeof(addr));
		if(socket >= 0)
		{
			sock.type |= NETTYPE_IPV6;
			sock.ipv6sock = socket;
		}
	}

	/* return */
	return sock;
}

int net_set_non_blocking(NETSOCKET sock)
{
	unsigned long mode = 1;
	if(sock.ipv4sock >= 0)
	{
#if defined(CONF_FAMILY_WINDOWS)
		ioctlsocket(sock.ipv4sock, FIONBIO, (unsigned long *)&mode);
#else
		ioctl(sock.ipv4sock, FIONBIO, (unsigned long *)&mode);
#endif
	}

	if(sock.ipv6sock >= 0)
	{
#if defined(CONF_FAMILY_WINDOWS)
		ioctlsocket(sock.ipv6sock, FIONBIO, (unsigned long *)&mode);
#else
		ioctl(sock.ipv6sock, FIONBIO, (unsigned long *)&mode);
#endif
	}

	return 0;
}

int net_set_blocking(NETSOCKET sock)
{
	unsigned long mode = 0;
	if(sock.ipv4sock >= 0)
	{
#if defined(CONF_FAMILY_WINDOWS)
		ioctlsocket(sock.ipv4sock, FIONBIO, (unsigned long *)&mode);
#else
		ioctl(sock.ipv4sock, FIONBIO, (unsigned long *)&mode);
#endif
	}

	if(sock.ipv6sock >= 0)
	{
#if defined(CONF_FAMILY_WINDOWS)
		ioctlsocket(sock.ipv6sock, FIONBIO, (unsigned long *)&mode);
#else
		ioctl(sock.ipv6sock, FIONBIO, (unsigned long *)&mode);
#endif
	}

	return 0;
}

int net_tcp_listen(NETSOCKET sock, int backlog)
{
	int err = -1;
	if(sock.ipv4sock >= 0)
		err = listen(sock.ipv4sock, backlog);
	if(sock.ipv6sock >= 0)
		err = listen(sock.ipv6sock, backlog);
	return err;
}

int net_tcp_accept(NETSOCKET sock, NETSOCKET *new_sock, NETADDR *a)
{
	int s;
	socklen_t sockaddr_len;

	*new_sock = invalid_socket;

	if(sock.ipv4sock >= 0)
	{
		struct sockaddr_in addr;
		sockaddr_len = sizeof(addr);

		s = accept(sock.ipv4sock, (struct sockaddr *)&addr, &sockaddr_len);

		if (s != -1)
		{
			sockaddr_to_netaddr((const struct sockaddr *)&addr, a);
			new_sock->type = NETTYPE_IPV4;
			new_sock->ipv4sock = s;
			return s;
		}
	}

	if(sock.ipv6sock >= 0)
	{
		struct sockaddr_in6 addr;
		sockaddr_len = sizeof(addr);

		s = accept(sock.ipv6sock, (struct sockaddr *)&addr, &sockaddr_len);

		if (s != -1)
		{
			sockaddr_to_netaddr((const struct sockaddr *)&addr, a);
			new_sock->type = NETTYPE_IPV6;
			new_sock->ipv6sock = s;
			return s;
		}
	}

	return -1;
}

int net_tcp_connect(NETSOCKET sock, const NETADDR *a)
{
	if(a->type&NETTYPE_IPV4)
	{
		struct sockaddr_in addr;
		netaddr_to_sockaddr_in(a, &addr);
		return connect(sock.ipv4sock, (struct sockaddr *)&addr, sizeof(addr));
	}

	if(a->type&NETTYPE_IPV6)
	{
		struct sockaddr_in6 addr;
		netaddr_to_sockaddr_in6(a, &addr);
		return connect(sock.ipv6sock, (struct sockaddr *)&addr, sizeof(addr));
	}

	return -1;
}

int net_tcp_connect_non_blocking(NETSOCKET sock, NETADDR bindaddr)
{
	int res = 0;

	net_set_non_blocking(sock);
	res = net_tcp_connect(sock, &bindaddr);
	net_set_blocking(sock);

	return res;
}

long net_tcp_send(NETSOCKET sock, const void *data, unsigned int size)
{
	long bytes = -1;

	if(sock.ipv4sock >= 0)
		bytes = send((int)sock.ipv4sock, (const char*)data, size, 0);
	if(sock.ipv6sock >= 0)
		bytes = send((int)sock.ipv6sock, (const char*)data, size, 0);

	return bytes;
}

long net_tcp_recv(NETSOCKET sock, void *data, unsigned int maxsize)
{
	long bytes = -1;

	if(sock.ipv4sock >= 0)
		bytes = recv((int)sock.ipv4sock, (char*)data, maxsize, 0);
	if(sock.ipv6sock >= 0)
		bytes = recv((int)sock.ipv6sock, (char*)data, maxsize, 0);

	return bytes;
}

int net_tcp_close(NETSOCKET sock)
{
	return priv_net_close_all_sockets(sock);
}

int net_errno()
{
#if defined(CONF_FAMILY_WINDOWS)
	return WSAGetLastError();
#else
	return errno;
#endif
}

char *net_err_str(char *result, unsigned size, int error)
{
	char buf[256];
#if defined(CONF_FAMILY_WINDOWS)
	if(FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS, 0, error, 0, buf, sizeof(buf), 0) == 0)
		str_copy(buf, "<no message>", size);
#else
	str_copyb(buf, strerror(error));
#endif
	str_format(result, size, "error %i (%s)", error, buf);
	return result;
}

int net_would_block()
{
#if defined(CONF_FAMILY_WINDOWS)
	return net_errno() == WSAEWOULDBLOCK;
#else
	return net_errno() == EWOULDBLOCK;
#endif
}

int net_init()
{
#if defined(CONF_FAMILY_WINDOWS)
	WSADATA wsaData;
	int err = WSAStartup(MAKEWORD(1, 1), &wsaData);
	dbg_assert_legacy(err == 0, "network initialization failed.");
	return err==0?0:1;
#endif

	return 0;
}

int fs_listdir_info(const char *dir, FS_LISTDIR_INFO_CALLBACK cb, int type, void *user)
{
#if defined(CONF_FAMILY_WINDOWS)
	WIN32_FIND_DATA finddata;
	HANDLE handle;
	char buffer[1024*2];
	int length;
	str_format(buffer, sizeof(buffer), "%s/*", dir);

	handle = FindFirstFileA(buffer, &finddata);

	if (handle == INVALID_HANDLE_VALUE)
		return 0;

	str_format(buffer, sizeof(buffer), "%s/", dir);
	length = str_length(buffer);

	/* add all the entries */
	do
	{
		str_copy(buffer+length, finddata.cFileName, (int)sizeof(buffer)-length);
		if(cb(finddata.cFileName, fs_getmtime(buffer), fs_is_dir(buffer), type, user))
			break;
	}
	while (FindNextFileA(handle, &finddata));

	FindClose(handle);
	return 0;
#else
	struct dirent *entry;
	char buffer[1024*2];
	int length;
	DIR *d = opendir(dir);

	if(!d)
		return 0;

	str_format(buffer, sizeof(buffer), "%s/", dir);
	length = str_length(buffer);

	while((entry = readdir(d)) != NULL)
	{
		str_copy(buffer+length, entry->d_name, (int)sizeof(buffer)-length);
		if(cb(entry->d_name, fs_getmtime(buffer), fs_is_dir(buffer), type, user))
			break;
	}

	/* close the directory and return */
	closedir(d);
	return 0;
#endif
}

int fs_listdir(const char *dir, FS_LISTDIR_CALLBACK cb, int type, void *user)
{
#if defined(CONF_FAMILY_WINDOWS)
	WIN32_FIND_DATA finddata;
	HANDLE handle;
	char buffer[1024*2];
	int length;
	str_format(buffer, sizeof(buffer), "%s/*", dir);

	handle = FindFirstFileA(buffer, &finddata);

	if (handle == INVALID_HANDLE_VALUE)
		return 0;

	str_format(buffer, sizeof(buffer), "%s/", dir);
	length = str_length(buffer);

	/* add all the entries */
	do
	{
		str_copy(buffer+length, finddata.cFileName, (int)sizeof(buffer)-length);
		if(cb(finddata.cFileName, fs_is_dir(buffer), type, user))
			break;
	}
	while (FindNextFileA(handle, &finddata));

	FindClose(handle);
	return 0;
#else
	struct dirent *entry;
	char buffer[1024*2];
	int length;
	DIR *d = opendir(dir);

	if(!d)
		return 0;

	str_format(buffer, sizeof(buffer), "%s/", dir);
	length = str_length(buffer);

	while((entry = readdir(d)) != NULL)
	{
		str_copy(buffer+length, entry->d_name, (int)sizeof(buffer)-length);
		if(cb(entry->d_name, fs_is_dir(buffer), type, user))
			break;
	}

	/* close the directory and return */
	closedir(d);
	return 0;
#endif
}

int fs_listdir_verbose(const char *dir, FS_LISTDIR_CALLBACK_VERBOSE cb, int type, void *user)
{
#if defined(CONF_FAMILY_WINDOWS)
	WIN32_FIND_DATA finddata;
	HANDLE handle;
	int result;
	char buffer[1024*2];
	int length;
	str_format(buffer, sizeof(buffer), "%s/*", dir);

	handle = FindFirstFileA(buffer, &finddata);

	if (handle == INVALID_HANDLE_VALUE)
		return 0;

	str_format(buffer, sizeof(buffer), "%s/", dir);
	length = str_length(buffer);
	result = 0;

	/* add all the entries */
	do
	{
		str_copy(buffer+length, finddata.cFileName, (int)sizeof(buffer)-length);
		if((result = cb(finddata.cFileName, buffer, fs_is_dir(buffer), type, user)))
			break;
	}
	while (FindNextFileA(handle, &finddata));

	FindClose(handle);
	return result;
#else
	struct dirent *entry;
	char buffer[1024*2];
	int length;
	DIR *d = opendir(dir);

	if(!d)
		return 0;

	str_format(buffer, sizeof(buffer), "%s/", dir);
	length = str_length(buffer);
	int result = 0;

	while((entry = readdir(d)) != NULL)
	{
		str_copy(buffer+length, entry->d_name, (int)sizeof(buffer)-length);
		if((result = cb(entry->d_name, buffer, fs_is_dir(buffer), type, user)))
			break;
	}

	/* close the directory and return */
	closedir(d);
	return result;
#endif
}

int fs_storage_path(const char *appname, char *path, int max)
{
#if defined(CONF_FAMILY_WINDOWS)
	char *home = getenv("APPDATA");
	if(!home)
		return -1;
	_snprintf(path, max, "%s/%s", home, appname);
	return 0;
#else
	char *home = getenv("HOME");
#if !defined(CONF_PLATFORM_MACOSX)
	int i;
#endif
	if(!home)
		return -1;

#if defined(CONF_PLATFORM_MACOSX)
	snprintf(path, max, "%s/Library/Application Support/%s", home, appname);
#else
	snprintf(path, max, "%s/.%s", home, appname);
	for(i = (int)(strlen(home) + 2); path[i]; i++)
		path[i] = (char)tolower(path[i]);
#endif

	return 0;
#endif
}

int fs_makedir_rec_for(const char *path)
{
	char buffer[1024*2];
	char *p;
	str_copy(buffer, path, sizeof(buffer));
	for(p = buffer+1; *p != '\0'; p++)
	{
		if(*p == '/' && *(p + 1) != '\0')
		{
			*p = '\0';
			if(fs_makedir(buffer) < 0)
				return -1;
			*p = '/';
		}
	}
	return 0;
}

int fs_makedir(const char *path)
{
#if defined(CONF_FAMILY_WINDOWS)
	if(_mkdir(path) == 0)
			return 0;
	if(errno == EEXIST)
		return 0;
	return -1;
#else
	if(mkdir(path, 0755) == 0)
		return 0;
	if(errno == EEXIST)
		return 0;
	return -1;
#endif
}

int fs_is_dir(const char *path)
{
#if defined(CONF_FAMILY_WINDOWS)
	/* TODO: do this smarter */
	WIN32_FIND_DATA finddata;
	HANDLE handle;
	char buffer[1024*2];
	str_format(buffer, sizeof(buffer), "%s/*", path);

	if ((handle = FindFirstFileA(buffer, &finddata)) == INVALID_HANDLE_VALUE)
		return 0;

	FindClose(handle);
	return 1;
#else
	struct stat sb;
	if (stat(path, &sb) == -1)
		return 0;

	if (S_ISDIR(sb.st_mode))
		return 1;
	else
		return 0;
#endif
}

int fs_exists(const char *path)
{
	struct stat sb;
	return stat(path, &sb) == 0;
}

time_t fs_getmtime(const char *path)
{
	struct stat sb;
	if (stat(path, &sb) == -1)
		return 0;

	return sb.st_mtime;
}

int fs_chdir(const char *path)
{
	if(fs_is_dir(path))
	{
		if(chdir(path))
			return 1;
		else
			return 0;
	}
	else
		return 1;
}

char *fs_getcwd(char *buffer, int buffer_size)
{
	if(buffer == 0)
		return 0;
#if defined(CONF_FAMILY_WINDOWS)
	return _getcwd(buffer, buffer_size);
#else
	return getcwd(buffer, buffer_size);
#endif
}

int fs_parent_dir(char *path)
{
	char *parent = 0;
	for(; *path; ++path)
	{
		if(*path == '/' || *path == '\\')
			parent = path;
	}

	if(parent)
	{
		*parent = 0;
		return 0;
	}
	return 1;
}

int fs_remove(const char *filename)
{
	if(remove(filename) != 0)
		return 1;
	return 0;
}

int fs_rename(const char *oldname, const char *newname)
{
#if defined(CONF_FAMILY_WINDOWS)
	if(MoveFileEx(oldname, newname, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0)
		return 1;
#else
	if(rename(oldname, newname) != 0)
		return 1;
#endif
	return 0;
}

int fs_compare(const char *a, const char *b)
{
#if defined(CONF_FAMILY_UNIX)
	return str_comp(a, b);
#elif defined(CONF_FAMILY_WINDOWS)
	return str_comp_nocase(a, b);
#endif
}

int fs_compare_num(const char *a, const char *b, int num)
{
#if defined(CONF_FAMILY_UNIX)
	return str_comp_num(a, b, num);
#elif defined(CONF_FAMILY_WINDOWS)
	return str_comp_nocase_num(a, b, num);
#endif
}



void swap_endian(void *data, unsigned elem_size, unsigned num)
{
	char *src = (char*) data;
	char *dst = src + (elem_size - 1);

	while(num)
	{
		unsigned n = elem_size>>1;
		char tmp;
		while(n)
		{
			tmp = *src;
			*src = *dst;
			*dst = tmp;

			src++;
			dst--;
			n--;
		}

		src = src + (elem_size>>1);
		dst = src + (elem_size - 1);
		num--;
	}
}

int net_socket_read_wait(NETSOCKET sock, int time)
{
	struct timeval tv;
	fd_set readfds;
	int sockid;

	tv.tv_sec = time / 1000000;
	tv.tv_usec = time % 1000000;
	sockid = 0;

	FD_ZERO(&readfds);
	if(sock.ipv4sock >= 0)
	{
		FD_SET(sock.ipv4sock, &readfds);
		sockid = sock.ipv4sock;
	}
	if(sock.ipv6sock >= 0)
	{
		FD_SET(sock.ipv6sock, &readfds);
		if(sock.ipv6sock > sockid)
			sockid = sock.ipv6sock;
	}
#if defined(CONF_WEBSOCKETS)
	if(sock.web_ipv4sock >= 0)
	{
		int maxfd = websocket_fd_set(sock.web_ipv4sock, &readfds);
		if (maxfd > sockid)
			sockid = maxfd;
	}
#endif

	/* don't care about writefds and exceptfds */
	if(time < 0)
		select(sockid+1, &readfds, NULL, NULL, NULL);
	else
		select(sockid+1, &readfds, NULL, NULL, &tv);

	if(sock.ipv4sock >= 0 && FD_ISSET(sock.ipv4sock, &readfds))
		return 1;

	if(sock.ipv6sock >= 0 && FD_ISSET(sock.ipv6sock, &readfds))
		return 1;

	return 0;
}

int64 time_timestamp()
{
	return time(0);
}

void str_append(char *dst, const char *src, int dst_size)
{
	int s = (int)strlen(dst);
	int i = 0;
	while(s < dst_size)
	{
		dst[s] = src[i];
		if(!src[i]) /* check for null termination */
			break;
		s++;
		i++;
	}

	dst[dst_size-1] = 0; /* assure null termination */
}

void str_copy(char *dst, const char *src, int dst_size)
{
	strncpy(dst, src, dst_size);
	dst[dst_size-1] = 0; /* assure null termination */
}

int str_length(const char *str)
{
#ifndef CONF_DEBUG
	if(!str) return 0;
#endif
	return (int)strlen(str);
}

int str_format(char *buffer, int buffer_size, const char *format, ...)
{
	int ret;
#if defined(CONF_FAMILY_WINDOWS)
	va_list ap;
	va_start(ap, format);
	ret = _vsnprintf(buffer, buffer_size, format, ap);
	va_end(ap);
#else
	va_list ap;
	va_start(ap, format);
	ret = vsnprintf(buffer, buffer_size, format, ap);
	va_end(ap);
#endif

	buffer[buffer_size-1] = 0; /* assure null termination */
	return ret;
}

char *str_trim_words(char *str, int words)
{
	while (words && *str)
	{
		if (isspace(*str) && !isspace(*(str + 1)))
			words--;
		str++;
	}
	return str;
}

/* replaces a single character within a string */
int str_replace_char(char *str_in, char find, char replace)
{
	int i, counter = 0;
	for(i = 0; i < str_length(str_in); i++)
	{
		if(str_in[i] == find)
		{
			str_in[i] = replace;
			counter++;
		}
	}
	return counter;
}

int str_replace_char_num(char *str_in, int max_replace, char find, char replace)
{
	int i, counter = 0;
	for(i = 0; i < str_length(str_in); i++)
	{
		if(str_in[i] == find)
		{
			str_in[i] = replace;
			if(max_replace >= 0 && ++counter >= max_replace)
				break;
		}
	}
	return counter;
}

int str_replace_char_rev_num(char *str_in, int max_replace, char find, char replace)
{
	int i, counter = 0;
	for(i = str_length(str_in)-1; i >= 0; i--)
	{
		if(str_in[i] == find)
		{
			str_in[i] = replace;
			if(max_replace >= 0 && ++counter >= max_replace)
				break;
		}
	}
	return counter;
}


/* makes sure that the string only contains the characters between 48-57, 65-95 & 97-122 */
void str_irc_sanitize(char *str_in)
{
	unsigned char *str = (unsigned char *)str_in;
	while(*str)
	{
		if(*str == 32 || (!(*str >= 65 && *str <= 95) && !(*str >= 97 && *str <= 122) && !(*str >= 48 && *str <= 57)))
			*str = 95; // '_'
		str++;
	}
}

/* makes sure that the string only contains the characters between 32 and 127 */
void str_sanitize_strong(char *str_in)
{
	unsigned char *str = (unsigned char *)str_in;
	while(*str)
	{
		*str &= 0x7f;
		if(*str < 32)
			*str = 32;
		str++;
	}
}

/* makes sure that the string only contains the characters between 32 and 255 */
void str_sanitize_cc(char *str_in)
{
	unsigned char *str = (unsigned char *)str_in;
	while(*str)
	{
		if(*str < 32)
			*str = ' ';
		str++;
	}
}

/* makes sure that the string only contains the characters between 32 and 255 + \r\n\t */
void str_sanitize(char *str_in)
{
	unsigned char *str = (unsigned char *)str_in;
	while(*str)
	{
		if(*str < 32 && !(*str == '\r') && !(*str == '\n') && !(*str == '\t'))
			*str = ' ';
		str++;
	}
}

int str_count_char(char *str, size_t size, char c)
{
	size_t i; int count = 0;
	for(i = 0; i < size && str[i] != '\0'; i++)
	{
		if(str[i] == c)
			count++;
	}
	return count;
}

char *str_skip_to_whitespace(char *str)
{
	while(*str && (*str != ' ' && *str != '\t' && *str != '\n'))
		str++;
	return str;
}

char *str_strip_right(char *str, const char *strip)
{
	int i;
	char *c = str + str_length(str)-1;
	while(c >= str)
	{
		for(i = 0; i < str_length(strip); i++)
			if(*c == strip[i])
			{
				*c = 0;
				break; // ...the for loop
			}
		if(*(c--) != '\0') // if the current letter hasn't been changed, there is no need to proceed further
			break;
	}
	return str;
}

char *str_strip_right_whitespaces(char *str)
{
	str_strip_right(str, " \t\n\r");
	return str;
}

char *str_skip_whitespaces(char *str)
{
	while(*str && (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r'))
		str++;
	return str;
}

const char *str_skip_whitespaces_const(const char *str)
{
	while(*str && (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r'))
		str++;
	return str;
}

char *str_split(char *dst, const char *str, int split, char dilem)
{
	char splits[512][256];
	mem_zerob(splits);

	int cmd = 0;
	int char_ = 0;

	unsigned int i;
	for (i = 0; i < str_length(str); i++)
	{
		if (str[i] == dilem)
		{
			cmd++;
			char_ = 0;
			continue;
		}

		splits[cmd][char_] = str[i];
		char_++;
	}

	str_copy(dst, splits[split], str_length(splits[split])+1);
	return dst;
}

/* case */
int str_comp_nocase(const char *a, const char *b)
{
#if defined(CONF_FAMILY_WINDOWS)
	return _stricmp(a,b);
#else
	return strcasecmp(a,b);
#endif
}

int str_comp_nocase_num(const char *a, const char *b, const int num)
{
#if defined(CONF_FAMILY_WINDOWS)
	return _strnicmp(a, b, num);
#else
	return strncasecmp(a, b, num);
#endif
}

int str_comp(const char *a, const char *b)
{
	return strcmp(a, b);
}

int str_comp_num(const char *a, const char *b, const int num)
{
	return strncmp(a, b, num);
}

int str_comp_filenames(const char *a, const char *b)
{
	int result;

	for(; *a && *b; ++a, ++b)
	{
		if(*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9')
		{
			result = 0;
			do
			{
				if(!result)
					result = *a - *b;
				++a; ++b;
			}
			while(*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9');

			if(*a >= '0' && *a <= '9')
				return 1;
			else if(*b >= '0' && *b <= '9')
				return -1;
			else if(result)
				return result;
		}

		if(*a != *b)
			break;
	}
	return *a - *b;
}

const char *str_find_nocase(const char *haystack, const char *needle)
{
	while(*haystack) /* native implementation */
	{
		const char *a = haystack;
		const char *b = needle;
		while(*a && *b && tolower(*a) == tolower(*b))
		{
			a++;
			b++;
		}
		if(!(*b))
			return haystack;
		haystack++;
	}

	return 0;
}


const char *str_find(const char *haystack, const char *needle)
{
	while(*haystack) /* native implementation */
	{
		const char *a = haystack;
		const char *b = needle;
		while(*a && *b && *a == *b)
		{
			a++;
			b++;
		}
		if(!(*b))
			return haystack;
		haystack++;
	}

	return 0;
}

const char *str_find_rev(const char *haystack, const char *needle)
{
	haystack = haystack + str_length(haystack) - 1;
	while(*haystack) /* native implementation */
	{
		const char *a = haystack;
		const char *b = needle;
		while(*a && *b && *a == *b)
		{
			a++;
			b++;
		}
		if(!(*b))
			return haystack + 1;
		haystack--;
	}

	return 0;
}

void str_hex(char *dst, int dst_size, const void *data, int data_size)
{
	static const char hex[] = "0123456789ABCDEF";
	int b;

	for(b = 0; b < data_size && b < dst_size/4-4; b++)
	{
		dst[b*3] = hex[((const unsigned char *)data)[b]>>4];
		dst[b*3+1] = hex[((const unsigned char *)data)[b]&0xf];
		dst[b*3+2] = ' ';
		dst[b*3+3] = 0;
	}
}
void str_hex_simple(char *dst, int dst_size, const unsigned char *data, int data_size)
{
	int i;
	mem_zero(dst, sizeof(dst));
	for(i = 0; i < data_size; i++)
	{
		char buf[3];
		str_format(buf, sizeof(buf), "%02x", data[i]);
		str_append(dst, buf, dst_size);
	}
}

static int hexval(char x)
{
    switch(x)
    {
    case '0': return 0;
    case '1': return 1;
    case '2': return 2;
    case '3': return 3;
    case '4': return 4;
    case '5': return 5;
    case '6': return 6;
    case '7': return 7;
    case '8': return 8;
    case '9': return 9;
    case 'a':
    case 'A': return 10;
    case 'b':
    case 'B': return 11;
    case 'c':
    case 'C': return 12;
    case 'd':
    case 'D': return 13;
    case 'e':
    case 'E': return 14;
    case 'f':
    case 'F': return 15;
    default: return -1;
    }
}

static int byteval(const char *byte, unsigned char *dst)
{
	int v1 = -1, v2 = -1;
	v1 = hexval(byte[0]);
	v2 = hexval(byte[1]);

	if(v1 < 0 || v2 < 0)
		return 1;

	*dst = v1 * 16 + v2;
	return 0;
}

int str_hex_decode(unsigned char *dst, int dst_size, const char *src)
{
	int len = str_length(src)/2;
	int i;
	if(len != dst_size)
		return 2;

	for(i = 0; i < len && dst_size; i++, dst_size--)
	{
		if(byteval(src + i * 2, dst++))
			return 1;
	}
	return 0;
}

void str_timestamp_ex(time_t time_data, char *buffer, unsigned int buffer_size, const char *format)
{
	struct tm *time_info;

	time_info = localtime(&time_data);
	strftime(buffer, buffer_size, format, time_info);
	buffer[buffer_size-1] = 0;	/* assure null termination */
}

void str_timestamp_format(char *buffer, int buffer_size, const char *format)
{
	time_t time_data;
	time(&time_data);
	str_timestamp_ex(time_data, buffer, buffer_size, format);
}

void str_timestamp(char *buffer, unsigned int buffer_size)
{
	time_t time_data;
	time(&time_data);
	str_timestamp_ex(time_data, buffer, buffer_size, "%Y-%m-%d_%H-%M-%S");
}

void str_clock_sec_impl(char *buffer, unsigned buffer_size, int time, const char *pLocalizeDay, const char *pLocalizeDays)
{
	// render the time in a nice format
	int negative = time < 0;
	if(negative)
		time = -time;
	if(time >= 60*60*24) // 60sec x 60min x 24h = 1 day
		str_format(buffer, buffer_size, "%s%d %s, %02d:%02d:%02d", negative ? "-" : "", time/60/60/24, time/60/60/24 == 1 ? pLocalizeDay : pLocalizeDays, (time%86400)/3600, (time/60)%60, (time)%60);
	else if(time >= 60*60) // 60sec x 60 min = 1 hour
		str_format(buffer, buffer_size, "%s%02d:%02d:%02d", negative ? "-" : "", time/60/60, (time/60)%60, time%60);
	else // only min:sec
		str_format(buffer, buffer_size, "%s%02d:%02d", negative ? "-" : "", time/60, time%60);
}


const char *str_next_word(char *str, char delim, char *buf, int *cursor)
{
	int i;

	if(str[*cursor] == '\0')
		return NULL;

	for(i = *cursor; ; ++i)
	{
		if(str[i] == delim || str[i] == '\0')
		{
			str_copy(buf, str + *cursor, i - *cursor);
			*cursor = i + 1;
			return buf;
		}
	}
}

void str_escape(char **dst, const char *src, const char *end)
{
	while(*src && *dst + 1 < end)
	{
		if(*src == '"' || *src == '\\') // escape \ and "
		{
			if(*dst + 2 < end)
				*(*dst)++ = '\\';
			else
				break;
		}
		*(*dst)++ = *src++;
	}
	**dst = 0;
}

void str_strip_path_and_extension(const char *filename, char *dst, int dst_size)
{
	const char *pFilenameEnd = filename + str_length(filename);
	const char *pExtractedName = filename;
	const char *pEnd = pFilenameEnd;
	const char *pIter;
	for(pIter = filename; *pIter; pIter++)
	{
		if(*pIter == '/' || *pIter == '\\')
		{
			pExtractedName = pIter + 1;
			pEnd = pFilenameEnd;
		}
		else if(*pIter == '.')
		{
			pEnd = pIter;
		}
	}

	int Length = (int)(pEnd - pExtractedName + 1);
	if(Length > dst_size) Length = dst_size;
	str_copy(dst, pExtractedName, Length);
}


int mem_comp(const void *a, const void *b, unsigned int size)
{
	return memcmp(a,b, size);
}

const MEMSTATS *mem_stats()
{
	return &memory_stats;
}

void net_stats(NETSTATS *stats_inout)
{
	*stats_inout = network_stats;
}

void gui_messagebox(const char *title, const char *message)
{
#if defined(CONF_PLATFORM_MACOSX)
	DialogRef theItem;
	DialogItemIndex itemIndex;

	/* FIXME: really needed? can we rely on glfw? */
	/* HACK - get events without a bundle */
	ProcessSerialNumber psn;
	GetCurrentProcess(&psn);
	TransformProcessType(&psn,kProcessTransformToForegroundApplication);
	SetFrontProcess(&psn);
	/* END HACK */

	CreateStandardAlert(kAlertStopAlert,
			CFStringCreateWithCString(NULL, title, kCFStringEncodingASCII),
			CFStringCreateWithCString(NULL, message, kCFStringEncodingASCII),
			NULL,
			&theItem);

	RunStandardAlert(theItem, NULL, &itemIndex);
#elif defined(CONF_FAMILY_UNIX)
	static char cmd[1024];
	int err;
	/* use xmessage which is available on nearly every X11 system */
	snprintf(cmd, sizeof(cmd), "xmessage -center -title \"%s\" \"%s\"",
		title,
		message);

//	dbg_msg("gui/msgbox", "command> %s", cmd);
	err = system(cmd);
	dbg_msg("gui/msgbox", "result = %i", err);
#elif defined(CONF_FAMILY_WINDOWS)
	MessageBox(NULL,
		message,
		title,
		MB_ICONEXCLAMATION | MB_OK);
#else
	/* this is not critical */
	#warning not implemented
#endif
}

int str_isspace(char c) { return c == ' ' || c == '\n' || c == '\t'; }

int str_isdigit(char c) { return c > 47 && c < 58; }

char str_uppercase(char c)
{
	if(c >= 'a' && c <= 'z')
		return (char)('A' + (c-'a'));
	return c;
}

int str_toint(const char *str) { return atoi(str); }
int str_toint_base(const char *str, int base) { return (int)strtol(str, NULL, base); }
unsigned long str_toulong_base(const char *str, int base) { return strtoul(str, NULL, base); }
float str_tofloat(const char *str) { return (float)atof(str); }


int str_utf8_isspace(int code)
{
	return code > 0x20 && code != 0xA0 && code != 0x034F && code != 0x2800 &&
		(code < 0x2000 || code > 0x200F) && (code < 0x2028 || code > 0x202F) &&
		(code < 0x205F || code > 0x2064) && (code < 0x206A || code > 0x206F) &&
		(code < 0xFE00 || code > 0xFE0F) && code != 0xFEFF &&
		(code < 0xFFF9 || code > 0xFFFC);
}

const char *str_utf8_skip_whitespaces(const char *str)
{
	const char *str_old;
	int code;

	while(*str)
	{
		str_old = str;
		code = str_utf8_decode(&str);

		// check if unicode is not empty
		if(str_utf8_isspace(code))
		{
			return str_old;
		}
	}

	return str;
}

int str_utf8_isstart(char c)
{
	if((c&0xC0) == 0x80) /* 10xxxxxx */
		return 0;
	return 1;
}

int str_utf8_rewind(const char *str, int cursor)
{
	while(cursor)
	{
		cursor--;
		if(str_utf8_isstart(*(str + cursor)))
			break;
	}
	return cursor;
}

int str_utf8_forward(const char *str, int cursor)
{
	const char *buf = str + cursor;
	if(!buf[0])
		return cursor;

	if((*buf&0x80) == 0x0)  /* 0xxxxxxx */
		return cursor+1;
	else if((*buf&0xE0) == 0xC0) /* 110xxxxx */
	{
		if(!buf[1]) return cursor+1;
		return cursor+2;
	}
	else  if((*buf & 0xF0) == 0xE0)	/* 1110xxxx */
	{
		if(!buf[1]) return cursor+1;
		if(!buf[2]) return cursor+2;
		return cursor+3;
	}
	else if((*buf & 0xF8) == 0xF0)	/* 11110xxx */
	{
		if(!buf[1]) return cursor+1;
		if(!buf[2]) return cursor+2;
		if(!buf[3]) return cursor+3;
		return cursor+4;
	}

	/* invalid */
	return cursor+1;
}

int str_utf8_encode(char *ptr, int chr)
{
	/* encode */
	if(chr <= 0x7F)
	{
		ptr[0] = (char)chr;
		return 1;
	}
	else if(chr <= 0x7FF)
	{
		ptr[0] = (char)(0xC0|((chr>>6)&0x1F));
		ptr[1] = (char)(0x80|(chr&0x3F));
		return 2;
	}
	else if(chr <= 0xFFFF)
	{
		ptr[0] = (char)(0xE0|((chr>>12)&0x0F));
		ptr[1] = (char)(0x80|((chr>>6)&0x3F));
		ptr[2] = (char)(0x80|(chr&0x3F));
		return 3;
	}
	else if(chr <= 0x10FFFF)
	{
		ptr[0] = (char)(0xF0|((chr>>18)&0x07));
		ptr[1] = (char)(0x80|((chr>>12)&0x3F));
		ptr[2] = (char)(0x80|((chr>>6)&0x3F));
		ptr[3] = (char)(0x80|(chr&0x3F));
		return 4;
	}

	return 0;
}

static unsigned char str_byte_next(const char **ptr)
{
	unsigned char byte = (unsigned char)**ptr;
	(*ptr)++;
	return byte;
}

static void str_byte_rewind(const char **ptr)
{
	(*ptr)--;
}

int str_utf8_decode(const char **ptr)
{
	// As per https://encoding.spec.whatwg.org/#utf-8-decoder.
	unsigned char utf8_lower_boundary = 0x80;
	unsigned char utf8_upper_boundary = 0xBF;
	int utf8_code_point = 0;
	int utf8_bytes_seen = 0;
	int utf8_bytes_needed = 0;
	while(1)
	{
		unsigned char byte = str_byte_next(ptr);
		if(utf8_bytes_needed == 0)
		{
			if(byte <= 0x7F)
			{
				return byte;
			}
			else if(0xC2 <= byte && byte <= 0xDF)
			{
				utf8_bytes_needed = 1;
				utf8_code_point = byte - 0xC0;
			}
			else if(0xE0 <= byte && byte <= 0xEF)
			{
				if(byte == 0xE0) utf8_lower_boundary = 0xA0;
				if(byte == 0xED) utf8_upper_boundary = 0x9F;
				utf8_bytes_needed = 2;
				utf8_code_point = byte - 0xE0;
			}
			else if(0xF0 <= byte && byte <= 0xF4)
			{
				if(byte == 0xF0) utf8_lower_boundary = 0x90;
				if(byte == 0xF4) utf8_upper_boundary = 0x8F;
				utf8_bytes_needed = 3;
				utf8_code_point = byte - 0xF0;
			}
			else
			{
				return -1; // Error.
			}
			utf8_code_point = utf8_code_point << (6 * utf8_bytes_needed);
			continue;
		}
		if(!(utf8_lower_boundary <= byte && byte <= utf8_upper_boundary))
		{
			// Resetting variables not necessary, will be done when
			// the function is called again.
			str_byte_rewind(ptr);
			return -1;
		}
		utf8_lower_boundary = 0x80;
		utf8_upper_boundary = 0xBF;
		utf8_bytes_seen += 1;
		utf8_code_point = utf8_code_point + ((byte - 0x80) << (6 * (utf8_bytes_needed - utf8_bytes_seen)));
		if(utf8_bytes_seen != utf8_bytes_needed)
		{
			continue;
		}
		// Resetting variables not necessary, see above.
		return utf8_code_point;
	}
}

int str_utf8_check(const char *str)
{
	int codepoint;
	while((codepoint = str_utf8_decode(&str)))
	{
		if(codepoint == -1)
		{
			return 0;
		}
	}
	return 1;
}


unsigned str_quickhash(const char *str)
{
	unsigned hash = 5381;
	for(; str && *str; str++)
		hash = ((hash << 5) + hash) + (*str); /* hash * 33 + c */
	return hash;
}

int pid()
{
#if defined(CONF_FAMILY_WINDOWS)
	return _getpid();
#else
	return getpid();
#endif
}

void shell_execute(const char *file)
{
#if defined(CONF_FAMILY_WINDOWS)
	ShellExecute(NULL, NULL, file, NULL, NULL, SW_SHOWDEFAULT);
#elif defined(CONF_FAMILY_UNIX)
	char *argv[2];
	pid_t pid;
	argv[0] = (char*) file;
	argv[1] = NULL;
	pid = fork();
	if(!pid)
		execv(file, argv);
#endif
}

int replace_process(const char **argv)
{
#if defined(CONF_FAMILY_WINDOWS)
	return _execv(argv[0], (char**)argv);
#elif defined(CONF_FAMILY_UNIX)
	return execv(argv[0], (char**)argv);
#else
	#error not implemented
#endif
}

int os_compare_version(unsigned int major, unsigned int minor)
{
#if defined(CONF_FAMILY_WINDOWS)
	OSVERSIONINFO ver;
	mem_zero(&ver, sizeof(OSVERSIONINFO));
	ver.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
	GetVersionEx(&ver);
	if(ver.dwMajorVersion > major || (ver.dwMajorVersion == major && ver.dwMinorVersion > minor))
		return 1;
	else if(ver.dwMajorVersion == major && ver.dwMinorVersion == minor)
		return 0;
	else
		return -1;
#else
	return 0; // unimplemented
#endif
}

struct SECURE_RANDOM_DATA
{
	int initialized;
#if defined(CONF_FAMILY_WINDOWS)
	HCRYPTPROV provider;
#else
	IOHANDLE urandom;
#endif
};

static struct SECURE_RANDOM_DATA secure_random_data = { 0 };

int secure_random_init()
{
	if(secure_random_data.initialized)
	{
		return 0;
	}
#if defined(CONF_FAMILY_WINDOWS)
	if(CryptAcquireContext(&secure_random_data.provider, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
	{
		secure_random_data.initialized = 1;
		return 0;
	}
	else
	{
		return 1;
	}
#else
	secure_random_data.urandom = io_open("/dev/urandom", IOFLAG_READ);
	if(secure_random_data.urandom)
	{
		secure_random_data.initialized = 1;
		return 0;
	}
	else
	{
		return 1;
	}
#endif
}

void generate_password(char *buffer, unsigned length, unsigned short *random, unsigned random_length)
{
	static const char VALUES[] = "ABCDEFGHKLMNPRSTUVWXYZabcdefghjkmnopqt23456789";
	static const size_t NUM_VALUES = sizeof(VALUES) - 1; // Disregard the '\0'.
	unsigned i;
	dbg_assert_legacy(length >= random_length * 2 + 1, "too small buffer");
	dbg_assert_legacy(NUM_VALUES * NUM_VALUES >= 2048, "need at least 2048 possibilities for 2-character sequences");

	buffer[random_length * 2] = 0;

	for(i = 0; i < random_length; i++)
	{
		unsigned short random_number = random[i] % 2048;
		buffer[2 * i + 0] = VALUES[random_number / NUM_VALUES];
		buffer[2 * i + 1] = VALUES[random_number % NUM_VALUES];
	}
}

#define MAX_PASSWORD_LENGTH 128

void secure_random_password(char *buffer, unsigned length, unsigned pw_length)
{
	unsigned short random[MAX_PASSWORD_LENGTH / 2];
	// With 6 characters, we get a password entropy of log(2048) * 6/2 = 33bit.
	dbg_assert_legacy(length >= pw_length + 1, "too small buffer");
	dbg_assert_legacy(pw_length >= 6, "too small password length");
	dbg_assert_legacy(pw_length % 2 == 0, "need an even password length");
	dbg_assert_legacy(pw_length <= MAX_PASSWORD_LENGTH, "too large password length");

	secure_random_fill(random, pw_length);

	generate_password(buffer, length, random, pw_length / 2);
}

#undef MAX_PASSWORD_LENGTH

void secure_random_fill(void *bytes, unsigned length)
{
	if(!secure_random_data.initialized)
	{
		dbg_msg("secure", "called secure_random_fill before secure_random_init");
		dbg_abort();
	}
#if defined(CONF_FAMILY_WINDOWS)
	if(!CryptGenRandom(secure_random_data.provider, length, bytes))
	{
		dbg_msg("secure", "CryptGenRandom failed, last_error=%lu", GetLastError());
		dbg_break();
	}
#else
	if(length != io_read(secure_random_data.urandom, bytes, length))
	{
		dbg_msg("secure", "io_read returned with a short read");
		dbg_break();
	}
#endif
}

MD5_HASH md5_simple(unsigned char *data, unsigned data_size)
{
	MD5_HASH result;
	mem_zero(&result, sizeof(result));

	md5_state_t md;

	md5_init(&md);
	md5_append(&md, data, data_size);
	md5_finish(&md, (md5_byte_t*)result.digest);

	return result;
}

int secure_rand()
{
	unsigned int i;
	secure_random_fill(&i, sizeof(i));
	return (int)(i%RAND_MAX);
}

unsigned secure_rand_u()
{
	unsigned int i;
	secure_random_fill(&i, sizeof(i));
	return (i%RAND_MAX);
}

uint8_t *str_aes128_encrypt(const char *str, const AES128_KEY *key, unsigned *output_size, AES128_IV *out_iv)
{
	int i;
	int str_len = str_length(str);
	int padded_len = str_len - str_len%16 + 16; // must be a multiple of 16
	*output_size = (unsigned int)((padded_len / 16 + 1) * 16) - 16;

	// copy the string and add padding
	uint8_t *input_buffer = mem_allocb(uint8_t, padded_len);
	for(i = 0; i < padded_len; i++)
	{
		if(i < str_len)
			input_buffer[i] = (uint8_t)str[i];
		else
			input_buffer[i] = (uint8_t)' '; // pad with whitespaces; they can easily be stripped
	}

	// prepare the initial vector (iv)
	uint8_t iv_start = (uint8_t)((secure_rand_u() % (0xFF - 0x11)) + 0x10);
	for(i = 0; i < 16; i++)
	{
		out_iv->iv[i] = (uint8_t)(iv_start+i);
	}

	// allocate the output buffer
	*output_size += 1;
	uint8_t *output_buffer = mem_allocb(uint8_t, *output_size);

	output_buffer[0] = iv_start;

	// set it off!
	AES128_CBC_encrypt_buffer(output_buffer+1, input_buffer, (uint32_t)padded_len, key->key, out_iv->iv);

	mem_free(input_buffer);

	return output_buffer;
}

char *str_aes128_decrypt(uint8_t *data, unsigned data_size, const AES128_KEY *key, char *buffer, unsigned buffer_size, AES128_IV *out_iv)
{
	int i;

	// reconstruct the iv
	uint8_t iv_start = data[0];
	for(i = 0; i < 16; i++)
	{
		out_iv->iv[i] = (uint8_t)(iv_start+i);
	}

	uint8_t *output_buffer = mem_allocb(uint8_t, buffer_size);
	mem_zero(output_buffer, buffer_size);
	AES128_CBC_decrypt_buffer(output_buffer, data+1, data_size-1, key->key, out_iv->iv);

	// convert back to a string
	mem_zero(buffer, buffer_size);
	for(i = 0; i < data_size && i < buffer_size; i++)
	{
		buffer[i] = (char)output_buffer[i];
	}

	mem_free(output_buffer);

	return str_strip_right_whitespaces(buffer);
}

void open_default_browser(const char *url)
{
	if (!url || url[0] == 0)
		return;

	// only read the first string before whitespace for prevent injection
	char aUrl[256];
	str_copy(aUrl, url, sizeof(aUrl));
	str_replace_char_num(aUrl, 1, ' ', '\0');

	// make sure the string is sane
	{
		int i = 0;
		for(; i < str_length(aUrl); i++)
			if(aUrl[i] == ';' || aUrl[i] == '$' || aUrl[i] == '`')
				aUrl[i] = '\0';
	}

#if defined(CONF_FAMILY_WINDOWS)
	ShellExecuteA(NULL, "open", aUrl, NULL, NULL, SW_SHOWNORMAL);
#elif defined(CONF_PLATFORM_MACOSX)
	CFURLRef cfurl = CFURLCreateWithBytes(NULL, (UInt8*)aUrl, str_length(aUrl), kCFStringEncodingASCII, NULL);
	LSOpenCFURLRef(cfurl, 0);
	CFRelease(cfurl);
#elif defined(CONF_PLATFORM_LINUX)
	//g_app_info_launch_default_for_uri(url, NULL, NULL);
	if (fork() == 0)
		execlp("xdg-open", "xdg-open", aUrl, NULL); // FIXME: Really dangerous, can crash if xdg-open doesn't exists :S
#endif
}

#if defined(__cplusplus)
}
#endif
