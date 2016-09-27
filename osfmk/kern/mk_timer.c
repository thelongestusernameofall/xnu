/*
 * Copyright (c) 2000-2004 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 * 
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * Copyright (c) 2000 Apple Computer, Inc.  All rights reserved.
 *
 * HISTORY
 *
 * 29 June 2000 (debo)
 *  Created.
 */

#include <mach/mach_types.h>
#include <mach/mach_traps.h>
#include <mach/mach_port_server.h>

#include <mach/mk_timer.h>

#include <ipc/ipc_space.h>

#include <kern/mk_timer.h>
#include <kern/thread_call.h>

static zone_t		mk_timer_zone;

static mach_port_qos_t mk_timer_qos = {
	FALSE, TRUE, 0, sizeof (mk_timer_expire_msg_t)
};

static void	mk_timer_expire(
				void			*p0,
				void			*p1);

mach_port_name_t
mk_timer_create_trap(
	__unused struct mk_timer_create_trap_args *args)
{
	mk_timer_t			timer;
	ipc_space_t			myspace = current_space();
	mach_port_name_t	name = MACH_PORT_NULL;
	ipc_port_t			port;
	kern_return_t		result;

	timer = (mk_timer_t)zalloc(mk_timer_zone);
	if (timer == NULL)
		return (MACH_PORT_NULL);

	result = mach_port_allocate_qos(myspace, MACH_PORT_RIGHT_RECEIVE,
														&mk_timer_qos, &name);
	if (result == KERN_SUCCESS)
		result = ipc_port_translate_receive(myspace, name, &port);

	if (result != KERN_SUCCESS) {
		zfree(mk_timer_zone, timer);

		return (MACH_PORT_NULL);
	}

	simple_lock_init(&timer->lock, 0);
	thread_call_setup(&timer->call_entry, mk_timer_expire, timer);
	timer->is_armed = timer->is_dead = FALSE;
	timer->active = 0;

	timer->port = port;
	ipc_kobject_set_atomically(port, (ipc_kobject_t)timer, IKOT_TIMER);

	port->ip_srights++;
	ip_reference(port);
	ip_unlock(port);

	return (name);
}

void
mk_timer_port_destroy(
	ipc_port_t			port)
{
	mk_timer_t			timer = NULL;

	ip_lock(port);
	if (ip_kotype(port) == IKOT_TIMER) {
		timer = (mk_timer_t)port->ip_kobject;
		assert(timer != NULL);
		ipc_kobject_set_atomically(port, IKO_NULL, IKOT_NONE);
		simple_lock(&timer->lock);
		assert(timer->port == port);
	}
	ip_unlock(port);

	if (timer != NULL) {
		if (thread_call_cancel(&timer->call_entry))
			timer->active--;
		timer->is_armed = FALSE;

		timer->is_dead = TRUE;
		if (timer->active == 0) {
			simple_unlock(&timer->lock);
			zfree(mk_timer_zone, timer);

			ipc_port_release_send(port);
			return;
		}

		simple_unlock(&timer->lock);
	}
}

void
mk_timer_init(void)
{
	int			s = sizeof (mk_timer_data_t);

	assert(!(mk_timer_zone != NULL));

	mk_timer_zone = zinit(s, (4096 * s), (16 * s), "mk_timer");

	zone_change(mk_timer_zone, Z_NOENCRYPT, TRUE);
}

static void
mk_timer_expire(
	void			*p0,
	__unused void		*p1)
{
	mk_timer_t			timer = p0;
	ipc_port_t			port;

	simple_lock(&timer->lock);

	if (timer->active > 1) {
		timer->active--;
		simple_unlock(&timer->lock);
		return;
	}

	port = timer->port;
	assert(port != IP_NULL);
	assert(timer->active == 1);

	while (timer->is_armed && timer->active == 1) {
		mk_timer_expire_msg_t		msg;

		timer->is_armed = FALSE;
		simple_unlock(&timer->lock);

		msg.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
		msg.header.msgh_remote_port = port;
		msg.header.msgh_local_port = MACH_PORT_NULL;
		msg.header.msgh_reserved = msg.header.msgh_id = 0;

		msg.unused[0] = msg.unused[1] = msg.unused[2] = 0;

		(void) mach_msg_send_from_kernel_proper(&msg.header, sizeof (msg));

		simple_lock(&timer->lock);
	}

	if (--timer->active == 0 && timer->is_dead) {
		simple_unlock(&timer->lock);
		zfree(mk_timer_zone, timer);

		ipc_port_release_send(port);
		return;
	}

	simple_unlock(&timer->lock);
}

