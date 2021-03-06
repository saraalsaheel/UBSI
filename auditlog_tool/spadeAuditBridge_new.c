/*
			--------------------------------------------------------------------------------
			SPADE - Support for Provenance Auditing in Distributed Environments.
			Copyright (C) 2015 SRI International

			This program is free software: you can redistribute it and/or
			modify it under the terms of the GNU General Public License as
			published by the Free Software Foundation, either version 3 of the
			License, or (at your option) any later version.

			This program is distributed in the hope that it will be useful,
			but WITHOUT ANY WARRANTY; without even the implied warranty of
			MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
			General Public License for more details.

			You should have received a copy of the GNU General Public License
			along with this program. If not, see <http://www.gnu.org/licenses/>.

			Code derived from:
http://www-01.ibm.com/support/knowledgecenter/ssw_i5_54/rzab6/xconoclient.htm
--------------------------------------------------------------------------------
	*/

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

#define SERVER_PATH     "/var/run/audispd_events"
#define BUFFER_LENGTH   10000
#define FALSE           0
#define TRUE		1

int UBSIAnalysis = FALSE;
int UBSI_buffer(const char *buf);
void UBSI_sig_handler(int signo);
int UBSI_buffer_flush();
int waitForEnd = FALSE;
int socketRead = FALSE;
int fileRead = FALSE;
int dirRead = FALSE;
char socketPath[256];
char filePath[256];
char dirPath[256];
char dirTimeBuf[256];
time_t dirTime = 0;
double curLogTime, lastGCTime;
int UBSIGCThreshold = 0;

// UBSI Unit analysis
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

// A struct to keep time as reported in audit log. Upto milliseconds.
// Doing it this way because double and long values don't seem to work with uthash in the structs where needed
typedef struct thread_time_t{
	int seconds;
	int milliseconds;
} thread_time_t;

typedef struct thread_unit_t {
		int tid;
		thread_time_t thread_time; // thread create time. seconds and milliseconds.
		int loopid; // loopid. in the output, we call this unitid.
		int iteration;
		double timestamp; // loop start time. Not iteration start.
		int count; // if two or more loops starts at the same timestamp. We use count to distinguish them.
} thread_unit_t;

typedef struct link_unit_t { // list of dependent units
		thread_unit_t id;
		UT_hash_handle hh;
} link_unit_t;

typedef struct mem_proc_t {
		long int addr;
		thread_unit_t last_written_unit;
		UT_hash_handle hh;
} mem_proc_t;

typedef struct thread_t {
		int tid; // pid in auditlog which is actually thread_id.
		thread_time_t thread_time; // thread create time. seconds and milliseconds.
} thread_t;

typedef struct unit_table_t {
		thread_t thread;
//		int tid; // pid in auditlog which is actually thread_id.
		int pid; // process id.  (main thread id)
		thread_unit_t cur_unit;
		bool valid; // is valid unit?
		long int r_addr;
		long int w_addr;
		link_unit_t *link_unit;
		mem_proc_t *mem_proc;
		char proc[1024];
		UT_hash_handle hh;
		double last_active_time; // the last time it has activity. Use this for Garbage collection.
} unit_table_t;

typedef struct event_buf_t {
		int id;
		int event_byte;
		char *event;
		UT_hash_handle hh;
} event_buf_t;

// Equality check is done using only tid, unitid, and iteration
typedef struct iteration_count_t{
	int tid;
	int unitid;
	int iteration;
	int count;
} iteration_count_t;

typedef struct thread_group_leader_t{
		thread_t thread;
		thread_t leader;
		UT_hash_handle hh;
} thread_group_leader_t;

// child --> thread_group_leader
typedef struct thread_hash_t{
		thread_t thread;
		UT_hash_handle hh;
} thread_hash_t;

/* thread_group_leader --> list of child threads
// sys_exit:  clear all child threads data only if I am the thread leader
// sys_exit_group: find thread leader and clear all child.
*/
typedef struct thread_group_t{
		thread_t leader;
		thread_hash_t *threads;
		UT_hash_handle hh;
} thread_group_t;

thread_group_leader_t *thread_group_leader_hash;
thread_group_t *thread_group_hash;
// Maximum iterations that can be buffered during a single timestamp
#define iteration_count_buffer_size 10000
// Total number of iterations so far in the iteration_count buffer
int current_time_iterations_index = 0;
// A buffer to keep iteration_count objects for iterations
iteration_count_t current_time_iterations[iteration_count_buffer_size];
// To keep track of whenever the timestamp changes on audit records
double last_time = -1;

// A list of thread start times for each pid seen being created
thread_time_t *thread_create_time;
// A flag to indicate that only a single file is to be processed with 'F' flag
bool singleFile = FALSE;

unit_table_t *unit_table;
event_buf_t *event_buf;

bool incomplete_record = false;

void syscall_handler(char *buf);

// garbage collecting inactive processes
void gc_inactive_processes();

// Debugging
long get_mem_usage();
int count_processes(char *str);

int memory_alloc[20];
int memory_free[20];
int memory_hash_del[20];
void *UBSI_malloc(size_t size, int no) 
{
		memory_alloc[no]++;
		return malloc(size);
}

void UBSI_free(void *ptr, int no)
{
		memory_free[no]++;
		return free(ptr);
}

#define UBSI_HASH_DEL(root, ptr, no) \
{ \
		HASH_DEL(root, ptr); \
		memory_hash_del[no]++; \
}

void print_memory_alloc()
{
		int i;
		fprintf(stderr, "Alloc:");
		for(i = 0; i < 12; i++) {
				if(memory_hash_del[i] > 0) 
						fprintf(stderr, "%d:%d/%d(%d), ",i, memory_alloc[i], memory_free[i], memory_hash_del[i]);
				else 
						fprintf(stderr, "%d:%d/%d, ",i, memory_alloc[i], memory_free[i]);
		}
		fprintf(stderr, "\n");
}

/*
			Java does not support reading from Unix domain sockets.

			This utility reads audit records from the audispd socket and writes them
			to the standard output stream.

			The Audit Reporter can invoke this utility and read from its standard
			output to obtain a stream of audit records.
	*/

void print_usage(char** argv) {
		printf("Usage: %s [OPTIONS]\n", argv[0]);
		printf("  -u, --unit                unit analysis\n");
		printf("  -s, --socket              socket name\n");
		printf("  -w, --wait-for-end        continue processing till the end of the log is reached\n");
		printf("  -f, --files               a filename that has a list of log files to process\n");  
		printf("  -F, --file				single file to process\n");  
		printf("  -d, --dir                 a directory name that contains log files\n");
		printf("  -t, --time                timestamp. Only handle log files modified after the timestamp. \n");
		printf("                            This option is only valid with -d option. (format: YYYY-MM-DD:HH:MM:SS,\n");
		printf("                              e.g., 2017-1-21:07:09:20)\n");
		printf("  -g, --garbage-collection		garbage-collection threshold in seconds. \n");
		printf("                            Perform garbage-collection to inactive processes. We assume that a process is\n");
		printf("                            inactive if it does not have any activitiy more than N seconds.\n");
		printf("  -h, --help                print this help and exit\n");
		printf("\n");

}

