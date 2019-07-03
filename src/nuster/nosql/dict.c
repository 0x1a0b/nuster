/*
 * nuster nosql dict functions.
 *
 * Copyright (C) Jiang Wenyuan, < koubunen AT gmail DOT com >
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <types/global.h>

#include <nuster/memory.h>
#include <nuster/shctx.h>
#include <nuster/nuster.h>


static int _nst_nosql_dict_resize(uint64_t size) {
    struct nst_nosql_dict dict;

    dict.size  = size;
    dict.used  = 0;
    dict.entry = malloc(sizeof(struct nst_nosql_entry*) * size);

    if(dict.entry) {
        int i;

        for(i = 0; i < size; i++) {
            dict.entry[i] = NULL;
        }

        if(!nuster.nosql->dict[0].entry) {
            nuster.nosql->dict[0] = dict;
            return 1;
        } else {
            nuster.nosql->dict[1] = dict;
            nuster.nosql->rehash_idx = 0;
            return 1;
        }

    }

    return 0;
}

static int _nst_nosql_dict_alloc(uint64_t size) {
    int i, entry_size = sizeof(struct nst_nosql_entry*);

    nuster.nosql->dict[0].size  = size / entry_size;
    nuster.nosql->dict[0].used  = 0;
    nuster.nosql->dict[0].entry =
        nst_memory_alloc(global.nuster.nosql.memory,
                global.nuster.nosql.memory->block_size);

    if(!nuster.nosql->dict[0].entry) {
        return NST_ERR;
    }

    for(i = 1; i < size / global.nuster.nosql.memory->block_size; i++) {
        if(!nst_memory_alloc(global.nuster.nosql.memory,
                    global.nuster.nosql.memory->block_size)) {
            return NST_ERR;
        }
    }

    for(i = 0; i < nuster.nosql->dict[0].size; i++) {
        nuster.nosql->dict[0].entry[i] = NULL;
    }

    return nst_shctx_init((&nuster.nosql->dict[0]));
}

int nst_nosql_dict_init() {
    int size = (global.nuster.nosql.memory->block_size
            + global.nuster.nosql.dict_size - 1)
        / global.nuster.nosql.memory->block_size
        * global.nuster.nosql.memory->block_size;

    return _nst_nosql_dict_alloc(size);
}

static int _nst_nosql_dict_rehashing() {
    return nuster.nosql->rehash_idx != -1;
}

/*
 * Rehash dict if nosql->dict[0] is almost full
 */
void nst_nosql_dict_rehash() {

    if(_nst_nosql_dict_rehashing()) {
        int max_empty = 10;
        struct nst_nosql_entry *entry = NULL;

        /* check max_empty entryies */
        while(!nuster.nosql->dict[0].entry[nuster.nosql->rehash_idx]) {
            nuster.nosql->rehash_idx++;
            max_empty--;

            if(nuster.nosql->rehash_idx >= nuster.nosql->dict[0].size) {
                return;
            }

            if(!max_empty) {
                return;
            }
        }

        /* move all entries in this bucket to dict[1] */
        entry = nuster.nosql->dict[0].entry[nuster.nosql->rehash_idx];
        while(entry) {
            int idx = entry->hash % nuster.nosql->dict[1].size;
            struct nst_nosql_entry *entry_next = entry->next;

            entry->next = nuster.nosql->dict[1].entry[idx];
            nuster.nosql->dict[1].entry[idx] = entry;
            nuster.nosql->dict[1].used++;
            nuster.nosql->dict[0].used--;
            entry = entry_next;
        }

        nuster.nosql->dict[0].entry[nuster.nosql->rehash_idx] = NULL;
        nuster.nosql->rehash_idx++;

        /* have we rehashed the whole dict? */
        if(nuster.nosql->dict[0].used == 0) {
            free(nuster.nosql->dict[0].entry);
            nuster.nosql->dict[0]       = nuster.nosql->dict[1];
            nuster.nosql->rehash_idx    = -1;
            nuster.nosql->cleanup_idx   = 0;
            nuster.nosql->dict[1].entry = NULL;
            nuster.nosql->dict[1].size  = 0;
            nuster.nosql->dict[1].used  = 0;
        }
    } else {

        /* should we rehash? */
        if(nuster.nosql->dict[0].used >= nuster.nosql->dict[0].size
                * NST_NOSQL_DEFAULT_LOAD_FACTOR) {

            _nst_nosql_dict_resize(nuster.nosql->dict[0].size
                    * NST_NOSQL_DEFAULT_GROWTH_FACTOR);
        }
    }
}

