/* thread.c
 * Copyright 1984-2017 Cisco Systems, Inc.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 * http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "system.h"

static thread_gc *free_thread_gcs;

/* locally defined functions */
#ifdef PTHREADS
static s_thread_rv_t start_thread(void *tc);
static IBOOL destroy_thread(ptr tc);
#endif

#if defined(__APPLE__) && defined(__arm64__) && !defined(WRITE_XOR_EXECUTE_CODE)
/* A forked child runs on a copy of the forking thread, and XNU gives that copy
   the thread's MAP_JIT view (pthread_jit_write_protect_np) as the kernel last
   SAVED it, at the thread's last context switch, not its live value. A thread
   descheduled while it wrote code (a collection enables code writes for its
   length) that re-protected afterwards without another switch forks a child
   whose code pages are writable and not executable: the child's first
   instruction back in Scheme faults (SIGBUS, pc = fault address). Re-protect
   in every child before it returns to Scheme. */
static void fork_child_reprotect_code(void) { S_ENABLE_CODE_WRITE(0); }
#endif

void S_thread_init(void) {
#if defined(__APPLE__) && defined(__arm64__) && !defined(WRITE_XOR_EXECUTE_CODE)
  { static int registered = 0;
    if (!registered) { registered = 1; pthread_atfork(NULL, NULL, fork_child_reprotect_code); } }
#endif
  if (S_boot_time) {
    S_protect(&S_G.threadno);
    S_G.threadno = FIX(0);

#ifdef PTHREADS
   /* this is also reset in scheme.c after heap restoration */
    s_thread_mutex_init(&S_tc_mutex.pmutex);
    S_tc_mutex.owner = s_thread_self();
    S_tc_mutex.count = 0;
    s_thread_cond_init(&S_collect_cond);
    s_thread_cond_init(&S_collect_thread0_cond);
    s_thread_mutex_init(&S_alloc_mutex.pmutex);
    s_thread_cond_init(&S_terminated_cond);
    S_alloc_mutex.owner = 0;
    S_alloc_mutex.count = 0;
    S_main_thread_id = s_thread_self();

# ifdef IMPLICIT_ATOMIC_AS_EXPLICIT
    s_thread_mutex_init(&S_implicit_mutex);
# endif
#endif /* PTHREADS */
  }
}

/* this needs to be reworked.  currently, S_create_thread_object is
   called from main to create the base thread, from fork_thread when
   there is already an active current thread, and from S_activate_thread
   when there is no current thread.  scheme.c does part of the initialization of the
   base thread (e.g., parameters, current input/output ports) in one
   or more places. */