int command_line_option(int argc, char **argv)
{
		int c;

		struct option   long_opt[] =
		{
				{"help",			no_argument,		NULL, 'h'},
				{"unit",			no_argument,		NULL, 'u'},
				{"socket",			required_argument,	NULL, 's'},
				{"files",			required_argument,	NULL, 'f'},
				{"file",			required_argument,	NULL, 'F'},
				{"dir",				required_argument,	NULL, 'd'},
				{"time",			required_argument,	NULL, 't'},
				{"wait-for-end",	no_argument,		NULL, 'w'},
				{"garbage-collection",	no_argument,		NULL, 'g'},
				{NULL,				0,					NULL,	0}
		};

		while((c = getopt_long(argc, argv, "hus:F:f:d:t:w:g:", long_opt, NULL)) != -1)
		{
				switch(c)
				{
						case 's':
								strncpy(socketPath, optarg, 256);
								socketRead = TRUE;
								break;
						case 'f':
								strncpy(filePath, optarg, 256);
								fileRead = TRUE;
								break;
						case 'F':
								strncpy(filePath, optarg, 256);
								fileRead = TRUE;
								singleFile = TRUE;
								break;
						case 'd':
								strncpy(dirPath, optarg, 256);
								dirRead = TRUE;
								break;

						case 't':
								strncpy(dirTimeBuf, optarg, 256);
								struct tm temp_tm;
								if(strptime(dirTimeBuf, "%Y-%m-%d:%H:%M:%S", &temp_tm) == 0) {
										fprintf(stderr, "time error: %s, dirTime = %ld\n", dirTimeBuf, dirTime);
										break;
								}
							 dirTime = mktime(&temp_tm);
								fprintf(stderr, "dirTime = %ld\n", dirTime);
								break;

						case 'w':
								waitForEnd = TRUE;
								break;

						case 'u':
								UBSIAnalysis = TRUE;
								break;

						case 'g':
								UBSIGCThreshold = atoi(optarg);
								fprintf(stderr, "Garbage-collection Threshold: %d second\n", UBSIGCThreshold);
								break;

						case 'h':
								print_usage(argv);
								exit(0);

						default:
								fprintf(stderr, "Try `%s --help' for more information.\n", argv[0]);
								exit(-2);
				};
		};

}

void socket_read(char *programName)
{
		int audispdSocketDescriptor = -1, charactersRead, bytesReceived;
		char buffer[BUFFER_LENGTH];
		struct sockaddr_un serverAddress;

		do {
				audispdSocketDescriptor = socket(AF_UNIX, SOCK_STREAM, 0);
				if (audispdSocketDescriptor < 0) {
						fprintf(stderr, "%s: Unable to construct a socket. Error: %s\n", programName, strerror(errno));
						break;
				}

				memset(&serverAddress, 0, sizeof (serverAddress));
				serverAddress.sun_family = AF_UNIX;
				strcpy(serverAddress.sun_path, socketPath);

				charactersRead = connect(audispdSocketDescriptor, (struct sockaddr *) &serverAddress, SUN_LEN(&serverAddress));
				if (charactersRead < 0) {
						fprintf(stderr, "%s: Unable to connect to the socket: %s. Error: %s\n", programName, socketPath, strerror(errno));
						break;
				}

				fprintf(stderr, "#CONTROL_MSG#pid=%d\n", getpid());

				while (TRUE) {
						memset(&buffer, 0, BUFFER_LENGTH);
						charactersRead = recv(audispdSocketDescriptor, & buffer[0], BUFFER_LENGTH - 1, 0);
						if (charactersRead < 0) {
								fprintf(stderr, "%s: Error while reading from the socket. Error: %s\n", programName, strerror(errno));
								break;
						} else if (charactersRead == 0) {
								fprintf(stderr, "%s: Server closed the connection. Errror: %s\n", programName, strerror(errno));
								break;
						}
						UBSI_buffer(buffer);
				}
		} while (FALSE);

		if (audispdSocketDescriptor != -1) close(audispdSocketDescriptor);
}

void read_log(FILE *fp, char* filepath)
{
		char buffer[BUFFER_LENGTH];
		//FILE *fp = stdin;

		fprintf(stderr, "#CONTROL_MSG#pid=%d\n", getpid());
		do{
				while (TRUE) {
						memset(&buffer, 0, BUFFER_LENGTH);
						if(fgets(& buffer[0], BUFFER_LENGTH, fp) == NULL) {
								fprintf(stderr, "Reached the end of file (%s).\n", filepath);
								//UBSI_buffer_flush();
								break;
						}
						UBSI_buffer(buffer);
				}
		} while (FALSE);
}

void read_file_path()
{
	// If 'F' flag was passed
	if(singleFile == TRUE){
		
		FILE *log_fp;
		fprintf(stderr, "reading a log file: %s", filePath);
		
		log_fp = fopen(filePath, "r");
		if(log_fp == NULL) {
				fprintf(stderr, "file open error: %s", filePath);
		}

		read_log(log_fp, filePath);
		fclose(log_fp);
		UBSI_buffer_flush();
		
	}else{ // If 'f' flag was passed
	
		FILE *fp = fopen(filePath, "r");
		FILE *log_fp;
		char tmp[1024];
		char buffer[BUFFER_LENGTH];

		if(fp == NULL) {
				fprintf(stderr, "file open error: %s\n", filePath);
				return;
		}

		while(!feof(fp)) {
				if(fgets(tmp, 1024, fp) == NULL) break;
				fprintf(stderr, "reading a log file: %s", tmp);
				if(tmp[strlen(tmp)-1] == '\n') tmp[strlen(tmp)-1] = '\0';
				
				log_fp = fopen(tmp, "r");
				if(log_fp == NULL) {
						fprintf(stderr, "file open error: %s", tmp);
						continue;
				}

				read_log(log_fp, tmp);
				fclose(log_fp);
		}

		UBSI_buffer_flush();
		fclose(fp);
	}
}

