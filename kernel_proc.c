
#include <assert.h>
#include "kernel_cc.h"
#include "kernel_proc.h"
#include "kernel_streams.h"


/*
 The process table and related system calls:
 - Exec
 - Exit
 - WaitPid
 - GetPid
 - GetPPid

 */

/* The process table */
PCB PT[MAX_PROC];
unsigned int process_count;

PCB* get_pcb(Pid_t pid)
{
  return PT[pid].pstate==FREE ? NULL : &PT[pid];
}

Pid_t get_pid(PCB* pcb)
{
  return pcb==NULL ? NOPROC : pcb-PT;
}

/* Initialize a PCB */
static inline void initialize_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->argl = 0;
  pcb->args = NULL;

  for(int i=0;i<MAX_FILEID;i++)
    pcb->FIDT[i] = NULL;

  rlnode_init(& pcb->children_list, NULL);
  rlnode_init(& pcb->exited_list, NULL);
  rlnode_init(& pcb->ptcb_list, NULL);
  rlnode_init(& pcb->children_node, pcb);
  rlnode_init(& pcb->exited_node, pcb);
 
  pcb->thread_count = 0;
  pcb->child_exit = COND_INIT;
}


static PCB* pcb_freelist;

void initialize_processes()
{
  /* initialize the PCBs */
  for(Pid_t p=0; p<MAX_PROC; p++) {
    initialize_PCB(&PT[p]);
  }

  /* use the parent field to build a free list */
  PCB* pcbiter;
  pcb_freelist = NULL;
  for(pcbiter = PT+MAX_PROC; pcbiter!=PT; ) {
    --pcbiter;
    pcbiter->parent = pcb_freelist;
    pcb_freelist = pcbiter;
  }

  process_count = 0;

  /* Execute a null "idle" process */
  if(Exec(NULL,0,NULL)!=0)
    FATAL("The scheduler process does not have pid==0");
}


/*
  Must be called with kernel_mutex held
*/
PCB* acquire_PCB()
{
  PCB* pcb = NULL;

  if(pcb_freelist != NULL) {
    pcb = pcb_freelist;
    pcb->pstate = ALIVE;
    pcb_freelist = pcb_freelist->parent;
    process_count++;
  }

  return pcb;
}

/*
  Must be called with kernel_mutex held
*/
void release_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->parent = pcb_freelist;
  pcb_freelist = pcb;
  process_count--;
}


/*
 *
 * Process creation
 *
 */

/**
  @brief Entry point for the main thread of a process.
 
  This function is used to initialize and execute the main task of a process.
  It is invoked as part of the process creation, serving as the starting
  point for the main thread. The function retrieves the main task,
  arguments, and other details from the current process's control block
  and ensures that the process exits cleanly after executing the task.

  The function is passed to the thread creation routine (spawn) to
  set up the execution context of the main thread.
*/
void start_main_thread()
{
  int exitval; // Variable to store the exit value returned by the main task.

  Task call =  CURPROC->main_task; // Retrieve the main task function to be executed for the process.

  int argl = CURPROC->argl; // Retrieve the size/length of the arguments for the main task.
  void* args = CURPROC->args; // Retrieve the pointer to the arguments for the main task.

  exitval = call(argl,args); // Execute the main task with the provided arguments and store its return value.
  Exit(exitval); //Terminate the process, passing the task's return value as the exit value.
}

 /**
  @brief Entry point for a new thread created by sys_CreateThread.
  This function is executed when a new thread is spawned. It retrieves
  the task, arguments, and other details associated with the thread,
  executes the task, and ensures the thread exits cleanly.

  The function acts as a wrapper to invoke the user-provided task
  function and manages the thread's lifecycle by handling the exit process.
*/

void start_thread()
{
  int exitval; // Variable to store the exit value returned by the task.

  TCB* cur_thr = NULL;
  cur_thr = cur_thread();
  // Retrieve the current thread's TCB (Thread Control Block).

  Task call = cur_thr->ptcb->task;// Extract the task function to be executed from the PTCB.
  int argl = cur_thr->ptcb->argl;// Extract the size/length of arguments passed to the task.
  void* args = cur_thr->ptcb->args;// Extract the pointer to the arguments passed to the task.

  exitval = call(argl,args);// Execute the task with the provided arguments and store its return value.
  sys_ThreadExit(exitval); // Terminate the thread, passing the task's return value as the exit value.
}

