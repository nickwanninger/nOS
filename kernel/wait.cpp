#include <cpu.h>
#include <errno.h>
#include <wait.h>

#include <sleep.h>
#include <sched.h>
#include <thread.h>

#ifndef container_of
#define container_of(ptr, type, member)                    \
  ({                                                       \
    const __decltype(((type *)0)->member) *__mptr = (ptr); \
    (type *)((char *)__mptr - offsetof(type, member));     \
  })
#endif


wait_entry::wait_entry() {
  wq = NULL;
  thd = curthd;
}

wait_entry::~wait_entry() {
  if (wq != NULL) {
    wq->finish(this);
  }
}

wait_result wait_entry::start() {
  /* Yield right away */
  auto res = sched::yield();

  if (res == sched::YieldResult::Interrupt) {
    this->result |= WAIT_RES_INTR;
  }

  return wait_result(this->result);
}




wait_queue::wait_queue(void) { task_list.init(); }

void wait_queue::wake_up_common(unsigned int mode, int nr_exclusive, int wake_flags, void *key) {
  int pid = 0;
  if (curproc) pid = curproc->pid;

  bool woke_someone = false;

  struct wait_entry *curr, *next;
  list_for_each_entry_safe(curr, next, &task_list, item) {
    unsigned flags = curr->flags;
    bool woke_up = curr->func(curr, mode, wake_flags, key);
    /* TODO: you could do a priority system here? */
    if (woke_up) woke_someone = true;
    if (woke_up && (flags & WQ_FLAG_EXCLUSIVE) && !--nr_exclusive) {
      break;
    }
  }

  if (woke_someone) cpu::current().woke_someone_up = true;
}


wait_result wait_queue::wait(struct wait_entry *ent, int state) {
  assert(ent->wq == NULL);

  unsigned long flags;
  ent->wq = this;
  ent->flags &= ~WQ_FLAG_EXCLUSIVE;
  flags = lock.lock_irqsave();
  sched::set_state(state);

  task_list.add(&ent->item);
  lock.unlock_irqrestore(flags);

  return ent->start();
}


wait_result wait_queue::wait(void) {
  struct wait_entry entry;
  return wait(&entry, PS_INTERRUPTIBLE);
}



wait_result wait_queue::wait_exclusive(struct wait_entry *ent, int state) {
  assert(ent->wq == NULL);

  unsigned long flags;
  ent->wq = this;
  ent->flags |= WQ_FLAG_EXCLUSIVE;
  flags = lock.lock_irqsave();
  sched::set_state(state);

  task_list.add_tail(&ent->item);
  lock.unlock_irqrestore(flags);

  return ent->start();
}


wait_result wait_queue::wait_exclusive(void) {
  struct wait_entry entry;
  return wait_exclusive(&entry, PS_INTERRUPTIBLE);
}



void wait_queue::wait_noint(void) {
  struct wait_entry entry;
  wait(&entry, PS_UNINTERRUPTIBLE);
}


void wait_queue::add(struct wait_entry *ent) {
  auto flags = lock.lock_irqsave();
  ent->wq = this;
  task_list.add_tail(&ent->item);
  lock.unlock_irqrestore(flags);
}


void wait_queue::remove(struct wait_entry *ent) {
  if (ent->wq == this) {
    auto flags = lock.lock_irqsave();
    ent->item.del_init();
    lock.unlock_irqrestore(flags);
  }
}

void wait_queue::finish(struct wait_entry *e) {
  if (e->wq == NULL) return;
  assert(e->wq == this);

  unsigned long flags;
  e->thd->set_state(PS_RUNNING);

  // "carefully" check if the entry's linkage is empty or not.
  // If it is, then we lock the waitqueue and unlink it
  if (!e->item.is_empty_careful()) {
    flags = lock.lock_irqsave();
    e->item.del_init();
    lock.unlock_irqrestore(flags);
  }
  e->wq = NULL;
}



wait_result wait_queue::wait_timeout(long long us) {
  sleep_waiter sw(us);
  wait_queue *queues[2];

  queues[0] = &sw.wq;
  queues[1] = this;
  int result = multi_wait(queues, 2);

  if (result == -EINTR) return wait_result(WAIT_RES_INTR);

  if (result == 0) return wait_result(WAIT_RES_TIMEOUT);

  return wait_result(0);
}




