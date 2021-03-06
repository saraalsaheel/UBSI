/* auditd-event.c -- 
	* Copyright 2004-08,2011,2013,2015-16 Red Hat Inc., Durham, North Carolina.
	* All Rights Reserved.
	*
	* This program is free software; you can redistribute it and/or modify
	* it under the terms of the GNU General Public License as published by
	* the Free Software Foundation; either version 2 of the License, or
	* (at your option) any later version.
	*
	* This program is distributed in the hope that it will be useful,
	* but WITHOUT ANY WARRANTY; without even the implied warranty of
	* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	* GNU General Public License for more details.
	*
	* You should have received a copy of the GNU General Public License
	* along with this program; if not, write to the Free Software
	* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
	*
	* Authors:
	*   Steve Grubb <sgrubb@redhat.com>
	*
	*/

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>	/* O_NOFOLLOW needs gnu defined */
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/vfs.h>
#include <limits.h>     /* POSIX_HOST_NAME_MAX */
#include <ctype.h>	/* toupper */
#include <libgen.h>	/* dirname */
#include "auditd-event.h"
#include "auditd-dispatch.h"
#include "auditd-listen.h"
#include "libaudit.h"
#include "private.h"
#include "auparse.h"

/* This is defined in auditd.c */
extern volatile int stop;

/* Local function prototypes */
static void write_to_log(const struct auditd_event *e);
static void check_log_file_size(void);
static void check_space_left(void);
static void do_space_left_action(int admin);
static void do_disk_full_action(void);
static void do_disk_error_action(const char *func, int err);
static void fix_disk_permissions(void);
static void check_excess_logs(void); 
static void rotate_logs_now(void);
static void rotate_logs(unsigned int num_logs);
static void shift_logs(void);
static int  open_audit_log(void);
static void change_runlevel(const char *level);
static void safe_exec(const char *exe);
static void reconfigure(struct auditd_event *e);
static void init_flush_thread(void);


/* Local Data */
static struct daemon_conf *config;
static int log_fd;
static FILE *log_file;
static unsigned int disk_err_warning = 0;
static int fs_space_warning = 0;
static int fs_admin_space_warning = 0;
static int fs_space_left = 1;
static int logging_suspended = 0;
static const char *SINGLE = "1";
static const char *HALT = "0";
static char *format_buf = NULL;
static off_t log_size = 0;
static pthread_t flush_thread;
static pthread_mutex_t flush_lock;
static pthread_cond_t do_flush;
static volatile int flush;

/* Local definitions */
#define FORMAT_BUF_LEN (MAX_AUDIT_MESSAGE_LENGTH + _POSIX_HOST_NAME_MAX)
#define MIN_SPACE_LEFT 24



// KYU modification start
#include <assert.h>
#include "uthash.h"
#define UENTRY 0xffffff9c
#define UEXIT 0xffffff9b
#define MREAD1 0xffffff38
#define MREAD2 0xffffff37
#define MWRITE1 0xfffffed4
#define MWRITE2 0xfffffed3

typedef int bool;
#define true 1
#define false 0

typedef struct thread_unit_t {
		int tid;
		int unitid;
} thread_unit_t;

typedef struct link_unit_t {
		thread_unit_t id;
		UT_hash_handle hh;
} link_unit_t;

typedef struct mem_proc_t {
		long int addr;
		int tid;				// last written tid
		int unitid;	// last written unit
		UT_hash_handle hh;
} mem_proc_t;

typedef struct mem_unit_t {
		long int addr;
		//int isWritten;
		UT_hash_handle hh;
} mem_unit_t;

typedef struct unit_table_t {
		int tid;
		int pid;
		int unitid;
		bool valid; // is valid unit?
		long int r_addr;
		long int w_addr;
		link_unit_t *link_unit;
		mem_proc_t *mem_proc;
		mem_unit_t *mem_unit; // mem_write_record in the unit
		UT_hash_handle hh;
} unit_table_t;

unit_table_t *unit_table;

void emit_log(unit_table_t *ut, char* buf)
{
		buf[strlen(buf)-1] = '\0';
		int rc = fprintf(log_file, "%s unitid=%d\n",buf, ut->unitid);
}

void delete_unit_hash(link_unit_t *hash_unit, mem_unit_t *hash_mem)
{
	//	HASH_CLEAR(hh, hash_unit);
	//	HASH_CLEAR(hh, hash_mem);
		
		link_unit_t *tmp_unit, *cur_unit;
		mem_unit_t *tmp_mem, *cur_mem;
		HASH_ITER(hh, hash_unit, cur_unit, tmp_unit) {
				if(hash_unit != cur_unit) 
						HASH_DEL(hash_unit, cur_unit); 
				if(cur_unit) free(cur_unit);  
		}
		//if(hash_unit) free(hash_unit);

		HASH_ITER(hh, hash_mem, cur_mem, tmp_mem) {
				if(hash_mem != cur_mem) 
						HASH_DEL(hash_mem, cur_mem); 
				if(cur_mem) free(cur_mem);  
		}
		//if(hash_mem) free(hash_mem);

}

void delete_proc_hash(mem_proc_t *mem_proc)
{
		//HASH_CLEAR(hh, mem_proc);
		
		mem_proc_t *tmp_mem, *cur_mem;
		HASH_ITER(hh, mem_proc, cur_mem, tmp_mem) {
				if(mem_proc != cur_mem) 
						HASH_DEL(mem_proc, cur_mem); 
				if(cur_mem) free(cur_mem);  
		}
		//if(mem_proc) free(mem_proc);
}

void unit_end(unit_table_t *unit)
{
		struct link_unit_t *ut;
		char buf[10240];

		if(unit->valid == true || HASH_COUNT(unit->link_unit) > 1) {
				bzero(buf, 10240);
				// emit linked unit lists;
				if(unit->link_unit != NULL) {
						sprintf(buf, "type=unit list=\"");
						for(ut=unit->link_unit; ut != NULL; ut=ut->hh.next) {
								sprintf(buf+strlen(buf), "%d-%d,", ut->id.tid, ut->id.unitid);
						}
						sprintf(buf+strlen(buf), "\" tid=%d \n", unit->tid);
						emit_log(unit, buf);
				}
		}

		delete_unit_hash(unit->link_unit, unit->mem_unit);
		//if(unit->link_unit != NULL) printf("link_unit is not NULL %p\n", unit->link_unit);
		unit->link_unit = NULL;
		unit->mem_unit = NULL;
		unit->valid = false;
		unit->r_addr = 0;
		unit->w_addr = 0;
		unit->unitid++;
}

void proc_end(unit_table_t *unit)
{
		unit_end(unit);
		delete_proc_hash(unit->mem_proc);
		unit->mem_proc = NULL;
}

void proc_group_end(unit_table_t *unit)
{
		int pid = unit->pid;
		unit_table_t *pt;

		if(pid != unit->tid) {
				HASH_FIND_INT(unit_table, &pid, pt);
				proc_end(pt);
		}

		proc_end(unit);
}

void flush_all_unit()
{
		unit_table_t *tmp_unit, *cur_unit;
		HASH_ITER(hh, unit_table, cur_unit, tmp_unit) {
				unit_end(cur_unit);
		}
}

