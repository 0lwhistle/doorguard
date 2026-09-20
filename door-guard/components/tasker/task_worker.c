/*
 * task_worker.c — tasker 执行线程实现
 *
 * 移植自模板 ovs/components/core/tasker/task_worker.c;差异:mem.h 换
 * malloc/free,logger 换 dg_log。5.2 修复(cond 超时等待/熔断/失败释放)
 * 全部保留,注释即"为什么"。
 */
#include "task_worker.h"
#include <errno.h>

const char* TASK_WORKER_TAG = "[TASK_WORKER]";

atomic_int worker_init_flag = 0;
static uint32_t s_spin_fallback_usleeps = 0;

struct task_worker_ctx s_task_worker_ctx = {0};



static inline uint64_t get_time_ms(void){
	return tasker_now_ms();
}

// forward declarations for static functions used before definition
static inline int enqueue_switcher(struct task_node* node);
static int worker_task_enqueue_nocancel(struct task_worker* des, struct task_node* node);

#include <string.h>

static void worker_release_partial(void);

int worker_init(void){

	int ret = 0;

	s_task_worker_ctx.little_worker = (struct task_worker*)malloc(sizeof(struct task_worker));
	s_task_worker_ctx.middle_worker = (struct task_worker*)malloc(sizeof(struct task_worker));
	s_task_worker_ctx.lots_worker = (struct task_worker*)malloc(sizeof(struct task_worker));
	s_task_worker_ctx.s_dispatcher = (struct task_worker*)malloc(sizeof(struct task_worker));
	s_task_worker_ctx.s_sched_table = (struct task_worker*)malloc(sizeof(struct task_worker));

	if (!s_task_worker_ctx.little_worker || !s_task_worker_ctx.middle_worker ||
		!s_task_worker_ctx.lots_worker || !s_task_worker_ctx.s_dispatcher ||
		!s_task_worker_ctx.s_sched_table) {
		DG_LOGE(TASK_WORKER_TAG, "task worker malloc fail!");
		worker_release_partial();
		return TASK_MEM_ERR;
	}

	if (worker_little_init() != TASK_OK ||
	    worker_middle_init() != TASK_OK ||
	    worker_lots_init() != TASK_OK ||
	    worker_dispatcher_init() != TASK_OK ||
	    worker_sched_init() != TASK_OK) {
		DG_LOGE(TASK_WORKER_TAG, "task worker init fail!");
		ret = TASK_INNER_ERR;
		/* 5.2 修复3: 失败路径释放已创建资源，不再泄漏 */
		worker_release_partial();
		return ret;
	}

	atomic_store(&worker_init_flag, 1);

	return ret;

}


/* 5.2 修复3: 释放 worker_init 部分创建的资源（幂等，未创建的成员为空） */
static void worker_release_one(struct task_worker* worker){
	if (!worker) return;
	if (worker->pt_created){
		atomic_store(&worker->stop, 1);
		pthread_cond_signal(&(worker->cond));
		pthread_join(worker->pt, NULL);
	}
	if (worker->timeout_timer){
		tasker_timer_delete(worker->timeout_timer);
		worker->timeout_timer = NULL;
	}
	if (worker->worker_queue){
		free(worker->worker_queue->queue);
		free(worker->worker_queue);
		worker->worker_queue = NULL;
	}
	pthread_mutex_destroy(&(worker->mtx));
	pthread_cond_destroy(&(worker->cond));
	free(worker);
}

static void worker_release_partial(void){
	worker_release_one(s_task_worker_ctx.little_worker);
	worker_release_one(s_task_worker_ctx.middle_worker);
	worker_release_one(s_task_worker_ctx.lots_worker);
	worker_release_one(s_task_worker_ctx.s_dispatcher);
	worker_release_one(s_task_worker_ctx.s_sched_table);
	memset(&s_task_worker_ctx, 0, sizeof(s_task_worker_ctx));
	atomic_store(&worker_init_flag, 0);
}

