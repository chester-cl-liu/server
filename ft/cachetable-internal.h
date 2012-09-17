/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:

#ifndef TokuDB_cachetable_internal_h
#define TokuDB_cachetable_internal_h

#ident "$Id: cachetable.h 46050 2012-07-24 02:26:17Z zardosht $"
#ident "Copyright (c) 2007-2012 Tokutek Inc.  All rights reserved."
#ident "The technology is licensed by the Massachusetts Institute of Technology, Rutgers State University of New Jersey, and the Research Foundation of State University of New York at Stony Brook under United States of America Serial No. 11/760379 and to the patents and/or patent applications resulting from it."

#include "frwlock.h"
#include "nonblocking_mutex.h"
#include "kibbutz.h"
#include "background_job_manager.h"
#include "partitioned_counter.h"

//////////////////////////////////////////////////////////////////////////////
//
// This file contains the classes and structs that make up the cachetable.
// The structs are:
//  - cachefile
//  - ctpair
//  - pair_list
//  - cachefile_list
//  - checkpointer
//  - evictor
//  - cleaner
//
// The rest of this comment assumes familiarity with the locks used in these
// classes/structs and what the locks protect. Nevertheless, here is 
// a list of the locks that we have:
//  - pair_list->list_lock
//  - pair_list->pending_lock_expensive
//  - pair_list->pending_lock_cheap
//  - cachefile_list->lock
//  - PAIR->mutex
//  - PAIR->value_rwlock
//  - PAIR->disk_nb_mutex
//
// Here are rules for how the locks interact:
//  - To grab any of the pair_list's locks, or the cachefile_list's lock,
//      the cachetable must be in existence
//  - To grab the PAIR mutex, we must know the PAIR will not dissappear:
//   - the PAIR must be pinned (value_rwlock or disk_nb_mutex is held)
//   - OR, the pair_list's list lock is held
//  - As a result, to get rid of a PAIR from the pair_list, we must hold
//     both the pair_list's list_lock and the PAIR's mutex
//  - To grab PAIR->value_rwlock, we must hold the PAIR's mutex
//  - To grab PAIR->disk_nb_mutex, we must hold the PAIR's mutex
//      and hold PAIR->value_rwlock
//
// Now let's talk about ordering. Here is an order from outer to inner (top locks must be grabbed first)
//  - pair_list->pending_lock_expensive
//  - pair_list->list_lock
//  - cachefile_list->lock
//  - PAIR->mutex
//  - pair_list->pending_lock_cheap <-- after grabbing this lock, 
//                                      NO other locks 
//                                      should be grabbed.
//  - when grabbing PAIR->value_rwlock or PAIR->disk_nb_mutex,
//     if the acquisition will not block, then it does not matter if any other locks held,
//     BUT if the acquisition will block, then NO other locks may be held besides
//     PAIR->mutex.
// 
// HERE ARE TWO EXAMPLES:
// To pin a PAIR on a client thread, the following must be done:
//  - first grab the list lock and find the PAIR
//  - with the list lock grabbed, grab PAIR->mutex
//  - with PAIR->mutex held:
//   - release list lock
//   - pin PAIR
//   - with PAIR pinned, grab pending_lock_cheap,
//   - copy and clear PAIR->checkpoint_pending,
//   - resolve checkpointing if necessary
//   - return to user.
//  The list lock may be held while pinning the PAIR if 
//  the PAIR has no contention. Otherwise, we may have
//  get a deadlock with another thread that has the PAIR pinned,
//  tries to pin some other PAIR, and in doing so, grabs the list lock.
//
// To unpin a PAIR on a client thread:
//  - because the PAIR is pinned, we don't need the pair_list's list_lock
//  - so, simply acquire PAIR->mutex
//  - unpin the PAIR
//  - return
//
//////////////////////////////////////////////////////////////////////////////
class evictor;
class pair_list;

///////////////////////////////////////////////////////////////////////////////
//
// Maps to a file on disk.
//
struct cachefile {
    CACHEFILE next;
    bool for_checkpoint; //True if part of the in-progress checkpoint