ptr S_create_thread_object(const char *who, ptr p_tc) {
  ptr thread, tc;
  thread_gc *tgc;
  INT i;

  tc_mutex_acquire();

  if (S_threads == Snil) {
    tc = TO_PTR(S_G.thread_context);
    tgc = &S_G.main_thread_gc;
    GCDATA(tc) = TO_PTR(tgc);
    tgc->tc = tc;
    tgc->native_state = native_running;
  } else { /* clone parent */
    ptr p_v = PARAMETERS(p_tc);
    iptr i, n = Svector_length(p_v);
    ptr v;

    tc = TO_PTR(malloc(size_tc));
    if (free_thread_gcs) {
      tgc = free_thread_gcs;
      free_thread_gcs = tgc->next;
    } else
      tgc = malloc(sizeof(thread_gc));

    if (tc == (ptr)0)
      S_error(who, "unable to malloc thread data structure");
    memcpy(TO_VOIDP(tc), TO_VOIDP(p_tc), size_tc);

    GCDATA(tc) = TO_PTR(tgc);
    tgc->tc = tc;

    {
      IGEN g; ISPC s;
      for (g = 0; g <= static_generation; g++) {
        for (s = 0; s <= max_real_space; s++) {
          tgc->base_loc[g][s] = (ptr)0;
          tgc->next_loc[g][s] = (ptr)0;
          tgc->bytes_left[g][s] = 0;
          tgc->sweep_loc[g][s] = (ptr)0;
          tgc->sweep_next[g][s] = NULL;
        }
        tgc->bitmask_overhead[g] = 0;
      }
    }

    tgc->during_alloc = 0;
    tgc->pending_ephemerons = (ptr)0;
    for (i = 0; i < (int)DIRTY_SEGMENT_LISTS; i++)
      tgc->dirty_segments[i] = NULL;
    tgc->queued_fire = 0;
    tgc->preserve_ownership = 0;
    tgc->native_state = native_running;

    v = S_vector_in(tc, space_new, 0, n);

    for (i = 0; i < n; i += 1)
      INITVECTIT(v, i) = Svector_ref(p_v, i);
    
    PARAMETERS(tc) = v;
    CODERANGESTOFLUSH(tc) = Snil;
  }

  tgc->sweeper = main_sweeper_index;
  tgc->mutator_bytes = 0;

  /* override nonclonable tc fields */
  THREADNO(tc) = S_G.threadno;
  S_G.threadno = S_add(S_G.threadno, FIX(1));

  CCHAIN(tc) = Snil;

  WINDERS(tc) = Snil;
  ATTACHMENTS(tc) = Snil;
  HANDLERSTACK(tc) = Sfalse;
  CACHEDFRAME(tc) = Sfalse;
  STACKLINK(tc) = SYMVAL(S_G.null_continuation_id);
  STACKCACHE(tc) = Snil;

 /* S_reset_scheme_stack initializes stack, size, esp, and sfp */
  S_reset_scheme_stack(tc, stack_slop);
  FRAME(tc,0) = TO_PTR(&CODEIT(S_G.dummy_code_object,size_rp_header));

 /* S_reset_allocation_pointer initializes ap and eap */
  alloc_mutex_acquire();
  S_reset_allocation_pointer(tc);
  alloc_mutex_release();
  S_maybe_fire_collector(tgc);

  RANDOMSEED(tc) = most_positive_fixnum < 0xffffffff ? most_positive_fixnum : 0xffffffff;
  X(tc) = Y(tc) = U(tc) = V(tc) = W(tc) = FIX(0);

  TIMERTICKS(tc) = Sfalse;
  DISABLECOUNT(tc) = Sfixnum(0);
  SIGNALINTERRUPTPENDING(tc) = Sfalse;
  SIGNALINTERRUPTQUEUE(tc) = S_allocate_scheme_signal_queue();
  KEYBOARDINTERRUPTPENDING(tc) = Sfalse;

  TARGETMACHINE(tc) = S_intern((const unsigned char *)MACHINE_TYPE);

 /* choosing not to clone virtual registers */
  for (i = 0 ; i < virtual_register_count ; i += 1) {
    VIRTREG(tc, i) = FIX(0);
  }

  DSTBV(tc) = SRCBV(tc) = Sfalse;

  thread = S_thread(tc);

  S_threads = S_cons_in(tc, space_new, 0, thread, S_threads);
  S_nthreads += 1;
  SETSYMVAL(S_G.active_threads_id,
   FIX(UNFIX(SYMVAL(S_G.active_threads_id)) + 1));
  ACTIVE(tc) = 1;

 /* collect request is only thing that can be pending for new thread.
    must do this after we're on the thread list in case the cons
    adding us onto the thread list set collect-request-pending */
  SOMETHINGPENDING(tc) = SYMVAL(S_G.collect_request_pending_id);

  GUARDIANENTRIES(tc) = Snil;

  LZ4OUTBUFFER(tc) = 0;

  CP(tc) = 0;

  /* if a collection is needed, then ask the new thread to check right away */
  if (Sboolean_value(S_symbol_value(S_G.collect_request_pending_id)))
    TRAP(tc) = (ptr)1;

  tc_mutex_release();

  return thread;
}