bool is_selected_syscall(int S, bool succ)
{
		if(S == 60 || S == 231)  return true;

		if(!succ) 
				return false;
		
		switch(S) {
				case 0: case 19: case 1: case 20: case 44: case 45: case 46: case 47: case 86: case 88: case 56: case 57: case 58:
				case 59: case 2: case 85: case 257: case 259: case 133: case 32: case 33: case 292: case 49: case 43: case 288:
				case 42: case 82: case 105: case 113: case 90: case 22: case 293: case 76: case 77: case 40: case 87: case 263: case 62:
				return true;
		}
		return false;
}

void mem_write(unit_table_t *ut, long int addr)
{
		// check dup_write
		mem_unit_t *umt;
		HASH_FIND(hh, ut->mem_unit, &addr, sizeof(long int), umt);

		if(umt != NULL) return;
		
		// not duplicated write
		umt = (mem_unit_t*) malloc(sizeof(mem_unit_t));
		umt->addr = addr;
//		umt->isWritten = 1;
		HASH_ADD(hh, ut->mem_unit, addr, sizeof(long int),  umt);

		// add it into process memory map
		int pid = ut->pid;
		unit_table_t *pt;
		if(pid == ut->tid) pt = ut;
		else {
				HASH_FIND_INT(unit_table, &pid, pt);
				if(pt == NULL) {
						assert(1);
				}
		}

		mem_proc_t *pmt;
		HASH_FIND(hh, pt->mem_proc, &addr, sizeof(long int), pmt);
		if(pmt == NULL) {
				pmt = (mem_proc_t*) malloc(sizeof(mem_proc_t));
				pmt->addr = addr;
				pmt->tid = ut->tid;
				pmt->unitid = ut->unitid;
				HASH_ADD(hh, pt->mem_proc, addr, sizeof(long int),  pmt);
		} else {
				pmt->tid = ut->tid;
				pmt->unitid = ut->unitid;
		}
}

void mem_read(unit_table_t *ut, long int addr)
{
		int pid = ut->pid;
		unit_table_t *pt;
		if(pid == ut->tid) pt = ut;
		else {
				HASH_FIND_INT(unit_table, &pid, pt);
				if(pt == NULL) {
						assert(1);
				}
		}

		mem_proc_t *pmt;
		HASH_FIND(hh, pt->mem_proc, &addr, sizeof(long int), pmt);
		if(pmt == NULL) return;

		if((pmt->tid != ut->tid) || (pmt->unitid != ut->unitid))
		{
				link_unit_t *lt;
				thread_unit_t lid;
				lid.tid = pmt->tid;
				lid.unitid = pmt->unitid;
				HASH_FIND(hh, ut->link_unit, &lid, sizeof(thread_unit_t), lt);
				if(lt == NULL) {
				//		printf("lt is null, now add hash link, ");
						lt = (link_unit_t*) malloc(sizeof(link_unit_t));
						lt->id.tid = pmt->tid;
						lt->id.unitid = pmt->unitid;
						HASH_ADD(hh, ut->link_unit, id, sizeof(thread_unit_t), lt);
				//		if(ut->link_unit == NULL) printf("It is NULL!\n");
					//	else printf("correctly inserted\n");
				}
		}
}

unit_table_t* add_unit(int tid, int pid, int unitid, bool valid)
{
		struct unit_table_t *ut;
		ut = malloc(sizeof(struct unit_table_t));
		ut->tid = tid;
		ut->pid = pid;
		ut->unitid = unitid;
		ut->valid = valid;
		ut->link_unit = NULL;
		ut->mem_proc = NULL;
		ut->mem_unit = NULL;
		HASH_ADD_INT(unit_table, tid, ut);
		return ut;
}

void set_pid(int tid, int pid)
{
		struct unit_table_t *ut;
		int ppid;

		HASH_FIND_INT(unit_table, &pid, ut);  /* looking for parent thread's pid */
		if(ut == NULL) ppid = pid;
		else ppid = ut->pid;

		ut = NULL;

		HASH_FIND_INT(unit_table, &tid, ut);  /* id already in the hash? */
		if (ut == NULL) {
				ut = add_unit(tid, ppid, 0, 0); 
		} else {
				ut->pid = ppid;
		}

}

void UBSI_event(long tid, long a0, long a1, char *buf)
{
		int isNewUnit = 0;
		struct unit_table_t *ut;
		HASH_FIND_INT(unit_table, &tid, ut);
		
		if(ut == NULL) {
				isNewUnit = 1;
				ut = add_unit(tid, tid, 0, 0);
		}

		switch(a0) {
				case UENTRY: 
				case UEXIT: 
						// if the unit exist, finish the unit.
						if(isNewUnit == false)
						{
								unit_end(ut);
						}
						break;
				case MREAD1:
						ut->r_addr = a1;
						ut->r_addr = ut->r_addr << 32;
						break;
				case MREAD2:
						ut->r_addr += a1;
						mem_read(ut, ut->r_addr);
						break;
				case MWRITE1:
						ut->w_addr = a1;
						ut->w_addr = ut->w_addr << 32;
						break;
				case MWRITE2:
						ut->w_addr += a1;
						mem_write(ut, ut->w_addr);
						break;
		}
}

void non_UBSI_event(long tid, int sysno, bool succ, char *buf)
{
		char *ptr;
		long a2;
		long ret;

		bool isNewUnit = false;
		struct unit_table_t *ut;
		
		if(!is_selected_syscall(sysno, succ))  return;

		HASH_FIND_INT(unit_table, &tid, ut);
		
		if(ut == NULL) {
				isNewUnit = true;
				ut = add_unit(tid, tid, 0, 0);
		}
		
		//emit system calls.
		emit_log(ut, buf);
		
		if(succ == true && (sysno == 56 || sysno == 57 || sysno == 58)) // clone or fork
		{
				ptr = strstr(buf, " a2=");
				a2 = strtol(ptr+4, NULL, 16);

				
				if(a2 > 0) { // thread_creat event
						ptr = strstr(buf, " exit=");
						ret = strtol(ptr+6, NULL, 10);
						set_pid(ret, tid);
				}
		} else if(succ == true && ( sysno == 59 || sysno == 322 || sysno == 60 || sysno == 231)) { // execve, exit or exit_group
				if(sysno == 231) { // exit_group call
						// TODO: need to finish all thread in the process group
						proc_group_end(ut);
				}
				unit_end(ut);
				proc_end(ut);
		} else {
				ut->valid = true;
		}
}

void get_comm(char *buf, char *comm)
{
		char *ptr;
		int i=0;

		ptr = strstr(buf, " comm=");
		ptr+=6;

		for(i=0; ptr[i] != ' '; i++)
		{
				comm[i] = ptr[i];
		}
		comm[i] = '\0';
		printf("comm = %s: %s", comm, buf);
}

bool get_succ(char *buf)
{
		char *ptr;
		char succ[16];
		int i=0;

		ptr = strstr(buf, " success=");
		if(ptr == NULL) {
				//printf("PTR NULL: %s\n", buf);
				return false;
		}
		ptr+=9;

		for(i=0; ptr[i] != ' '; i++)
		{
				succ[i] = ptr[i];
		}
		succ[i] = '\0';
		//printf("success = %s: %s", succ, buf);
		if(strncmp(succ, "yes", 3) == 0) return true;
		else false;
}