int worker_little_init(void){
	struct task_worker* worker = s_task_worker_ctx.little_worker;
	DG_LOGI(TASK_WORKER_TAG, "little worker pthread init");
	int ret = TASK_OK;
	
	worker->worker_queue = task_manager_init(LITTLE_TASK_QUEUE_SIZE);
	if (!worker->worker_queue){
		DG_LOGE(TASK_WORKER_TAG, "little worker task manager init fail");
		ret = TASK_MEM_ERR;
		return ret;
	}

	ret = pthread_mutex_init(&(worker->mtx), NULL);
	if (ret != 0) {
		DG_LOGE(TASK_WORKER_TAG, "Mutex init failed: %d", ret);
		ret = TASK_INNER_ERR;
		return ret;
	}

	ret = pthread_cond_init(&(worker->cond), NULL);
	if (ret != 0) {
		DG_LOGE(TASK_WORKER_TAG, "Cond init failed: %d", ret);
		ret = TASK_INNER_ERR;
		return ret;
	}

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, LITTLE_TASK_STACK_SIZE);
	ret = pthread_create(&(worker->pt), &attr, worker_little_handler, worker);
	pthread_attr_destroy(&attr);
	if (ret == 0) worker->pt_created = 1;

	return ret;
}

int worker_middle_init(void){
	DG_LOGI(TASK_WORKER_TAG, "middle worker pthread init");
	struct task_worker* worker = s_task_worker_ctx.middle_worker;

	int ret = TASK_OK;
	
	worker->worker_queue = task_manager_init(MIDDLE_TASK_QUEUE_SIZE);

	if (!worker->worker_queue){
		ret = TASK_MEM_ERR;
		return ret;
	}
	ret = pthread_mutex_init(&(worker->mtx), NULL);
	if (ret != 0) {
		DG_LOGE(TASK_WORKER_TAG, "Mutex init failed: %d", ret);
		ret = TASK_INNER_ERR;
		return ret;
	}

	ret = pthread_cond_init(&(worker->cond), NULL);
	if (ret != 0) {
		DG_LOGE(TASK_WORKER_TAG, "Cond init failed: %d", ret);
		ret = TASK_INNER_ERR;
		return ret;
	}

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, MIDDLE_TASK_STACK_SIZE);
	ret = pthread_create(&(worker->pt), &attr, worker_middle_handler, worker);
	pthread_attr_destroy(&attr);
	if (ret == 0) worker->pt_created = 1;


	return ret;

}

int worker_lots_init(void){
	DG_LOGI(TASK_WORKER_TAG, "lots worker pthread init");
	struct task_worker* worker = s_task_worker_ctx.lots_worker;
	int ret = TASK_OK;
	
	worker->worker_queue = task_manager_init(LOTS_TASK_QUEUE_SIZE);

	if (!worker->worker_queue){
		ret = TASK_MEM_ERR;
		return ret;
	}
	ret = pthread_mutex_init(&(worker->mtx), NULL);
	if (ret != 0) {
		DG_LOGE(TASK_WORKER_TAG, "Mutex init failed: %d", ret);
		ret = TASK_INNER_ERR;
		return ret;
	}

	ret = pthread_cond_init(&(worker->cond), NULL);
	if (ret != 0) {
		DG_LOGE(TASK_WORKER_TAG, "Cond init failed: %d", ret);
		ret = TASK_INNER_ERR;
		return ret;
	}

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, LOTS_TASK_STACK_SIZE);
	ret = pthread_create(&(worker->pt), &attr, worker_lots_handler, worker);
	pthread_attr_destroy(&attr);
	if (ret == 0) worker->pt_created = 1;

	
	return ret;

}

