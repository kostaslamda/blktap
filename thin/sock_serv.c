#include <stdlib.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/queue.h> /* non POSIX */
#include <stdbool.h>
#include "blktap.h"
#include "payload.h"

#define BACKLOG 5

static inline int process_payload(int, struct payload *);
static inline int dummy_reply(int, struct payload *);
static int (*req_reply)(int , struct payload * );
static int handle_request(struct payload * buf);
static int handle_query(struct payload * buf);
static void * process_req(void *);
static int increase_size(off64_t size, const char * path);
static void parse_cmdline(int, char **);
static int do_daemon(void);
static int handle_cli(struct payload *);
static void split_command(char *, char **);
static int add_vg(char *vg);
static int del_vg(char *vg);

/* queue structures */
SIMPLEQ_HEAD(sqhead, sq_entry);
struct kpr_queue {
	struct sqhead qhead;
	pthread_mutex_t mtx;
	pthread_cond_t cnd;
} *def_in_queue, *out_queue;
struct sq_entry {
	struct payload data;
	SIMPLEQ_ENTRY(sq_entry) entries;
};

static struct sq_entry * find_and_remove(struct sqhead *, pid_t);

/* thread structures */
struct kpr_thread_info {
	pthread_t thr_id;
	struct kpr_queue *r_queue;
};

/* list structures */
LIST_HEAD(vg_list_head, vg_entry);
struct kpr_vg_list {
	struct vg_list_head head;
	pthread_mutex_t mtx;
} vg_pool;
struct vg_entry {
	char name[PAYLOAD_MAX_PATH_LENGTH];
	struct kpr_thread_info thr;
	struct kpr_queue *r_queue;
	LIST_ENTRY(vg_entry) entries;
};

static struct vg_entry * vg_pool_find(char *, bool);
static struct vg_entry * vg_pool_find_and_remove(char *);


int daemonize;


static struct kpr_queue *
alloc_init_queue(void)
{
	struct kpr_queue *sqp;

	sqp = malloc(sizeof(*sqp));
	if (sqp) {
		SIMPLEQ_INIT(&sqp->qhead);
		if (pthread_mutex_init(&sqp->mtx, NULL) != 0)
			goto out;
		if (pthread_cond_init(&sqp->cnd, NULL) != 0)
			goto out;
	}
	return sqp;

	out:
		free(sqp);
		return NULL;
}

int
main(int argc, char *argv[]) {

	/* Init pool */
	LIST_INIT(&vg_pool.head);
	if (pthread_mutex_init(&vg_pool.mtx, NULL) != 0)
		return 1;	

	/* Init default queues */
	def_in_queue = alloc_init_queue();
	if(!def_in_queue)
		return 1; /*no free: return from main */
	out_queue = alloc_init_queue();
	if(!out_queue)
		return 1; /*no free: return from main */

	daemonize = 0;

	/* accept command line opts */
	parse_cmdline(argc, argv);

	/* daemonize if required */
	if (do_daemon() == -1)
		return 1; /* can do better */

	/* prepare and spawn thread */
	struct kpr_thread_info thr_info;
	thr_info.r_queue = def_in_queue;
	if (pthread_create(&thr_info.thr_id, NULL, process_req, &thr_info)) {
		printf("failed worker thread creation\n");
		return 1;
	}


	struct sockaddr_un addr;
	int sfd, cfd;
	ssize_t numRead;
	struct payload buf;

	req_reply = dummy_reply;

	sfd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
	if (sfd == -1)
		return -errno;

	memset(&addr, 0, sizeof(struct sockaddr_un));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, THIN_CONTROL_SOCKET, sizeof(addr.sun_path) - 1);

	if (bind(sfd, (struct sockaddr *) &addr, sizeof(struct sockaddr_un)) == -1)
		return -errno;

	if (listen(sfd, BACKLOG) == -1)
		return -errno;

	for (;;) {

		cfd = accept4(sfd, NULL, NULL, SOCK_CLOEXEC);
		if (cfd == -1)
			return -errno;

		while ((numRead = read(cfd, &buf, sizeof(buf))) > 0)
			process_payload(cfd, &buf);

		if (numRead == -1)
			return -errno;

		if (close(cfd) == -1)
			return -errno;
	}
}