#ifdef PTHREADS
/* Collect-safe foreign calls without the tc mutex.

   A collect-safe call used to deactivate the thread outright: take the tc
   mutex, lock the code object the call returns into, give up the thread's
   place in $active-threads, and undo all of it on return. Instead, the
   thread now only marks itself native_in_native in its own word and stays
   counted. That is a CAS each way and nothing shared.

   Whoever looks at $active-threads has to see such a thread gone from
   it, as a deactivating call always showed it: the "only one thread
   active" checks in collect and compute-size-increments, and the
   rendezvous deciding to collect. So the first two call S_convert_native_threads under the tc
   mutex before they read it (s/7.ss, s/inspect.ss), and a thread about to
   wait for a collection (S_condition_wait on a collect condition) does the
   same: every native_in_native thread is deactivated on its behalf,
   native_in_native -> native_converted, ACTIVE cleared, count lowered. A
   waiter that converted anyone goes round its loop again instead of
   waiting for a thread that is not coming. Only readers pay for the walk
   over the thread list; taking the tc mutex for anything else does not.

   A thread returning from C claims its word back, native_in_native ->
   native_running, and goes on. If it was converted meanwhile, it takes
   the tc mutex (which a collection in progress holds until it is done,
   and under which the conversion finished) and reactivates.

   The CASes on the word decide every race between the thread and a
   converter. The one that remains is a thread entering C just as a
   collection is requested: it marks itself and then reads
   collect-request-pending, with a full fence between, so either it sees
   the request and deactivates itself the ordinary way, or a converter
   that looks after the request was set finds it marked.

   The code a native thread returns into is locked by S_do_gc for the
   collection's length, as it is for every inactive thread. */

/* A StoreLoad barrier. atomic.h's fences are store or acquire fences, and
   on some compilers (MSVC on x64) none at all, so this one is spelled out. */
#if defined(_MSC_VER)
# define NATIVE_FULL_FENCE() MemoryBarrier()
#else
# define NATIVE_FULL_FENCE() __sync_synchronize()
#endif

static IBOOL native_state_cas(thread_gc *tgc, uptr from, uptr to) {
  /* the arm64 CAS can fail spuriously, so a failure only counts when the
     word really holds something else */
  for (;;) {
    if (tgc->native_state != from) return 0;
    if (COMPARE_AND_SWAP_PTR(&tgc->native_state, TO_VOIDP(from), TO_VOIDP(to))) return 1;
  }
}

/* With the tc mutex held: deactivate every thread sitting in C as
   native_in_native, so that a reader of $active-threads sees it gone, as a
   deactivating collect-safe call always showed it. Answers how many were.
   A thread converted when no collection follows only pays the ordinary
   reactivation on its way back. */
iptr S_convert_native_threads(void) {
  ptr ls; iptr n = 0;
  for (ls = S_threads; ls != Snil; ls = Scdr(ls)) {
    ptr t_tc = (ptr)THREADTC(Scar(ls));
    thread_gc *tgc = THREAD_GC(t_tc);
    if (native_state_cas(tgc, native_in_native, native_converted)) {
      ACTIVE(t_tc) = 0;
      SETSYMVAL(S_G.active_threads_id,
       FIX(UNFIX(SYMVAL(S_G.active_threads_id)) - 1));
      n += 1;
    }
  }
  return n;
}

/* S_do_gc counts collections; a collect waiter records the count it
   converted native threads for (S_condition_wait). Both under the tc mutex. */
uptr S_collections_done;
static uptr collect_waiters_converted = (uptr)-1;

/* Back from C: claim the thread's place again. */
static void return_from_native(ptr tc) {
  thread_gc *tgc = THREAD_GC(tc);
  if (native_state_cas(tgc, native_in_native, native_running)) return;
  if (tgc->native_state == native_converted) {
    /* The converter takes the word first and then, still under the tc
       mutex, clears ACTIVE and gives up the count; ACTIVE is only
       settled once that mutex is free, so read it under the mutex rather
       than through reactivate_thread's unlocked check. The mutex is also
       what a collection in progress holds until it is done. */
    tc_mutex_acquire();
    if (!ACTIVE(tc)) {
      SETSYMVAL(S_G.active_threads_id,
       FIX(UNFIX(SYMVAL(S_G.active_threads_id)) + 1));
      ACTIVE(tc) = 1;
    }
    tgc->native_state = native_running;
    tc_mutex_release();
  } else
    reactivate_thread(tc)
}