    // If set and the cachefile closes, the file will be removed.
    // Clients must not operate on the cachefile after setting this,
    // nor attempt to open any cachefile with the same fname (dname)
    // until this cachefile has been fully closed and unlinked.
    bool unlink_on_close;
    int fd;       /* Bug: If a file is opened read-only, then it is stuck in read-only.  If it is opened read-write, then subsequent writers can write to it too. */
    CACHETABLE cachetable;
    struct fileid fileid;
    FILENUM filenum;
    char *fname_in_env; /* Used for logging */

    void *userdata;
    int (*log_fassociate_during_checkpoint)(CACHEFILE cf, void *userdata); // When starting a checkpoint we must log all open files.
    int (*log_suppress_rollback_during_checkpoint)(CACHEFILE cf, void *userdata); // When starting a checkpoint we must log which files need rollbacks suppressed
    int (*close_userdata)(CACHEFILE cf, int fd, void *userdata, char **error_string, bool lsnvalid, LSN); // when closing the last reference to a cachefile, first call this function. 
    int (*begin_checkpoint_userdata)(LSN lsn_of_checkpoint, void *userdata); // before checkpointing cachefiles call this function.
    int (*checkpoint_userdata)(CACHEFILE cf, int fd, void *userdata); // when checkpointing a cachefile, call this function.
    int (*end_checkpoint_userdata)(CACHEFILE cf, int fd, void *userdata); // after checkpointing cachefiles call this function.
    int (*note_pin_by_checkpoint)(CACHEFILE cf, void *userdata); // add a reference to the userdata to prevent it from being removed from memory
    int (*note_unpin_by_checkpoint)(CACHEFILE cf, void *userdata); // add a reference to the userdata to prevent it from being removed from memory
    BACKGROUND_JOB_MANAGER bjm;
};


///////////////////////////////////////////////////////////////////////////////
//
//  The pair represents the data stored in the cachetable.
//
struct ctpair {
    // these fields are essentially constants. They do not change.
    CACHEFILE cachefile;
    CACHEKEY key;
    uint32_t fullhash;
    CACHETABLE_FLUSH_CALLBACK flush_callback;
    CACHETABLE_PARTIAL_EVICTION_EST_CALLBACK pe_est_callback;
    CACHETABLE_PARTIAL_EVICTION_CALLBACK pe_callback;
    CACHETABLE_CLEANER_CALLBACK cleaner_callback;
    CACHETABLE_CLONE_CALLBACK clone_callback;
    void *write_extraargs;

    // access to these fields are protected by disk_nb_mutex
    void* cloned_value_data; // cloned copy of value_data used for checkpointing
    long cloned_value_size; // size of cloned_value_data, used for accounting of size_current
    void* disk_data; // data used to fetch/flush value_data to and from disk.

    // access to these fields are protected by value_rwlock
    void* value_data; // data used by client threads, FTNODEs and ROLLBACK_LOG_NODEs
    PAIR_ATTR attr;
    enum cachetable_dirty dirty;

    // protected by PAIR->mutex
    uint32_t count;        // clock count

    // locks
    toku::frwlock value_rwlock;
    struct nb_mutex disk_nb_mutex; // single writer, protects disk_data, is used for writing cloned nodes for checkpoint
    toku_mutex_t mutex;

    // Access to checkpoint_pending is protected by two mechanisms,
    // the value_rwlock and the pair_list's pending locks (expensive and cheap).
    // checkpoint_pending may be true of false. 
    // Here are the rules for reading/modifying this bit.
    //  - To transition this field from false to true during begin_checkpoint,
    //   we must be holding both of the pair_list's pending locks.
    //  - To transition this field from true to false during end_checkpoint,
    //   we must be holding the value_rwlock.
    //  - For a non-checkpoint thread to read the value, we must hold both the
    //   value_rwlock and one of the pair_list's pending locks
    //  - For the checkpoint thread to read the value, we must 
    //   hold the value_rwlock
    //
    bool checkpoint_pending; // If this is on, then we have got to resolve checkpointing modifying it.