void syscall_handler(char *buf)
{
		char *ptr;
		int sysno;
		long a0, a1, pid;
		char comm[64];
		bool succ;

		ptr = strstr(buf, " syscall=");
		if(ptr == NULL) {
				printf("ptr = NULL: %s\n", buf);
				return;
		}
		//sysno = atoi(ptr);
		sysno = strtol(ptr+9, NULL, 10);
		//printf("SYSNO %d: %s\n", sysno, ptr);
		
		ptr = strstr(ptr, " pid=");
		pid = strtol(ptr+5, NULL, 10);

		succ = get_succ(buf);
		//if(!succ) printf("succ=NO!, %s\n", buf);
		//get_comm(buf);

		if(sysno == 62)
		{
				ptr = strstr(buf, " a0=");
				a0 = strtol(ptr+4, NULL, 16);
				if(a0 == UENTRY || a0 == UEXIT || a0 == MREAD1 || a0 == MREAD2 || a0 == MWRITE1 || a0 ==MWRITE2)
				{
						ptr = strstr(ptr, " a1=");
						a1 = strtol(ptr+4, NULL, 16);
						UBSI_event(pid, a0, a1, buf);
						//printf("pid %d, a0 %x, a1 %x: %s\n", pid, a0, a1, buf);
				} else {
						non_UBSI_event(pid, sysno, succ, buf);
				}
		} else {
				non_UBSI_event(pid, sysno, succ, buf);
		}
}

int buffering(char *buf)
{
		static long last_eid = 0;
		static char stag_buf[1048576];
		long eid;
		char *ptr;

		if(strncmp(buf, "type=UNKNOWN",12) == 0 || strncmp(buf, "type=PROCTITLE",14) == 0 ) {
				return 0;
		}

		ptr = strstr(buf, ":");
		eid = strtol(ptr+1, NULL, 10);

		if(last_eid == eid) {
				strcat(stag_buf, buf);
				strcat(stag_buf, "\n");
		} else {
				if(strncmp(stag_buf, "type=SYSCALL",12) == 0) {
						syscall_handler(stag_buf);
				}
				strcpy(stag_buf, buf);
				strcat(stag_buf, "\n");
				last_eid = eid;
		}
		return strlen(buf);
}

// KYU Modification end

int dispatch_network_events(void)
{
		return config->distribute_network_events;
}

void shutdown_events(void)
{
		/* Give it 5 seconds to clear the queue */
		alarm(5);

		// Nudge the flush thread
		pthread_cond_signal(&do_flush);
		pthread_join(flush_thread, NULL);

		free((void *)format_buf);
		fclose(log_file);
}

int init_event(struct daemon_conf *conf)
{
		/* Store the netlink descriptor and config info away */
		config = conf;
		log_fd = -1;

		/* Now open the log */
		if (config->daemonize == D_BACKGROUND) {
				fix_disk_permissions();
				if (open_audit_log())
						return 1;
		} else {
				log_fd = 1; // stdout
				log_file = fdopen(log_fd, "a");
				if (log_file == NULL) {
						audit_msg(LOG_ERR, 
										"Error setting up stdout descriptor (%s)", 
										strerror(errno));
						return 1;
				}
				/* Set it to line buffering */
				setlinebuf(log_file);
		}

		if (config->daemonize == D_BACKGROUND) {
				check_log_file_size();
				check_excess_logs();
				check_space_left();
		}
		format_buf = (char *)malloc(FORMAT_BUF_LEN);
		if (format_buf == NULL) {
				audit_msg(LOG_ERR, "No memory for formatting, exiting");
				fclose(log_file);
				return 1;
		}
		init_flush_thread();
		return 0;
}

/* This tells the OS that pending writes need to get going.
	* Its only used when flush == incremental_async. */
static void *flush_thread_main(void *arg)
{
		sigset_t sigs;

		/* This is a worker thread. Don't handle signals. */
		sigemptyset(&sigs);
		sigaddset(&sigs, SIGALRM);
		sigaddset(&sigs, SIGTERM);
		sigaddset(&sigs, SIGHUP);
		sigaddset(&sigs, SIGUSR1);
		sigaddset(&sigs, SIGUSR2);
		pthread_sigmask(SIG_SETMASK, &sigs, NULL);

		while (!stop) {
				pthread_mutex_lock(&flush_lock);

				// In the event that the logging thread requests another
				// flush before the first completes, this simply turns
				// into a loop of fsyncs.
				while (flush == 0) {
						pthread_cond_wait(&do_flush, &flush_lock);
						if (stop)
								return NULL;
				}
				flush = 0;
				pthread_mutex_unlock(&flush_lock);

				fsync(log_fd);
		}
		return NULL;
}

/* We setup the flush thread no matter what. This is incase a reconfig
	* changes from non incremental to incremental or vise versa. */
static void init_flush_thread(void)
{
		pthread_mutex_init(&flush_lock, NULL);
		pthread_cond_init(&do_flush, NULL);
		flush = 0;
		pthread_create(&flush_thread, NULL, flush_thread_main, NULL);
}

static void replace_event_msg(struct auditd_event *e, const char *buf)
{
		if (buf) {
				size_t len = strlen(buf);

				if (len < MAX_AUDIT_MESSAGE_LENGTH - 1)
						e->reply.message = strdup(buf);
				else {
						// If too big, we must truncate the event due to API
						e->reply.message = strndup(buf, MAX_AUDIT_MESSAGE_LENGTH-1);
						len = MAX_AUDIT_MESSAGE_LENGTH;
				}
				e->reply.msg.nlh.nlmsg_len = e->reply.len;
				e->reply.len = len;
		}
}

/*
	* This function will take an audit structure and return a
	* text buffer that's formatted for writing to disk. If there
	* is an error the return value is NULL.
	*/
static const char *format_raw(const struct audit_reply *rep)
{
		char *ptr;

		if (rep == NULL) {
				if (config->node_name_format != N_NONE)
						snprintf(format_buf, FORMAT_BUF_LEN - 32,
										"node=%s type=DAEMON_ERR op=format-raw msg=NULL res=failed",
										config->node_name);
				else
						snprintf(format_buf, MAX_AUDIT_MESSAGE_LENGTH,
										"type=DAEMON_ERR op=format-raw msg=NULL res=failed");
		} else {
				int len, nlen;
				const char *type, *message;
				char unknown[32];
				type = audit_msg_type_to_name(rep->type);
				if (type == NULL) {
						snprintf(unknown, sizeof(unknown), 
										"UNKNOWN[%d]", rep->type);
						type = unknown;
				}
				if (rep->message == NULL) {
						message = "lost";
						len = 4;
				} else {
						message = rep->message;
						len = rep->len;
				}

				// Note: This can truncate messages if 
				// MAX_AUDIT_MESSAGE_LENGTH is too small
				if (config->node_name_format != N_NONE)
						nlen = snprintf(format_buf, FORMAT_BUF_LEN - 32,
										"node=%s type=%s msg=%.*s\n",
										config->node_name, type, len, message);
				else
						nlen = snprintf(format_buf,
										MAX_AUDIT_MESSAGE_LENGTH - 32,
										"type=%s msg=%.*s", type, len, message);

				/* Replace \n with space so it looks nicer. */
				ptr = format_buf;
				while ((ptr = strchr(ptr, 0x0A)) != NULL)
						*ptr = ' ';

				/* Trim trailing space off since it wastes space */
				if (format_buf[nlen-1] == ' ')
						format_buf[nlen-1] = 0;
		}
		return format_buf;
}

static int sep_done = 0;
static int add_separator(unsigned int len_left)
{
		if (sep_done == 0) {
				format_buf[FORMAT_BUF_LEN - len_left] = AUDIT_INTERP_SEPARATOR;
				sep_done++;
				return 1;
		}
		sep_done++;
		return 0;
}