static inline int
process_payload(int fd, struct payload * buf)
{
	int err;

	print_payload(buf);
	err = req_reply(fd, buf);

	return err;
}

static int
dummy_reply(int fd, struct payload * buf)
{
	switch (buf->reply) {
	case PAYLOAD_REQUEST:
		handle_request(buf);
		break;
	case PAYLOAD_QUERY:
		handle_query(buf);
		break;
	case PAYLOAD_CLI:
		handle_cli(buf);
		break;
	default:
		buf->reply = PAYLOAD_UNDEF;
		print_payload(buf);
	}
	print_payload(buf);
	printf("EOM\n\n");

	/* TBD: very basic write, need a while loop */
	if (write(fd, buf, sizeof(*buf)) != sizeof(*buf))
		return -errno;

	return 0;
}

static int
handle_request(struct payload * buf)
{
	struct sq_entry * req;

	if (buf->reply != PAYLOAD_REQUEST)
		return 1;

	printf("I promise I will do something about it..\n");
	req = malloc(sizeof(struct sq_entry));
	req->data = *buf;
	buf->reply = PAYLOAD_ACCEPTED;
	pthread_mutex_lock(&def_in_queue->mtx);
	
	SIMPLEQ_INSERT_TAIL(&def_in_queue->qhead, req, entries);

	pthread_cond_signal(&def_in_queue->cnd);
	pthread_mutex_unlock(&def_in_queue->mtx);

	return 0;
}

static int
handle_query(struct payload * buf)
{
	struct sq_entry * req;

	if (buf->reply != PAYLOAD_QUERY)
		return 1;

	/* Check we have something ready */
	pthread_mutex_lock(&out_queue->mtx);
	if (SIMPLEQ_EMPTY(&out_queue->qhead)) {
		pthread_mutex_unlock(&out_queue->mtx);
		buf->reply = PAYLOAD_WAIT;
		return 0;
	}

	/* check if we have a served request for this query */
	req = find_and_remove(&out_queue->qhead, buf->id);
	if (req) {
		pthread_mutex_unlock(&out_queue->mtx);
		buf->reply = req->data.reply;
		free(req);
	} else { /* wait */
		pthread_mutex_unlock(&out_queue->mtx);
		buf->reply = PAYLOAD_WAIT;		
	}

	return 0;
}


static int
handle_cli(struct payload * buf)
{
	char command[PAYLOAD_MAX_PATH_LENGTH];
	char *cmd[2];
	int ret;

	/* we reuse the path field for CLI */
	strcpy(command, buf->path);

	split_command(command, cmd);
	if(!cmd[0])
		return 1;

	if (!strcmp("add", cmd[0])) {
		if(!cmd[1])
			return 1;
		ret = add_vg(cmd[1]);
		
	}
	else if (!strcmp("del", cmd[0])) {
		if(!cmd[1])
			return 1;
		ret = del_vg(cmd[1]);
	}
	else
		ret = 1;

	if (ret)
		strcpy(buf->path, "fail");
	else
		strcpy(buf->path, "ok");

	return 0;
}


/* This function must be invoked with the corresponding mutex locked */
static struct sq_entry *
find_and_remove(struct sqhead * head, pid_t id)
{
	struct sq_entry * entry;
	SIMPLEQ_FOREACH(entry, head, entries) {
		if (entry->data.id == id) {
			SIMPLEQ_REMOVE(head, entry, sq_entry, entries);
			return entry;
		}
	}
	/* No matches */
	return NULL;
}