    // these are variables that are only used to transfer information to background threads
    // we cache them here to avoid a malloc. In the future, we should investigate if this
    // is necessary, as having these fields here is not technically necessary
    long size_evicting_estimate;
    evictor* ev;
    pair_list* list;

    // A PAIR is stored in a pair_list (which happens to be PAIR->list).
    // These variables are protected by the list lock in the pair_list
    //
    // clock_next,clock_prev represent a circular doubly-linked list.
    PAIR clock_next,clock_prev; // In clock.
    PAIR hash_chain;

    // pending_next,pending_next represent a non-circular doubly-linked list.
    PAIR pending_next;
    PAIR pending_prev;
};

//
// This initializes the fields and members of the pair.
//
void pair_init(PAIR p,
    CACHEFILE cachefile,
    CACHEKEY key,
    void *value,
    PAIR_ATTR attr,
    enum cachetable_dirty dirty,
    uint32_t fullhash,
    CACHETABLE_WRITE_CALLBACK write_callback,
    evictor *ev,
    pair_list *list);


///////////////////////////////////////////////////////////////////////////////
//
//  The pair list maintains the set of PAIR's that make up
//  the cachetable.
//
class pair_list {
public:
    //
    // the following fields are protected by the list lock
    // 
    uint32_t m_n_in_table; // number of pairs in the hash table
    uint32_t m_table_size; // number of buckets in the hash table
    PAIR *m_table; // hash table
    // 
    // The following fields are the heads of various linked lists.
    // They also protected by the list lock, but their 
    // usage is not as straightforward. For each of them,
    // only ONE thread is allowed iterate over them with 
    // a read lock on the list lock. All other threads
    // that want to modify elements in the lists or iterate over
    // the lists must hold the write list lock. Here is the
    // association between what threads may hold a read lock
    // on the list lock while iterating:
    //  - clock_head -> eviction thread (evictor)
    //  - cleaner_head -> cleaner thread (cleaner)
    //  - pending_head -> checkpoint thread (checkpointer)
    //
    PAIR m_clock_head; // of clock . head is the next thing to be up for decrement. 
    PAIR m_cleaner_head; // for cleaner thread. head is the next thing to look at for possible cleaning.
    PAIR m_pending_head; // list of pairs marked with checkpoint_pending

    // this field is public so we are still POD

    // usage of this lock is described above
    toku_pthread_rwlock_t m_list_lock;
    //
    // these locks are the "pending locks" referenced 
    // in comments about PAIR->checkpoint_pending. There
    // are two of them, but both serve the same purpose, which
    // is to protect the transition of a PAIR's checkpoint pending
    // value from false to true during begin_checkpoint.
    // We use two locks, because threads that want to read the
    // checkpoint_pending value may hold a lock for varying periods of time.
    // Threads running eviction may need to protect checkpoint_pending
    // while writing a node to disk, which is an expensive operation,
    // so it uses pending_lock_expensive. Client threads that
    // want to pin PAIRs will want to protect checkpoint_pending
    // just long enough to read the value and wipe it out. This is
    // a cheap operation, and as a result, uses pending_lock_cheap.
    //
    // By having two locks, and making begin_checkpoint first 
    // grab pending_lock_expensive and then pending_lock_cheap,
    // we ensure that threads that want to pin nodes can grab
    // only pending_lock_cheap, and never block behind threads
    // holding pending_lock_expensive and writing a node out to disk
    //
    toku_pthread_rwlock_t m_pending_lock_expensive;
    toku_pthread_rwlock_t m_pending_lock_cheap;
    void init();
    int destroy();
    void evict(PAIR pair);
    void put(PAIR pair);
    PAIR find_pair(CACHEFILE file, CACHEKEY key, uint32_t hash);
    void pending_pairs_remove (PAIR p);
    void verify();
    void get_state(int *num_entries, int *hash_size);
    void read_list_lock();
    void read_list_unlock();
    void write_list_lock();
    void write_list_unlock();
    void read_pending_exp_lock();
    void read_pending_exp_unlock();
    void write_pending_exp_lock();
    void write_pending_exp_unlock();
    void read_pending_cheap_lock();
    void read_pending_cheap_unlock();
    void write_pending_cheap_lock();
    void write_pending_cheap_unlock();

private:
    void pair_remove (PAIR p);
    void rehash (uint32_t newtable_size);
    void add_to_clock (PAIR p);
    PAIR remove_from_hash_chain (PAIR remove_me, PAIR list);
};