ino_t find_next_file(time_t time, ino_t cur_inode)
{
		DIR *d;
		struct dirent *dir;
		char file[1024];
		struct stat sbuf;
		char time_buf[256];
		struct tm tm;
		
		char eFile[1024];
		time_t eTime = 0; // the earliest file mod time but later than dirTime
		int eInode = 0;

		d = opendir(dirPath);

		if(d == NULL) {
				fprintf(stderr, "dir open error: %s\n", dirPath);
				return -1;
		}

		//strftime(time_buf, sizeof(time_buf), "%Y-%m-%d:%H:%M:%S", localtime(&time));
		//printf("DirTime %s(%ld)\n", time_buf, time);

		while((dir = readdir(d)) != NULL)
		{
				sprintf(file, "%s/%s", dirPath, dir->d_name);
				if(stat(file, &sbuf) == -1) {
						//fprintf(stderr, "stat error 1: %s\n", file);
						//fprintf(stderr, "errno %d\n", errno);
						continue;
				}
				if(!S_ISREG(sbuf.st_mode)) continue; // if the file is not a regular file (e.g., dir)
				
				if(sbuf.st_mtime > time)
				{
						if(cur_inode == sbuf.st_ino) continue; // this is current file.
						if(eTime == 0 || sbuf.st_mtime < eTime) {
								eTime = sbuf.st_mtime;
								eInode = sbuf.st_ino;
						}
				}
//				strftime(time_buf, sizeof(time_buf), "%Y-%m-%d:%H:%M:%S", localtime(&sbuf.st_mtime));
//				printf("file: %s, last modified time %s(%ld)\n", file, time_buf, sbuf.st_mtime);
		}
		
		if(eInode > 0) {
//				strftime(time_buf, sizeof(time_buf), "%Y-%m-%d:%H:%M:%S", localtime(&eTime));
//				printf("Read next file: (inode %d, last modified time %s(%ld)\n", eInode, time_buf, eTime);
		} 
		closedir(d);
		return eInode;
}

FILE *open_inode(ino_t inode)
{
		DIR *d;
		struct dirent *dir;
		char file[1024];
		struct stat sbuf;
		FILE *fp;

		d = opendir(dirPath);

		if(d == NULL) {
				fprintf(stderr, "dir open error: %s\n", dirPath);
				return NULL;
		}

		while((dir = readdir(d)) != NULL)
		{
				sprintf(file, "%s/%s", dirPath, dir->d_name);
				if(stat(file, &sbuf) == -1) {
						fprintf(stderr, "stat error 2: %s\n", file);
						continue;
				}
				if(sbuf.st_ino == inode) {
						fp = fopen(file, "r");
						closedir(d);
						return fp;
				}
		}

		closedir(d);
		return NULL;
}

ino_t read_log_online(ino_t inode)
{
		char buffer[BUFFER_LENGTH];
		struct stat sbuf;
		time_t time;

		FILE *fp = open_inode(inode);
		
		if(fp == NULL) {
				fprintf(stderr, "file open error 1: inode %ld\n", inode);
				return -1;
		}
		
		do{
				while (TRUE) {
						memset(&buffer, 0, BUFFER_LENGTH);
						while(fgets(& buffer[0], BUFFER_LENGTH, fp) == NULL) {

								if(fstat(fileno(fp), &sbuf) == -1) {
										fprintf(stderr, "stat fails: inode %ld\n", inode);
										continue;
								}
								time = sbuf.st_mtime;
								ino_t next_inode = find_next_file(time, sbuf.st_ino);
								if(next_inode  > 0) {
										while(fgets(& buffer[0], BUFFER_LENGTH, fp) != NULL) { // check the log again.
												UBSI_buffer(buffer);
										}
										// At this point, the next log is available and the current log does not have any new event. 
										//Safe to close the current one and process the next log
										fclose(fp); 
										return next_inode;
								}
						}
						UBSI_buffer(buffer);
				}
		} while (FALSE);
}

void dir_read()
{
		ino_t inode = 0;
		
		fprintf(stderr, "#CONTROL_MSG#pid=%d\n", getpid());
		
		while((inode = find_next_file(dirTime, 0)) <= 0) sleep(1);
		//printf("Next file: inode %ld\n", inode);

		while((inode = read_log_online(inode)) > 0)
		{
				//printf("Next file: inode %ld\n", inode);
		}
}

int main(int argc, char *argv[]) {
		int max_pid, i;
		char *programName = argv[0];
		int audispdSocketDescriptor = -1, charactersRead, bytesReceived;
		char buffer[BUFFER_LENGTH];
		struct sockaddr_un serverAddress;

		
		putenv("TZ=EST5EDT"); // set timezone
		tzset();

		command_line_option(argc, argv);

		signal(SIGINT, UBSI_sig_handler);
		signal(SIGKILL, UBSI_sig_handler);
		signal(SIGTERM, UBSI_sig_handler);

		max_pid = get_max_pid() + 1;
		max_pid = max_pid*2;
		thread_create_time = (thread_time_t*) UBSI_malloc(sizeof(thread_time_t)*max_pid, 1);
		for(i = 0; i < max_pid; i++) {
				thread_create_time[i].seconds = 0;
				thread_create_time[i].milliseconds = 0;
		}

		if(socketRead) socket_read(programName);
		else if(fileRead) read_file_path();
		else if(dirRead) dir_read();
		else read_log(stdin, "stdin");

		return 0;
}

/*
 * Checks if an iteration exists with the arguments provided
 * 
 * If exists, then increments the count for it and returns that count.
 * If doesn't exist then adds this iteration_count and returns the count
 * value which would be zero.
 */
int get_iteration_count(int tid, int unitid, int iteration){
	int count = -1;
	// Check if the iteration exists
	if(current_time_iterations_index != 0){
			int a = 0;
			for(; a<current_time_iterations_index; a++){
					if(current_time_iterations[a].tid == tid 
							&& current_time_iterations[a].unitid == unitid
								&& current_time_iterations[a].iteration == iteration){
							current_time_iterations[a].count++;
							// if found then increment the count and set that count to the return value
							count = current_time_iterations[a].count;
							break;
					}
			}
	}
	// If not found then try to add it
	if(count == -1){
		// If buffer already full then print an error. -1 would be returned
		if(current_time_iterations_index >= iteration_count_buffer_size){
			fprintf(stderr, "Not enough space for another iteration. Increase 'iteration_count_buffer_size' and rerun\n");
		}else{
			// Add to the end of the buffer and set the count to the return value
			// without incrementing (counts start from 0)
			current_time_iterations[current_time_iterations_index].tid = tid;
			current_time_iterations[current_time_iterations_index].unitid = unitid;
			current_time_iterations[current_time_iterations_index].iteration = iteration;
			current_time_iterations[current_time_iterations_index].count = 0;
			count = current_time_iterations[current_time_iterations_index].count;
			current_time_iterations_index++;
		}
	}
	return count;
}

// Just resets the index instead of resetting each individual struct in the buffer
// Starts overriding the structs from the previous timestamp
void reset_current_time_iteration_counts(){
	current_time_iterations_index = 0;	
}