// returns length used, 0 on error
#define NAME_SIZE 64
static int add_simple_field(auparse_state_t *au, size_t len_left, int encode)
{
		const char *value, *nptr;
		char *enc = NULL;
		char *ptr, field_name[NAME_SIZE];
		size_t nlen, vlen, tlen;
		unsigned int i;
		int num;

		// prepare field name
		i = 0;
		nptr = auparse_get_field_name(au);
		while (*nptr && i < (NAME_SIZE - 1)) {
				field_name[i] = toupper(*nptr);
				i++;
				nptr++;
		}
		field_name[i] = 0;
		nlen = i;

		// get the translated value
		value = auparse_interpret_field(au);
		if (value == NULL)
				value = "?";
		vlen = strlen(value);

		if (encode) {
				enc = audit_encode_nv_string(field_name, value, vlen);
				if (enc == NULL)
						return 0;
				tlen = 1 + strlen(enc) + 1;
		} else
				// calculate length to use
				tlen = 1 + nlen + 1 + vlen + 1;

		// If no room, do not truncate - just do nothing
		if (tlen >= len_left) {
				free(enc);
				return 0;
		}

		// Setup pointer
		ptr = &format_buf[FORMAT_BUF_LEN - len_left];
		if (sep_done > 1) {
				*ptr = ' ';
				ptr++;
				num = 1;
		} else
				num = 0;

		// Add the field
		if (encode) {
				num += snprintf(ptr, tlen, "%s", enc);
				free(enc);
		} else
				num += snprintf(ptr, tlen, "%s=%s", field_name, value);

		return num;
}

/*
	* This function will take an audit structure and return a
	* text buffer that's formatted and enriched. If there is an
	* error the return value is NULL.
	*/
static const char *format_enrich(const struct audit_reply *rep)
{
		if (rep == NULL) {
				if (config->node_name_format != N_NONE)
						snprintf(format_buf, FORMAT_BUF_LEN - 32,
										"node=%s type=DAEMON_ERR op=format-enriched msg=NULL res=failed",
										config->node_name);
				else
						snprintf(format_buf, MAX_AUDIT_MESSAGE_LENGTH,
										"type=DAEMON_ERR op=format-enriched msg=NULL res=failed");
		} else {
				int rc;
				size_t mlen, len;
				auparse_state_t *au;
				char *message;
				// Do raw format to get event started
				format_raw(rep);

				// How much room is left?
				mlen = strlen(format_buf);
				len = FORMAT_BUF_LEN - mlen;
				if (len <= MIN_SPACE_LEFT)
						return format_buf;

				// create copy to parse up
				format_buf[mlen] = 0x0A;
				format_buf[mlen+1] = 0;
				message = strdup(format_buf);
				format_buf[mlen] = 0;

				// init auparse
				au = auparse_init(AUSOURCE_BUFFER, message);
				if (au == NULL) {
						free(message);
						return format_buf;
				}
				auparse_set_escape_mode(NULL, AUPARSE_ESC_RAW);
				sep_done = 0;

				// Loop over all fields while possible to add field
				rc = auparse_first_record(au);
				while (rc > 0 && len > MIN_SPACE_LEFT) {
						// See what kind of field we have
						size_t vlen;
						int type = auparse_get_field_type(au);
						switch (type)
						{
								case AUPARSE_TYPE_UID:
								case AUPARSE_TYPE_GID:
										if (add_separator(len))
												len--;
										vlen = add_simple_field(au, len, 1);
										len -= vlen;
										break;
								case AUPARSE_TYPE_SYSCALL:
								case AUPARSE_TYPE_ARCH:
								case AUPARSE_TYPE_SOCKADDR:
										if (add_separator(len))
												len--;
										vlen = add_simple_field(au, len, 0);
										len -= vlen;
										break;
								default:
										break;
						}
						rc = auparse_next_field(au);
				}

				auparse_destroy(au);
				free(message);
		}
		return format_buf;
}

void format_event(struct auditd_event *e)
{
		const char *buf;

		switch (config->log_format)
		{
				case LF_RAW:
						buf = format_raw(&e->reply);
						break;
				case LF_ENRICHED:
						buf = format_enrich(&e->reply);
						break;
				default:
						buf = NULL;
						break;
		}

		replace_event_msg(e, buf);
}

/* This function free's all memory associated with events */
void cleanup_event(struct auditd_event *e)
{
		free((void *)e->reply.message);
		free(e);
}

/* This function takes a  reconfig event and sends it to the handler */
void enqueue_event(struct auditd_event *e)
{
		e->ack_func = NULL;
		e->ack_data = NULL;
		e->sequence_id = 0;

		handle_event(e);
		cleanup_event(e);
}

/* This function allocates memory and fills the event fields with
			passed arguements. Caller must free memory. */
struct auditd_event *create_event(char *msg, ack_func_type ack_func,
				void *ack_data, uint32_t sequence_id)
{
		struct auditd_event *e;

		e = (struct auditd_event *)calloc(1, sizeof (*e));
		if (e == NULL) {
				audit_msg(LOG_ERR, "Cannot allocate audit reply");
				return NULL;
		}

		e->ack_func = ack_func;
		e->ack_data = ack_data;
		e->sequence_id = sequence_id;

		/* Network originating events need things adjusted to mimic netlink. */
		if (e->ack_func)
				replace_event_msg(e, msg);

		return e;
}

/* This function takes the event and handles it. */
static unsigned int count = 0L;
void handle_event(struct auditd_event *e)
{
		if (e->reply.type == AUDIT_DAEMON_RECONFIG && e->ack_func == NULL) {
				reconfigure(e);
				if (config->write_logs == 0)
						return;
				format_event(e);
		} else if (e->reply.type == AUDIT_DAEMON_ROTATE) {
				rotate_logs_now();
				if (config->write_logs == 0)
						return;
		}
		if (!logging_suspended) {
				write_to_log(e);

				/* See if we need to flush to disk manually */
				if (config->flush == FT_INCREMENTAL ||
								config->flush == FT_INCREMENTAL_ASYNC) {
						count++;
						if ((count % config->freq) == 0) {
								int rc;
								errno = 0;
								do {
										rc = fflush_unlocked(log_file);
								} while (rc < 0 && errno == EINTR);
								if (errno) {
										if (errno == ENOSPC && 
														fs_space_left == 1) {
												fs_space_left = 0;
												do_disk_full_action();
										} else
												//EIO is only likely failure mode
												do_disk_error_action("flush", 
																errno);
								}

								if (config->daemonize == D_BACKGROUND) {
										if (config->flush == FT_INCREMENTAL) {
												/* EIO is only likely failure */
												if (fsync(log_fd) != 0) {
														do_disk_error_action(
																		"fsync",
																		errno);
												}
										} else {
												pthread_mutex_lock(&flush_lock);
												flush = 1;
												pthread_cond_signal(&do_flush);
												pthread_mutex_unlock(&flush_lock);
										}
								}
						}
				}
		}
}

static void send_ack(const struct auditd_event *e, int ack_type,
				const char *msg)
{
		if (e->ack_func) {
				unsigned char header[AUDIT_RMW_HEADER_SIZE];

				AUDIT_RMW_PACK_HEADER(header, 0, ack_type, strlen(msg),
								e->sequence_id);

				e->ack_func(e->ack_data, header, msg);
		}
}

void resume_logging(void)
{
		logging_suspended = 0; 
		fs_space_left = 1;
		disk_err_warning = 0;
		fs_space_warning = 0;
		fs_admin_space_warning = 0;
		audit_msg(LOG_ERR, "Audit daemon is attempting to resume logging.");
}