int worker_dispatcher_init(void){

	DG_LOGI(TASK_WORKER_TAG, "dispatcher worker pthread init");
	struct task_worker* worker = s_task_worker_ctx.s_dispatcher;
	int ret = TASK_OK;
	
	worker->worker_queue = task_manager_init(DISPATCHER_TASK_QUEUE_SIZE);
	if (!worker->worker_queue){
		ret = TASK_MEM_ERR;
		return ret;
	}
	ret = pthread_mutex_init(&(worker->mtx), NULL);
	if (ret != 0) {
		DG_LOGE(TASK_WORKER_TAG, "Mutex init failed: %d", ret);
		ret = TASK_INNER_ERR;
		return ret;
	}

	ret = pthread_cond_init(&(worker->cond), NULL);
	if (ret != 0) {
		DG_LOGE(TASK_WORKER_TAG, "Cond init failed: %d", ret);
		ret = TASK_INNER_ERR;
		return ret;
	}

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, DISPATCHER_TASK_QUEUE_STACK_SIZE);
	ret = pthread_create(&(worker->pt), &attr, worker_dispatcher_handler, worker);
	pthread_attr_destroy(&attr);
	if (ret == 0) worker->pt_created = 1;

	return ret;
}


int worker_sched_init(void){

	DG_LOGI(TASK_WORKER_TAG, "sched worker pthread init");
	struct task_worker* worker = s_task_worker_ctx.s_sched_table;
	int ret = TASK_OK;
	worker->worker_queue = task_manager_init(SCHED_TASK_QUEUE_SIZE);
	if (!worker->worker_queue){
		ret = TASK_MEM_ERR;
		return ret;
	}
	ret = pthread_mutex_init(&(worker->mtx), NULL);
	if (ret != 0) {
		DG_LOGE(TASK_WORKER_TAG, "Mutex init failed: %d", ret);
		ret = TASK_INNER_ERR;
		return ret;
	}

	/* 5.2 修复1: cond 超时等待替代忙轮询。
	 * 时钟用默认 CLOCK_REALTIME（ESP pthread 对 MONOTONIC condattr 支持
	 * 不确定，曾致 timedwait 立即返回 → IDLE0 饿死），睡眠上限 1s 限损害 */
	ret = pthread_cond_init(&(worker->cond), NULL);
	if (ret != 0) {
		DG_LOGE(TASK_WORKER_TAG, "Cond init failed: %d", ret);
		ret = TASK_INNER_ERR;
		return ret;
	}

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, SCHED_TASK_QUEUE_STACK_SIZE);
	ret = pthread_create(&(worker->pt), &attr, worker_sched_handler, worker);
	pthread_attr_destroy(&attr);
	if (ret == 0) worker->pt_created = 1;

	return ret;
}


/* 归还节点给 sched 表后唤醒其等待（5.2 修复1 配套：借出节点回归即醒） */
static void sched_wake(void){
	struct task_worker* sched = s_task_worker_ctx.s_sched_table;
	if (sched && !atomic_load(&sched->stop)){
		pthread_cond_signal(&sched->cond);
	}
}

void* worker_little_handler(void* arg){
	struct task_worker* worker = (struct task_worker*)arg;
	worker_do_handler(worker);
	return NULL;
}

void* worker_middle_handler(void* arg){
	struct task_worker* worker = (struct task_worker*)arg;
	worker_do_handler(worker);
	return NULL;
}

void* worker_lots_handler(void* arg){
	struct task_worker* worker = (struct task_worker*)arg;
	worker_do_handler(worker);
	return NULL;
}