bool is_same_unit(thread_unit_t u1, thread_unit_t u2)
{
		if(u1.tid == u2.tid && 
				 u1.thread_time.seconds == u2.thread_time.seconds &&
				 u1.thread_time.milliseconds == u2.thread_time.milliseconds &&
					u1.loopid == u2.loopid &&
					u1.iteration == u2.iteration &&
					u1.timestamp == u2.timestamp &&
					u1.count == u2.count) return true;

		return false;
}

void get_time_and_eventid(char *buf, double *time, long *eventId)
{
		// Expected string format -> "type=<TYPE> msg=audit(<TIME>:<EVENTID>): ...."
		char *ptr;

		ptr = strstr(buf, "(");
		if(ptr == NULL) {
				incomplete_record = true;
				return;
		}

		sscanf(ptr+1, "%lf:%ld", time, eventId);

		return;

}

double get_timestamp_double(char *buf){
		char *ptr;
		double time;
		ptr = strstr(buf, "(");
		if(ptr == NULL) {
				incomplete_record = true;
				return 0;
		}

		sscanf(ptr+1, "%lf", &time);

		return time;
}

void get_timestamp(char *buf, int* seconds, int* millis)
{
		char *ptr;
// record format: 'type=X msg=audit(123.456:890): ...' OR 'type=X msg=ubsi(123.456:890): ...'
		ptr = strstr(buf, "(");
		if(ptr == NULL){
			*seconds = -1;
			*millis = -1;
		 incomplete_record = true;
		}else{
			sscanf(ptr+1, "%d", seconds);
			
			ptr = strstr(buf, ".");
			if(ptr == NULL){
				*seconds = -1;
				*millis = -1;
				incomplete_record = true;
			}else{
				sscanf(ptr+1, "%d", millis);
			}
		}
}

// Reads timestamp from audit record and then sets the seconds and milliseconds to the thread_time struct ref passed
void set_thread_time(char *buf, thread_time_t* thread_time)
{
		get_timestamp(buf, &thread_time->seconds, &thread_time->milliseconds);
		//thread_time->seconds = (int)time;
		//thread_time->milliseconds = (int)((time - thread_time->seconds) * 1000);
}

void set_thread_seen_time_conditionally(int pid, char* buf){
		thread_time_t* thread_time;
		thread_time = &thread_create_time[pid];
		if(thread_time->seconds == 0 && thread_time->milliseconds == 0){ // 0 means not set before
				set_thread_time(buf, thread_time);      
		}
}

long get_eventid(char* buf){
		char *ptr;
		long eventId;

		// Expected string format -> "type=<TYPE> msg=audit(<TIME>:<EVENTID>): ...."
		ptr = strstr(buf, ":");
		if(ptr == NULL) {
				incomplete_record = true;
				return 0;
		}

		sscanf(ptr+1, "%ld", &eventId);

		return eventId;
}

int emit_log(unit_table_t *ut, char* buf, bool print_unit, bool print_proc)
{
		if(print_unit == false || print_proc == false ) return 0;
		if(incomplete_record == true) {
				fprintf(stderr, "imcomplete record!: %s\n", buf);
				return 0;
		}
		if(print_proc && ut->proc[0] == '\0') return 0;
		int rc = 0;

		if(!print_unit && !print_proc) {
				rc = printf("%s", buf);
				return rc;
		}

		buf[strlen(buf)-1] = '\0';
		
		rc = printf("%s", buf);
		if(print_unit) {
				rc += printf(" unit=(pid=%d thread_time=%d.%03d unitid=%d iteration=%d time=%.3lf count=%d) "
							,ut->cur_unit.tid, ut->thread.thread_time.seconds, ut->thread.thread_time.milliseconds, ut->cur_unit.loopid, ut->cur_unit.iteration, ut->cur_unit.timestamp, ut->cur_unit.count);
		} 

		if(print_proc) {
				rc += printf("%s", ut->proc);
		}

		if(!print_proc) rc += printf("\n");

		return rc;
}

void delete_unit_hash(link_unit_t *hash_unit)
{
		link_unit_t *tmp_unit, *cur_unit;
		HASH_ITER(hh, hash_unit, cur_unit, tmp_unit) {
				//if(hash_unit != cur_unit) 
				UBSI_HASH_DEL(hash_unit, cur_unit, 4); 
				if(cur_unit) UBSI_free(cur_unit, 4);
		}
}

void delete_proc_hash(mem_proc_t *mem_proc)
{
		mem_proc_t *tmp_mem, *cur_mem;
		HASH_ITER(hh, mem_proc, cur_mem, tmp_mem) {
				//if(mem_proc != cur_mem) 
				UBSI_HASH_DEL(mem_proc, cur_mem, 3); 
				if(cur_mem) UBSI_free(cur_mem, 3);
		}
}

void loop_entry(unit_table_t *unit, long a1, char* buf, double time)
{
		char *ptr;

		unit->cur_unit.loopid = a1;
		unit->cur_unit.iteration = 0;
		unit->cur_unit.timestamp = time;
		
		ptr = strstr(buf, " ppid=");
		if(ptr == NULL) {
				fprintf(stderr, "loop_entry error! cannot find proc info: %s", buf);
				incomplete_record = true;
		} else {
				ptr++;
				strncpy(unit->proc, ptr, strlen(ptr));
				unit->proc[strlen(ptr)] = '\0';
		}
}

void loop_exit(unit_table_t *unit, char *buf)
{
		char tmp[10240];
		double time;
		long eventId;

		get_time_and_eventid(buf, &time, &eventId);
		// Adding extra space at the end of UBSI_EXIT string below because last character is overwritten with NULL char
		if(incomplete_record == false && unit->proc[0] != '\0') {
				sprintf(tmp, "type=UBSI_EXIT msg=ubsi(%.3f:%ld):  ", time, eventId);
				//sprintf(tmp,  "type=UBSI_EXIT pid=%d  ", unit->cur_unit.tid);
				emit_log(unit, tmp, false, true);
				unit->valid = false;
		}
}

void unit_entry(unit_table_t *unit, long a1, char* buf)
{
		char tmp[10240];
		int tid = unit->thread.tid;
		double time;
		long eventid;

		time = get_timestamp_double(buf);
		eventid = get_eventid(buf);

		if(last_time == -1){
			last_time = time;	
		}else if(last_time != time){
			last_time = time;
			reset_current_time_iteration_counts();
		}

		if(unit->valid == false) // this is an entry of a new loop.
		{
				loop_entry(unit, a1, buf, time);
		} else {
				unit->cur_unit.iteration++;
		}
		unit->valid = true;
		unit->cur_unit.timestamp = time;
		
		int iteration_count_value = get_iteration_count(tid, 
												unit->cur_unit.loopid,
												unit->cur_unit.iteration);
		// Can return -1 which means that the buffer is full. Error printed in 
		// get_iteration_count function
		unit->cur_unit.count = iteration_count_value;
		
		if(incomplete_record == false && unit->proc[0] != '\0') {
				sprintf(tmp, "type=UBSI_ENTRY msg=ubsi(%.3f:%ld): ", time, eventid);
				emit_log(unit, tmp, true, true);
		}
}