static void *
process_req(void * ap)
{
	struct sq_entry * req;
	struct payload * data;
	struct kpr_thread_info *thr_arg;
	struct kpr_queue *r_queue;
	int ret;

	/* We must guarantee this structure is properly polulated or
	   check it and fail in case it is not. In the latter case
	   we need to check if the thread has returned.
	*/
	thr_arg = (struct kpr_thread_info *) ap;
	r_queue = thr_arg->r_queue;

	for(;;) {
		pthread_mutex_lock(&r_queue->mtx);

		while (SIMPLEQ_EMPTY(&r_queue->qhead)) {
			pthread_cond_wait(&r_queue->cnd, &r_queue->mtx);
		}

		/* pop from requests queue and unlock */
		req = SIMPLEQ_FIRST(&r_queue->qhead);
		SIMPLEQ_REMOVE_HEAD(&r_queue->qhead, entries);
		pthread_mutex_unlock(&r_queue->mtx);

		data = &req->data;
		/* For the time being we use PAYLOAD_UNDEF as a way
		   to notify threads to exit
		*/
		if (data->reply == PAYLOAD_UNDEF) {
			fprintf(stderr, "Thread cancellation received\n");
			return NULL;
		}

		/* Fulfil request */
		ret = increase_size(data->curr, data->path);
		if (ret == 0 || ret == 3) /* 3 means big enough */
			data->reply = PAYLOAD_DONE;
		else
			data->reply = PAYLOAD_REJECTED;
		printf("worker_thread: completed %u %s (%d)\n\n",
		       (unsigned)data->id, data->path, ret);

		/* push to served queue */
		pthread_mutex_lock(&out_queue->mtx);
		SIMPLEQ_INSERT_TAIL(&out_queue->qhead, req, entries);
		pthread_mutex_unlock(&out_queue->mtx);
	}
	return NULL;
}

/*
 * @size: current size to increase in bytes
 * @path: device full path
 */
static int
increase_size(off64_t size, const char * path) {
#define NCHARS 16
	pid_t pid;
	int status, num_read;
	char ssize[NCHARS]; /* enough for G bytes */
	size += 104857600; /* add 100 MB */

	/* prepare size for command line */
	num_read = snprintf(ssize, NCHARS, "-L""%"PRIu64"b", size);
	if (num_read >= NCHARS)
		return -1; /* size too big */

	switch (pid = fork()) {
	case -1:
		return -1;
	case 0: /* child */
		execl("/usr/sbin/lvextend", "lvextend", ssize,
		      path, (char *)NULL);
		_exit(127); /* TBD */
	default: /* parent */
		if (waitpid(pid, &status, 0) == -1)
			return -1;
		else if (WIFEXITED(status)) /* normal exit? */
			status = WEXITSTATUS(status);
		else
			return -1;
		return status; /* actually 3 is as fine as 0.. */
	}
}


static void
parse_cmdline(int argc, char ** argv)
{
	int arg, fd_open = 0;

	while ((arg = getopt(argc, argv, "df")) != EOF ) {
		switch(arg) {
		case 'd': /* daemonize and close fd */
			daemonize = 1;
			break;
		case 'f': /* if daemonized leave fd open */
			fd_open = 1;
		default:
			break;
		}
	}
	daemonize += daemonize ? fd_open : 0;
	return;
}


static int
do_daemon()
{
	if (!daemonize)
		return 0;

	return daemon(0, daemonize - 1); /* root dir and close if needed */
}


static void
split_command(char *command, char **cmd_vec)
{
	char *token;
	int i;

	token = strtok(command, " ");
	for(i = 0; token && (i < 2); ++i) {
		cmd_vec[i] = token;
		token = strtok(NULL, " ");
	}

	if (i < 2)
		cmd_vec[1] = '\0';

	return;
}


