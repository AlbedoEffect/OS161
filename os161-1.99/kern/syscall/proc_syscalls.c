#include <types.h>
#include <kern/errno.h>
#include <mips/trapframe.h>
#include <kern/unistd.h>
#include <kern/wait.h>
#include <lib.h>
#include <syscall.h>
#include <current.h>
#include <synch.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <copyinout.h>


  /* this implementation of sys__exit does not do anything with the exit code */
  /* this needs to be fixed to get exit() and waitpid() working properly */

void sys__exit(int exitcode) {

  struct addrspace *as;
  struct proc *p = curproc;
  int pid = curproc->pid;
  /* for now, just include this to keep the compiler from complaining about
     an unused variable */
  (void)exitcode;

  DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitcode);

  KASSERT(curproc->p_addrspace != NULL);
  onExit(proc,exitCode);
  as_deactivate();
  /*
   * clear p_addrspace before calling as_destroy. Otherwise if
   * as_destroy sleeps (which is quite possible) when we
   * come back we'll be calling as_activate on a
   * half-destroyed address space. This tends to be
   * messily fatal.
   */
  as = curproc_setas(NULL);
  as_destroy(as);

  /* detach this thread from its process */
  /* note: curproc cannot be used after this call */
  proc_remthread(curthread);

  /* if this is the last user process in the system, proc_destroy()
     will wake up the kernel menu thread */
  proc_destroy(p);
  
  thread_exit();
  /* thread_exit() does not return, so we should never get here */
  panic("return from thread_exit in sys_exit\n");
}


/* stub handler for getpid() system call                */
int
sys_getpid(pid_t *retval)
{
  int val = curproc->pid;
  *retval = val;
  return(0);
}

/* stub handler for waitpid() system call                */

int
sys_waitpid(pid_t pid,
	    userptr_t status,
	    int options,
	    pid_t *retval)
{
  int exitstatus;
  int result;
  
  /* this is just a stub implementation that always reports an
     exit status of 0, regardless of the actual exit status of
     the specified process.   
     In fact, this will return 0 even if the specified process
     is still running, and even if it never existed in the first place.

     Fix this!
  */

  if (options != 0) {
    return(EINVAL);
  }
  lock_acquire(pidManagerLock);
  struct pidEntry * childEntry = getChildEntry(pid);
 
  if(childEntry == NULL || childEntry->parent != curproc){
	return(EINVAL);
  }
  P(childEntry->waitSem);
  exitstatus = childEntry->exitStatus;
  lock_release(pidManagerLock);
  result = copyout((void *)&exitstatus,status,sizeof(int));
  if (result) {
    return(result);
  }
  *retval = pid;
  return(0);
}

int
sys_fork(struct trapframe *tf,pid_t *retval){
	struct proc* child = proc_create_runprogram("new_child");	
	struct addrspace *childAddrspace;
	if(child == NULL){
		return ENOMEM;
	}
	as_copy(curproc_getas(), &childAddrspace);
	if(childAddrspace == NULL){
		proc_destroy(child);
		return ENOMEM;
	}	
	child->p_addrspace = childAddrspace;
	child->parent = curproc;
	struct trapframe *childTf = kmalloc(sizeof(*tf));
	*childTf = *tf;	
	thread_fork("child proc",child,enter_forked_process,childTf,0);	
	*retval = child->pid;
	return 0;
}
