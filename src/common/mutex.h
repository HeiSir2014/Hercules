/**
 * This file is part of Hercules.
 * http://herc.ws - http://github.com/HerculesWS/Hercules
 *
 * Copyright (C) 2012-2015  Hercules Dev Team
 * Copyright (C)  rAthena Project (www.rathena.org)
 *
 * Hercules is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef COMMON_MUTEX_H
#define COMMON_MUTEX_H

#include "common/cbasetypes.h"
#include "common/hercules.h"

typedef struct ramutex ramutex; // Mutex
typedef struct racond racond; // Condition Var

struct mutex_interface {
	/**
	* Creates a Mutex
	*
	* @return not NULL
	*/
	ramutex *(*create) (void);

	/**
	* Destroys a Mutex
	*
	* @param m - the mutex to destroy
	*/
	void (*destroy) (ramutex *m);

	/**
	* Gets a lock
	*
	* @param m - the mutex to lock
	*/
	void (*lock) (ramutex *m);

	/**
	* Trys to get the Lock
	*
	* @param m - the mutex try to lock
	*
	* @return boolean (true = got the lock)
	*/
	bool (*trylock) (ramutex *m);

	/**
	* Unlocks a mutex
	*
	* @param m - the mutex to unlock
	*/
	void (*unlock) (ramutex *m);


	/**
	* Creates a Condition variable
	*
	* @return not NULL
	*/
	racond *(*cond_create) (void);

	/**
	* Destroy a Condition variable
	*
	* @param c - the condition variable to destroy
	*/
	void (*cond_destroy) (racond *c);

	/**
	* Waits Until state is signaled
	*
	* @param c - the condition var to wait for signaled state
	* @param m - the mutex used for synchronization
	* @param timeout_ticks - timeout in ticks ( -1 = INFINITE )
	*/
	void (*cond_wait) (racond *c, ramutex *m, sysint timeout_ticks);

	/**
	* Sets the given condition var to signaled state
	*
	* @param c - condition var to set in signaled state.
	*
	* @note:
	*  Only one waiter gets notified.
	*/
	void (*cond_signal) (racond *c);

	/**
	* Sets notifies all waiting threads thats signaled.
	* @param c - condition var to set in signaled state
	*
	* @note:
	*  All Waiters getting notified.
	*/
	void (*cond_broadcast) (racond *c);
};

#ifdef HERCULES_CORE
void mutex_defaults(void);
#endif // HERCULES_CORE

HPShared struct mutex_interface *mutex;

#endif /* COMMON_MUTEX_H */