void* worker_dispatcher_handler(void* arg){
	struct task_worker* worker = (struct task_worker*)arg;
	int size;
	int ret;
	int need_sort;
	atomic_store(&worker->stop, 0);
	while(!atomic_load(&worker->stop)){
		pthread_mutex_lock(&(worker->mtx));
		while(!worker->stop && task_manager_is_empty(worker->worker_queue))	{
			pthread_cond_wait(&worker->cond, &worker->mtx);
		}
		if (atomic_load(&worker->stop)) {
			pthread_mutex_unlock(&(worker->mtx));
			break;
		}

		need_sort = 0;
		size = worker->worker_queue->size;
		struct task_manager* worker_queue = worker->worker_queue;
		for (int i = 0; i < size; ++i){
			struct task_node* node = worker_queue->queue[i];
			if (!node) continue;

			if (atomic_load(&node->cancel) || atomic_load(&node->done)){
				worker_queue->queue[i] = NULL;
				atomic_store(&node->done, 1);
				atomic_store(&node->dispatched, 0);
				sched_wake();
				continue;
			}

			ret = enqueue_switcher(node);

			if (ret == TASK_STOP) {
				pthread_mutex_unlock(&(worker->mtx));
				return NULL;
			}

			if (ret == TASK_QUEUE_FULL) {
				need_sort = 1;
				task_node_pri_up(node);
				continue;
			}

			/* Pointer handoff: dispatcher slot no longer references the node. */
			worker_queue->queue[i] = NULL;
		}
		if (need_sort)
			task_manager_pri_sort(worker->worker_queue);

		pthread_mutex_unlock(&(worker->mtx));

	}
	return NULL;
}
#define SCHED_MAX_SLEEP_MS 1000u   /* 无近期任务时的最大睡眠（兼顾 done/cancel 残留清理） */

void* worker_sched_handler(void* arg){
	struct task_worker* worker = (struct task_worker*)arg;
	int ret = 0;
	atomic_store(&worker->stop, 0);
	while(!atomic_load(&worker->stop)){
		pthread_mutex_lock(&(worker->mtx));

		/* 5.2 修复1: 计算最近截止时间，cond 超时等待替代 vTaskDelay(1) 忙轮询。
		 * enqueue 的 cond_signal 提前唤醒；空闲时零 CPU。 */
		{
			uint64_t now = get_time_ms();
			uint64_t deadline = now + SCHED_MAX_SLEEP_MS;
			for (unsigned int i = 0; i < worker->worker_queue->size; ++i){
				struct task_node* n = worker->worker_queue->queue[i];
				if (!n || atomic_load(&n->cancel) || atomic_load(&n->done) ||
				    atomic_load(&n->dispatched)) continue;
				if (n->period == 0 && n->run_cnt > 0){
					deadline = now;            /* 待执行一次性任务：立刻醒 */
					break;
				}
				if (n->period > 0 && n->run_cnt != 0){
					uint64_t due = n->inject_time + (uint64_t)n->period;
					if (due <= now) { deadline = now; break; }
					if (due < deadline) deadline = due;
				}
			}
			if (deadline > now){
				struct timespec ts;
				clock_gettime(CLOCK_REALTIME, &ts);   /* 与 cond 默认时钟一致 */
				uint64_t delta_ms = deadline - now;   /* 已被 SCHED_MAX_SLEEP_MS 封顶 */
				ts.tv_sec += (time_t)(delta_ms / 1000u);
				ts.tv_nsec += (long)(delta_ms % 1000u) * 1000000L;
				if (ts.tv_nsec >= 1000000000L){
					ts.tv_sec += 1;
					ts.tv_nsec -= 1000000000L;
				}
				int rc = pthread_cond_timedwait(&worker->cond, &worker->mtx, &ts);
				if (rc != 0 && rc != ETIMEDOUT){
					/* 时钟/参数异常兜底：绝对等待失效时保底睡 2ms，绝不自旋 */
					s_spin_fallback_usleeps++;
					usleep(2000);
				}
			}
		}

		/* 自旋熔断：调度循环空转过快（如 timedwait 异常立即返回）时强制降速
		 * 并打诊断，绝不允许饿死 IDLE（真机教训 2026-09-09） */
		{
			static uint64_t last_loop_ms = 0;
			static int fast_loops = 0;
			uint64_t now_loop = get_time_ms();
			if (now_loop == last_loop_ms){
				if (++fast_loops > 200){
					fast_loops = 0;
					DG_LOGE(TASK_WORKER_TAG, "sched spinning detected, throttle 10ms "
					     "(nodes=%d)", worker->worker_queue->size);
					usleep(10000);
				}
			} else {
				fast_loops = 0;
			}
			last_loop_ms = now_loop;
		}

		int size = worker->worker_queue->size;
		struct task_manager* worker_queue = worker->worker_queue;
		uint64_t cur = get_time_ms();
		for (int i = 0; i < size; ++i){
			struct task_node* node = worker_queue->queue[i];
			if (!node) continue;

			if (atomic_load(&node->cancel) || atomic_load(&node->done)){
				if (!atomic_load(&node->dispatched)){
					task_node_pool_free(node);
					worker_queue->queue[i] = NULL;
				}
				continue;
			}

			/* Node is currently borrowed by dispatcher/worker; wait for it to return. */
			if (atomic_load(&node->dispatched)) continue;

			int need_sched = 0;
			if (node->period == 0 && atomic_load(&node->run_cnt) > 0) {
				need_sched = 1;
			}
			else if (node->period > 0 && atomic_load(&node->run_cnt) != 0 &&
				cur - node->inject_time >= (uint64_t)node->period) {
				need_sched = 1;
			}

			if (!need_sched) continue;

			uint64_t now = get_time_ms();
			node->inject_time = now;
			if (atomic_load(&node->run_cnt) > 0) atomic_fetch_sub(&node->run_cnt, 1);

			/* Move the node pointer to dispatcher without copying task metadata. */
			atomic_store(&node->dispatched, 1);
			ret = worker_task_enqueue_nocancel(s_task_worker_ctx.s_dispatcher, node);
			if (ret == TASK_OK) continue;

			atomic_store(&node->dispatched, 0);
			if (ret == TASK_QUEUE_FULL) {
				node->inject_time = cur;
				if (atomic_load(&node->run_cnt) >= 0) atomic_fetch_add(&node->run_cnt, 1);
				task_node_pri_up(node);
			}
		}

		pthread_mutex_unlock(&(worker->mtx));
	}
	return NULL;

}
static inline void timer_callback(void* arg){
	struct task_worker* worker = (struct task_worker*)arg;
	worker->timeout_flag = 1;
	return;
}