static int
add_vg(char *vg)
{
	struct vg_entry *p_vg;

	printf("CLI: add_vg\n");

	/* check we already have it */
	if(vg_pool_find(vg, true)) {
		printf("%s already added\n", vg);
		return 0;
	}

	/* allocate and init vg_entry */
	p_vg = malloc(sizeof(*p_vg));
	if (!p_vg)
		return 1;

	/* We rely on CLI to avoid truncated name or non-NULL terminated
	   strings. Moreover, by dest is not smaller then src */
	strcpy(p_vg->name, vg);

	/* VG and thread specific thread allocated */
	p_vg->r_queue = alloc_init_queue();
	if(!p_vg->r_queue)
		goto out;

	/* Prepare and start VG specific thread */
	p_vg->thr.r_queue = p_vg->r_queue;
	if (pthread_create(&p_vg->thr.thr_id, NULL, process_req, &p_vg->thr)) {
		fprintf(stderr, "Failed worker thread creation for %s\n",
			p_vg->name);
		goto out2;
	}

	/* Everything ok. Add vg to pool */
	LIST_INSERT_HEAD(&vg_pool.head, p_vg, entries);

	return 0;
out2:
	free(p_vg->r_queue);
out:
	free(p_vg);
	return 1;
}


static int
del_vg(char *vg)
{
	struct vg_entry *p_vg;
	struct sq_entry *req;
	int ret;

	printf("CLI: del_vg\n");

	/* Once removed from the pool no new requests can be served
	   any more
	*/
	p_vg = vg_pool_find_and_remove(vg);
	if(!p_vg) {
		fprintf(stderr, "Nothing removed\n");
		return 1;
	}

	/* The thread is still able to crunch requests in its queue
	   so we "poison" the queue to stop the thread
	 */
	req = malloc(sizeof(*req));
	if(!req) {
		/* FIXME: we are going to return but the vg is no more in the
		 pool while the thread is still running.
		 We are returning with a runnig thread, not able to receive new
		 requests and 2 memory leaks..
		*/
		fprintf(stderr, "Error with malloc!! Thread still running\n"
			"and memory leaked\n");
		return 1;
	}
	init_payload(&req->data);
	req->data.reply = PAYLOAD_UNDEF;
	/* Insert in queue */
	pthread_mutex_lock(&p_vg->r_queue->mtx);
	SIMPLEQ_INSERT_TAIL(&p_vg->r_queue->qhead, req, entries);
	pthread_cond_signal(&p_vg->r_queue->cnd); /* Wake thread if needed */
	pthread_mutex_unlock(&p_vg->r_queue->mtx);

	/* Wait for thread to complete */
	ret = pthread_join(p_vg->thr.thr_id, NULL);
	if (ret != 0)
		fprintf(stderr, "Problem joining thread..FIXME\n");

	/*
	 * Thread is dead, let's free resources
	 */
	/* By design the queue must be empty but we check */
	if (!SIMPLEQ_EMPTY(&p_vg->r_queue->qhead))
		fprintf(stderr, "queue not empty, memory leak! FIXME\n");
	free(p_vg->r_queue);
	free(p_vg);

	return 0;
}


/**
 * This function searches the vg_pool for an entry with a given VG name.
 * If invoked with locking no mutexes must be hold
 *
 * @param vg_name name of the volume group to search for
 * @param lock ask for function to take care of locking
 * @return NULL if not in the pool or a pointer to the entry
*/
static struct vg_entry *
vg_pool_find(char *vg_name, bool lock)
{
	struct vg_entry *entry, *ret;
	ret = NULL;

	if(lock)
		pthread_mutex_lock(&vg_pool.mtx);
	LIST_FOREACH(entry, &vg_pool.head, entries) {
		/* looking for exact match */
		if (strcmp(entry->name, vg_name) == 0) {
			ret = entry;
			break;
		}
	}
	if(lock)
		pthread_mutex_unlock(&vg_pool.mtx);

	return ret;
}


/**
 * This function removes from vg_pool the entry named vg_name.
 * The pool lock is automatic so make sure you are not holding
 * any mutex
 *
 * @param vg_name name of the volume group to remove
 * @return NULL if nothing is removed or a pointer to removed item
*/
static struct vg_entry *
vg_pool_find_and_remove(char *vg_name)
{
	struct vg_entry *entry;

	pthread_mutex_lock(&vg_pool.mtx);
	entry = vg_pool_find(vg_name, false);
	if(!entry)
		return NULL;
	LIST_REMOVE(entry, entries);
	pthread_mutex_unlock(&vg_pool.mtx);

	return entry;
}