/* This function writes the given buf to the current log file */
static void write_to_log(const struct auditd_event *e)
{
		int rc;
		int ack_type = AUDIT_RMW_TYPE_ACK;
		const char *msg = "";

		/* write it to disk */
		//rc = fprintf(log_file, "%s\n", e->reply.message);
		rc = buffering(e->reply.message); // KYU MODIFIED

		/* error? Handle it */
		if (rc < 0) {
				if (errno == ENOSPC) {
						ack_type = AUDIT_RMW_TYPE_DISKFULL;
						msg = "disk full";
						send_ack(e, ack_type, msg);
						if (fs_space_left == 1) {
								fs_space_left = 0;
								do_disk_full_action();
						}
				} else  {
						int saved_errno = errno;
						ack_type = AUDIT_RMW_TYPE_DISKERROR;
						msg = "disk write error";
						send_ack(e, ack_type, msg);
						do_disk_error_action("write", saved_errno);
				}
		} else {
				/* check log file size & space left on partition */
				if (config->daemonize == D_BACKGROUND) {
						// If either of these fail, I consider it an
						// inconvenience as opposed to something that is
						// actionable. There may be some temporary condition
						// that the system recovers from. The real error
						// occurs on write.
						log_size += rc;
						check_log_file_size();
						// Keep loose tabs on the free space
						if ((log_size % 3) < 2)
								check_space_left();
				}

				if (fs_space_warning)
						ack_type = AUDIT_RMW_TYPE_DISKLOW;
				send_ack(e, ack_type, msg);
				disk_err_warning = 0;
		}
}

static void check_log_file_size(void)
{
		/* did we cross the size limit? */
		off_t sz = log_size / MEGABYTE;

		if (sz >= config->max_log_size && (config->daemonize == D_BACKGROUND)) {
				switch (config->max_log_size_action)
				{
						case SZ_IGNORE:
								break;
						case SZ_SYSLOG:
								audit_msg(LOG_ERR,
												"Audit daemon log file is larger than max size");
								break;
						case SZ_SUSPEND:
								audit_msg(LOG_ERR,
												"Audit daemon is suspending logging due to logfile size.");
								logging_suspended = 1;
								break;
						case SZ_ROTATE:
								if (config->num_logs > 1) {
										audit_msg(LOG_NOTICE,
														"Audit daemon rotating log files");
										rotate_logs(0);
								}
								break;
						case SZ_KEEP_LOGS:
								audit_msg(LOG_NOTICE,
												"Audit daemon rotating log files with keep option");
								shift_logs();
								break;
						default:
								audit_msg(LOG_ALERT, 
												"Audit daemon log file is larger than max size and unknown action requested");
								break;
				}
		}
}

static void check_space_left(void)
{
		int rc;
		struct statfs buf;

		rc = fstatfs(log_fd, &buf);
		if (rc == 0) {
				if (buf.f_bavail < 5) {
						/* we won't consume the last 5 blocks */
						fs_space_left = 0;
						do_disk_full_action();
				} else {
						unsigned long blocks;
						unsigned long block_size = buf.f_bsize;
						blocks = config->space_left * (MEGABYTE/block_size);
						if (buf.f_bavail < blocks) {
								if (fs_space_warning == 0) {
										do_space_left_action(0);
										fs_space_warning = 1;
								}
						} else if (fs_space_warning &&
										config->space_left_action == FA_SYSLOG){
								// Auto reset only if failure action is syslog
								fs_space_warning = 0;
						}
						blocks=config->admin_space_left * (MEGABYTE/block_size);
						if (buf.f_bavail < blocks) {
								if (fs_admin_space_warning == 0) {
										do_space_left_action(1);
										fs_admin_space_warning = 1;
								}
						} else if (fs_admin_space_warning &&
										config->admin_space_left_action == FA_SYSLOG) {
								// Auto reset only if failure action is syslog
								fs_admin_space_warning = 0;
						}
				}
		}
		else audit_msg(LOG_DEBUG, "fstatfs returned:%d, %s", rc, 
						strerror(errno));
}

extern int sendmail(const char *subject, const char *content, 
				const char *mail_acct);
static void do_space_left_action(int admin)
{
		int action;

		if (admin)
				action = config->admin_space_left_action;
		else
				action = config->space_left_action;

		switch (action)
		{
				case FA_IGNORE:
						break;
				case FA_SYSLOG:
						audit_msg(LOG_ALERT, 
										"Audit daemon is low on disk space for logging");
						break;
				case FA_ROTATE:
						if (config->num_logs > 1) {
								audit_msg(LOG_NOTICE,
												"Audit daemon rotating log files");
								rotate_logs(0);
						}
						break;
				case FA_EMAIL:
						if (admin == 0) {
								sendmail("Audit Disk Space Alert", 
												"The audit daemon is low on disk space for logging! Please take action\nto ensure no loss of service.",
												config->action_mail_acct);
								audit_msg(LOG_ALERT, 
												"Audit daemon is low on disk space for logging");
						} else {
								sendmail("Audit Admin Space Alert", 
												"The audit daemon is very low on disk space for logging! Immediate action\nis required to ensure no loss of service.",
												config->action_mail_acct);
								audit_msg(LOG_ALERT, 
												"Audit daemon is very low on disk space for logging");
						}
						break;
				case FA_EXEC:
						if (admin)
								safe_exec(config->admin_space_left_exe);
						else
								safe_exec(config->space_left_exe);
						break;
				case FA_SUSPEND:
						audit_msg(LOG_ALERT,
										"Audit daemon is suspending logging due to low disk space.");
						logging_suspended = 1;
						break;
				case FA_SINGLE:
						audit_msg(LOG_ALERT, 
										"The audit daemon is now changing the system to single user mode");
						change_runlevel(SINGLE);
						break;
				case FA_HALT:
						audit_msg(LOG_ALERT, 
										"The audit daemon is now halting the system");
						change_runlevel(HALT);
						break;
				default:
						audit_msg(LOG_ALERT, 
										"Audit daemon is low on disk space for logging and unknown action requested");
						break;
		}
}

static void do_disk_full_action(void)
{
		audit_msg(LOG_ALERT,
						"Audit daemon has no space left on logging partition");
		switch (config->disk_full_action)
		{
				case FA_IGNORE:
				case FA_SYSLOG: /* Message is syslogged above */
						break;
				case FA_ROTATE:
						if (config->num_logs > 1) {
								audit_msg(LOG_NOTICE,
												"Audit daemon rotating log files");
								rotate_logs(0);
						}
						break;
				case FA_EXEC:
						safe_exec(config->disk_full_exe);
						break;
				case FA_SUSPEND:
						audit_msg(LOG_ALERT,
										"Audit daemon is suspending logging due to no space left on logging partition.");
						logging_suspended = 1;
						break;
				case FA_SINGLE:
						audit_msg(LOG_ALERT, 
										"The audit daemon is now changing the system to single user mode due to no space left on logging partition");
						change_runlevel(SINGLE);
						break;
				case FA_HALT:
						audit_msg(LOG_ALERT, 
										"The audit daemon is now halting the system due to no space left on logging partition");
						change_runlevel(HALT);
						break;
				default:
						audit_msg(LOG_ALERT, "Unknown disk full action requested");
						break;
		} 
}