/*
  System call to create a new process.
 */
Pid_t sys_Exec(Task call, int argl, void* args)
{
  PCB *curproc, *newproc;
 
  /* The new process PCB */
  newproc = acquire_PCB();

  if(newproc == NULL) goto finish;  /* We have run out of PIDs! */

  if(get_pid(newproc)<=1) {
    /* Processes with pid<=1 (the scheduler and the init process)
       are parentless and are treated specially. */
    newproc->parent = NULL;
  }
  else
  {
    /* Inherit parent */
    curproc = CURPROC;

    /* Add new process to the parent's child list */
    newproc->parent = curproc;
    rlist_push_front(& curproc->children_list, & newproc->children_node);

    /* Inherit file streams from parent */
    for(int i=0; i<MAX_FILEID; i++) {
       newproc->FIDT[i] = curproc->FIDT[i];
       if(newproc->FIDT[i])
          FCB_incref(newproc->FIDT[i]);
    }
  }


  /* Set the main thread's function */
  newproc->main_task = call;

  /* Copy the arguments to new storage, owned by the new process */
  newproc->argl = argl;
  if(args!=NULL) {
    newproc->args = malloc(argl);
    memcpy(newproc->args, args, argl);
  }
  else
    newproc->args=NULL;

  /*
    Create and wake up the thread for the main function. This must be the last thing
    we do, because once we wakeup the new thread it may run! so we need to have finished
    the initialization of the PCB.
   */

  //here we also intialize the first  of the new pcb
  if(call != NULL) {
    newproc->main_thread = spawn_thread(newproc, start_main_thread);
    aquire_PTCB(newproc->main_thread, call, argl, args);

    newproc->thread_count++;

   
 
    wakeup(newproc->main_thread);
   
  }


finish:
  return get_pid(newproc);
}


/* System call */
Pid_t sys_GetPid()
{
  return get_pid(CURPROC);
}


Pid_t sys_GetPPid()
{
  return get_pid(CURPROC->parent);
}


static void cleanup_zombie(PCB* pcb, int* status)
{
  if(status != NULL)
    *status = pcb->exitval;

  rlist_remove(& pcb->children_node);
  rlist_remove(& pcb->exited_node);

  release_PCB(pcb);
}


static Pid_t wait_for_specific_child(Pid_t cpid, int* status)
{

  /* Legality checks */
  if((cpid<0) || (cpid>=MAX_PROC)) {
    cpid = NOPROC;
    goto finish;
  }

  PCB* parent = CURPROC;
  PCB* child = get_pcb(cpid);
  if( child == NULL || child->parent != parent)
  {
    cpid = NOPROC;
    goto finish;
  }

  /* Ok, child is a legal child of mine. Wait for it to exit. */
  while(child->pstate == ALIVE)
    kernel_wait(& parent->child_exit, SCHED_USER);
 
  cleanup_zombie(child, status);
 
finish:
  return cpid;
}


static Pid_t wait_for_any_child(int* status)
{
  Pid_t cpid;

  PCB* parent = CURPROC;

  /* Make sure I have children! */
  int no_children, has_exited;
  while(1) {
    no_children = is_rlist_empty(& parent->children_list);
    if( no_children ) break;

    has_exited = ! is_rlist_empty(& parent->exited_list);
    if( has_exited ) break;

    kernel_wait(& parent->child_exit, SCHED_USER);    
  }

  if(no_children)
    return NOPROC;

  PCB* child = parent->exited_list.next->pcb;
  assert(child->pstate == ZOMBIE);
  cpid = get_pid(child);
  cleanup_zombie(child, status);

  return cpid;
}


Pid_t sys_WaitChild(Pid_t cpid, int* status)
{
  /* Wait for specific child. */
  if(cpid != NOPROC) {
    return wait_for_specific_child(cpid, status);
  }
  /* Wait for any child */
  else {
    return wait_for_any_child(status);
  }

}


/**
  @brief Terminate the current process and perform necessary cleanup.
 
  This function is called to terminate the current process. It updates the
  process's exit value, performs cleanup for the process and its threads,
  and ensures that any remaining child processes are handled appropriately.
  If the current process is the initial process (PID 1), it waits for all
  its child processes to exit before proceeding.

  @param exitval The exit value of the process, which will be reported to its parent.
*/