void unit_end(unit_table_t *unit, long a1)
{
		if(unit == NULL) return;
		struct link_unit_t *ut;
		char *buf;
		int buf_size;
/*
		int link_count = HASH_COUNT(unit->link_unit);

		if(unit->valid == true || link_count > 1) {
				buf_size = link_count * 200;
				buf = (char*) malloc(buf_size);
				bzero(buf, buf_size);
				// emit linked unit lists;
				if(unit->link_unit != NULL) {
						sprintf(buf, "type=UBSI_DEP2 list=\"");
						for(ut=unit->link_unit; ut != NULL; ut=ut->hh.next) {
								// < 200 bytes,
								sprintf(buf+strlen(buf), "(pid=%d thread_time=%d.000 unitid=%d iteration=%d time=%.3lf count=%d),"
								,ut->id.tid, ut->id.threadtime, ut->id.loopid, ut->id.iteration, ut->id.timestamp, ut->id.count);
						}
						sprintf(buf+strlen(buf), "\" ");
						emit_log(unit, buf, true, true);
						free(buf);
				}
		}
*/		

		delete_unit_hash(unit->link_unit);
		unit->link_unit = NULL;
		unit->r_addr = 0;
		unit->w_addr = 0;
}

void clear_proc(unit_table_t *unit)
{
		if(unit == NULL) return;

		unit_end(unit, -1);
		delete_proc_hash(unit->mem_proc);
		unit->mem_proc = NULL;

}

void proc_end(unit_table_t *unit)
{
		if(unit == NULL) return;

		thread_group_leader_t *tgl;
		HASH_FIND(hh, thread_group_leader_hash, &(unit->thread), sizeof(thread_t), tgl);
		if(tgl) {
				UBSI_HASH_DEL(thread_group_leader_hash, tgl, 8);
				UBSI_free(tgl, 8);
		}
		clear_proc(unit);

		UBSI_HASH_DEL(unit_table, unit,5);
		UBSI_free(unit, 5);

		return;
}

void proc_group_end(unit_table_t *unit)
{
		int pid = unit->pid;
		unit_table_t *pt;

		thread_group_leader_t *tgl;
		thread_group_t *tg;
		thread_hash_t *cur_t, *tmp_t;
		unit_table_t *ut;

		HASH_FIND(hh, thread_group_leader_hash, &(unit->thread), sizeof(thread_t), tgl);
		if(tgl == NULL) return;
		
		HASH_FIND(hh, thread_group_hash, &(tgl->leader), sizeof(thread_t), tg);
		if(tg == NULL)	return;
		
		HASH_ITER(hh, tg->threads, cur_t, tmp_t) {
				HASH_FIND(hh, unit_table, &(cur_t->thread), sizeof(thread_t), ut); 
				proc_end(ut);
				UBSI_HASH_DEL(tg->threads, cur_t, 7);
				UBSI_free(cur_t, 7);
		}

		HASH_FIND(hh, unit_table, &(tgl->thread), sizeof(thread_t), ut); 
		proc_end(ut);

		UBSI_HASH_DEL(thread_group_hash, tg, 6);
		UBSI_free(tg, 6);
}

void flush_all_unit()
{
		unit_table_t *tmp_unit, *cur_unit;
		HASH_ITER(hh, unit_table, cur_unit, tmp_unit) {
				unit_end(cur_unit, -1);
		}
}

void mem_write(unit_table_t *ut, long int addr, char* buf)
{
		if(ut->cur_unit.loopid == 0 || ut->cur_unit.timestamp == 0) return;
		
		// store mem_write into thread_group_leader's map
		thread_group_leader_t *tgl;
		unit_table_t *tgl_ut;

		// store mem_write info only into thread_leader's map
		HASH_FIND(hh, thread_group_leader_hash, &(ut->thread), sizeof(thread_t), tgl);
		if(tgl != NULL) {
				HASH_FIND(hh, unit_table, &(tgl->thread), sizeof(thread_t), tgl_ut);
				// thread leader should already have unit_table structure.
				if(tgl_ut == NULL) {
						incomplete_record = true;
						return;
				}
		} else {
				tgl_ut = ut;
		}

		// check for dup_write
		mem_proc_t *mp;
		HASH_FIND(hh, tgl_ut->mem_proc, &addr, sizeof(long int), mp);

		if(mp != NULL) {
				// addr is already in the memory. update the last_write_unit.
				mp->last_written_unit = ut->cur_unit;
				return;
		}

		// not duplicated write, add it into thread_leader's memory map
		mp = (mem_proc_t*) UBSI_malloc(sizeof(mem_proc_t), 2);
		assert(mp);
		mp->addr = addr;
		mp->last_written_unit = ut->cur_unit;
		HASH_ADD(hh, tgl_ut->mem_proc, addr, sizeof(long int),  mp);
}

void mem_read(unit_table_t *ut, long int addr, char *buf)
{
		if(ut->cur_unit.loopid == 0 || ut->cur_unit.timestamp == 0) return;

		char tmp[2048];
		double time;
		long eventId;

		// check thread_group_leader's map
		thread_group_leader_t *tgl;
		unit_table_t *tgl_ut;

		HASH_FIND(hh, thread_group_leader_hash, &(ut->thread), sizeof(thread_t), tgl);
		if(tgl != NULL) {
				HASH_FIND(hh, unit_table, &(tgl->thread), sizeof(thread_t), tgl_ut);
				// thread leader should already have unit_table structure.
				if(tgl_ut == NULL) {
						incomplete_record = true;
						return;
				}
		} else {
				tgl_ut = ut;
		}

		mem_proc_t *mp;
		HASH_FIND(hh, tgl_ut->mem_proc, &addr, sizeof(long int), mp);
		if(mp == NULL) return;

		thread_unit_t lid;
		if(mp->last_written_unit.timestamp != 0 && !is_same_unit(mp->last_written_unit, ut->cur_unit))
		{
				link_unit_t *lt;
				lid = mp->last_written_unit;
				HASH_FIND(hh, ut->link_unit, &lid, sizeof(thread_unit_t), lt);
				if(lt == NULL) {
						// emit the dependence.
						lt = (link_unit_t*) UBSI_malloc(sizeof(link_unit_t),4);
						assert(lt);
						lt->id = mp->last_written_unit;
						HASH_ADD(hh, ut->link_unit, id, sizeof(thread_unit_t), lt);

						get_time_and_eventid(buf, &time, &eventId);
						if(incomplete_record == false && ut->proc[0] != '\0') {
								sprintf(tmp, "type=UBSI_DEP msg=ubsi(%.3f:%ld): dep=(pid=%d thread_time=%d.%03d unitid=%d iteration=%d time=%.3lf count=%d), "
												,time, eventId, lt->id.tid, lt->id.thread_time.seconds, lt->id.thread_time.milliseconds, lt->id.loopid, lt->id.iteration, lt->id.timestamp, lt->id.count);
								emit_log(ut, tmp, true, true);
						}
				}
		}
}

