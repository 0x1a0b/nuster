/*
 * nuster cache dict functions.
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

#include <nuster/nuster.h>

static int _nst_cache_dict_alloc(uint64_t size) {
    int i;
    int entry_size = sizeof(struct nst_dict_entry*);
    int block_size = global.nuster.cache.memory->block_size;

    nuster.cache->dict.size  = size / entry_size;
    nuster.cache->dict.used  = 0;
    nuster.cache->dict.entry = nst_cache_memory_alloc(block_size);

    if(!nuster.cache->dict.entry) {
        return NST_ERR;
    }

    for(i = 1; i < size / block_size; i++) {

        if(!nst_cache_memory_alloc(block_size)) {
            return NST_ERR;
        }
    }

    for(i = 0; i < nuster.cache->dict.size; i++) {
        nuster.cache->dict.entry[i] = NULL;
    }

    return nst_shctx_init((&nuster.cache->dict));
}

int nst_cache_dict_init() {
    int block_size = global.nuster.cache.memory->block_size;
    int dict_size = global.nuster.cache.dict_size;
    int size = (block_size + dict_size - 1) / block_size * block_size;

    return _nst_cache_dict_alloc(size);
}

/*
 * Check entry validity, free the entry if its invalid,
 * If its invalid set entry->data->invalid to true,
 * entry->data is freed by _cache_data_cleanup
 */
void nst_cache_dict_cleanup() {
    struct nst_dict_entry *entry = nuster.cache->dict.entry[nuster.cache->cleanup_idx];

    struct nst_dict_entry *prev  = entry;

    if(!nuster.cache->dict.used) {
        return;
    }

    while(entry) {

        if(nst_dict_entry_invalid(entry)) {
            struct nst_dict_entry *tmp = entry;

            if(entry->data) {
                entry->data->invalid = 1;
            }

            if(prev == entry) {
                nuster.cache->dict.entry[nuster.cache->cleanup_idx] = entry->next;
                prev = entry->next;
            } else {
                prev->next = entry->next;
            }

            entry = entry->next;

            nst_cache_memory_free(tmp->buf.area);
            nst_cache_memory_free(tmp->key.data);
            nst_cache_memory_free(tmp);

            nuster.cache->dict.used--;
        } else {
            prev  = entry;
            entry = entry->next;
        }
    }

    nuster.cache->cleanup_idx++;

    /* if we have checked the whole dict */
    if(nuster.cache->cleanup_idx == nuster.cache->dict.size) {
        nuster.cache->cleanup_idx = 0;
    }
}

struct nst_dict_entry *nst_cache_dict_set(struct nst_cache_ctx *ctx) {
    struct nst_dict  *dict  = NULL;
    struct nst_data  *data  = NULL;
    struct nst_dict_entry *entry = NULL;
    struct nst_key key = { .data = NULL };
    struct buffer buf  = { .area = NULL };
    int idx;

    dict = &nuster.cache->dict;

    idx = ctx->rule->key->idx;

    key.size = ctx->keys[idx].size;
    key.hash = ctx->keys[idx].hash;
    key.data = nst_cache_memory_alloc(key.size);

    if(!key.data) {
        goto err;
    }

    memcpy(key.data, ctx->keys[idx].data, key.size);

    buf.size = ctx->txn.buf->data;
    buf.data = ctx->txn.buf->data;
    buf.area = nst_cache_memory_alloc(buf.size);

    if(!buf.area) {
        goto err;
    }

    memcpy(buf.area, ctx->txn.buf->area, buf.data);

    entry = nst_cache_memory_alloc(sizeof(*entry));

    if(!entry) {
        goto err;
    }

    memset(entry, 0, sizeof(*entry));

    if(ctx->rule->disk != NST_DISK_ONLY) {
        data = nst_cache_data_new();

        if(!data) {
            goto err;
        }
    }

    idx = key.hash % dict->size;

    /* prepend entry to dict->entry[idx] */
    entry->next      = dict->entry[idx];
    dict->entry[idx] = entry;
    dict->used++;

    /* init entry */
    entry->data   = data;
    entry->state  = NST_DICT_ENTRY_STATE_CREATING;
    entry->expire = 0;
    entry->pid    = ctx->pid;
    entry->file   = NULL;
    entry->ttl    = ctx->rule->ttl;

    entry->key  = key;
    entry->rule = ctx->rule;

    entry->extend[0] = ctx->rule->extend[0];
    entry->extend[1] = ctx->rule->extend[1];
    entry->extend[2] = ctx->rule->extend[2];
    entry->extend[3] = ctx->rule->extend[3];

    entry->header_len = ctx->txn.res.header_len;

