#include <types.h>
#include <kern/errno.h>
#include <mips/trapframe.h>
#include <kern/fcntl.h>
#include <kern/unistd.h>
#include <kern/wait.h>
#include <lib.h>
#include <syscall.h>
#include <array.h>
#include <vfs.h>
#include <current.h>
#include <synch.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <copyinout.h>


  /* this implementation of sys__exit does not do anything with the exit code */
  /* this needs to be fixed to get exit() and waitpid() working properly */

void sys__exit(int exitCode) {

  struct addrspace *as;
  struct proc *p = curproc;
  DEBUG(DB_SYSCALL,"Syscall: _exit(%d)\n",exitCode);

  KASSERT(curproc->p_addrspace != NULL);
  onExit(p,exitCode);
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

  lock_acquire(pidManagerLock);
  struct pidEntry * childEntry = getChildEntry(pid);
  if(options == WNOHANG){
	if(childEntry->waitSem->sem_count == 0){
		lock_release(pidManagerLock);
		return ECHILD;
	}
  }/*else if(options == WAIT_ANY){
	P(curproc->waitAnySem);
	return 
  }*/
  if(childEntry == NULL || childEntry->parent != curproc){
	lock_release(pidManagerLock);
	return(EINVAL);
  }
  lock_release(pidManagerLock);
  P(childEntry->waitSem);
  lock_acquire(pidManagerLock);
  exitstatus = _MKWAIT_EXIT(childEntry->exitCode);
  result = copyout((void *)&exitstatus,status,sizeof(int));
  lock_release(pidManagerLock);
  if (result) {
    return(result);
  }
  *retval = childEntry->pid;
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
	struct pidEntry * pidEntry = getChildEntry(child->pid);
	if(pidEntry == NULL){
		return EINVAL;
	}
	pidEntry->parent = curproc;
	struct trapframe *childTf = kmalloc(sizeof(*tf));
	*childTf = *tf;	
	thread_fork("child proc",child,enter_forked_process,childTf,0);	
	*retval = child->pid;
	return 0;
}

int
performCopy(int argc, char **argv, vaddr_t *stackPtr, userptr_t * argvAddr){
	vaddr_t stackptr = *stackPtr;
	char **argv1 = kmalloc(sizeof(char*)*(argc+1));
	size_t len = 0;
	int result;	
	*(argv1+argc) = NULL;
	for(int i = 0; i < argc; i++){
		int curLen = strlen(*(argv+i))+1;
		stackptr -= ROUNDUP(curLen,8);

		result = copyoutstr(*(argv+i),(userptr_t)stackptr,curLen,&len);
		if(result){
			return result;	
		}
		*(argv1+i) = (char *)stackptr;
	}
	for(int i = 0; i <= argc; i++){
		stackptr -= sizeof(char*);
		result = copyout(argv1+(argc-i),(userptr_t)stackptr,sizeof(char*));
		if(result){
			return result;
		}
	}
	*argvAddr = (userptr_t) stackptr;
	stackptr -= stackptr % 8 == 0 ? 8 : 4;
	*stackPtr = stackptr;
	return 0;
}

int
sys_execv(const char *program, userptr_t args){
	(void)program;
	(void)args;
	struct vnode * v;
	vaddr_t entrypoint, stackptr;

	size_t size;
	char * progName = kmalloc(sizeof(program));
	int result = copyinstr((const_userptr_t)program,progName,strlen(program)+1,&size);
	if(result){
		kfree(progName);
		return result;
	}
	void * kernArgs = kmalloc(sizeof(args));
	struct array *argStrs = array_create();
	int argc = 0;

	result = copyin(args,kernArgs,sizeof(args));
	if(result){
		kfree(kernArgs);
		return result;
	}

	unsigned index = 0;
	while((*((char **)kernArgs)) != NULL){
		int len  = strlen(*((char**)(kernArgs))) + 1;
		char * temp = kmalloc(len);
		result = copyinstr((userptr_t)*((char**)(kernArgs)),temp,len, &index);
		if(result){
			kfree(kernArgs);
			return result;
		}
		result = array_add(argStrs,temp,&index);
		if(result){
			kfree(kernArgs);
			return result;
		}
		argc++;
		result = copyin(args+4*argc,kernArgs,sizeof(args+4*argc));
		if(result){
			kfree(kernArgs);
			return result;
		}
	}

	result = array_add(argStrs,NULL, &index);
	if(result){
		kfree(kernArgs);
		return result;
	}

	result = vfs_open(progName, O_RDONLY,0,&v);
	if(result){
		return result;
	}

	struct addrspace *tempSpace = curproc_getas();

	struct addrspace *as;	
	as = as_create();
	if (as == NULL){
		return ENOMEM;
	}

	curproc_setas(as);
	as_activate();

	result = load_elf(v,&entrypoint);
	if (result){
		vfs_close(v);
		curproc_setas(tempSpace);
		as_destroy(as);
		return result;
	}
	vfs_close(v);

	result = as_define_stack(as,&stackptr);
	if (result){
		as_destroy(as);
		return result;
	}
	userptr_t argvAddr;
	result = performCopy(argc,(char**)(argStrs->v),&stackptr, &argvAddr);
	if(result){
		curproc_setas(tempSpace);
		as_destroy(as);
		return result;
	}

	as_destroy(tempSpace);

	enter_new_process(argc /*argc*/,argvAddr/*userspace addr of argv*/,
		 stackptr, entrypoint);
	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;
}