unit_table_t* add_unit(int tid, int pid, bool valid)
{
		struct unit_table_t *ut;
		ut = UBSI_malloc(sizeof(struct unit_table_t),5);
		assert(ut);
		//ut->tid = tid;
		ut->thread.tid = tid;
		ut->thread.thread_time.seconds = thread_create_time[tid].seconds;
		ut->thread.thread_time.milliseconds = thread_create_time[tid].milliseconds;
		ut->pid = pid;
		ut->valid = valid;

		ut->cur_unit.tid = tid;
		ut->cur_unit.thread_time.seconds = thread_create_time[tid].seconds;
		ut->cur_unit.thread_time.milliseconds = thread_create_time[tid].milliseconds;
		ut->cur_unit.loopid = 0;
		ut->cur_unit.iteration = 0;
		ut->cur_unit.timestamp = 0;
		ut->cur_unit.count = 0; 

		ut->link_unit = NULL;
		ut->mem_proc = NULL;
		//HASH_ADD_INT(unit_table, tid, ut);
		HASH_ADD(hh, unit_table, thread, sizeof(thread_t), ut);
		return ut;
}

void print_thread_group(thread_t thread)
{
		thread_group_leader_t *ut;
		thread_group_t *gt;
		thread_hash_t *cur_t, *tmp_t;

		HASH_FIND(hh, thread_group_leader_hash, &thread, sizeof(thread_t), ut);

		fprintf(stdout, "THREAD_GROUP: thread %d\n", thread.tid);
		if(ut == NULL) {
				fprintf(stdout, "THREAD_GROUP: EMPTY!\n\n");
				return;
		}
		
		fprintf(stdout, "  Leader: %d\n", ut->leader.tid);

		HASH_FIND(hh, thread_group_hash, &(ut->leader), sizeof(thread_t), gt);
		if(gt == NULL) {
				fprintf(stdout, "     NO child.\n\n");
				return;
		}
		
		HASH_ITER(hh, gt->threads, cur_t, tmp_t) {
				fprintf(stdout, "     Child: %d\n", cur_t->thread.tid);
		}
		fprintf(stdout, "\n");
}

void set_thread_group(thread_t leader, thread_t child)
{
		thread_group_t *ut;
		thread_hash_t *lt;

		HASH_FIND(hh, thread_group_hash, &leader, sizeof(thread_t), ut);
		if(ut == NULL) {
				ut = UBSI_malloc(sizeof(struct thread_group_t),6);
				assert(ut);
				ut->leader = leader;
				ut->threads = NULL;

				lt = UBSI_malloc(sizeof(thread_hash_t), 7);
				assert(lt);
				lt->thread = child;
				HASH_ADD(hh, ut->threads, thread, sizeof(thread_t), lt);

				HASH_ADD(hh, thread_group_hash, leader, sizeof(thread_t), ut);
		} else {
				HASH_FIND(hh, ut->threads, &child, sizeof(thread_t), lt);
				if(lt == NULL) {
						lt = UBSI_malloc(sizeof(thread_hash_t), 7);
						assert(lt);
						lt->thread = child;
						HASH_ADD(hh, ut->threads, thread, sizeof(thread_t), lt);
				}
		}
}

thread_group_leader_t* add_thread_group_leader(thread_t thread, thread_t leader)
{
		thread_group_leader_t *ut = UBSI_malloc(sizeof(struct thread_group_leader_t), 8);
		assert(ut);
		ut->thread = thread;
		ut->leader = leader;
		
		HASH_ADD(hh, thread_group_leader_hash, thread, sizeof(thread_t), ut);

		return ut;
}

void set_thread_group_leader(thread_t child, thread_t parent)
{
		thread_group_leader_t *ut;
		HASH_FIND(hh, thread_group_leader_hash, &child, sizeof(thread_t), ut);

		if(ut != NULL) return; // child is already in the hash

		HASH_FIND(hh, thread_group_leader_hash, &parent, sizeof(thread_t), ut);
		if(ut == NULL) {
				// parent is not in the hash
				ut = add_thread_group_leader(parent, parent);
		}
		
		ut = add_thread_group_leader(child, ut->leader);

		set_thread_group(ut->leader, child);
}

void set_pid(int tid, int pid)
{
		struct unit_table_t *ut;
		int ppid;

		thread_t th_child, th_parent; 
		th_parent.tid = pid; 
		th_parent.thread_time.seconds = thread_create_time[pid].seconds;
		th_parent.thread_time.milliseconds = thread_create_time[pid].milliseconds;
		HASH_FIND(hh, unit_table, &th_parent, sizeof(thread_t), ut);  /* looking for parent thread's pid */
		//HASH_FIND_INT(unit_table, &pid, ut);  /* looking for parent thread's pid */

		if(ut == NULL) ppid = pid;
		else ppid = ut->pid;

		ut = NULL;

		th_child.tid = tid; 
		th_child.thread_time.seconds = thread_create_time[tid].seconds;
		th_child.thread_time.milliseconds = thread_create_time[tid].milliseconds;
		HASH_FIND(hh, unit_table, &th_child, sizeof(thread_t), ut);  /* id already in the hash? */
		//HASH_FIND_INT(unit_table, &tid, ut);  /* id already in the hash? */
		if (ut == NULL) {
				ut = add_unit(tid, ppid, 0);
		} else {
				ut->pid = ppid;
		}

		set_thread_group_leader(th_child, th_parent);
		//print_thread_group(th_child);
}

void UBSI_event(long tid, long a0, long a1, char *buf)
{
		int isNewUnit = 0;
		struct unit_table_t *ut;
		thread_t th;
		th.tid = tid; 
		th.thread_time.seconds = thread_create_time[tid].seconds;
		th.thread_time.milliseconds = thread_create_time[tid].milliseconds;
		HASH_FIND(hh, unit_table, &th, sizeof(thread_t), ut); 
		//HASH_FIND_INT(unit_table, &tid, ut);

		if(ut == NULL) {
				isNewUnit = 1;
				ut = add_unit(tid, tid, 0);
		}

		ut->last_active_time = get_timestamp_double(buf);
		curLogTime = ut->last_active_time;
		switch(a0) {
				case UENTRY: 
						if(ut->valid) unit_end(ut, a1);
						unit_entry(ut, a1, buf);
						break;
				case UEXIT: 
						if(isNewUnit == false)
						{
								unit_end(ut, a1);
								loop_exit(ut, buf);
						}
						break;
				case MREAD1:
						ut->r_addr = a1;
						ut->r_addr = ut->r_addr << 32;
						break;
				case MREAD2:
						ut->r_addr += a1;
						mem_read(ut, ut->r_addr, buf);
						break;
				case MWRITE1:
						ut->w_addr = a1;
						ut->w_addr = ut->w_addr << 32;
						break;
				case MWRITE2:
						ut->w_addr += a1;
						mem_write(ut, ut->w_addr, buf);
						break;
		}
}