static inline void worker_timer_init(struct task_worker* worker){
	if (tasker_timer_create(&worker->timeout_timer, &timer_callback, worker) != 0){
		DG_LOGE(TASK_WORKER_TAG, "timeout timer create failed");
		worker->timeout_timer = NULL;
	}
}

void worker_do_handler(struct task_worker* worker){

	enum task_t status;
	atomic_store(&worker->stop, 0);
	worker->timeout_flag = 0;
	worker->timeout_timer = NULL;

	worker_timer_init(worker);

	while(!atomic_load(&worker->stop)){
		pthread_mutex_lock(&(worker->mtx));
		while(!worker->stop && task_manager_is_empty(worker->worker_queue))	{
			pthread_cond_wait(&worker->cond, &worker->mtx);
		}
		if (atomic_load(&worker->stop)) {
			pthread_mutex_unlock(&(worker->mtx));
			break;
		}

		int size = worker->worker_queue->size;
		struct task_manager* worker_queue = worker->worker_queue;
		for (int i = 0; i < size; ++i){
			struct task_node* node = worker_queue->queue[i];
			if (!node) continue;

			if (atomic_load(&node->cancel) || atomic_load(&node->done)){
				worker_queue->queue[i] = NULL;
				if (atomic_load(&node->dispatched)){
					atomic_store(&node->done, 1);
					atomic_store(&node->dispatched, 0);
				}
				continue;
			}

			task_fn fn = node->fn;
			void* ctx = node->ctx;
			int timeout = node->timeout;
			worker->timeout_flag = 0;

			pthread_mutex_unlock(&(worker->mtx));

			if (timeout > 0 && worker->timeout_timer) {
				tasker_timer_start_once(worker->timeout_timer, (uint64_t)timeout * 1000u);
			}
			status = fn(ctx);

			if (timeout > 0 && worker->timeout_timer) {
				tasker_timer_stop(worker->timeout_timer);
			}

			pthread_mutex_lock(&(worker->mtx));

			/* The node remains in sched table; only the worker slot is cleared here. */
			node = worker_queue->queue[i];
			if (!node) continue;

			if (atomic_load(&node->cancel) || atomic_load(&node->done)){
				worker_queue->queue[i] = NULL;
				if (atomic_load(&node->dispatched)){
					atomic_store(&node->done, 1);
					atomic_store(&node->dispatched, 0);
				}
				continue;
			}

			if (status == TASK_OK){
				node->is_timeout = worker->timeout_flag;
				if (worker->timeout_flag && atomic_load(&node->level) < level_lots){
					task_node_leve_up(node);
				}
			} else {
				DG_LOGW(TASK_WORKER_TAG, "task node returned error, schedule will retry it.");
			}

			worker_queue->queue[i] = NULL;
			atomic_store(&node->dispatched, 0);
			sched_wake();
			if (atomic_load(&node->run_cnt) == 0 || atomic_load(&node->cancel)
			    || atomic_load(&node->done)){
				atomic_store(&node->done, 1);
			}
		}
		pthread_mutex_unlock(&(worker->mtx));
	}
	if (worker->timeout_timer) tasker_timer_delete(worker->timeout_timer);
}
int worker_task_enqueue(struct task_worker* des, struct task_node* node){
	if (des->stop){
		DG_LOGW(TASK_WORKER_TAG, "worker is already stop.");
		return TASK_STOP;
	}

	pthread_mutex_lock(&(des->mtx));

	int size = des->worker_queue->size;
	struct task_manager* worker_queue = des->worker_queue;
	for (int i = 0; i < size; ++i){
		struct task_node* slot = worker_queue->queue[i];
		if (slot == NULL){

			worker_queue->queue[i] = node;
			DG_LOGD(TASK_WORKER_TAG, "task: %s enqueue successfully.", node->name);

			pthread_cond_signal(&(des->cond));
			pthread_mutex_unlock(&(des->mtx));
			return TASK_OK;
		}

		if ((atomic_load(&slot->cancel) || atomic_load(&slot->done))
		    && !atomic_load(&slot->dispatched)){
			task_node_pool_free(slot);
			worker_queue->queue[i] = node;
			DG_LOGD(TASK_WORKER_TAG, "task: %s enqueue successfully.", node->name);

			pthread_cond_signal(&(des->cond));
			pthread_mutex_unlock(&(des->mtx));
			return TASK_OK;
		}
	}

	pthread_mutex_unlock(&(des->mtx));
	task_node_pri_up(node);

	return TASK_QUEUE_FULL;
}
static int worker_task_enqueue_nocancel(struct task_worker* des, struct task_node* node){
	if (des->stop){
		DG_LOGW(TASK_WORKER_TAG, "worker is already stop.");
		return TASK_STOP;
	}

	pthread_mutex_lock(&(des->mtx));

	int size = des->worker_queue->size;
	struct task_manager* worker_queue = des->worker_queue;
	for (int i = 0; i < size; ++i){
		struct task_node* slot = worker_queue->queue[i];
		if (slot == NULL){

			worker_queue->queue[i] = node;
			DG_LOGD(TASK_WORKER_TAG, "task: %s enqueue successfully.", node->name);

			pthread_cond_signal(&(des->cond));
			pthread_mutex_unlock(&(des->mtx));
			return TASK_OK;
		}

		if ((atomic_load(&slot->cancel) || atomic_load(&slot->done))
		    && !atomic_load(&slot->dispatched)){
			task_node_pool_free(slot);
			worker_queue->queue[i] = node;
			DG_LOGD(TASK_WORKER_TAG, "task: %s enqueue successfully.", node->name);

			pthread_cond_signal(&(des->cond));
			pthread_mutex_unlock(&(des->mtx));
			return TASK_OK;
		}
	}

	pthread_mutex_unlock(&(des->mtx));

	return TASK_QUEUE_FULL;
}

