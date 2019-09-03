#pragma once

#include "SDL.h"

struct list_head {
	struct list_head *next, *prev;
};


#define LIST_HEAD_INIT(name) { &(name), &(name) }

#define LIST_HEAD(name) \
		   struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list)
{
	list->next = list;
	list->prev = list;
}

static inline void __list_add(struct list_head *_new,
	struct list_head *prev,
	struct list_head *next)
{
	next->prev = _new;
	_new->next = next;
	_new->prev = prev;
	prev->next = _new;
}

static inline void __list_del(struct list_head * prev, struct list_head * next)
{
	next->prev = prev;
	prev->next = next;
}


static inline void list_add(struct list_head *_new, struct list_head *head)
{
	__list_add(_new, head, head->next);
}


static inline void list_add_tail(struct list_head *_new, struct list_head *head)
{
	__list_add(_new, head->prev, head);
}

static inline void list_del(struct list_head *entry)
{
	__list_del(entry->prev, entry->next);
}


static inline int list_empty(const struct list_head *head)
{
	return (head->next == head);
}

//list_entry(ptr,fn_ffmpeg_packet_qnode_t,list)
//(fn_ffmpeg_packet_qnode_t *)((char *)(ptr) - (unsigned long)(&((fn_ffmpeg_packet_qnode_t *)0)->list)))

#define list_entry(ptr, type, member) \
   ((type *)((char *)(ptr)-(unsigned long)(&((type *)0)->member)))


////////////////////////// q  API ///////////////////////////////////////////

struct q_head {
	struct list_head lst_head;
	int node_nr;
	SDL_mutex* mutex;  // q mutex lock
};


static inline int q_init(struct q_head *q_head)
{
	INIT_LIST_HEAD(&q_head->lst_head);
	q_head->node_nr = 0;
	q_head->mutex = SDL_CreateMutex();
	if (!q_head->mutex)
	{
		return -1;
	}
	return 0;
}

static inline void q_destroy(struct q_head *q_head)
{
	SDL_DestroyMutex(q_head->mutex);
}


static inline void q_push(struct list_head *_new, struct q_head *q_head)
{
	SDL_LockMutex(q_head->mutex);
	list_add_tail(_new, &q_head->lst_head);
	q_head->node_nr++;
	//printf("e:q_head = %x,q_head->node_nr = %d\n",q_head,q_head->node_nr);
	SDL_UnlockMutex(q_head->mutex);
}


static inline int q_is_empty(struct q_head *q_head)
{
	int rst;
	SDL_LockMutex(q_head->mutex);
	rst = list_empty(&q_head->lst_head);
	SDL_UnlockMutex(q_head->mutex);
	return rst;
}

// if already empty, returns NULL
static inline struct list_head *q_pop(struct q_head *q_head)
{
	struct list_head *poped_item;
	SDL_LockMutex(q_head->mutex);
	if (list_empty(&q_head->lst_head))
	{
		SDL_UnlockMutex(q_head->mutex);
		return NULL;
	}
	poped_item = q_head->lst_head.next;
	//	list_del(q_head->lst_head.next);
	list_del(poped_item);
	q_head->node_nr--;
	SDL_UnlockMutex(q_head->mutex);
	return poped_item;
}

// if already empty, returns NULL
// get first element
static inline struct list_head *q_get_first(struct q_head *q_head)
{
	struct list_head *got_item;
	SDL_LockMutex(q_head->mutex);
	if (list_empty(&q_head->lst_head))
	{
		SDL_UnlockMutex(q_head->mutex);
		return NULL;
	}
	got_item = q_head->lst_head.next;
	SDL_UnlockMutex(q_head->mutex);
	return got_item;
}

// if already empty, returns NULL
// get last element (latest element that inserted on the q)
static inline struct list_head *q_get_last(struct q_head *q_head)
{
	struct list_head *got_item;
	SDL_LockMutex(q_head->mutex);
	if (list_empty(&q_head->lst_head))
	{
		SDL_UnlockMutex(q_head->mutex);
		return NULL;
	}
	got_item = q_head->lst_head.prev;
	SDL_UnlockMutex(q_head->mutex);
	return got_item;
}

static inline int q_list_count(struct q_head *q_head)
{
	int rst = 0;
	struct list_head *got_item;

	SDL_LockMutex(q_head->mutex);
	got_item = q_head->lst_head.prev;
	while (got_item != &q_head->lst_head)
	{
		rst++;
		got_item = got_item->prev;
	}

	SDL_UnlockMutex(q_head->mutex);
	return rst;
}

