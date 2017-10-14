#include <types.h>
#include <lib.h>
#include <synchprobs.h>
#include <synch.h>
#include <opt-A1.h>

/* 
 * This simple default synchronization mechanism allows only vehicle at a time
 * into the intersection.   The intersectionSem is used as a a lock.
 * We use a semaphore rather than a lock so that this code will work even
 * before locks are implemented.
 */

/* 
 * Replace this default synchronization mechanism with your own (better) mechanism
 * needed for your solution.   Your mechanism may use any of the available synchronzation
 * primitives, e.g., semaphores, locks, condition variables.   You are also free to 
 * declare other global variables if your solution requires them.
 */

/*
 * replace this with declarations of any synchronization and other variables you need here
 */
struct Pair {
	int first;
	int second;
};

static struct lock *inter_lock;
static struct cv *cvs[4][4]; 
int inter_state[4][4];

/* 
 * The simulation driver will call this function once before starting
 * the simulation
 *
 * You can use it to initialize synchronization and other variables.
 * 
 */
void
intersection_sync_init(void)
{
  inter_lock = lock_create("inter_lock");
  if (inter_lock == NULL) {
    panic("could not create lock!");
  }
  for(int i = 0; i < 4; i++){
	for(int j = 0; j < 4; j++){
		cvs[i][j] = cv_create("intCV");
		if(cvs[i][j] == NULL){
			panic("could not create cv!");
		}
	}
  }
  return;
}

/* 
 * The simulation driver will call this function once after
 * the simulation has finished
 *
 * You can use it to clean up any synchronization and other variables.
 *
 */
void
intersection_sync_cleanup(void)
{
  KASSERT(inter_lock != NULL);
  lock_destroy(inter_lock);
  for(int i = 0; i < 4; i++){
	for(int j = 0; j < 4; j++){
		KASSERT(cvs[i][j] != NULL);
		cv_destroy(cvs[i][j]);
	}
  }
}

static bool
right_turn(Direction origin, Direction destination) {
  if (((origin == west) && (destination == south)) ||
      ((origin == south) && (destination == east)) ||
      ((origin == east) && (destination == north)) ||
      ((origin == north) && (destination == west))){ 
    return true;
  } else {
    return false;
  }
}

static bool 
is_conflict(Direction o1, Direction d1, Direction o2, Direction d2){
	return !((o1 == o2) || ((o1 == d2) && (o2 == d1)) || (d1 != d2 && 
		(right_turn(o1,d1) || right_turn(o2,d2))));
}

struct Pair* exists_conflict(Direction origin, Direction destination);
struct Pair*
exists_conflict(Direction origin, Direction destination){
   struct Pair * pair = kmalloc(sizeof(struct Pair));
   for(int i = 0; i < 4; i++){
	for(int j = 0; j < 4; j++){
	    if(inter_state[i][j] > 0 && is_conflict(i,j,origin,destination)){
		pair->first = i;
		pair->second = j;
		return pair;
	    }
	}
   }
   pair->first = -1;
   pair->second = -1;
   return pair;
}

/*
 * The simulation driver will call this function each time a vehicle
 * tries to enter the intersection, before it enters.
 * This function should cause the calling simulation thread 
 * to block until it is OK for the vehicle to enter the intersection.
 *
 * parameters:
 *    * origin: the Direction from which the vehicle is arriving
 *    * destination: the Direction in which the vehicle is trying to go
 *
 * return value: none
 */
void
intersection_before_entry(Direction origin, Direction destination) 
{
  lock_acquire(inter_lock);
  struct Pair *curConflict;
  curConflict = exists_conflict(origin,destination);
  struct Pair *invPair = kmalloc(sizeof(struct Pair));
  invPair->first = -1;
  invPair->second = -1; 
  while(curConflict->first != invPair->first && curConflict->second != invPair->second){
	cv_wait(cvs[curConflict->first][curConflict->second],inter_lock);
	kfree(curConflict);
	curConflict = exists_conflict(origin,destination);
  }
  inter_state[origin][destination]++;
  kfree(invPair);
  lock_release(inter_lock);
}


/*
 * The simulation driver will call this function each time a vehicle
 * leaves the intersection.
 *
 * parameters:
 *    * origin: the Direction from which the vehicle arrived
 *    * destination: the Direction in which the vehicle is going
 *
 * return value: none
 */

void
intersection_after_exit(Direction origin, Direction destination) 
{
	lock_acquire(inter_lock);
	cv_signal(cvs[origin][destination],inter_lock);
	inter_state[origin][destination]--;
	lock_release(inter_lock);
}
