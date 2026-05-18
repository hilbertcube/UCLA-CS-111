#include "hash-table-base.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>

#include <pthread.h>

/*
structure: 

hash table: an array of type hash table entry
|	|	|	|	|	|	|

- each table entry contains a list
- to use each function that access shared data, lock it, and then unlock when finishing

shared data is all mutable state inside the hash table object
- bucket array entires
- each bucket's linked list

*/

struct list_entry {
	const char *key;
	uint32_t value;
	SLIST_ENTRY(list_entry) pointers;
};

// define the list head
// SLIST_HEAD(HEADNAME, TYPE) head; defines a new struct HEADNAME to manage list elements of type struct TYPE
SLIST_HEAD(list_head, list_entry);

struct hash_table_entry {
	struct list_head list_head;
};

// constructor
struct hash_table_v1 {
	// array of entries
	struct hash_table_entry entries[HASH_TABLE_CAPACITY];
	pthread_mutex_t mutex;
};

struct hash_table_v1 *hash_table_v1_create()
{
	struct hash_table_v1 *hash_table = calloc(1, sizeof(struct hash_table_v1));
	assert(hash_table != NULL);

	// Initialize, return 0 if success
	int err = pthread_mutex_init(&hash_table->mutex, NULL);
	if(err != 0) exit(err); // terminate if fail

	for (size_t i = 0; i < HASH_TABLE_CAPACITY; ++i) {
		// point to the ith index of the hash table
		struct hash_table_entry *entry = &hash_table->entries[i];

		// create a linked list, link the entry to the head
		SLIST_INIT(&entry->list_head);
	}
	return hash_table;
}

static struct hash_table_entry *get_hash_table_entry(struct hash_table_v1 *hash_table,
                                                     const char *key)
{
	assert(key != NULL);
	uint32_t index = bernstein_hash(key) % HASH_TABLE_CAPACITY;
	struct hash_table_entry *entry = &hash_table->entries[index];
	return entry;
}

static struct list_entry *get_list_entry(struct hash_table_v1 *hash_table,
                                         const char *key,
                                         struct list_head *list_head)
{
	assert(key != NULL);

	struct list_entry *entry = NULL;
	
	SLIST_FOREACH(entry, list_head, pointers) {
		if (strcmp(entry->key, key) == 0) {
			return entry;
		}
	}
	return NULL;
}

bool hash_table_v1_contains(struct hash_table_v1 *hash_table,
                            const char *key)
{
	// pthread_mutex_lock(&hash_table->mutex);

	struct hash_table_entry *hash_table_entry = get_hash_table_entry(hash_table, key);
	struct list_head *list_head = &hash_table_entry->list_head;
	struct list_entry *list_entry = get_list_entry(hash_table, key, list_head);

	// pthread_mutex_unlock(&hash_table->mutex);
	return list_entry != NULL;
}

// add thread safe
void hash_table_v1_add_entry(struct hash_table_v1 *hash_table,
                             const char *key,
                             uint32_t value)
{
	int err;
	err = pthread_mutex_lock(&hash_table->mutex);
	if(err != 0) exit(err); 

	struct hash_table_entry *hash_table_entry = get_hash_table_entry(hash_table, key);
	struct list_head *list_head = &hash_table_entry->list_head;
	struct list_entry *list_entry = get_list_entry(hash_table, key, list_head);
	

	/* Update the value if it already exists */
	if (list_entry != NULL) {
		list_entry->value = value;
		err = pthread_mutex_unlock(&hash_table->mutex);
		if(err != 0) exit(err); 

		return;
	}

	list_entry = calloc(1, sizeof(struct list_entry));
	list_entry->key = key;
	list_entry->value = value;
	SLIST_INSERT_HEAD(list_head, list_entry, pointers);

	err = pthread_mutex_unlock(&hash_table->mutex);
	if(err != 0) exit(err); 
}

// we copy out the value while the mutex is still held, then unlock, then return the copied value
// why? because if we return list_entry->value after we unlock, another thread can call and we get the incorrect value
uint32_t hash_table_v1_get_value(struct hash_table_v1 *hash_table,
                                 const char *key)
{
	// pthread_mutex_lock(&hash_table->mutex);

	struct hash_table_entry *hash_table_entry = get_hash_table_entry(hash_table, key);
	struct list_head *list_head = &hash_table_entry->list_head;
	struct list_entry *list_entry = get_list_entry(hash_table, key, list_head);
	assert(list_entry != NULL);

	uint32_t value = list_entry->value;
	// pthread_mutex_unlock(&hash_table->mutex);
	return value;
}

void hash_table_v1_destroy(struct hash_table_v1 *hash_table)
{
	for (size_t i = 0; i < HASH_TABLE_CAPACITY; ++i) {
		struct hash_table_entry *entry = &hash_table->entries[i];
		struct list_head *list_head = &entry->list_head;
		struct list_entry *list_entry = NULL;
		while (!SLIST_EMPTY(list_head)) {
			list_entry = SLIST_FIRST(list_head);
			SLIST_REMOVE_HEAD(list_head, pointers);
			free(list_entry);
		}
	}
	// must destroy the mutex before freeing hash table
	// why? because if we destroy after, it's gonna be an invalid memory access
	// because hash table is already destroyed (free), so we're trying to access mutex
	// through it, which is invalid
	int err = pthread_mutex_destroy(&hash_table->mutex);
	if(err != 0) exit(err); 
	free(hash_table);
}