static void do_disk_error_action(const char *func, int err)
{
		char text[128];

		switch (config->disk_error_action)
		{
				case FA_IGNORE:
						break;
				case FA_SYSLOG:
						if (disk_err_warning < 5) {
								snprintf(text, sizeof(text), 
												"%s: Audit daemon detected an error writing an event to disk (%s)",
												func, strerror(err));
								audit_msg(LOG_ALERT, "%s", text);
								disk_err_warning++;
						}
						break;
				case FA_EXEC:
						safe_exec(config->disk_error_exe);
						break;
				case FA_SUSPEND:
						audit_msg(LOG_ALERT,
										"Audit daemon is suspending logging due to previously mentioned write error");
						logging_suspended = 1;
						break;
				case FA_SINGLE:
						audit_msg(LOG_ALERT, 
										"The audit daemon is now changing the system to single user mode due to previously mentioned write error");
						change_runlevel(SINGLE);
						break;
				case FA_HALT:
						audit_msg(LOG_ALERT, 
										"The audit daemon is now halting the system due to previously mentioned write error.");
						change_runlevel(HALT);
						break;
				default:
						audit_msg(LOG_ALERT, 
										"Unknown disk error action requested");
						break;
		} 
}

static void rotate_logs_now(void)
{
		if (config->max_log_size_action == SZ_KEEP_LOGS) 
				shift_logs();
		else
				rotate_logs(0);
}

/* Check for and remove excess logs so that we don't run out of room */
static void check_excess_logs(void)
{
		int rc;
		unsigned int i, len;
		char *name;

		// Only do this if rotate is the log size action
		// and we actually have a limit
		if (config->max_log_size_action != SZ_ROTATE ||
						config->num_logs < 2)
				return;

		len = strlen(config->log_file) + 16;
		name = (char *)malloc(len);
		if (name == NULL) { /* Not fatal - just messy */
				audit_msg(LOG_ERR, "No memory checking excess logs");
				return;
		}

		// We want 1 beyond the normal logs	
		i = config->num_logs;
		rc = 0;
		while (rc == 0) {
				snprintf(name, len, "%s.%d", config->log_file, i++);
				rc=unlink(name);
				if (rc == 0)
						audit_msg(LOG_NOTICE,
										"Log %s removed as it exceeds num_logs parameter",
										name);
		}
		free(name);
}

static void fix_disk_permissions(void)
{
		char *path, *dir;
		unsigned int i, len;

		if (config == NULL || config->log_file == NULL)
				return;

		len = strlen(config->log_file) + 16;

		path = malloc(len);
		if (path == NULL)
				return;

		// Start with the directory
		strcpy(path, config->log_file);
		dir = dirname(path);
		chmod(dir, config->log_group ? S_IRWXU|S_IRGRP|S_IXGRP : S_IRWXU);
		chown(dir, 0, config->log_group ? config->log_group : 0);

		// Now, for each file...
		for (i = 1; i < config->num_logs; i++) {
				int rc;
				snprintf(path, len, "%s.%d", config->log_file, i);
				rc = chmod(path, config->log_group ? S_IRUSR|S_IRGRP : S_IRUSR);
				if (rc && errno == ENOENT)
						break;
		}

		// Now the current file
		chmod(config->log_file, config->log_group ? S_IWUSR|S_IRUSR|S_IRGRP :
						S_IWUSR|S_IRUSR);

		free(path);
}

static void rotate_logs(unsigned int num_logs)
{
		int rc;
		unsigned int len, i;
		char *oldname, *newname;

		if (config->max_log_size_action == SZ_ROTATE &&
						config->num_logs < 2)
				return;

		/* Close audit file. fchmod and fchown errors are not fatal because we
			* already adjusted log file permissions and ownership when opening the
			* log file. */
		if (fchmod(log_fd, config->log_group ? S_IRUSR|S_IRGRP : S_IRUSR) < 0){
				audit_msg(LOG_NOTICE, "Couldn't change permissions while "
								"rotating log file (%s)", strerror(errno));
		}
		if (fchown(log_fd, 0, config->log_group) < 0) {
				audit_msg(LOG_NOTICE, "Couldn't change ownership while "
								"rotating log file (%s)", strerror(errno));
		}
		fclose(log_file);

		/* Rotate */
		len = strlen(config->log_file) + 16;
		oldname = (char *)malloc(len);
		if (oldname == NULL) { /* Not fatal - just messy */
				audit_msg(LOG_ERR, "No memory rotating logs");
				logging_suspended = 1;
				return;
		}
		newname = (char *)malloc(len);
		if (newname == NULL) { /* Not fatal - just messy */
				audit_msg(LOG_ERR, "No memory rotating logs");
				free(oldname);
				logging_suspended = 1;
				return;
		}

		/* If we are rotating, get number from config */
		if (num_logs == 0)
				num_logs = config->num_logs;

		/* Handle this case first since it will not enter the for loop */
		if (num_logs == 2) 
				snprintf(oldname, len, "%s.1", config->log_file);

		for (i=num_logs - 1; i>1; i--) {
				snprintf(oldname, len, "%s.%d", config->log_file, i-1);
				snprintf(newname, len, "%s.%d", config->log_file, i);
				/* if the old file exists */
				rc = rename(oldname, newname);
				if (rc == -1 && errno != ENOENT) {
						// Likely errors: ENOSPC, ENOMEM, EBUSY
						int saved_errno = errno;
						audit_msg(LOG_ERR, 
										"Error rotating logs from %s to %s (%s)",
										oldname, newname, strerror(errno));
						if (saved_errno == ENOSPC && fs_space_left == 1) {
								fs_space_left = 0;
								do_disk_full_action();
						} else
								do_disk_error_action("rotate", saved_errno);
				}
		}
		free(newname);

		/* At this point, oldname should point to lowest number - use it */
		newname = oldname;
		rc = rename(config->log_file, newname);
		if (rc == -1 && errno != ENOENT) {
				// Likely errors: ENOSPC, ENOMEM, EBUSY
				int saved_errno = errno;
				audit_msg(LOG_ERR, "Error rotating logs from %s to %s (%s)",
								config->log_file, newname, strerror(errno));
				if (saved_errno == ENOSPC && fs_space_left == 1) {
						fs_space_left = 0;
						do_disk_full_action();
				} else
						do_disk_error_action("rotate2", saved_errno);

				/* At this point, we've failed to rotate the original log.
					* So, let's make the old log writable and try again next
					* time */
				chmod(config->log_file, 
								config->log_group ? S_IWUSR|S_IRUSR|S_IRGRP :
								S_IWUSR|S_IRUSR);
		}
		free(newname);

		/* open new audit file */
		if (open_audit_log()) {
				int saved_errno = errno;
				audit_msg(LOG_NOTICE, 
								"Could not reopen a log after rotating.");
				logging_suspended = 1;
				do_disk_error_action("reopen", saved_errno);
		}
}

static int last_log = 1;
static void shift_logs(void)
{
		// The way this has to work is to start scanning from .1 up until
		// no file is found. Then do the rotate algorithm using that number
		// instead of log_max.
		unsigned int num_logs, len;
		char *name;

		len = strlen(config->log_file) + 16;
		name = (char *)malloc(len);
		if (name == NULL) { /* Not fatal - just messy */
				audit_msg(LOG_ERR, "No memory shifting logs");
				return;
		}

		// Find last log
		num_logs = last_log;
		while (num_logs) {
				snprintf(name, len, "%s.%d", config->log_file, 
								num_logs);
				if (access(name, R_OK) != 0)
						break;
				num_logs++;
		}

		/* Our last known file disappeared, start over... */
		if (num_logs <= last_log && last_log > 1) {
				audit_msg(LOG_WARNING, "Last known log disappeared (%s)", name);
				num_logs = last_log = 1;
				while (num_logs) {
						snprintf(name, len, "%s.%d", config->log_file, 
										num_logs);
						if (access(name, R_OK) != 0)
								break;
						num_logs++;
				}
				audit_msg(LOG_INFO, "Next log to use will be %s", name);
		}
		last_log = num_logs;
		rotate_logs(num_logs+1);
		free(name);
}

