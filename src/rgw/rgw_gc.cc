

#include "rgw_gc.h"
#include "include/rados/librados.hpp"
#include "cls/rgw/cls_rgw_client.h"
#include "cls/lock/cls_lock_client.h"
#include "auth/Crypto.h"

#include <list>

#define dout_subsys ceph_subsys_rgw

using namespace std;
using namespace librados;

static string gc_oid_prefix = "gc";
static string gc_index_lock_name = "gc_process";

void RGWGC::initialize(CephContext *_cct, RGWRados *_store) {
  cct = _cct;
  store = _store;

  max_objs = cct->_conf->rgw_gc_max_objs;
  obj_names = new string[max_objs];

  for (int i = 0; i < max_objs; i++) {
    obj_names[i] = gc_oid_prefix;
    char buf[32];
    snprintf(buf, 32, ".%d", i);
    obj_names[i].append(buf);
  }
}

void RGWGC::finalize()
{
  for (int i = 0; i < max_objs; i++) {
    delete[] obj_names;
  }
}

int RGWGC::tag_index(const string& tag)
{
  return ceph_str_hash_linux(tag.c_str(), tag.size()) % max_objs;
}

void RGWGC::add_chain(ObjectWriteOperation& op, cls_rgw_obj_chain& chain, const string& tag, bool create)
{
  cls_rgw_gc_obj_info info;
  info.chain = chain;
  info.tag = tag;

  cls_rgw_gc_set_entry(op, cct->_conf->rgw_gc_obj_min_wait, info, create);
}

int RGWGC::send_chain(cls_rgw_obj_chain& chain, const string& tag, bool create)
{
  ObjectWriteOperation op;
  add_chain(op, chain, tag, create);

  int i = tag_index(tag);

  return store->gc_operate(obj_names[i], &op);
}

int RGWGC::remove(int index, const std::list<string>& tags)
{
  ObjectWriteOperation op;
  cls_rgw_gc_remove(op, tags);
  return store->gc_operate(obj_names[index], &op);
}

int RGWGC::list(int *index, string& marker, uint32_t max, std::list<cls_rgw_gc_obj_info>& result, bool *truncated)
{
  result.clear();

  for (; *index < cct->_conf->rgw_gc_max_objs && result.size() < max; (*index)++, marker.clear()) {
    std::list<cls_rgw_gc_obj_info> entries;
    int ret = cls_rgw_gc_list(store->gc_pool_ctx, obj_names[*index], marker, max - result.size(), entries, truncated);
    if (ret == -ENOENT)
      continue;
    if (ret < 0)
      return ret;

    std::list<cls_rgw_gc_obj_info>::iterator iter;
    for (iter = entries.begin(); iter != entries.end(); ++iter) {
      result.push_back(*iter);
    }

    if (*index == cct->_conf->rgw_gc_max_objs - 1) {
      /* we cut short here, truncated will hold the correct value */
      return 0;
    }

    if (result.size() == max) {
      /* close approximation, it might be that the next of the objects don't hold
       * anything, in this case truncated should have been false, but we can find
       * that out on the next iteration
       */
      *truncated = true;
      return 0;
    }

  }
  *truncated = false;

  return 0;
}

int RGWGC::process(int index, int max_secs)
{
  rados::cls::lock::Lock l(gc_index_lock_name);
  utime_t end = ceph_clock_now(g_ceph_context);
  std::list<string> remove_tags;

  /* max_secs should be greater than zero. We don't want a zero max_secs
   * to be translated as no timeout, since we'd then need to break the
   * lock and that would require a manual intervention. In this case
   * we can just wait it out. */
  if (max_secs <= 0)
    return -EAGAIN;

  end += max_secs;
  utime_t time(max_secs, 0);
  l.set_duration(time);

  int ret = l.lock_exclusive(store->gc_pool_ctx, obj_names[index]);
  if (ret == -EEXIST) /* already locked by another gc processor */
    return 0;
  if (ret < 0)
    return ret;

  string marker;
  bool truncated;
  IoCtx *ctx = NULL;
  do {
    int max = 100;
    std::list<cls_rgw_gc_obj_info> entries;
    ret = cls_rgw_gc_list(store->gc_pool_ctx, obj_names[index], marker, max, entries, &truncated);
    if (ret == -ENOENT) {
      ret = 0;
      goto done;
    }
    if (ret < 0)
      goto done;

    string last_pool;
    ctx = new IoCtx;
    std::list<cls_rgw_gc_obj_info>::iterator iter;
    for (iter = entries.begin(); iter != entries.end(); ++iter) {
      cls_rgw_gc_obj_info& info = *iter;
      std::list<cls_rgw_obj>::iterator liter;
      cls_rgw_obj_chain& chain = info.chain;

      utime_t now = ceph_clock_now(g_ceph_context);
      if (now >= end)
        goto done;

      for (liter = chain.objs.begin(); liter != chain.objs.end(); ++liter) {
        cls_rgw_obj& obj = *liter;

        if (obj.pool != last_pool) {
          if (ctx) {
            delete ctx;
            ctx = new IoCtx;
          }
	  ret = rgwstore->rados->ioctx_create(obj.pool.c_str(), *ctx);
	  if (ret < 0) {
	    dout(0) << "ERROR: failed to create ioctx pool=" << obj.pool << dendl;
	    continue;
	  }
          last_pool = obj.pool;
        }

        ctx->locator_set_key(obj.key);
	dout(0) << "gc::process: removing " << obj.pool << ":" << obj.oid << dendl;
        ret = ctx->remove(obj.oid);
	if (ret == -ENOENT)
	  ret = 0;
        if (ret < 0) {
          dout(0) << "failed to remove " << obj.pool << ":" << obj.oid << "@" << obj.key << dendl;
        }
	if (!ret) {
          remove_tags.push_back(info.tag);
#define MAX_REMOVE_CHUNK 16
          if (remove_tags.size() > MAX_REMOVE_CHUNK) {
            remove(index, remove_tags);
            remove_tags.clear();
          }
	}
      }
    }
  } while (truncated);

done:
  if (remove_tags.size())
    remove(index, remove_tags);
  l.unlock(store->gc_pool_ctx, obj_names[index]);
  delete ctx;
  return 0;
}

int RGWGC::process()
{
  int max_objs = cct->_conf->rgw_gc_max_objs;
  int max_secs = cct->_conf->rgw_gc_processor_max_time;

  unsigned start;
  int ret = get_random_bytes((char *)&start, sizeof(start));
  if (ret < 0)
    return ret;

  for (int i = 0; i < max_objs; i++) {
    int index = (i + start) % max_objs;
    ret = process(index, max_secs);
    if (ret < 0)
      return ret;
  }

  return 0;
}

