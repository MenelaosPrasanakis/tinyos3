
#include "tinyos.h"
#include "kernel_sched.h"
#include "kernel_proc.h"
#include "kernel_cc.h"
#include "kernel_streams.h"

/** 
  @brief Create a new thread in the current process.
  @param task The function to be executed by the new thread.
  @param argl The size of the arguments passed to the thread.
  @param args Pointer to the arguments passed to the thread.
  @return The thread ID (Tid_t) of the newly created thread.
  */
Tid_t sys_CreateThread(Task task, int argl, void* args)
{
  TCB* new_thread;

  //Spawn a new thread for the current process and initialize its execution function. 
  new_thread = spawn_thread(CURPROC, start_thread);

  // Initialize the PTCB and link it with the new thread.
  aquire_PTCB(new_thread, task, argl, args);

  // Increment the thread count for the current process.
  PCB* curproc = CURPROC;
  curproc->thread_count++;

  // Make the new thread ready to run.
  wakeup(new_thread);

  // Return the PTCB as the thread ID.
  return (Tid_t)new_thread->ptcb;
}


/**
  @brief Initialize the PTCB and associate it with a TCB.
  @param tcb Pointer to the thread control block.
  @param call The task function to be executed.
  @param argl The size of the arguments.
  @param args Pointer to the arguments.
*/

void aquire_PTCB(TCB* tcb, Task call, int argl, void* args){

  // Allocate memory for the PTCB.
  PTCB* ptcb = (PTCB*)xmalloc(sizeof(PTCB));

  // Initialize the PTCB fields.
  ptcb->tcb = tcb;
  tcb->ptcb = ptcb;
  ptcb->task = call;
  ptcb->argl = argl;
  ptcb->args = args;
  ptcb->exitval = 0;
  ptcb->detached = 0;
  ptcb->exited = 0;
  ptcb->exit_cv = COND_INIT;
  ptcb->refcount=0;


  //Add the PTCB to the process's PTCB list.
  rlnode_init(&ptcb->ptcb_list_node, ptcb);
  rlist_push_back(&tcb->owner_pcb->ptcb_list, &ptcb->ptcb_list_node);
}

/**
  @brief Get the thread ID of the current thread.
  @return The thread ID (Tid_t) of the current thread.
*/

Tid_t sys_ThreadSelf()
{
  return (Tid_t) cur_thread()->ptcb;
}


/**
  @brief Wait for a thread to terminate.
  @param tid The thread ID of the thread to join.
  @param exitval Pointer to store the exit value of the joined thread.
  @return 0 on success, or -1 on failure.
*/

int sys_ThreadJoin(Tid_t tid, int* exitval)
{
 
  PTCB* ptcb=(PTCB*) tid;
  PCB *curproc=CURPROC;

  // Check if the thread exists in the current process's PTCB list.
  if (rlist_find(&curproc->ptcb_list,ptcb,NULL)==NULL){
  return -1;
 }

 // Prevent a thread from joining itself.
 if(ptcb==cur_thread()->ptcb){
  return -1;
 }


 // Check if the thread is detached.
 if (ptcb->detached==1){
  return -1;
 }

// Increment the reference count for the thread.
ptcb->refcount++;


// Wait until the thread exits or becomes detached.
while(ptcb->exited!=1 && ptcb->detached!=1){
  kernel_wait(&(ptcb->exit_cv),SCHED_USER);
}

 // Decrement the reference count.
 ptcb->refcount--;
  if (ptcb->detached==1){
  return -1;
 }

// Retrieve the exit value if requested.
if(exitval!=NULL){
  *exitval=ptcb->exitval;
}

// Free the PTCB if no references remain.
if(ptcb->refcount==0){
  rlist_remove(&ptcb->ptcb_list_node);
  free(ptcb);
}

  return 0;
}


/**
  @brief Detach a thread.
  @param tid The thread ID of the thread to detach.
  @return 0 on success, or -1 on failure.
*/

int sys_ThreadDetach(Tid_t tid)
{

  
  PTCB* temp = (PTCB*)tid;

  // Check if the thread exists in the current process's PTCB list.
  if(rlist_find(&CURPROC->ptcb_list, (PTCB*)tid, NULL)==NULL) {
    return -1;
  }


   // Check if the thread has already exited.
  if(temp->exited == 1){
    return -1;
  }

  
  // Mark the thread as detached and notify all waiting threads.
  temp->detached = 1;
  //wakes up all threads with the given cond variable
  kernel_broadcast(&(temp->exit_cv));
  return 0;
}

/**
  @brief Terminate the current thread and perform cleanup.
  @param exitval The exit value to store in the thread's PTCB.
*/

void sys_ThreadExit(int exitval)
{

  PTCB* ptcb = cur_thread()->ptcb;


  // Mark the thread as exited and set the exit value.
  ptcb->exitval = exitval;
  ptcb->exited = 1;

  PCB* curproc = CURPROC;

  // Notify all waiting threads.
  kernel_broadcast(&(ptcb->exit_cv));

  curproc->thread_count--;

  // If the current process has no threads left, perform process cleanup.
  if(curproc->thread_count==0) {
    //we clean the exit
    if(get_pid(curproc)!=1) {
    // Reparent any children of the exiting process to the
    //initial task
    PCB* initpcb = get_pcb(1);
    while(!is_rlist_empty(& curproc->children_list)) {
      rlnode* child = rlist_pop_front(& curproc->children_list);
      child->pcb->parent = initpcb;
      rlist_push_front(& initpcb->children_list, child);
    }

    // Add exited children to the initial task's exited list
    // and signal the initial task
    if(!is_rlist_empty(& curproc->exited_list)) {
      rlist_append(& initpcb->exited_list, &curproc->exited_list);
      kernel_broadcast(& initpcb->child_exit);
    }

    // Put me into my parent's exited list
    rlist_push_front(& curproc->parent->exited_list, &curproc->exited_node);
    kernel_broadcast(& curproc->parent->child_exit);

  }

  assert(is_rlist_empty(& curproc->children_list));
  assert(is_rlist_empty(& curproc->exited_list));



  //  Do all the other cleanup we want here, close files etc.

   

  // Release the args data
  if(curproc->args) {
    free(curproc->args);
    curproc->args = NULL;
  }

  // Clean up FIDT
  for(int i=0;i<MAX_FILEID;i++) {
    if(curproc->FIDT[i] != NULL) {
      FCB_decref(curproc->FIDT[i]);
      curproc->FIDT[i] = NULL;
    }

  }
   rlnode *temp;
while(!is_rlist_empty(&(curproc->ptcb_list))){
    
    temp = rlist_pop_front(&(curproc->ptcb_list));
    free(temp->ptcb);
 }


  // Disconnect my main_thread
  curproc->main_thread = NULL;

  // Now, mark the process as exited.
  curproc->pstate = ZOMBIE;

}
  
  kernel_sleep(EXITED, SCHED_USER);


}