/*
 * mk_timer_destroy_trap: Destroy the Mach port associated with a timer
 *
 * Parameters:  args                     User argument descriptor (see below)
 *
 * Indirect:     args->name               Mach port name
 *
 *
 * Returns:        0                      Success
 *                !0                      Not success           
 *
 */
kern_return_t
mk_timer_destroy_trap(
	struct mk_timer_destroy_trap_args *args)
{
	mach_port_name_t	name = args->name;
	ipc_space_t			myspace = current_space();
	ipc_port_t			port;
	kern_return_t		result;

	result = ipc_port_translate_receive(myspace, name, &port);
	if (result != KERN_SUCCESS)
		return (result);

	if (ip_kotype(port) == IKOT_TIMER) {
		ip_unlock(port);
		result = mach_port_destroy(myspace, name);
	}
	else {
		ip_unlock(port);
		result = KERN_INVALID_ARGUMENT;
	}

	return (result);
}

/*
 * mk_timer_arm_trap: Start (arm) a timer
 *
 * Parameters:  args                     User argument descriptor (see below)
 *
 * Indirect:     args->name               Mach port name
 *               args->expire_time        Time when timer expires
 *
 *
 * Returns:        0                      Success
 *                !0                      Not success           
 *
 */
kern_return_t
mk_timer_arm_trap(
	struct mk_timer_arm_trap_args *args)
{
	mach_port_name_t	name = args->name;
	uint64_t			expire_time = args->expire_time;
	mk_timer_t			timer;
	ipc_space_t			myspace = current_space();
	ipc_port_t			port;
	kern_return_t		result;

	result = ipc_port_translate_receive(myspace, name, &port);
	if (result != KERN_SUCCESS)
		return (result);

	if (ip_kotype(port) == IKOT_TIMER) {
		timer = (mk_timer_t)port->ip_kobject;
		assert(timer != NULL);
		simple_lock(&timer->lock);
		assert(timer->port == port);
		ip_unlock(port);

		if (!timer->is_dead) {
			timer->is_armed = TRUE;

			if (!thread_call_enter_delayed(&timer->call_entry, expire_time))
				timer->active++;
		}

		simple_unlock(&timer->lock);
	}
	else {
		ip_unlock(port);
		result = KERN_INVALID_ARGUMENT;
	}

	return (result);
}

/*
 * mk_timer_cancel_trap: Cancel a timer
 *
 * Parameters:  args                     User argument descriptor (see below)
 *
 * Indirect:     args->name               Mach port name
 *               args->result_time        The armed time of the cancelled timer (return value)
 *
 *
 * Returns:        0                      Success
 *                !0                      Not success           
 *
 */
kern_return_t
mk_timer_cancel_trap(
	struct mk_timer_cancel_trap_args *args)
{
	mach_port_name_t	name = args->name;
	mach_vm_address_t	result_time_addr = args->result_time; 
	uint64_t			armed_time = 0;
	mk_timer_t			timer;
	ipc_space_t			myspace = current_space();
	ipc_port_t			port;
	kern_return_t		result;

	result = ipc_port_translate_receive(myspace, name, &port);
	if (result != KERN_SUCCESS)
		return (result);

	if (ip_kotype(port) == IKOT_TIMER) {
		timer = (mk_timer_t)port->ip_kobject;
		assert(timer != NULL);
		simple_lock(&timer->lock);
		assert(timer->port == port);
		ip_unlock(port);

		if (timer->is_armed) {
			armed_time = timer->call_entry.tc_call.deadline;
			if (thread_call_cancel(&timer->call_entry))
				timer->active--;
			timer->is_armed = FALSE;
		}

		simple_unlock(&timer->lock);
	}
	else {
		ip_unlock(port);
		result = KERN_INVALID_ARGUMENT;
	}

	if (result == KERN_SUCCESS)
		if (	result_time_addr != 0										&&
				copyout((void *)&armed_time, result_time_addr,
								sizeof (armed_time)) != 0					)
			result = KERN_FAILURE;

	return (result);
}