    entry->buf = buf;

    entry->host.ptr = buf.area + (ctx->txn.req.host.ptr - ctx->txn.buf->area);
    entry->host.len = ctx->txn.req.host.len;

    entry->path.ptr = buf.area + (ctx->txn.req.path.ptr - ctx->txn.buf->area);
    entry->path.len = ctx->txn.req.path.len;

    entry->etag.ptr = buf.area + (ctx->txn.res.etag.ptr - ctx->txn.buf->area);
    entry->etag.len = ctx->txn.res.etag.len;

    entry->last_modified.ptr = buf.area + (ctx->txn.res.last_modified.ptr - ctx->txn.buf->area);
    entry->last_modified.len = ctx->txn.res.last_modified.len;

    return entry;

err:

    nst_cache_memory_free(key.data);
    nst_cache_memory_free(buf.area);
    nst_cache_memory_free(entry);

    return NULL;
}

/*
 * Get entry
 */

struct nst_dict_entry *nst_cache_dict_get(struct nst_key *key) {
    struct nst_dict_entry  *entry = NULL;
    int                     idx;

    if(nuster.cache->dict.used == 0) {
        return NULL;
    }

    idx   = key->hash % nuster.cache->dict.size;
    entry = nuster.cache->dict.entry[idx];

    while(entry) {

        if(entry->key.hash == key->hash && entry->key.size == key->size
                && !memcmp(entry->key.data, key->data, key->size)) {

            int expired = nst_dict_entry_expired(entry);

            uint64_t max = 1000 * entry->expire + 1000 * entry->ttl * entry->extend[3] / 100;

            entry->atime = get_current_timestamp();

            if(expired && entry->extend[0] != 0xFF && entry->atime <= max
                    && entry->access[3] > entry->access[2]
                    && entry->access[2] > entry->access[1]) {

                entry->expire    += entry->ttl;

                entry->access[0] += entry->access[1];
                entry->access[0] += entry->access[2];
                entry->access[0] += entry->access[3];
                entry->access[1]  = 0;
                entry->access[2]  = 0;
                entry->access[3]  = 0;
                entry->extended  += 1;

                if(entry->file) {
                    nst_persist_update_expire(entry->file, entry->expire);
                }

                expired = 0;
            }

            /* check expire
             * change state only, leave the free stuff to cleanup
             * */
            if(entry->state == NST_DICT_ENTRY_STATE_VALID && expired) {
                entry->state         = NST_DICT_ENTRY_STATE_EXPIRED;
                entry->data->invalid = 1;
                entry->data          = NULL;
                entry->expire        = 0;
                entry->access[0]     = 0;
                entry->access[1]     = 0;
                entry->access[2]     = 0;
                entry->access[3]     = 0;
                entry->extended      = 0;

                return NULL;
            }

            return entry;
        }

        entry = entry->next;
    }

    return NULL;
}

int nst_cache_dict_set_from_disk(struct buffer *buf, struct ist host, struct ist path,
        struct nst_key *key, char *file, char *meta) {

    struct nst_dict  *dict  = NULL;
    struct nst_dict_entry *entry = NULL;
    int idx;
    uint64_t ttl_extend;

    key->hash = nst_persist_meta_get_hash(meta);

    ttl_extend = nst_persist_meta_get_ttl_extend(meta);

    dict = &nuster.cache->dict;

    entry = nst_cache_memory_alloc(sizeof(*entry));

    if(!entry) {
        return NST_ERR;
    }

    memset(entry, 0, sizeof(*entry));

    entry->file = nst_cache_memory_alloc(strlen(file));

    if(!entry->file) {
        nst_cache_memory_free(entry);

        return NST_ERR;
    }

    idx = key->hash % dict->size;
    /* prepend entry to dict->entry[idx] */
    entry->next      = dict->entry[idx];
    dict->entry[idx] = entry;
    dict->used++;

    /* init entry */
    entry->state  = NST_DICT_ENTRY_STATE_INVALID;
    entry->key    = *key;
    entry->expire = nst_persist_meta_get_expire(meta);
    memcpy(entry->file, file, strlen(file));

    entry->header_len = nst_persist_meta_get_header_len(meta);

    entry->buf = *buf;

    entry->host = host;
    entry->path = path;

    entry->extend[0] = *( uint8_t *)(&ttl_extend);
    entry->extend[1] = *((uint8_t *)(&ttl_extend) + 1);
    entry->extend[2] = *((uint8_t *)(&ttl_extend) + 2);
    entry->extend[3] = *((uint8_t *)(&ttl_extend) + 3);

    entry->ttl = ttl_extend >> 32;

    return NST_OK;
}