///////////////////////////////////////////////////////////////////////////////
//
// Wrapper for the head of our cachefile list.
//
class cachefile_list {
public:
    void init();
    void destroy();
    void read_lock();
    void read_unlock();
    void write_lock();
    void write_unlock();
    // access to these fields are protected by the lock
    CACHEFILE m_head;
    FILENUM m_next_filenum_to_use;
    toku_pthread_rwlock_t m_lock; // this field is publoc so we are still POD
};


///////////////////////////////////////////////////////////////////////////////
//
//  The checkpointer handles starting and finishing checkpoints of the 
//  cachetable's data.
//
class checkpointer {
public:
    void init(pair_list *_pl, TOKULOGGER _logger, evictor *_ev, cachefile_list *files);
    void destroy();
    int set_checkpoint_period(uint32_t new_period);
    uint32_t get_checkpoint_period();
    int shutdown();
    bool has_been_shutdown();
    int begin_checkpoint();
    void add_background_job();
    void remove_background_job();
    int end_checkpoint(bool aggressive, void (*testcallback_f)(void*),  void* testextra);
    TOKULOGGER get_logger();
    // used during begin_checkpoint
    void increment_num_txns();
private:
    uint32_t m_checkpoint_num_txns;   // how many transactions are in the checkpoint
    TOKULOGGER m_logger;
    LSN m_lsn_of_checkpoint_in_progress;
    uint32_t m_checkpoint_num_files; // how many cachefiles are in the checkpoint
    struct minicron m_checkpointer_cron; // the periodic checkpointing thread
    cachefile_list *m_cf_list;
    pair_list *m_list;
    evictor *m_ev;
    
    // variable used by the checkpoint thread to know
    // when all work induced by cloning on client threads is done
    BACKGROUND_JOB_MANAGER m_checkpoint_clones_bjm;
    // private methods for begin_checkpoint    
    void update_cachefiles();
    void log_begin_checkpoint();
    void turn_on_pending_bits();
    // private methods for end_checkpoint    
    void fill_checkpoint_cfs(CACHEFILE* checkpoint_cfs);
    void checkpoint_pending_pairs(bool aggressive);
    void checkpoint_userdata(CACHEFILE* checkpoint_cfs);
    void log_end_checkpoint();
    void end_checkpoint_userdata(CACHEFILE* checkpoint_cfs);
    int remove_cachefiles(CACHEFILE* checkpoint_cfs);
    
    // Unit test struct needs access to private members.
    friend struct checkpointer_test;
};

//
// This is how often we want the eviction thread
// to run, in seconds.
//
const int EVICTION_PERIOD = 1;

///////////////////////////////////////////////////////////////////////////////
//
// The evictor handles the removal of pairs from the pair list/cachetable.
//
class evictor {
public:
    void init(long _size_limit, pair_list* _pl, KIBBUTZ _kibbutz, uint32_t eviction_period);
    void destroy();
    void add_pair_attr(PAIR_ATTR attr);
    void remove_pair_attr(PAIR_ATTR attr);    
    void change_pair_attr(PAIR_ATTR old_attr, PAIR_ATTR new_attr);
    void add_to_size_current(long size);
    void remove_from_size_current(long size);
    uint64_t reserve_memory(double fraction);
    void release_reserved_memory(uint64_t reserved_memory);
    void run_eviction_thread();
    void do_partial_eviction(PAIR p);
    void evict_pair(PAIR p, bool checkpoint_pending);
    void wait_for_cache_pressure_to_subside();
    void signal_eviction_thread();
    bool should_client_thread_sleep();
    bool should_client_wake_eviction_thread();
    // function needed for testing
    void get_state(long *size_current_ptr, long *size_limit_ptr);
    void fill_engine_status();
private:
    void run_eviction();
    bool run_eviction_on_pair(PAIR p);
    void try_evict_pair(PAIR p);
    void decrease_size_evicting(long size_evicting_estimate);
    bool should_sleeping_clients_wakeup();
    bool eviction_needed();
    