/* Present only in a kernel with this scheme: a caller can ask with
   (foreign-entry? "(cs)lockfree_activation") whether collect-safe calls
   are cheap here. */
INT S_lockfree_activation(void) { return 1; }

IBOOL Sactivate_thread(void) { /* create or reactivate current thread */
  ptr tc = get_thread_context();

  if (tc == (ptr)0) { /* thread created by someone else */
    ptr thread;

   /* borrow base thread to clone */
    thread = S_create_thread_object("Sactivate_thread", TO_PTR(S_G.thread_context));
    s_thread_setspecific(S_tc_key, TO_VOIDP(THREADTC(thread)));
    return 1;
  } else {
    return_from_native(tc);
    return 0;
  }
}

int S_activate_thread(void) { /* Like Sactivate_thread(), but returns a mode to revert the effect */
  ptr tc = get_thread_context();

  if (tc == (ptr)0) {
    Sactivate_thread();
    return unactivate_mode_destroy;
  } else if (THREAD_GC(tc)->native_state != native_running || !ACTIVE(tc)) {
    return_from_native(tc);
    return unactivate_mode_deactivate;
  } else
    return unactivate_mode_noop;
}

void S_unactivate_thread(int mode) { /* Reverts a previous S_activate_thread() effect */
  switch (mode) {
  case unactivate_mode_deactivate:
    Sdeactivate_thread();
    break;
  case unactivate_mode_destroy:
    Sdestroy_thread();
    break;
  case unactivate_mode_noop:
  default:
    break;
  }
}

void Sdeactivate_thread(void) { /* deactivate current thread */
  ptr tc = get_thread_context();
  thread_gc *tgc;
  if (tc == (ptr)0 || !ACTIVE(tc)) return;
  tgc = THREAD_GC(tc);
  if (tgc->native_state != native_running) return;
  tgc->native_state = native_in_native;
  NATIVE_FULL_FENCE(); /* the mark is visible before the request is read */
  if (Sboolean_value(*(volatile ptr *)&SYMVAL(S_G.collect_request_pending_id))) {
    /* a collection is wanted: give up the thread's place now, as a
       deactivation always did, unless a waiter already has */
    tc_mutex_acquire();
    if (native_state_cas(tgc, native_in_native, native_converted))
      deactivate_thread(tc)
    tc_mutex_release();
  }
}

int Sdestroy_thread(void) { /* destroy current thread */
  ptr tc = get_thread_context();
  if (tc != (ptr)0) {
    s_thread_setspecific(S_tc_key, NULL);
    if (destroy_thread(tc)) return 1;
  }
  return 0;
}