/*
	* This function handles opening a descriptor for the audit log
	* file and ensuring the correct options are applied to the descriptor.
	* It returns 0 on success and 1 on failure.
	*/
static int open_audit_log(void)
{
		int flags, lfd;

		flags = O_WRONLY|O_APPEND|O_NOFOLLOW;
		if (config->flush == FT_DATA)
				flags |= O_DSYNC;
		else if (config->flush == FT_SYNC)
				flags |= O_SYNC;

		// Likely errors for open: Almost anything
		// Likely errors on rotate: ENFILE, ENOMEM, ENOSPC
retry:
		lfd = open(config->log_file, flags);
		if (lfd < 0) {
				if (errno == ENOENT) {
						lfd = create_log_file(config->log_file);
						if (lfd < 0) {
								audit_msg(LOG_ERR,
												"Couldn't create log file %s (%s)",
												config->log_file,
												strerror(errno));
								return 1;
						}
						close(lfd);
						lfd = open(config->log_file, flags);
						log_size = 0;
				} else if (errno == ENFILE) {
						// All system descriptors used, try again...
						goto retry;
				}
				if (lfd < 0) {
						audit_msg(LOG_ERR, "Couldn't open log file %s (%s)",
										config->log_file, strerror(errno));
						return 1;
				}
		} else {
				// Get initial size
				struct stat st;

				int rc = fstat(lfd, &st);
				if (rc == 0)
						log_size = st.st_size;
				else {
						close(lfd);
						return 1;
				}
		}

		if (fcntl(lfd, F_SETFD, FD_CLOEXEC) == -1) {
				audit_msg(LOG_ERR, "Error setting log file CLOEXEC flag (%s)",
								strerror(errno));
				close(lfd);
				return 1;
		}
		if (fchmod(lfd, config->log_group ? S_IRUSR|S_IWUSR|S_IRGRP :
								S_IRUSR|S_IWUSR) < 0) {
				audit_msg(LOG_ERR,
								"Couldn't change permissions of log file (%s)",
								strerror(errno));
				close(lfd);
				return 1;
		}
		if (fchown(lfd, 0, config->log_group) < 0) {
				audit_msg(LOG_ERR, "Couldn't change ownership of log file (%s)",
								strerror(errno));
				close(lfd);
				return 1;
		}

		log_fd = lfd;
		log_file = fdopen(lfd, "a");
		if (log_file == NULL) {
				audit_msg(LOG_ERR, "Error setting up log descriptor (%s)",
								strerror(errno));
				close(lfd);
				return 1;
		}

		/* Set it to line buffering */
		setlinebuf(log_file);
		return 0;
}

static void change_runlevel(const char *level)
{
		char *argv[3];
		int pid;
		struct sigaction sa;
		static const char *init_pgm = "/sbin/init";

		pid = fork();
		if (pid < 0) {
				audit_msg(LOG_ALERT, 
								"Audit daemon failed to fork switching runlevels");
				return;
		}
		if (pid)	/* Parent */
				return;
		/* Child */
		sigfillset (&sa.sa_mask);
		sigprocmask (SIG_UNBLOCK, &sa.sa_mask, 0);

		argv[0] = (char *)init_pgm;
		argv[1] = (char *)level;
		argv[2] = NULL;
		execve(init_pgm, argv, NULL);
		audit_msg(LOG_ALERT, "Audit daemon failed to exec %s", init_pgm);
		exit(1);
}

static void safe_exec(const char *exe)
{
		char *argv[2];
		int pid;
		struct sigaction sa;

		if (exe == NULL) {
				audit_msg(LOG_ALERT,
								"Safe_exec passed NULL for program to execute");
				return;
		}

		pid = fork();
		if (pid < 0) {
				audit_msg(LOG_ALERT, 
								"Audit daemon failed to fork doing safe_exec");
				return;
		}
		if (pid)	/* Parent */
				return;
		/* Child */
		sigfillset (&sa.sa_mask);
		sigprocmask (SIG_UNBLOCK, &sa.sa_mask, 0);

		argv[0] = (char *)exe;
		argv[1] = NULL;
		execve(exe, argv, NULL);
		audit_msg(LOG_ALERT, "Audit daemon failed to exec %s", exe);
		exit(1);
}

