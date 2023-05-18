/******************************************************************************
 * Copyright (c) Huawei Technologies Co., Ltd. 2023. All rights reserved.
 * qtfs licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 * http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v2 for more details.
 * Author: Liqiang
 * Create: 2023-03-20
 * Description: 
 *******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <netinet/ip.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <glib.h>
#include <malloc.h>
#include <errno.h>

#include <sys/epoll.h>

#include "comm.h"
#include "ipc/uds_main.h"

char wl_type_str[QTFS_WHITELIST_MAX][10] = {"Open", "Write", "Read", "Readdir", "Mkdir", "Rmdir", "Create", "Unlink", "Rename", "Setattr", "Setxattr", "Mount"};

#define engine_out(info, ...) \
	do {\
		printf("[Engine::%s:%3d]"info"\n", __func__, __LINE__, ##__VA_ARGS__);\
	} while (0);

#define engine_out2(info, ...) \
		do {\
			printf("[Engine::%s:%3d]"info, __func__, __LINE__, ##__VA_ARGS__);\
		} while (0);

#define engine_err(info, ...) \
	do {\
		printf("[ERROR:Engine::%s:%3d]"info"\n", __func__, __LINE__, ##__VA_ARGS__);\
	} while (0);

#define WHITELIST_FILE "/etc/qtfs/whitelist"

struct whitelist *whitelist[QTFS_WHITELIST_MAX] = {0};

struct engine_arg {
	int psize;
	int fd;
	int thread_idx;
};

#define QTFS_USERP_MAXSIZE 65536
#define QTFS_USERP_SIZE QTFS_USERP_MAXSIZE
#define QTFS_SERVER_FILE "/dev/qtfs_server"

int qtfs_fd;
int engine_run = 1;
static void qtfs_engine_userp_free(struct qtfs_server_userp_s *userp, int thread_nums)
{
	for (int i = 0; i < thread_nums; i++) {
		if (userp[i].userp != NULL) 
			free(userp[i].userp);
		if (userp[i].userp2 != NULL) 
			free(userp[i].userp2);
	}
	free(userp);
	return;
}

static struct qtfs_server_userp_s *qtfs_engine_thread_init(int fd, int thread_nums, int psize)
{
	struct qtfs_server_userp_s *userp;
	userp = (struct qtfs_server_userp_s *)calloc(thread_nums, sizeof(struct qtfs_server_userp_s));
	if (userp == NULL) {
		engine_out("engine thread init malloc failed.");
		return NULL;
	}
	for (int i = 0; i < thread_nums; i++) {
		userp[i].size = psize;
		userp[i].userp = (void *)memalign(sysconf(_SC_PAGESIZE), psize);
		if (userp[i].userp == NULL) {
			engine_out("engine userp malloc failed.");
			goto rollback;
		}
		userp[i].userp2 = (void *)memalign(sysconf(_SC_PAGESIZE), psize);
		if (userp[i].userp2 == NULL) {
			engine_out("engine userp2 malloc failed.");
			goto rollback;
		}
	}
	struct qtfs_thread_init_s init_userp;
	init_userp.thread_nums = thread_nums;
	init_userp.userp = userp;
	(void)ioctl(fd, QTFS_IOCTL_THREAD_INIT, (unsigned long)&init_userp);
	return userp;
rollback:
	qtfs_engine_userp_free(userp, thread_nums);
	return NULL;
}

static void *qtfs_engine_kthread(void *arg)
{
	struct engine_arg *parg = (struct engine_arg *)arg;
	int psize = parg->psize;
	long ret;

	while (engine_run) {
		ret = ioctl(parg->fd, QTFS_IOCTL_THREAD_RUN, 0);
		if (ret == QTEXIT) {
			engine_out("qtfs server thread:%d exit.", parg->thread_idx);
			break;
		}
	}

end:
	engine_out("qtfs user engine over.");
	return NULL;
}

static void qtfs_signal_int(int signum)
{
	engine_out("qtfs engine recv signal number:%d.", signum);

	if (qtfs_fd < 0) {
		engine_err("qtfs engine signal int file:%s open failed, fd:%d.", QTFS_SERVER_FILE, qtfs_fd);
		return;
	}
	long ret = ioctl(qtfs_fd, QTFS_IOCTL_EXIT, 0);
	engine_out("qtfs engine send QTFS_IOCTL_EXIT to kernel, get return value:%d.", ret);
	engine_run = 0;

	return;
}

static void *qtfs_engine_epoll_thread(void *data)
{
	struct engine_arg *arg = (struct engine_arg *)data;
	int fd = arg->fd;
	long ret;

	engine_out("qtfs server epoll thread run.");

	do {
		ret = ioctl(fd, QTFS_IOCTL_EPOLL_THREAD_INIT, NULL);
		if (ret == QTEXIT) {
			engine_out("qtfs server epoll thread init exit.");
			goto end;
		}
	} while (ret != 0 && engine_run);
	while (engine_run) {
		ret = ioctl(fd, QTFS_IOCTL_EPOLL_THREAD_RUN, NULL);
		if (ret == QTEXIT) {
			engine_out("qtfs server epoll thread exit.");
			break;
		}
	}
end:
	engine_out("qtfs server epoll thread exit, ret:%d.", ret);

	return NULL;
}

int qtfs_epoll_init(int fd)
{
	int epfd = epoll_create1(0);
	if (epfd < 0) {
		engine_err("epoll create error, ret:%d.", epfd);
		return -1;
	}

	struct qtfs_server_epoll_s ep;
	struct epoll_event *evts;
	evts = calloc(QTFS_MAX_EPEVENTS_NUM, sizeof(struct epoll_event));
	if (evts == NULL) {
		engine_err("calloc events failed.");
		close(epfd);
		return -1;
	}
	engine_out("qtfs engine set epoll arg, fd:%d event nums:%d.", epfd, QTFS_MAX_EPEVENTS_NUM);
	ep.epfd = epfd;
	ep.event_nums = QTFS_MAX_EPEVENTS_NUM;
	ep.events = evts;
	int ret = ioctl(fd, QTFS_IOCTL_EPFDSET, &ep);
	if (ret != 0) {
		engine_err("failed to set epoll fd, ret:%d.", ret);
		close(epfd);
		return -1;
	}

	return epfd;
}

static int qtfs_whitelist_transfer(int fd, GKeyFile *config, int type)
{
	int64_t i, wl_count;
	int ret;
	char **items;
	struct whitelist *whitelist = NULL;
	if (type >= QTFS_WHITELIST_MAX) {
		engine_err("whitelist item index out of range:%d, no more than %d.", type, QTFS_WHITELIST_MAX);
		return -1;
	}
	items = g_key_file_get_string_list(config, wl_type_str[type], "Path", &wl_count, NULL);
	if (wl_count <= 0) {
		engine_err("Can't find whitelist item %s", wl_type_str[type]);
		return -1;
	}
	if (wl_count > 64)
		wl_count = 64;
	whitelist = (struct whitelist *)malloc(sizeof(struct whitelist) + sizeof(struct wl_item) * wl_count);
	if (!whitelist) {
		engine_err("alloc memory for whitelist failed");
		ret = -ENOMEM;
		goto end;
	}
	memset(whitelist, 0, sizeof(struct whitelist) + sizeof(struct wl_item) * wl_count);
	engine_out("%s:\n", wl_type_str[type]);
	whitelist->len = wl_count;
	whitelist->type = type;
	for(i = 0; i < wl_count; i++){
		engine_out("%s\n", items[i]);
		whitelist->wl[i].len = strlen(items[i]);
		if (strlen(items[i]) >= sizeof(whitelist->wl[i].path)) {
			engine_err("item[%s] too long. should less than %d", items[i], sizeof(whitelist->wl[i].path));
			ret = -1;
			goto end;
		}
		strncpy(whitelist->wl[i].path, items[i], sizeof(whitelist->wl[i].path));
	}
	ret = ioctl(fd, QTFS_IOCTL_WHITELIST, whitelist);
	if (ret != 0) {
		engine_err("Can't set whitelist item %s", wl_type_str[type]);
	}
end:
	if (whitelist)
		free(whitelist);
	for (int j = 0; i < wl_count; j++) {
		if (items[j])
			free(items[j]);
	}
	if (items)
		free(items);
	// whitelist will be free in qtfs_whitelist_init
	return ret;
}

int qtfs_whitelist_init(int fd)
{
	int ret, i;
	GKeyFile *config = g_key_file_new();
	g_key_file_load_from_file(config, WHITELIST_FILE, G_KEY_FILE_KEEP_COMMENTS|G_KEY_FILE_KEEP_TRANSLATIONS, NULL);
	for (i = 0; i < QTFS_WHITELIST_MAX; i++) {
		ret = qtfs_whitelist_transfer(fd, config, i);
		if (ret != 0) {
			engine_err("failed to set whitelist item %s, get error:%d", wl_type_str[i], ret);
			// failure of one whitelist type should not stop others.
			continue;
		}
	}
	g_key_file_free(config);
	return 0;
}

static int qtfs_engine_check_port(unsigned short port, char *ip)
{
	struct sockaddr_in sin;
	if (inet_pton(AF_INET, ip, &sin.sin_addr) != 1) {
		engine_err("%s inet_pton error.", ip);
		return -1;
	}
	int sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) {
		engine_err("socket error, fd:%", sockfd);
		return -1;
	}
	bzero(&sin, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	if (bind(sockfd, (struct sockaddr *)&sin, sizeof(struct sockaddr)) < 0) {
		engine_err("ip:%s port:%u bind failed, errno:%d.", ip, port, errno);
		close(sockfd);
		return -1;
	}
	close(sockfd);
	return 0;
}

#define IS_NUMBER(c) (c >= '0' && c <= '9')
static int qtfs_engine_env_check(char *argv[])
{
	struct qtinfo diag;
	int ret;
	int fd;
	for (int i = 0; i < strlen(argv[1]); i ++) {
		if (!IS_NUMBER(argv[1][i])) {
			engine_err("invalid thread number :%s", argv[1]);
			return -1;
		}
	}
	fd = open(QTFS_SERVER_DEV, O_RDONLY|O_NONBLOCK);
	if (fd < 0) {
		engine_err("not find server dev:%s", QTFS_SERVER_DEV);
		return -1;
	}
	ret = ioctl(fd, QTFS_IOCTL_ALLINFO, &diag);
	close(fd);
	if (ret != QTOK) {
		engine_err("check ioctl return failed:%d", ret);
		return -1;
	}

	if (qtfs_engine_check_port(diag.server_port, diag.server_ip) < 0)
		goto err;
	if (qtfs_engine_check_port(diag.server_port + 1, diag.server_ip) < 0)
		goto err;
	if (qtfs_engine_check_port(atoi(argv[4]), argv[3]) < 0)
		goto err;

	return 0;
err:
	return -1;
}

int main(int argc, char *argv[])
{
	int ret = 0;
	if (argc != 7) {
		engine_out("Usage: %s <number of threads> <uds proxy thread num> <host ip> <uds proxy port> <dpu ip> <uds proxy port>.", argv[0]);
		engine_out("     Example: %s 16 1 192.168.10.10 12121 192.168.10.11 12121.", argv[0]);
		return -1;
	}
	if (qtfs_engine_env_check(argv) < 0) {
		engine_err("Environment check failed, engine exit.");
		return -1;
	}
	int thread_nums = atoi(argv[1]);
	int fd = open(QTFS_SERVER_FILE, O_RDONLY);
	if (fd < 0) {
		engine_err("qtfs server file:%s open failed, fd:%d.", QTFS_SERVER_FILE, fd);
		return -1;
	}
	qtfs_fd = fd;
	// init epoll
	int epfd = qtfs_epoll_init(fd);
	if (epfd < 0) {
		close(fd);
		return -1;
	}
	ret = qtfs_whitelist_init(fd);
	if (ret)
		goto end;

	umask(0);

	pthread_t texec[QTFS_MAX_THREADS];
	pthread_t tepoll;
	if (thread_nums < 0 || thread_nums > QTFS_MAX_THREADS) {
		engine_err("qtfs engine parm invalid, thread_nums:%d(must <= %d).",
				thread_nums, QTFS_MAX_THREADS);
		ret = -1;
		goto end;
	}
	(void)ioctl(fd, QTFS_IOCTL_EXIT, 1);
	signal(SIGINT, qtfs_signal_int);
	signal(SIGKILL, qtfs_signal_int);
	signal(SIGTERM, qtfs_signal_int);

	struct qtfs_server_userp_s *userp = qtfs_engine_thread_init(fd, thread_nums, QTFS_USERP_SIZE);
	if (userp == NULL) {
		engine_out("qtfs engine userp init failed.");
		ret = -1;
		goto end;
	}
	struct engine_arg arg[QTFS_MAX_THREADS];
	for (int i = 0; i < thread_nums; i++) {
		arg[i].psize = QTFS_USERP_SIZE;
		arg[i].fd = fd;
		arg[i].thread_idx = i;
		(void)pthread_create(&texec[i], NULL, qtfs_engine_kthread, &arg[i]);
	}
	(void)pthread_create(&tepoll, NULL, qtfs_engine_epoll_thread, &arg[0]);
	// 必须放在这个位置，uds main里面最终也有join
	if (uds_proxy_main(6, &argv[1]) != 0) {
		engine_out("uds proxy start failed.");
		ret = -1;
		goto engine_free;
	}
	for (int i = 0; i < thread_nums; i++) {
		pthread_join(texec[i], NULL);
		engine_out("qtfs engine join thread %d.", i);
	}
	pthread_join(tepoll, NULL);
engine_free:
	qtfs_engine_userp_free(userp, thread_nums);
	engine_out("qtfs engine join epoll thread.");
end:
	close(epfd);
	close(fd);
	engine_out("qtfs engine over.");
	return ret;
}