void non_UBSI_event(long tid, int sysno, bool succ, char *buf)
{
		char *ptr;
		long a2;
		long ret;
		int time;

		struct unit_table_t *ut;

		thread_t th;  
		th.tid = tid; 
		th.thread_time.seconds = thread_create_time[tid].seconds;
		th.thread_time.milliseconds = thread_create_time[tid].milliseconds;
		HASH_FIND(hh, unit_table, &th, sizeof(thread_t), ut); 
		//HASH_FIND_INT(unit_table, &tid, ut);

		if(ut == NULL) {
				ut = add_unit(tid, tid, 0);
		}

		ut->last_active_time = get_timestamp_double(buf);
		curLogTime = ut->last_active_time;

		emit_log(ut, buf, false, false);

		if(succ == true && (sysno == 56 || sysno == 57 || sysno == 58)) // clone or fork
		{
				ptr = strstr(buf, " a2=");
				if(ptr == NULL) return;
				a2 = strtol(ptr+4, NULL, 16);
				ptr = strstr(buf, " exit=");
				if(ptr == NULL) return;
				ret = strtol(ptr+6, NULL, 10);

				set_thread_time(buf, &thread_create_time[ret]); /* set thread_create_time */
				if(a2 > 0) { // thread_creat event
						set_pid(ret, tid);
				}
		} else if(succ == true && ( sysno == 59 || sysno == 322 || sysno == 60 || sysno == 231)) { // execve, exit or exit_group
				if(sysno == 231) { // exit_group call
						proc_group_end(ut);
				} else if(sysno == 60) {
						proc_end(ut);
				} else {
						clear_proc(ut);
						if(sysno == 59){ // execve
								set_thread_time(buf, &thread_create_time[tid]);
								// updated start time to the time when execve happened. Done to reflect what happens in Audit reporter.
						}
				}
				if(sysno == 231 || sysno == 60){ // exit_group or exit
						// Need to set time to zero because it means that time hasn't been set for this process.
						// The zero condition is used to set seen time for process otherwise it would be updated each time.
						thread_create_time[tid].seconds = thread_create_time[tid].milliseconds = 0;
				}
		}
}

bool get_succ(char *buf)
{
		char *ptr;
		char succ[16];
		int i=0;

		ptr = strstr(buf, " success=");
		if(ptr == NULL) {
				incomplete_record = true;
				return false;
		}
		ptr+=9;

		for(i=0; ptr[i] != ' '; i++)
		{
				succ[i] = ptr[i];
		}
		succ[i] = '\0';
		if(strncmp(succ, "yes", 3) == 0) {
				return true;
		}
		return false;
}

void ubsi_intercepted_handler(char* buf){

		char* tmp;
		char* ptr_start;
		char* ptr_end;
		int tmp_current_index = 0;
		int buf_len;

		ptr_start = buf;

		if(ptr_start != NULL){
				buf_len = strlen(buf) + 1; // null char
				tmp = (char*)UBSI_malloc(sizeof(char)*buf_len, 9);
				
				if(tmp != NULL){
					memset(tmp, 0, buf_len);
					
					ptr_end = strstr(buf, "ubsi_intercepted=");

					if(ptr_end != NULL){			
							tmp_current_index = (ptr_end - ptr_start);
							strncpy(&tmp[0], buf, tmp_current_index);

							ptr_start = strstr(buf, "syscall=");

							if(ptr_start != NULL){
									strncpy(&tmp[tmp_current_index], ptr_start, (&buf[strlen(buf)] - ptr_start - 2));

									tmp[strlen(tmp)] = '\n';

									syscall_handler(tmp);
							}else{
								incomplete_record = true;
									fprintf(stderr, "ERROR: Malformed UBSI record: 'syscall' not found\n");	
							}
					}else{
						 incomplete_record = true;
							fprintf(stderr, "ERROR: Malformed UBSI record: 'ubsi_intercepted' not found\n");
					}
					UBSI_free(tmp, 9);
				}else{
				 incomplete_record = true;
					fprintf(stderr, "ERROR: Failed to allocate memory for 'ubsi_intercepted' record\n");	
				}
		}else{
				incomplete_record = true;
				fprintf(stderr, "ERROR: NULL buffer in UBSI record handler\n");	
		}
}

void syscall_handler(char *buf)
{
		char *ptr;
		int sysno;
		long a0, a1, pid, ppid;
		bool succ = false;

		incomplete_record = false;

		ptr = strstr(buf, " syscall=");
		if(ptr == NULL) {
				fprintf(stderr, "ERROR: ptr = NULL: %s\n", buf);
				return;
		}
		sysno = strtol(ptr+9, NULL, 10);
		
		ptr = strstr(ptr, " ppid=");
		if(ptr == NULL) return;
		ppid = strtol(ptr+6, NULL, 10);

		ptr = strstr(ptr, " pid=");
		if(ptr == NULL) return;
		pid = strtol(ptr+5, NULL, 10);

		succ = get_succ(buf);

		// Syscall exit(60) and exit_group(231) do not return, thus do not have "success" field. They always succeed.
		if(sysno == 60 || sysno == 231) succ=true; 
		
		// Set seen time here if not already set. thread_create_time is used in the functions below.
		set_thread_seen_time_conditionally(pid, buf);

		if(sysno == 62)
		{
				ptr = strstr(buf, " a0=");
				if(ptr == NULL) return;
				a0 = strtol(ptr+4, NULL, 16);
				if(a0 == UENTRY || a0 == UEXIT || a0 == MREAD1 || a0 == MREAD2 || a0 == MWRITE1 || a0 ==MWRITE2)
				{
						ptr = strstr(ptr, " a1=");
						if(ptr == NULL) return;
						a1 = strtol(ptr+4, NULL, 16);
						UBSI_event(pid, a0, a1, buf);
				} else {
						non_UBSI_event(pid, sysno, succ, buf);
				}
		} else {
				non_UBSI_event(pid, sysno, succ, buf);
		}

		// Periodically print out memory usage for debugging

#define PRINT_INTERVAL 60 // second
		static double last_print_time = 0;
		double cur_time = get_timestamp_double(buf);
		long mem_usage;
		int num_ff;

		if(PRINT_INTERVAL > 0 && (cur_time - PRINT_INTERVAL > last_print_time)) {
			 mem_usage = get_mem_usage();
				num_ff = count_processes("firefox");
				fprintf(stderr, "time, %lf, mem, %ld, Kb, firefox processes, %d\n", cur_time, mem_usage, num_ff); 
				last_print_time = cur_time;
				//print_memory_alloc();
		}

}