void sys_Exit(int exitval)
{
  PCB* curproc = CURPROC; // Retrieve the current process control block (PCB).

  curproc->exitval = exitval;// Set the exit value of the current process.

  // Special handling for the initial process (PID 1).
  // The initial process must wait for all child processes to terminate before it exits.
  if(get_pid(curproc)==1) {
    while(sys_WaitChild(NOPROC,NULL)!=NOPROC);
  }

 
  // Terminate the main thread of the current process.
  // This call ensures thread-level cleanup and signals any waiting threads.
  sys_ThreadExit(exitval);

 
}



/* This structure defines the file operations (file_ops) for procinfo file-related operations,
including functions for opening, reading, writing, and closing procinfo files.*/
static file_ops procinfo_file_ops = {
  .Open = NULL,     // No specific open operation for procinfo files.
  .Read = procinfo_read,  // Function pointer to the procinfo read operation.
  .Write = NULL,    // No specific write operation for procinfo files.
  .Close = procinfo_close // Function pointer to the procinfo close operation.
};



Fid_t sys_OpenInfo()
{
  Fid_t fid ;
  FCB* fcb ;
//reserve 1 fid fcb 
  int reserve=FCB_reserve(1,&fid,&fcb);


//check reservation succesfull or not
  if(reserve==0){
    return -1;
  }

//creates the socket (the space for the socket)
  procinfo_CB* info_proc=(procinfo_CB*)xmalloc(sizeof(procinfo_CB));

// Check if memory allocation for procinfo control block was successful
  if(info_proc==NULL){
    return NOFILE;
  }


  info_proc->PCB_cursor=0;  //initialization for the cursor in the portmap
  fcb->streamobj=info_proc; // Set the stream object and stream functions in the FCB
  fcb->streamfunc=&procinfo_file_ops;

//return the reserved fid
return fid;

}


/* This function reads the process information from the procinfo control block,
   updates the cursor to the next process entry, and copies the process information
   to the provided buffer. */
int procinfo_read(void* procinfocb, char* buf, unsigned int size)
{
  procinfo_CB* info = (procinfo_CB*) procinfocb;

    // Check if the procinfo control block is valid.
  if(info==NULL){
    return -1;
  }

  // Find the next non-FREE process entry in the process table.
  while(info->PCB_cursor < MAX_PROC && PT[info->PCB_cursor].pstate == FREE) {
    info->PCB_cursor++;
  }

  // Check if there are no more process entries.
  if(info->PCB_cursor == MAX_PROC) {
    return 0;
  }

  procinfo* proc_info = info-> curinfo;

  // Allocate memory for the procinfo structure.
  proc_info = xmalloc(sizeof(procinfo));



  // Get information from the process table and populate the procinfo structure.
  PCB proc = PT[info->PCB_cursor];

  proc_info->pid = get_pid(&PT[info->PCB_cursor]);
  proc_info->ppid = get_pid(proc.parent);


  // Determine the process state (alive or zombie).
  if(proc.pstate == ZOMBIE){
    proc_info->alive = 0;
  }
  else{
    proc_info->alive = 1;
  }
  proc_info->thread_count = proc.thread_count;
  proc_info->main_task = proc.main_task;
  proc_info->argl = proc.argl;

      // Copy process arguments to the procinfo structure.
  if(proc.argl < PROCINFO_MAX_ARGS_SIZE ) {
    memcpy(proc_info->args, proc.args, proc.argl);
  }
  else {
    memcpy(proc_info->args, proc.args, PROCINFO_MAX_ARGS_SIZE);
  }

  // Copy procinfo data to the provided buffer.
  memcpy(buf, proc_info, size);
   
  // Free the allocated memory for the procinfo structure.
  free(proc_info);

  // Move the cursor to the next process entry in the process table.
  info->PCB_cursor++;

  // Return the number of bytes read.
  return size;
}

/*This function checks if the provided procinfo control block pointer is valid,
and if so, frees the memory associated with it.*/
int procinfo_close(void* this){
  procinfo_CB* proc=(procinfo*) this;
  // Free the memory associated with the procinfo_cb structure
  if(proc!=NULL){
    //proc=NULL;
    free(proc);
    return 0;
  }
  return -1;
}