static IBOOL destroy_thread(ptr tc) {
  ptr *ls; IBOOL status;

  status = 0;
  tc_mutex_acquire();
  ls = &S_threads;
  while (*ls != Snil) {
    ptr thread = Scar(*ls);
    if (THREADTC(thread) == (uptr)tc) {
      *ls = Scdr(*ls);
      S_nthreads -= 1;

      alloc_mutex_acquire();

     /* process remembered set before dropping allocation area */
      S_scan_dirty((ptr *)TO_VOIDP(EAP(tc)), (ptr *)TO_VOIDP(REAL_EAP(tc)));

     /* close off thread-local allocation */
      S_thread_start_code_write(tc, static_generation, 0, NULL, 0);
      {
        ISPC s; IGEN g;
        thread_gc *tgc = THREAD_GC(tc);
        for (g = 0; g <= static_generation; g++)
          for (s = 0; s <= max_real_space; s++)
            if (tgc->next_loc[g][s])
              S_close_off_thread_local_segment(tc, s, g);
      }
      S_thread_end_code_write(tc, static_generation, 0, NULL, 0);

      alloc_mutex_release();

     /* process guardian entries */
      {
	ptr target, ges, obj, next; seginfo *si;
	target = S_G.guardians[0];
	for (ges = GUARDIANENTRIES(tc); ges != Snil; ges = next) {
	  obj = GUARDIANOBJ(ges);
	  next = GUARDIANNEXT(ges);
	  if (!FIXMEDIATE(obj) && (si = MaybeSegInfo(ptr_get_segment(obj))) != NULL && si->generation != static_generation) {
	    INITGUARDIANNEXT(ges) = target;
	    target = ges;
	  }
	}
	S_G.guardians[0] = target;
      }

     /* deactivate thread */
      if (ACTIVE(tc)) {
        SETSYMVAL(S_G.active_threads_id,
         FIX(UNFIX(SYMVAL(S_G.active_threads_id)) - 1));
        if (Sboolean_value(SYMVAL(S_G.collect_request_pending_id))
            && SYMVAL(S_G.active_threads_id) == FIX(0)) {
          s_thread_cond_signal(&S_collect_cond);
          s_thread_cond_signal(&S_collect_thread0_cond);
        }
      }

      if (LZ4OUTBUFFER(tc) != (ptr)0) free(TO_VOIDP(LZ4OUTBUFFER(tc)));
      if (SIGNALINTERRUPTQUEUE(tc) != (ptr)0) free(TO_VOIDP(SIGNALINTERRUPTQUEUE(tc)));

      if (THREAD_GC(tc)->preserve_ownership)
        --S_num_preserve_ownership_threads;

      /* Never free a thread_gc, since it may be recorded in a segment
         as the segment's creator. Recycle manually, instead. */
      THREAD_GC(tc)->sweeper = main_sweeper_index;
      THREAD_GC(tc)->tc = (ptr)0;
      THREAD_GC(tc)->next = free_thread_gcs;
      free_thread_gcs = THREAD_GC(tc);

      free(TO_VOIDP(tc));
      
      THREADTC(thread) = 0; /* mark it dead */
      status = 1;

      s_thread_cond_broadcast(&S_terminated_cond);
      break;
    }
    ls = &Scdr(*ls);
  }
  tc_mutex_release();
  return status;
}

ptr S_fork_thread(ptr thunk) {
  ptr thread;
  int status;

  /* pass the current thread's context as the parent thread */
  thread = S_create_thread_object("fork-thread", get_thread_context());
  CP(THREADTC(thread)) = thunk;

  if ((status = s_thread_create(start_thread, TO_VOIDP(THREADTC(thread)))) != 0) {
    destroy_thread((ptr)THREADTC(thread));
    S_error1("fork-thread", "failed: ~a", S_strerror(status));
  }

  return thread;
}

static s_thread_rv_t start_thread(void *p) {
  ptr tc = TO_PTR(p); ptr cp;

  s_thread_setspecific(S_tc_key, TO_VOIDP(tc));

  cp = CP(tc);
  CP(tc) = Svoid; /* should hold calling code object, which we don't have */
  TRAP(tc) = (ptr)default_timer_ticks;
  Scall0(cp);
 /* caution: calling into Scheme may result into a collection, so we
    can't access any Scheme objects, e.g., cp, after this point.  But tc
    is static, so we can access it. */

 /* find and destroy our thread */
  s_thread_setspecific(S_tc_key, NULL);
  destroy_thread(tc);

  s_thread_return;
}


ptr S_make_mutex(void) {
  scheme_mutex_t *m;

  m = (scheme_mutex_t *)malloc(sizeof(scheme_mutex_t));

  if (m == (scheme_mutex_t *)0)
    S_error("make-mutex", "unable to malloc mutex");
  s_thread_mutex_init(&m->pmutex);
  m->owner = s_thread_self();
  m->count = 0;

  return TO_PTR(m);
}

void S_mutex_free(scheme_mutex_t *m) {
  s_thread_mutex_destroy(&m->pmutex);
  free(m);
}