#define EVENT_LENGTH 1048576
#define REORDERING_WINDOW 10000
int next_event_id = 0;

int UBSI_buffer_flush()
{
		struct event_buf_t *eb;
		fprintf(stderr, "UBSI flush the log buffer: %d events\n", HASH_COUNT(event_buf));

		while(HASH_COUNT(event_buf) > 0)
		{
				HASH_FIND_INT(event_buf, &next_event_id, eb);
				next_event_id++;
				if(eb != NULL) {
						if(strstr(eb->event, "ubsi_intercepted=") != NULL){
								if(UBSIAnalysis) ubsi_intercepted_handler(eb->event);
								else printf("%s", eb->event);
						} else if(strstr(eb->event, "type=SYSCALL") != NULL) {
								if(UBSIAnalysis) syscall_handler(eb->event);
								else printf("%s", eb->event);
						} else {
								printf("%s", eb->event);
						}
						UBSI_HASH_DEL(event_buf, eb, 11);
						UBSI_free(eb->event, 11);
						UBSI_free(eb,10);
				} 
		}
}

int UBSI_buffer(const char *buf)
{
		int cursor = 0;
		int event_start = 0;
		long id = 0;
		char event[EVENT_LENGTH];
		int event_byte = 0;
		char *ptr;
		static char remain[BUFFER_LENGTH];
		static int remain_byte = 0;

		struct event_buf_t *eb;

		for(cursor=0; cursor < strlen(buf); cursor++) {
				if(buf[cursor] == '\n') {
						if(event_start == 0 && remain_byte > 0) {
								strncpy(event, remain, remain_byte-1);
								strncpy(event+remain_byte-1, buf, cursor+1);
								event[remain_byte + cursor] = '\0';
								event_byte = remain_byte + cursor;
								remain_byte = 0;
						} else {
								strncpy(event, buf+event_start, cursor-event_start+1);
								event[cursor-event_start+1] = '\0';
								event_byte = cursor-event_start+1;
						}
						if(strstr(event, "type=DAEMON_START") != NULL) {
								// flush events in reordering buffer.
								UBSI_buffer_flush();
						}

						if(strstr(event, "type=EOE") == NULL && strstr(event, "type=UNKNOWN[") == NULL && strstr(event, "type=PROCTILE") == NULL) {
								ptr = strstr(event, ":");
								if(ptr == NULL) {
										id = -1; // to indicate error. it is set back to zero once it gets out of the if condition.
										printf("ERROR: cannot parse event id.\n");
								} else {
										id = strtol(ptr+1, NULL, 10);
										if(next_event_id == 0) next_event_id = id;
								}
								if(id != -1){
									HASH_FIND_INT(event_buf, &id, eb);
									if(eb == NULL) {
											eb = (event_buf_t*) UBSI_malloc(sizeof(event_buf_t),10);
										 assert(eb);
											eb->id = id;
											//eb->event = (char*) malloc(sizeof(char) * EVENT_LENGTH);
											eb->event = (char*) UBSI_malloc(sizeof(char) * (event_byte+1), 11);
										 assert(eb->event);
											eb->event_byte = event_byte;
											strncpy(eb->event, event, event_byte+1);
											HASH_ADD_INT(event_buf, id, eb);
											if(next_event_id > id) {
													next_event_id = id;
											}
									} else {
											eb->event = (char*) realloc(eb->event, sizeof(char) * (eb->event_byte+event_byte+1));
											strncpy(eb->event+eb->event_byte, event, event_byte+1);
											eb->event_byte += event_byte;
									}
								}
						}
						event_start = cursor+1;
						id = 0;
				}
		}
		if(buf[strlen(buf)-1] != '\n') {
				remain_byte = cursor - event_start+1;
				strncpy(remain, buf+event_start, remain_byte);
				remain[remain_byte] = '\0';
		} else {
				remain_byte = 0;
		}

		// if we have enough events in the buffer..
		while(HASH_COUNT(event_buf) > REORDERING_WINDOW)
		{
				HASH_FIND_INT(event_buf, &next_event_id, eb);
				next_event_id++;
				if(eb != NULL) {
						if(strstr(eb->event, "ubsi_intercepted=") != NULL){
								if(UBSIAnalysis) ubsi_intercepted_handler(eb->event);
								else printf("%s", eb->event);
						} else if(strstr(eb->event, "type=SYSCALL") != NULL) {
								if(UBSIAnalysis) syscall_handler(eb->event);
								else printf("%s", eb->event);
						} else {
								printf("%s", eb->event);
						}
						UBSI_HASH_DEL(event_buf, eb,11);
						UBSI_free(eb->event, 11);
						UBSI_free(eb, 10);
				}
		}
		// Garbage collecting inactive processes
		if(UBSIGCThreshold > 0 && 
				  (lastGCTime < (curLogTime - UBSIGCThreshold)))
		{
				gc_inactive_processes();
				lastGCTime = curLogTime;
				print_memory_alloc();
		}
}

void UBSI_sig_handler(int signo)
{
		if(waitForEnd == FALSE) {
				UBSI_buffer_flush();
				exit(0);
		} else {
				// ignore the signal and the process continues until the end of the input stream/file.
		}
}

int get_max_pid()
{
		int max_pid;
	 FILE *fp = fopen("/proc/sys/kernel/pid_max", "r");
		fscanf(fp, "%d", &max_pid);
		fclose(fp);

		return max_pid;
}

void gc_inactive_processes()
{
		unit_table_t *cur_proc, *tmp_proc;
		HASH_ITER(hh, unit_table, cur_proc, tmp_proc) {
				if(cur_proc->last_active_time < (curLogTime - UBSIGCThreshold))
				{
						proc_end(cur_proc);
				}
		}
}

long get_mem_usage() 
{
		char tmp[1024];
		long mem = 0; 
		FILE* fp_s = fopen( "/proc/self/status", "r" );
		while(1) {
				if(fp_s == NULL) {
						fprintf(stderr, "ERROR: proc/self\n");
						break;
				}    
				fgets(tmp, 1024, fp_s);
				if(strncmp(tmp, "VmSize:", 7) == 0) { 
						sscanf(tmp, "VmSize:\t%ld", &mem);
						//fprintf(stderr, "MEMUSAGE %s: %s\n", str, tmp);
						break;
				}    
		}
		fclose(fp_s);

		return mem; 
}

int count_processes(char *str)
{
		int ret = 0;

		unit_table_t *cur_proc, *tmp_proc;
		HASH_ITER(hh, unit_table, cur_proc, tmp_proc) {
				if(strstr(cur_proc->proc, str) != NULL) ret++;
		}
		return ret;
}