int worker_sched_enqueue(struct task_node* node){
	if (!node) return TASK_PARA_ERR;

	struct task_worker* des = s_task_worker_ctx.s_sched_table;
	if (des->stop){
		DG_LOGW(TASK_WORKER_TAG, "worker is already stop.");
		return TASK_STOP;
	}

	pthread_mutex_lock(&(des->mtx));

	int size = des->worker_queue->size;
	struct task_manager* worker_queue = des->worker_queue;
	int slot = -1;
	for (int i = 0; i < size; ++i){
		struct task_node* cur = worker_queue->queue[i];
		if (cur == NULL){
			slot = i;
			break;
		}
		if ((atomic_load(&cur->cancel) || atomic_load(&cur->done))
		    && !atomic_load(&cur->dispatched)){
			task_node_pool_free(cur);
			slot = i;
			break;
		}
	}

	if (slot < 0){
		pthread_mutex_unlock(&(des->mtx));
		return TASK_QUEUE_FULL;
	}

	/* Reclaim first, then take a pool node, so a full stale sched table can still admit tasks. */
	struct task_node* owned = task_node_pool_alloc();
	if (!owned){
		pthread_mutex_unlock(&(des->mtx));
		return TASK_QUEUE_FULL;
	}
	/* atomic 成员不整块拷贝，逐字段赋值 */
	owned->timeout = node->timeout;
	owned->is_timeout = 0;
	owned->period = node->period;
	atomic_store(&owned->run_cnt, atomic_load(&node->run_cnt));
	atomic_store(&owned->pri, atomic_load(&node->pri));
	atomic_store(&owned->level, atomic_load(&node->level));
	owned->fn = node->fn;
	owned->inject_time = node->inject_time;
	memcpy(owned->name, node->name, sizeof(owned->name));
	owned->ctx = node->ctx;
	atomic_store(&owned->cancel, 0);
	atomic_store(&owned->done, 0);
	atomic_store(&owned->dispatched, 0);

	worker_queue->queue[slot] = owned;
	DG_LOGI(TASK_WORKER_TAG, "task: %s enqueue successfully.", owned->name);

	pthread_cond_signal(&(des->cond));
	pthread_mutex_unlock(&(des->mtx));
	return TASK_OK;
}