void S_mutex_acquire(scheme_mutex_t *m) NO_THREAD_SANITIZE {
  s_thread_t self = s_thread_self();
  iptr count;
  INT status;

  if ((count = m->count) > 0 && s_thread_equal(m->owner, self)) {
    if (count == most_positive_fixnum)
      S_error1("mutex-acquire", "recursion limit exceeded for ~s", TO_PTR(m));
    m->count = count + 1;
    return;
  }

  if ((status = s_thread_mutex_lock(&m->pmutex)) != 0)
    S_error1("mutex-acquire", "failed: ~a", S_strerror(status));
  m->owner = self;
  m->count = 1;
}

INT S_mutex_tryacquire(scheme_mutex_t *m) NO_THREAD_SANITIZE {
  s_thread_t self = s_thread_self();
  iptr count;
  INT status;

  if ((count = m->count) > 0 && s_thread_equal(m->owner, self)) {
    if (count == most_positive_fixnum)
      S_error1("mutex-acquire", "recursion limit exceeded for ~s", TO_PTR(m));
    m->count = count + 1;
    return 0;
  }

  status = s_thread_mutex_trylock(&m->pmutex);
  if (status == 0) {
    m->owner = self;
    m->count = 1;
  } else if (status != EBUSY) {
    S_error1("mutex-acquire", "failed: ~a", S_strerror(status));
  }
  return status;
}

IBOOL S_mutex_is_owner(scheme_mutex_t *m) NO_THREAD_SANITIZE {
  s_thread_t self = s_thread_self();
  return ((m->count > 0) && s_thread_equal(m->owner, self));
}

void S_mutex_release(scheme_mutex_t *m) NO_THREAD_SANITIZE {
  s_thread_t self = s_thread_self();
  iptr count;
  INT status;

  if ((count = m->count) == 0 || !s_thread_equal(m->owner, self))
    S_error1("mutex-release", "thread does not own mutex ~s", TO_PTR(m));

  if ((m->count = count - 1) == 0) {
    m->owner = 0; /* needed for a memory model like ARM, for example */
    if ((status = s_thread_mutex_unlock(&m->pmutex)) != 0)
      S_error1("mutex-release", "failed: ~a", S_strerror(status));
  }
}

s_thread_cond_t *S_make_condition() {
  s_thread_cond_t *c;

  c = (s_thread_cond_t *)malloc(sizeof(s_thread_cond_t));
  if (c == (s_thread_cond_t *)0)
    S_error("make-condition", "unable to malloc condition");
  s_thread_cond_init(c);
  return c;
}

void S_condition_free(s_thread_cond_t *c) {
  s_thread_cond_destroy(c);
  free(c);
}

#ifdef FEATURE_WINDOWS

static inline int s_thread_cond_timedwait(s_thread_cond_t *cond, s_thread_mutex_t *mutex, int typeno, I64 sec, long nsec) {
  if (typeno == time_utc) {
    struct timespec now;
    S_gettime(time_utc, &now);
    sec -= now.tv_sec;
    nsec -= now.tv_nsec;
    if (nsec < 0) {
      sec -= 1;
      nsec += 1000000000;
    }
  }
  if (sec < 0) {
    sec = 0;
    nsec = 0;
  }
  if (SleepConditionVariableCS(cond, mutex, (DWORD)(sec*1000 + (nsec+500000)/1000000))) {
    return 0;
  } else if (GetLastError() == ERROR_TIMEOUT) {
    return ETIMEDOUT;
  } else {
    return EINVAL;
  }
}

#else /* FEATURE_WINDOWS */

static inline int s_thread_cond_timedwait(s_thread_cond_t *cond, s_thread_mutex_t *mutex, int typeno, I64 sec, long nsec) {
  struct timespec t;
  if (typeno == time_duration) {
    struct timespec now;
    S_gettime(time_utc, &now);
    t.tv_sec = (time_t)(now.tv_sec + sec);
    t.tv_nsec = now.tv_nsec + nsec;
    if (t.tv_nsec >= 1000000000) {
      t.tv_sec += 1;
      t.tv_nsec -= 1000000000;
    }
  } else {
    t.tv_sec = sec;
    t.tv_nsec = nsec;
  }
  return pthread_cond_timedwait(cond, mutex, &t);
}