    pair_list* m_pl;
    int64_t m_size_current;            // the sum of the sizes of the pairs in the cachetable
    // changes to these two values are protected
    // by ev_thread_lock
    int64_t m_size_reserved;           // How much memory is reserved (e.g., by the loader)
    int64_t m_size_evicting;           // the sum of the sizes of the pairs being written

    // these are constants
    int64_t m_low_size_watermark; // target max size of cachetable that eviction thread aims for
    int64_t m_low_size_hysteresis; // if cachetable grows to this size, client threads wake up eviction thread upon adding data
    int64_t m_high_size_watermark; // if cachetable grows to this size, client threads sleep upon adding data
    int64_t m_high_size_hysteresis; // if > cachetable size, then sleeping client threads may wake up

    // mutex that protects fields listed immedietly below
    toku_mutex_t m_ev_thread_lock;
    // the eviction thread
    toku_pthread_t m_ev_thread;
    // condition variable that controls the sleeping period
    // of the eviction thread
    toku_cond_t m_ev_thread_cond;
    // number of client threads that are currently sleeping
    // due to an over-subscribed cachetable
    uint32_t m_num_sleepers;
    // states if the eviction thread should run. set to true
    // in init, set to false during destroy
    bool m_run_thread;
    // bool that states if the eviction thread is currently running
    bool m_ev_thread_is_running;
    // period which the eviction thread sleeps
    uint32_t m_period_in_seconds;
    // condition variable on which client threads wait on when sleeping
    // due to an over-subscribed cachetable
    toku_cond_t m_flow_control_cond;

    // variables for engine status
    PARTITIONED_COUNTER m_size_nonleaf;
    PARTITIONED_COUNTER m_size_leaf;
    PARTITIONED_COUNTER m_size_rollback;
    PARTITIONED_COUNTER m_size_cachepressure;
    
    KIBBUTZ m_kibbutz;

    // this variable is ONLY used for testing purposes
    uint64_t m_num_eviction_thread_runs;
    friend class evictor_test_helpers;
    friend class evictor_unit_test;
};

///////////////////////////////////////////////////////////////////////////////
//
// Iterates over the clean head in the pair list, calling the cleaner
// callback on each node in that list.
//
class cleaner {
public:
    void init(uint32_t cleaner_iterations, pair_list* _pl, CACHETABLE _ct);
    void destroy(void);
    uint32_t get_iterations(void);
    void set_iterations(uint32_t new_iterations);
    uint32_t get_period(void);
    uint32_t get_period_unlocked(void);
    void set_period(uint32_t new_period);
    int run_cleaner(void);
    
private:
    pair_list* m_pl;
    CACHETABLE m_ct;
    struct minicron m_cleaner_cron; // the periodic cleaner thread
    uint32_t m_cleaner_iterations; // how many times to run the cleaner per
                                  // cleaner period (minicron has a
                                  // minimum period of 1s so if you want
                                  // more frequent cleaner runs you must
                                  // use this)
};

///////////////////////////////////////////////////////////////////////////////
//
// The cachetable is as close to an ENV as we get.
//
struct cachetable {
    pair_list list;
    cleaner cl;
    evictor ev;
    checkpointer cp;
    cachefile_list cf_list;
    
    KIBBUTZ client_kibbutz; // pool of worker threads and jobs to do asynchronously for the client.
    KIBBUTZ ct_kibbutz; // pool of worker threads and jobs to do asynchronously for the cachetable
    KIBBUTZ checkpointing_kibbutz; // small pool for checkpointing cloned pairs

    char *env_dir;
};

#endif // End of header guardian.
