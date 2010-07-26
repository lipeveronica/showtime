/*
 *  Property trees
 *  Copyright (C) 2008 Andreas Öman
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef PROP_I_H__
#define PROP_I_H__


#include "prop.h"

extern hts_mutex_t prop_mutex;



TAILQ_HEAD(prop_queue, prop);
LIST_HEAD(prop_list, prop);
LIST_HEAD(prop_sub_list, prop_sub);



/**
 *
 */
TAILQ_HEAD(prop_notify_queue, prop_notify);

struct prop_courier {

  struct prop_notify_queue pc_queue_nor;
  struct prop_notify_queue pc_queue_exp;

  hts_mutex_t *pc_entry_mutex;
  hts_cond_t pc_cond;
  int pc_has_cond;

  hts_thread_t pc_thread;
  int pc_run;
  int pc_detached;

  void (*pc_notify)(void *opaque);
  void *pc_opaque;
  
};



/**
 * Property types
 */
typedef enum {
  PROP_VOID,
  PROP_DIR,
  PROP_STRING,
  PROP_FLOAT,
  PROP_INT,
  PROP_PIXMAP,
  PROP_LINK,
  PROP_ZOMBIE, /* Destroyed can never be changed again */
} prop_type_t;


/**
 *
 */
struct prop {

  /**
   * Refcount. Not protected by mutex. Modification needs to be issued
   * using atomic ops. This refcount only protects the memory allocated
   * for this property, or in other words you can assume that a pointer
   * to a prop_t is valid as long as you own a reference to it.
   *
   * Note: hp_xref which is another refcount protecting contents of the
   * entire property
   */
  int hp_refcount;

  /**
   * Property name. Protected by mutex
   */
  const char *hp_name;

  /**
   * Parent linkage. Protected by mutex
   */
  struct prop *hp_parent;
  TAILQ_ENTRY(prop) hp_parent_link;


  /**
   * Originating property. Used when reflecting properties
   * in the tree (aka symlinks). Protected by mutex
   */
  struct prop *hp_originator;
  LIST_ENTRY(prop) hp_originator_link;

  /**
   * Properties receiving our values. Protected by mutex
   */
  struct prop_list hp_targets;


  /**
   * Subscriptions. Protected by mutex
   */
  struct prop_sub_list hp_value_subscriptions;
  struct prop_sub_list hp_canonical_subscriptions;

  /**
   * Payload type
   * Protected by mutex
   */
  uint8_t hp_type;

  /**
   * Various flags
   * Protected by mutex
   */
  uint8_t hp_flags;



  /**
   * Extended refcount. Used to keep contents of the property alive
   * We limit this to 255, should never be a problem. And it's checked
   * in the code as well
   * Protected by mutex
   */
  uint8_t hp_xref;


  /**
   * Actual payload
   * Protected by mutex
   */
  union {
    struct {
      float val, min, max;
    } f;
    struct {
      int val, min, max;
    } i;
    rstr_t *rstr;
    struct {
      struct prop_queue childs;
      struct prop *selected;
    } c;
    struct pixmap *pixmap;
    struct {
      rstr_t *rtitle;
      rstr_t *rurl;
    } link;
  } u;

#define hp_rstring   u.rstr
#define hp_float    u.f.val
#define hp_int      u.i.val
#define hp_childs   u.c.childs
#define hp_selected u.c.selected
#define hp_pixmap   u.pixmap
#define hp_link_rtitle u.link.rtitle
#define hp_link_rurl   u.link.rurl

};



/**
 *
 */
struct prop_sub {

  /**
   * Refcount. Not protected by mutex. Modification needs to be issued
   * using atomic ops.
   */
  int hps_refcount;

  /**
   * Callback. May never be changed. Not protected by mutex
   */
  void *hps_callback;

  /**
   * Opaque value for callback
   */
  void *hps_opaque;

  /**
   * Trampoline. A tranform function that invokes the actual user
   * supplied callback.
   * May never be changed. Not protected by mutex.
   */
  prop_trampoline_t *hps_trampoline;

  /**
   * Pointer to courier, May never be changed. Not protected by mutex
   */
  prop_courier_t *hps_courier;

  /**
   * Lock to be held when invoking callback. It must also be held
   * when destroying the subscription.
   */
  void *hps_lock;

  /**
   * Function to call to obtain / release the lock.
   */
  prop_lockmgr_t *hps_lockmgr;

  /**
   * Set when a subscription is destroyed. Protected by hps_lock.
   * In other words. It's impossible to destroy a subscription
   * if no lock is specified.
   */
  uint8_t hps_zombie;

  /**
   * Used to avoid sending two notification when relinking
   * to another tree. Protected by global mutex
   */
  uint8_t hps_pending_unlink;

  /**
   * Flags as passed to prop_subscribe(). May never be changed
   */
  uint8_t hps_flags;

  /**
   * Linkage to property. Protected by global mutex
   */
  LIST_ENTRY(prop_sub) hps_value_prop_link;
  prop_t *hps_value_prop;

  /**
   * Linkage to property. Protected by global mutex
   */
  LIST_ENTRY(prop_sub) hps_canonical_prop_link;
  prop_t *hps_canonical_prop;

};

#endif // PROP_I_H__
