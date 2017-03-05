/***********************************************************************
 *
 *  Copyright (c) 2006-2007  Broadcom Corporation
 *  All Rights Reserved
 *
 * <:label-BRCM:2011:DUAL/GPL:standard
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as published by
 * the Free Software Foundation (the "GPL").
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * 
 * A copy of the GPL is available at http://www.broadcom.com/licenses/GPLv2.php, or by
 * writing to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 * 
:>
 *
 ************************************************************************/

/*!\file cms_ftrace.h
 * \brief Header file containing constant definitions and helper functions
 *        related to Ftrace.
 *
 */

#ifndef CMS_FTRACE_H_
#define CMS_FTRACE_H_

/** This is where the debugfs filesystem will be mounted if ftrace is enabled.
 *  The ftrace directories and files are all in the tracing sub-directory.
 */
#define FTRACE_MOUNTPOINT "/var/debug"

/** This is the file that controls tracing on or off.
 */
#define FTRACE_ON         "/var/debug/tracing/tracing_on"

/** This is the file that allows userspace apps to insert a trace line
 */
#define FTRACE_INSERT_MARKER "/var/debug/tracing/trace_marker"

/** This is the file that selects which type of tracing we are doing
 */
#define FTRACE_CURRENT_TRACER  "/var/debug/tracing/current_tracer"

/** This is the file that allows you to get and set current options.
 */
#define FTRACE_TRACE_OPTIONS  "/var/debug/tracing/trace_options"



/** Enable tracing
 */
void cmsFtr_enable(void);

/** Disable tracing
 */
void cmsFtr_disable(void);

/** Insert a string in the current trace.
 * @param (IN) string to insert
 */
void cmsFtr_insertMarker(const char *s);

/** Set the specified option in trace_option.
 *  This function does not do error checking on the option
 *  string, so make sure the option name is correct.
 */
void cmsFtr_setTraceOption(const char *s);

/** Set the current trace type to function tracing.
 *  Tracing must still be enabled/disabled separately.
 */
void cmsFtr_doFunctionTracing(void);

/** Set the current trace type to function graph tracing.
 *  Tracing must still be enabled/disabled separately.
 */
void cmsFtr_doFunctionGraphTracing(void);

/** Set the current trace type to nothing.
 *  This will also clear the trace buffer.
 */
void cmsFtr_doNopTracing(void);


#endif /* CMS_FTRACE_H_ */