/*
 * Check entry validity, free the entry if its invalid,
 * If its invalid set entry->data->invalid to true,
 * entry->data is freed by _nosql_data_cleanup
 */
void nst_nosql_dict_cleanup() {
    struct nst_nosql_entry *entry =
        nuster.nosql->dict[0].entry[nuster.nosql->cleanup_idx];

    struct nst_nosql_entry *prev  = entry;

    if(!nuster.nosql->dict[0].used) {
        return;
    }

    while(entry) {

        if(nst_nosql_entry_invalid(entry)) {
            struct nst_nosql_entry *tmp = entry;

            if(entry->data) {
                entry->data->invalid = 1;
            }

            if(prev == entry) {
                nuster.nosql->dict[0].entry[nuster.nosql->cleanup_idx] =
                    entry->next;

                prev = entry->next;
            } else {
                prev->next = entry->next;
            }

            entry = entry->next;
            nst_memory_free(global.nuster.nosql.memory, tmp->key);
            nst_memory_free(global.nuster.nosql.memory, tmp->host.data);
            nst_memory_free(global.nuster.nosql.memory, tmp->path.data);
            nst_memory_free(global.nuster.nosql.memory, tmp);
            nuster.nosql->dict[0].used--;
        } else {
            prev  = entry;
            entry = entry->next;
        }
    }

    nuster.nosql->cleanup_idx++;

    /* if we have checked the whole dict */
    if(nuster.nosql->cleanup_idx == nuster.nosql->dict[0].size) {
        nuster.nosql->cleanup_idx = 0;
    }
}

/*
 * Add a new nst_nosql_entry to nosql_dict
 */
struct nst_nosql_entry *nst_nosql_dict_set(struct nst_nosql_ctx *ctx) {

    struct nst_nosql_dict  *dict  = NULL;
    struct nst_nosql_data  *data  = NULL;
    struct nst_nosql_entry *entry = NULL;
    int idx;

    dict = _nst_nosql_dict_rehashing()
        ? &nuster.nosql->dict[1] : &nuster.nosql->dict[0];

    entry = nst_memory_alloc(global.nuster.nosql.memory, sizeof(*entry));
    if(!entry) {
        return NULL;
    }

    if(ctx->rule->disk != NST_DISK_ONLY) {
        data = nst_nosql_data_new();

        if(!data) {
            nst_memory_free(global.nuster.nosql.memory, entry);
            return NULL;
        }
    }

    idx = ctx->hash % dict->size;
    /* prepend entry to dict->entry[idx] */
    entry->next      = dict->entry[idx];
    dict->entry[idx] = entry;
    dict->used++;

    /* init entry */
    entry->data   = data;
    entry->state  = NST_NOSQL_ENTRY_STATE_CREATING;
    entry->key    = ctx->key;
    entry->hash   = ctx->hash;
    ctx->key      = NULL;
    entry->expire = 0;
    entry->rule   = ctx->rule;
    entry->pid    = ctx->pid;

    entry->header_len = ctx->header_len;

    entry->host.data   = ctx->req.host.data;
    entry->host.len    = ctx->req.host.len;
    ctx->req.host.data = NULL;

    entry->path.data   = ctx->req.path.data;
    entry->path.len    = ctx->req.path.len;
    ctx->req.path.data = NULL;

    return entry;
}

/*
 * Get entry
 */
struct nst_nosql_entry *nst_nosql_dict_get(struct buffer *key, uint64_t hash) {
    int i, idx;
    struct nst_nosql_entry *entry = NULL;

    if(nuster.nosql->dict[0].used + nuster.nosql->dict[1].used == 0) {
        return NULL;
    }

    for(i = 0; i <= 1; i++) {
        idx   = hash % nuster.nosql->dict[i].size;
        entry = nuster.nosql->dict[i].entry[idx];

        while(entry) {

            if(entry->hash == hash
                    && entry->key->data == key->data
                    && !memcmp(entry->key->area, key->area, key->data)) {
                /* check expire
                 * change state only, leave the free stuff to cleanup
                 * */
                if(entry->state == NST_NOSQL_ENTRY_STATE_VALID
                        && nst_nosql_dict_entry_expired(entry)) {

                    entry->state         = NST_NOSQL_ENTRY_STATE_EXPIRED;
                    entry->data->invalid = 1;
                    entry->data          = NULL;
                    entry->expire        = 0;
                    return NULL;
                }

                return entry;
            }

            entry = entry->next;
        }

        if(!_nst_nosql_dict_rehashing()) {
            return NULL;
        }
    }

    return NULL;
}