bool autoremove_wake_function(struct wait_entry *entry, unsigned mode, int sync, void *key) {
  bool ret = true;

  sched::unblock(*entry->thd, false);
  entry->result = 0;


  if (entry->thd->rudely_awoken) {
    entry->result |= WAIT_RES_INTR;
  }

  entry->flags |= WQ_FLAG_RUDELY;
  if (ret) entry->item.del_init();
  return ret;
}


//////////////////////////
//
//  M U L T I - W A I T
//
//////////////////////////


struct multi_wake_entry {
  struct wait_entry entry;
  int index;
  bool en; /* idk im trying my best. */
  /* Was this the entry that was awoken? */
  bool awoken = false;
};


bool multi_wait_wake_function(struct wait_entry *entry, unsigned mode, int sync, void *key) {
  struct multi_wake_entry *e = container_of(entry, struct multi_wake_entry, entry);
  e->awoken = true;

  return autoremove_wake_function(entry, mode, sync, key);
}


int multi_wait(wait_queue **queues, size_t nqueues, bool exclusive) {
  bool irqs_enabled = false;
  for (int i = 0; i < nqueues; i++) {
    auto *wq = queues[i];
    bool en = wq->lock.lock_irqsave();
    if (i == 0) irqs_enabled = en;
  }
  return multi_wait_prelocked(queues, nqueues, irqs_enabled, exclusive);
}

int multi_wait_prelocked(wait_queue **queues, size_t nqueues, bool irqs_enabled, bool exclusive) {
  // I know, variable stack arrays are bad. Whatever, I wrote all this code.
  struct multi_wake_entry ents[nqueues];

  /* First, we must go through each of the queues and take their lock.
   * We have to do this first becasue we can't contend the lock while in the  */
  for (int i = 0; i < nqueues; i++) {
    auto *wq = queues[i];

    ents[i].entry.wq = wq;
    ents[i].index = i;
    ents[i].entry.func = multi_wait_wake_function;
    if (exclusive) {
      ents[i].entry.flags = WQ_FLAG_EXCLUSIVE;
    }
  }

  /* Set the process state now that we have all the locks. This means we won't be interrupted */
  sched::set_state(PS_INTERRUPTIBLE);


  /* Now, we go through each of the waitqueues and add our entries to them */
  for (int i = 0; i < nqueues; i++) {
    auto *wq = queues[i];
    auto &ent = ents[i].entry;
    wq->task_list.add(&ent.item);
  }

  /* Unlock each of the waitqueues now that we have added each of the entries */
  for (int i = 0; i < nqueues; i++) {
    auto *wq = queues[i];
    wq->lock.unlock();
  }



  if (irqs_enabled) {
    arch_enable_ints();
  } else {
    printf("interrupts were disabled at the start of multi_wait\n");
  }

  /* Yield (block) with the new process state. */
  sched::yield();

  /* Which of the queues was awoken? If -1, we were interrupted */
  int which = -1;

  /* Figure out which queue */
  for (int i = 0; i < nqueues; i++) {
    auto &ent = ents[i];
    if (ent.awoken) {
      which = i;
      /* If we were awoken, but also interrupted, break with -1  */
      if (wait_result(ent.entry.result).interrupted()) {
        which = -1;
        break;
      }
      break;
    }
  }


  if (which == -1) return -EINTR;

  return which;
}



void prepare_to_wait(struct wait_queue &wq, struct wait_entry &ent, bool in) {
  int state = in ? PS_INTERRUPTIBLE : PS_UNINTERRUPTIBLE;

  unsigned long flags;
  ent.wq = &wq;
  ent.flags &= ~WQ_FLAG_EXCLUSIVE;
  flags = wq.lock.lock_irqsave();
  sched::set_state(state);
  wq.task_list.add(&ent.item);
  wq.lock.unlock_irqrestore(flags);
}

void prepare_to_wait_exclusive(struct wait_queue &wq, struct wait_entry &ent, bool in) {
  int state = in ? PS_INTERRUPTIBLE : PS_UNINTERRUPTIBLE;

  unsigned long flags;
  ent.wq = &wq;
  ent.flags |= WQ_FLAG_EXCLUSIVE;
  flags = wq.lock.lock_irqsave();
  sched::set_state(state);
  wq.task_list.add_tail(&ent.item);
  wq.lock.unlock_irqrestore(flags);
}

wait_result do_wait(struct wait_entry &ent) {
  sched::yield();

  return wait_result(ent.result);
}


void finish_wait(struct wait_queue &wq, struct wait_entry &ent) { wq.finish(&ent); }