static inline int enqueue_switcher(struct task_node* node){
	switch (atomic_load(&node->level)){
		case level_little:
			return worker_task_enqueue(s_task_worker_ctx.little_worker, node);
		
		case level_middle:
			return worker_task_enqueue(s_task_worker_ctx.middle_worker, node);

		case level_lots:
			return worker_task_enqueue(s_task_worker_ctx.lots_worker, node);

		default:
			DG_LOGE(TASK_WORKER_TAG, "unknown level in enqueue_switcher");
			return TASK_INNER_ERR;
	}
}


void worker_task_done(struct task_worker* worker, struct task_node* node){
	pthread_mutex_lock(&(worker->mtx));
	struct task_node* des = find_task_node_by_name(worker->worker_queue, node->name);
	if (des) atomic_store(&des->done, 1);
	pthread_mutex_unlock(&(worker->mtx));
}

void worker_task_cancel(struct task_worker* worker, struct task_node* node){
	pthread_mutex_lock(&(worker->mtx));
	struct task_node* des = find_task_node_by_name(worker->worker_queue, node->name);
	if (des) atomic_store(&des->cancel, 1);
	pthread_mutex_unlock(&(worker->mtx));
}



void worker_delete(struct task_worker* worker){
	if (!worker) return;
	atomic_store(&worker->stop, 1);
	pthread_cond_signal(&(worker->cond));
	if (worker->pt_created){
		pthread_join(worker->pt, NULL);
		worker->pt_created = 0;
	}
	if (worker->timeout_timer){
		tasker_timer_delete(worker->timeout_timer);
		worker->timeout_timer = NULL;
	}
	pthread_mutex_destroy(&(worker->mtx));
	pthread_cond_destroy(&(worker->cond));
	if (worker->worker_queue){
		free(worker->worker_queue->queue);
		free(worker->worker_queue);
		worker->worker_queue = NULL;
	}
	free(worker);
}