static void reconfigure(struct auditd_event *e)
{
		struct daemon_conf *nconf = e->reply.conf;
		struct daemon_conf *oconf = config;
		uid_t uid = nconf->sender_uid;
		pid_t pid = nconf->sender_pid;
		const char *ctx = nconf->sender_ctx;
		struct timeval tv;
		char txt[MAX_AUDIT_MESSAGE_LENGTH];
		char date[40];
		unsigned int seq_num;
		int need_size_check = 0, need_reopen = 0, need_space_check = 0;

		snprintf(txt, sizeof(txt),
						"config change requested by pid=%d auid=%u subj=%s",
						pid, uid, ctx);
		audit_msg(LOG_NOTICE, "%s", txt);

		/* Do the reconfiguring. These are done in a specific
			* order from least invasive to most invasive. We will
			* start with general system parameters. */

		// start with disk error action.
		oconf->disk_error_action = nconf->disk_error_action;
		free((char *)oconf->disk_error_exe);
		oconf->disk_error_exe = nconf->disk_error_exe;
		disk_err_warning = 0;

		// number of logs
		oconf->num_logs = nconf->num_logs;

		// flush freq
		oconf->freq = nconf->freq;

		// priority boost
		if (oconf->priority_boost != nconf->priority_boost) {
				int rc;

				oconf->priority_boost = nconf->priority_boost;
				errno = 0;
				rc = nice(-oconf->priority_boost);
				if (rc == -1 && errno) 
						audit_msg(LOG_NOTICE, "Cannot change priority in "
										"reconfigure (%s)", strerror(errno));
		}

		// log format
		oconf->log_format = nconf->log_format;

		if (oconf->write_logs != nconf->write_logs) {
				oconf->write_logs = nconf->write_logs;
				need_reopen = 1;
		}

		// log_group
		if (oconf->log_group != nconf->log_group) {
				oconf->log_group = nconf->log_group;
				need_reopen = 1;
		}

		// action_mail_acct
		if (strcmp(oconf->action_mail_acct, nconf->action_mail_acct)) {
				free((void *)oconf->action_mail_acct);
				oconf->action_mail_acct = nconf->action_mail_acct;
		} else
				free((void *)nconf->action_mail_acct);

		// node_name
		if (oconf->node_name_format != nconf->node_name_format || 
						(oconf->node_name && nconf->node_name && 
							strcmp(oconf->node_name, nconf->node_name) != 0)) {
				oconf->node_name_format = nconf->node_name_format;
				free((char *)oconf->node_name);
				oconf->node_name = nconf->node_name;
		}

		/* Now look at audit dispatcher changes */
		oconf->qos = nconf->qos; // dispatcher qos

		// do the dispatcher app change
		if (oconf->dispatcher || nconf->dispatcher) {
				// none before, start new one
				if (oconf->dispatcher == NULL) {
						oconf->dispatcher = strdup(nconf->dispatcher);
						if (oconf->dispatcher == NULL) {
								int saved_errno = errno;
								audit_msg(LOG_NOTICE,
												"Could not allocate dispatcher memory"
												" in reconfigure");
								// Likely errors: ENOMEM
								do_disk_error_action("reconfig", saved_errno);
						}
						if(init_dispatcher(oconf)) {// dispatcher & qos is used
								int saved_errno = errno;
								audit_msg(LOG_NOTICE,
												"Could not start dispatcher %s"
												" in reconfigure", oconf->dispatcher);
								// Likely errors: Socketpairs or exec perms
								do_disk_error_action("reconfig", saved_errno);
						}
				} 
				// have one, but none after this
				else if (nconf->dispatcher == NULL) {
						shutdown_dispatcher();
						free((char *)oconf->dispatcher);
						oconf->dispatcher = NULL;
				} 
				// they are different apps
				else if (strcmp(oconf->dispatcher, nconf->dispatcher)) {
						shutdown_dispatcher();
						free((char *)oconf->dispatcher);
						oconf->dispatcher = strdup(nconf->dispatcher);
						if (oconf->dispatcher == NULL) {
								int saved_errno = errno;
								audit_msg(LOG_NOTICE,
												"Could not allocate dispatcher memory"
												" in reconfigure");
								// Likely errors: ENOMEM
								do_disk_error_action("reconfig", saved_errno);
						}
						if(init_dispatcher(oconf)) {// dispatcher & qos is used
								int saved_errno = errno;
								audit_msg(LOG_NOTICE,
												"Could not start dispatcher %s"
												" in reconfigure", oconf->dispatcher);
								// Likely errors: Socketpairs or exec perms
								do_disk_error_action("reconfig", saved_errno);
						}
				}
				// they are the same app - just signal it
				else {
						reconfigure_dispatcher(oconf);
						free((char *)nconf->dispatcher);
						nconf->dispatcher = NULL;
				}
		}

		// network listener
		auditd_tcp_listen_reconfigure(nconf, oconf);

		// distribute network events	
		oconf->distribute_network_events = nconf->distribute_network_events;

		/* At this point we will work on the items that are related to 
			* a single log file. */

		// max logfile action
		if (oconf->max_log_size_action != nconf->max_log_size_action) {
				oconf->max_log_size_action = nconf->max_log_size_action;
				need_size_check = 1;
		}

		// max log size
		if (oconf->max_log_size != nconf->max_log_size) {
				oconf->max_log_size = nconf->max_log_size;
				need_size_check = 1;
		}

		if (need_size_check) {
				logging_suspended = 0;
				check_log_file_size();
		}

		// flush technique
		if (oconf->flush != nconf->flush) {
				oconf->flush = nconf->flush;
				need_reopen = 1;
		}

		// logfile
		if (strcmp(oconf->log_file, nconf->log_file)) {
				free((void *)oconf->log_file);
				oconf->log_file = nconf->log_file;
				need_reopen = 1;
				need_space_check = 1; // might be on new partition
		} else
				free((void *)nconf->log_file);

		if (need_reopen) {
				fclose(log_file);
				fix_disk_permissions();
				if (open_audit_log()) {
						int saved_errno = errno;
						audit_msg(LOG_NOTICE, 
										"Could not reopen a log after reconfigure");
						logging_suspended = 1;
						// Likely errors: ENOMEM, ENOSPC
						do_disk_error_action("reconfig", saved_errno);
				} else {
						logging_suspended = 0;
						check_log_file_size();
				}
		}

		/* At this point we will start working on items that are 
			* related to the amount of space on the partition. */

		// space left
		if (oconf->space_left != nconf->space_left) {
				oconf->space_left = nconf->space_left;
				need_space_check = 1;
		}

		// space left action
		if (oconf->space_left_action != nconf->space_left_action) {
				oconf->space_left_action = nconf->space_left_action;
				need_space_check = 1;
		}

		// space left exe
		if (oconf->space_left_exe || nconf->space_left_exe) {
				if (nconf->space_left_exe == NULL)
						; /* do nothing if new one is blank */
				else if (oconf->space_left_exe == NULL && nconf->space_left_exe)
						need_space_check = 1;
				else if (strcmp(oconf->space_left_exe, nconf->space_left_exe))
						need_space_check = 1;
				free((char *)oconf->space_left_exe);
				oconf->space_left_exe = nconf->space_left_exe;
		}

		// admin space left
		if (oconf->admin_space_left != nconf->admin_space_left) {
				oconf->admin_space_left = nconf->admin_space_left;
				need_space_check = 1;
		}

		// admin space action
		if (oconf->admin_space_left_action != nconf->admin_space_left_action) {
				oconf->admin_space_left_action = nconf->admin_space_left_action;
				need_space_check = 1;
		}

		// admin space left exe
		if (oconf->admin_space_left_exe || nconf->admin_space_left_exe) {
				if (nconf->admin_space_left_exe == NULL)
						; /* do nothing if new one is blank */
				else if (oconf->admin_space_left_exe == NULL &&
								nconf->admin_space_left_exe)
						need_space_check = 1;
				else if (strcmp(oconf->admin_space_left_exe,
										nconf->admin_space_left_exe))
						need_space_check = 1;
				free((char *)oconf->admin_space_left_exe);
				oconf->admin_space_left_exe = nconf->admin_space_left_exe;
		}
		// disk full action
		if (oconf->disk_full_action != nconf->disk_full_action) {
				oconf->disk_full_action = nconf->disk_full_action;
				need_space_check = 1;
		}

		// disk full exe
		if (oconf->disk_full_exe || nconf->disk_full_exe) {
				if (nconf->disk_full_exe == NULL)
						; /* do nothing if new one is blank */
				else if (oconf->disk_full_exe == NULL && nconf->disk_full_exe)
						need_space_check = 1;
				else if (strcmp(oconf->disk_full_exe, nconf->disk_full_exe))
						need_space_check = 1;
				free((char *)oconf->disk_full_exe);
				oconf->disk_full_exe = nconf->disk_full_exe;
		}

		if (need_space_check) {
				/* note save suspended flag, then do space_left. If suspended
					* is still 0, then copy saved suspended back. This avoids
					* having to call check_log_file_size to restore it. */
				int saved_suspend = logging_suspended;

				fs_space_warning = 0;
				fs_admin_space_warning = 0;
				fs_space_left = 1;
				logging_suspended = 0;
				check_excess_logs();
				check_space_left();
				if (logging_suspended == 0)
						logging_suspended = saved_suspend;
		}

		// Next document the results
		srand(time(NULL));
		seq_num = rand()%10000;
		if (gettimeofday(&tv, NULL) == 0) {
				snprintf(date, sizeof(date), "audit(%lu.%03u:%u)", tv.tv_sec,
								(unsigned)(tv.tv_usec/1000), seq_num);
		} else {
				snprintf(date, sizeof(date),
								"audit(%lu.%03u:%u)", (unsigned long)time(NULL),
								0, seq_num);
		}

		e->reply.type = AUDIT_DAEMON_CONFIG;
		e->reply.len = snprintf(e->reply.msg.data, MAX_AUDIT_MESSAGE_LENGTH-2, 
						"%s op=reconfigure state=changed auid=%u pid=%d subj=%s res=success",
						date, uid, pid, ctx );
		e->reply.message = e->reply.msg.data;
		audit_msg(LOG_NOTICE, "%s", e->reply.message);
		free((char *)ctx);
}