#endif /* FEATURE_WINDOWS */

#define Srecord_ref(x,i) (((ptr *)TO_VOIDP(((uptr)(x)+record_data_disp)))[i])

IBOOL S_condition_wait(s_thread_cond_t *c, scheme_mutex_t *m, ptr t) {
  ptr tc = get_thread_context();
  s_thread_t self = s_thread_self();
  iptr count;
  INT typeno;
  I64 sec;
  long nsec;
  INT status;
  IBOOL is_collect;
  iptr collect_index = 0;

  if ((count = m->count) == 0 || !s_thread_equal(m->owner, self))
    S_error1("condition-wait", "thread does not own mutex ~s", TO_PTR(m));

  if (count != 1)
    S_error1("condition-wait", "mutex ~s is recursively locked", TO_PTR(m));

  if (t != Sfalse) {
    /* Keep in sync with ts record in s/date.ss */
    typeno = Sinteger32_value(Srecord_ref(t,0));
    sec = Sinteger64_value(Scar(Srecord_ref(t,1)));
    nsec = Sinteger32_value(Scdr(Srecord_ref(t,1)));
  } else {
    typeno = 0;
    sec = 0;
    nsec = 0;
  }

  is_collect = (c == &S_collect_cond || c == &S_collect_thread0_cond);

  /* a thread in a collect-safe call is still counted active: deactivate it
     on its behalf and let the caller look at the count again rather than
     wait for a thread that is not coming (see Sdeactivate_thread). Once per
     collection is enough: a thread that enters C after this walk sees the
     request already pending and deactivates itself, so the other waiters
     (every thread in the process, in a rendezvous) skip the walk. */
  if (is_collect && collect_waiters_converted != S_collections_done) {
    collect_waiters_converted = S_collections_done;
    if (S_convert_native_threads() > 0) return 1;
  }

  if (is_collect) {
    /* Remember the index where we record this tc, because a thread
       might temporarily wait for collection, but then get woken
       up (e.g., to make the main thread drive the collection) before
       a collection actually happens. */
    int i;
    S_collect_waiting_threads++;
    collect_index = maximum_parallel_collect_threads;
    if (S_collect_waiting_threads <= maximum_parallel_collect_threads) {
      /* look for an open slot in `S_collect_waiting_tcs` */
      for (i = 0; i < maximum_parallel_collect_threads; i++) {
        if (S_collect_waiting_tcs[i] == (ptr)0) {
          collect_index = i;
          S_collect_waiting_tcs[collect_index] = tc;
          break;
        }
      }
    }
  }

  if (is_collect || DISABLECOUNT(tc) == 0) {
    deactivate_thread_signal_collect(tc, !is_collect)
  }

  m->count = 0;
  status = (t == Sfalse) ? s_thread_cond_wait(c, &m->pmutex) :
    s_thread_cond_timedwait(c, &m->pmutex, typeno, sec, nsec);
  m->owner = self;
  m->count = 1;

  if (is_collect || DISABLECOUNT(tc) == 0) {
    reactivate_thread(tc)
  }

  if (is_collect) {
    --S_collect_waiting_threads;
    if (collect_index < maximum_parallel_collect_threads)
      S_collect_waiting_tcs[collect_index] = (ptr)0;
  }

  if (status == 0) {
    return 1;
  } else if (status == ETIMEDOUT) {
    return 0;
  } else {
    S_error1("condition-wait", "failed: ~a", S_strerror(status));
    return 0;
  }
}
#endif /* PTHREADS */

#ifndef PTHREADS
/* (cs)convert_native_threads is called by collect whatever the build;
   without threads there is no one to convert. */
iptr S_convert_native_threads(void) { return 0; }
#endif
