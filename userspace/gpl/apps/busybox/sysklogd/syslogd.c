/* vi: set sw=4 ts=4: */
/*
 * Mini syslogd implementation for busybox
 *
 * Copyright (C) 1999-2004 by Erik Andersen <andersen@codepoet.org>
 *
 * Copyright (C) 2000 by Karl M. Hegbloom <karlheg@debian.org>
 *
 * "circular buffer" Copyright (C) 2001 by Gennady Feldman <gfeldman@gena01.com>
 *
 * Maintainer: Gennady Feldman <gfeldman@gena01.com> as of Mar 12, 2001
 *
 * Licensed under the GPL v2 or later, see the file LICENSE in this tarball.
 */

/*
 * Done in syslogd_and_logger.c:
#include "libbb.h"
#define SYSLOG_NAMES
#define SYSLOG_NAMES_CONST
#include <syslog.h>
*/

#include <sys/un.h>
#include <sys/uio.h>

#if ENABLE_FEATURE_REMOTE_LOG
#include <netinet/in.h>
#endif

#if ENABLE_FEATURE_IPC_SYSLOG
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#endif

// brcm begin
#include "cms_util.h"
#include "cms_msg.h"
// brcm end

#ifdef MSTC_COMMON_PATCH //__MTS__, TengChang
#include "cms_msg.h"
#include "cms_log.h"
#include "cms_mem.h"

void *msgHandle=NULL; /* handle to communications link to smd */
static const char *flashLogFilePath = FLASH_LOG_FILE;
static int alarmCapacityPercent = 80;  //When this value is zero, do not send log full alarm 
static int isLogFull = 0;
static int isLogFullAlarmSent = 0;
static int fullAlarmSentCount = 0;
static int isLogCategorySet = 0;

static time_t last_dns_resolve;
static time_t last_alert;
#define CHECK_MAILSEND_INTERVAL 60
static time_t last_check_mailsend;

static char logCategory[32][32]={0};
static char alertCategory[8][32]={0};
static struct mail_setting
{
	char mailServer[128];
	char mailSubject[128];
	char mailFrom[128];
	char logAddress[128];
	char alertAddress[128];
	unsigned int alertInterval;
	int isAuth;
	char user[128];
	char password[128];
} emailCfg = {
	{0}, {0}, {0}, 0, {0}, {0}
};
static void sendAlertByMail(char *subject, char *contentFile, int saveResult);

#endif //__MTS__, TengChang

#define DEBUG 0

/* MARK code is not very useful, is bloat, and broken:
 * can deadlock if alarmed to make MARK while writing to IPC buffer
 * (semaphores are down but do_mark routine tries to down them again) */
#undef SYSLOGD_MARK

/* Write locking does not seem to be useful either */
#undef SYSLOGD_WRLOCK

// brcm begin
/* All the access to /dev/log will be redirected to /var/log/log
 *  * which is TMPFS, memory file system.
 **/
#define BRCM_PATH_LOG "/var/log/log"
// brcm end
enum {
	MAX_READ = CONFIG_FEATURE_SYSLOGD_READ_BUFFER_SIZE,
	DNS_WAIT_SEC = 2 * 60,
};

/* Semaphore operation structures */
struct shbuf_ds {
	int32_t size;   /* size of data - 1 */
	int32_t tail;   /* end of message list */
	char data[1];   /* data/messages */
};

#if ENABLE_FEATURE_REMOTE_LOG
typedef struct {
	int remoteFD;
	unsigned last_dns_resolve;
	len_and_sockaddr *remoteAddr;
	const char *remoteHostname;
} remoteHost_t;
#endif

/* Allows us to have smaller initializer. Ugly. */
#define GLOBALS \
	const char *logFilePath;                \
	int logFD;                              \
	/* interval between marks in seconds */ \
	/*int markInterval;*/                   \
	/* level of messages to be logged */    \
	int logLevel;                           \
	int remotelogLevel;                     \
IF_FEATURE_ROTATE_LOGFILE( \
	/* max size of file before rotation */  \
	unsigned logFileSize;                   \
	/* number of rotated message files */   \
	unsigned logFileRotate;                 \
	unsigned curFileSize;                   \
	smallint isRegular;                     \
) \
IF_FEATURE_IPC_SYSLOG( \
	int shmid; /* ipc shared memory id */   \
	int s_semid; /* ipc semaphore id */     \
	int shm_size;                           \
	struct sembuf SMwup[1];                 \
	struct sembuf SMwdn[3];                 \
)

struct init_globals {
	GLOBALS
};

struct globals {
	GLOBALS

#if ENABLE_FEATURE_REMOTE_LOG
	llist_t *remoteHosts;
#endif
#if ENABLE_FEATURE_IPC_SYSLOG
	struct shbuf_ds *shbuf;
#endif
	time_t last_log_time;
	/* localhost's name. We print only first 64 chars */
	char *hostname;

	/* We recv into recvbuf... */
	char recvbuf[MAX_READ * (1 + ENABLE_FEATURE_SYSLOGD_DUP)];
	/* ...then copy to parsebuf, escaping control chars */
	/* (can grow x2 max) */
	char parsebuf[MAX_READ*2];
	/* ...then sprintf into printbuf, adding timestamp (15 chars),
	 * host (64), fac.prio (20) to the message */
	/* (growth by: 15 + 64 + 20 + delims = ~110) */
	char printbuf[MAX_READ*2 + 128];
};

static const struct init_globals init_data = {
	.logFilePath = "/var/log/messages",
	.logFD = -1,
#ifdef SYSLOGD_MARK
	.markInterval = 60 * 60, // brcm
#endif
	.logLevel = -1,
	.remotelogLevel = -1, // brcm
#if ENABLE_FEATURE_ROTATE_LOGFILE
	.logFileSize = 200 * 1024,
	.logFileRotate = 1,
#endif
#if ENABLE_FEATURE_IPC_SYSLOG
	.shmid = -1,
	.s_semid = -1,
	.shm_size = ((CONFIG_FEATURE_IPC_SYSLOG_BUFFER_SIZE)*1024), // default shm size
	.SMwup = { {1, -1, IPC_NOWAIT} },
	.SMwdn = { {0, 0}, {1, 0}, {1, +1} },
#endif
};

#define G (*ptr_to_globals)
#define INIT_G() do { \
	SET_PTR_TO_GLOBALS(memcpy(xzalloc(sizeof(G)), &init_data, sizeof(init_data))); \
} while (0)


/* Options */
enum {
	OPTBIT_mark = 0, // -m
	OPTBIT_nofork, // -n
	OPTBIT_outfile, // -O
	OPTBIT_loglevel, // -l
	OPTBIT_remoteloglevel, // -r  // brcm
	OPTBIT_small, // -S
	IF_FEATURE_ROTATE_LOGFILE(OPTBIT_filesize   ,)	// -s
	IF_FEATURE_ROTATE_LOGFILE(OPTBIT_rotatecnt  ,)	// -b
	IF_FEATURE_REMOTE_LOG(    OPTBIT_remotelog  ,)	// -R
	IF_FEATURE_REMOTE_LOG(    OPTBIT_locallog   ,)	// -L
	IF_FEATURE_IPC_SYSLOG(    OPTBIT_circularlog,)	// -C
	IF_FEATURE_SYSLOGD_DUP(   OPTBIT_dup        ,)	// -D

	OPT_mark        = 1 << OPTBIT_mark    ,
	OPT_nofork      = 1 << OPTBIT_nofork  ,
	OPT_outfile     = 1 << OPTBIT_outfile ,
	OPT_loglevel    = 1 << OPTBIT_loglevel,
	OPT_remoteloglevel    = 1 << OPTBIT_remoteloglevel, // brcm
	OPT_small       = 1 << OPTBIT_small   ,
	OPT_filesize    = IF_FEATURE_ROTATE_LOGFILE((1 << OPTBIT_filesize   )) + 0,
	OPT_rotatecnt   = IF_FEATURE_ROTATE_LOGFILE((1 << OPTBIT_rotatecnt  )) + 0,
	OPT_remotelog   = IF_FEATURE_REMOTE_LOG(    (1 << OPTBIT_remotelog  )) + 0,
	OPT_locallog    = IF_FEATURE_REMOTE_LOG(    (1 << OPTBIT_locallog   )) + 0,
	OPT_circularlog = IF_FEATURE_IPC_SYSLOG(    (1 << OPTBIT_circularlog)) + 0,
	OPT_dup         = IF_FEATURE_SYSLOGD_DUP(   (1 << OPTBIT_dup        )) + 0,
};
#define OPTION_STR "m:nO:l:r:S" \
	IF_FEATURE_ROTATE_LOGFILE("s:" ) \
	IF_FEATURE_ROTATE_LOGFILE("b:" ) \
	IF_FEATURE_REMOTE_LOG(    "R:" ) \
	IF_FEATURE_REMOTE_LOG(    "L"  ) \
	IF_FEATURE_IPC_SYSLOG(    "C::") \
	IF_FEATURE_SYSLOGD_DUP(   "D"  )
#define OPTION_DECL *opt_m, *opt_l, *opt_r \
	IF_FEATURE_ROTATE_LOGFILE(,*opt_s) \
	IF_FEATURE_ROTATE_LOGFILE(,*opt_b) \
	IF_FEATURE_IPC_SYSLOG(    ,*opt_C = NULL)
#define OPTION_PARAM &opt_m, &G.logFilePath, &opt_l , &opt_r\
	IF_FEATURE_ROTATE_LOGFILE(,&opt_s) \
	IF_FEATURE_ROTATE_LOGFILE(,&opt_b) \
	IF_FEATURE_REMOTE_LOG(	  ,&remoteAddrList) \
	IF_FEATURE_IPC_SYSLOG(    ,&opt_C)


/* circular buffer variables/structures */
#if ENABLE_FEATURE_IPC_SYSLOG

#if CONFIG_FEATURE_IPC_SYSLOG_BUFFER_SIZE < 4
#error Sorry, you must set the syslogd buffer size to at least 4KB.
#error Please check CONFIG_FEATURE_IPC_SYSLOG_BUFFER_SIZE
#endif

/* our shared key (syslogd.c and logread.c must be in sync) */
enum { KEY_ID = 0x414e4547 }; /* "GENA" */

static void ipcsyslog_cleanup(void)
{
	if (G.shmid != -1) {
		shmdt(G.shbuf);
	}
	if (G.shmid != -1) {
		shmctl(G.shmid, IPC_RMID, NULL);
	}
	if (G.s_semid != -1) {
		semctl(G.s_semid, 0, IPC_RMID, 0);
	}
}

static void ipcsyslog_init(void)
{
	if (DEBUG)
		printf("shmget(%x, %d,...)\n", (int)KEY_ID, G.shm_size);

	G.shmid = shmget(KEY_ID, G.shm_size, IPC_CREAT | 0644);
	if (G.shmid == -1) {
		bb_perror_msg_and_die("shmget");
	}

	G.shbuf = shmat(G.shmid, NULL, 0);
	if (G.shbuf == (void*) -1L) { /* shmat has bizarre error return */
		bb_perror_msg_and_die("shmat");
	}

	memset(G.shbuf, 0, G.shm_size);
	G.shbuf->size = G.shm_size - offsetof(struct shbuf_ds, data) - 1;
	/*G.shbuf->tail = 0;*/

	// we'll trust the OS to set initial semval to 0 (let's hope)
	G.s_semid = semget(KEY_ID, 2, IPC_CREAT | IPC_EXCL | 1023);
	if (G.s_semid == -1) {
		if (errno == EEXIST) {
			G.s_semid = semget(KEY_ID, 2, 0);
			if (G.s_semid != -1)
				return;
		}
		bb_perror_msg_and_die("semget");
	}
}

/* Write message to shared mem buffer */
static void log_to_shmem(const char *msg, int len)
{
	int old_tail, new_tail;

	if (semop(G.s_semid, G.SMwdn, 3) == -1) {
		bb_perror_msg_and_die("SMwdn");
	}

	/* Circular Buffer Algorithm:
	 * --------------------------
	 * tail == position where to store next syslog message.
	 * tail's max value is (shbuf->size - 1)
	 * Last byte of buffer is never used and remains NUL.
	 */
	len++; /* length with NUL included */
 again:
	old_tail = G.shbuf->tail;
	new_tail = old_tail + len;
	if (new_tail < G.shbuf->size) {
		/* store message, set new tail */
		memcpy(G.shbuf->data + old_tail, msg, len);
		G.shbuf->tail = new_tail;
	} else {
		/* k == available buffer space ahead of old tail */
		int k = G.shbuf->size - old_tail;
		/* copy what fits to the end of buffer, and repeat */
		memcpy(G.shbuf->data + old_tail, msg, k);
		msg += k;
		len -= k;
		G.shbuf->tail = 0;
		goto again;
	}
	if (semop(G.s_semid, G.SMwup, 1) == -1) {
		bb_perror_msg_and_die("SMwup");
	}
	if (DEBUG)
		printf("tail:%d\n", G.shbuf->tail);
}
#else
void ipcsyslog_cleanup(void);
void ipcsyslog_init(void);
void log_to_shmem(const char *msg);
#endif /* FEATURE_IPC_SYSLOG */


/* Print a message to the log file. */
static void log_locally(time_t now, char *msg)
{
#ifdef SYSLOGD_WRLOCK
	struct flock fl;
#endif
	int len = strlen(msg);

#if ENABLE_FEATURE_IPC_SYSLOG
	if ((option_mask32 & OPT_circularlog) && G.shbuf) {
		log_to_shmem(msg, len);
		return;
	}
#endif
	if (G.logFD >= 0) {
		/* Reopen log file every second. This allows admin
		 * to delete the file and not worry about restarting us.
		 * This costs almost nothing since it happens
		 * _at most_ once a second.
		 */
		if (!now)
			now = time(NULL);
		if (G.last_log_time != now) {
			G.last_log_time = now;
			close(G.logFD);
			goto reopen;
		}
	} else {
 reopen:
		G.logFD = open(G.logFilePath, O_WRONLY | O_CREAT
					| O_NOCTTY | O_APPEND | O_NONBLOCK,
					0666);
		if (G.logFD < 0) {
			/* cannot open logfile? - print to /dev/console then */
			int fd = device_open(DEV_CONSOLE, O_WRONLY | O_NOCTTY | O_NONBLOCK);
			if (fd < 0)
				fd = 2; /* then stderr, dammit */
			full_write(fd, msg, len);
			if (fd != 2)
				close(fd);
			return;
		}
#if ENABLE_FEATURE_ROTATE_LOGFILE
		{
			struct stat statf;
			G.isRegular = (fstat(G.logFD, &statf) == 0 && S_ISREG(statf.st_mode));
			/* bug (mostly harmless): can wrap around if file > 4gb */
			G.curFileSize = statf.st_size;
		}
#endif
	}

#ifdef SYSLOGD_WRLOCK
	fl.l_whence = SEEK_SET;
	fl.l_start = 0;
	fl.l_len = 1;
	fl.l_type = F_WRLCK;
	fcntl(G.logFD, F_SETLKW, &fl);
#endif

#if ENABLE_FEATURE_ROTATE_LOGFILE
	if (G.logFileSize && G.isRegular && G.curFileSize > G.logFileSize) {
		if (G.logFileRotate) { /* always 0..99 */
			int i = strlen(G.logFilePath) + 3 + 1;
			char oldFile[i];
			char newFile[i];
			i = G.logFileRotate - 1;
			/* rename: f.8 -> f.9; f.7 -> f.8; ... */
			while (1) {
				sprintf(newFile, "%s.%d", G.logFilePath, i);
				if (i == 0) break;
				sprintf(oldFile, "%s.%d", G.logFilePath, --i);
				/* ignore errors - file might be missing */
				rename(oldFile, newFile);
			}
			/* newFile == "f.0" now */
			rename(G.logFilePath, newFile);
#ifdef SYSLOGD_WRLOCK
			fl.l_type = F_UNLCK;
			fcntl(G.logFD, F_SETLKW, &fl);
#endif
			close(G.logFD);
			goto reopen;
		}
		ftruncate(G.logFD, 0);
	}
	G.curFileSize +=
#endif
			full_write(G.logFD, msg, len);
#ifdef SYSLOGD_WRLOCK
	fl.l_type = F_UNLCK;
	fcntl(G.logFD, F_SETLKW, &fl);
#endif
}

static int parse_fac_prio_20(int pri, char *res20)
{
	const CODE *c_pri, *c_fac;
// brcm begin
	int localLog=1;
	int remoteLog=1;
// brcm end

	if (pri != 0) {
		c_fac = facilitynames;
		while (c_fac->c_name) {
			if (c_fac->c_val != (LOG_FAC(pri) << 3)) {
				c_fac++;
				continue;
			}
			/* facility is found, look for prio */
			c_pri = prioritynames;
// brcm begin
			if (c_pri->c_val > G.logLevel)
			    localLog = 0;
			if (c_pri->c_val > G.remotelogLevel)
			    remoteLog = 0;
// brcm end
			while (c_pri->c_name) {
				if (c_pri->c_val != LOG_PRI(pri)) {
					c_pri++;
					continue;
				}
				snprintf(res20, 20, "%s.%s",
						c_fac->c_name, c_pri->c_name);
// brcm begin
				if (!localLog && !remoteLog)
				    return 1;
				else
				    return 0;
// brcm end
			}
			/* prio not found, bail out */
			break;
		}
		snprintf(res20, 20, "<%d>", pri);
	}
// brcm begin
	if (!localLog && !remoteLog)
	    return 1;
	else
	    return 0;
// brcm end
}

#ifdef MSTC_COMMON_PATCH //__MTS__, TengChang
//Set saveResult to 1 if you want syslogd to track mailsend result
//Before call this API, write the mail content to /var/alert
static void sendAlertByMail(char *subject, char *contentFile_path, int saveResult)
{
	char cmd [1024];
	time_t now = time(NULL);

	if(!strcmp(emailCfg.mailServer,"")){
		printf("Send alert by mail, but mail server is not set!\n");
		return;
	}

	if ((now - last_alert) >= emailCfg.alertInterval){
		last_alert = now;

		if(emailCfg.isAuth)
			snprintf(cmd, 1024, "mailsend -f %s -smtp %s -d %s -t %s -sub \"%s\"  -attach \"%s,text/plain,i\" -starttls -auth -user %s -pass '%s' +cc +bc", emailCfg.mailFrom, emailCfg.mailServer, emailCfg.mailServer, emailCfg.alertAddress, subject, contentFile_path, emailCfg.user, emailCfg.password);
		else
			snprintf(cmd, 1024, "mailsend -f %s -smtp %s -d %s -t %s -sub \"%s\"  -attach \"%s,text/plain,i\" +cc +bc", emailCfg.mailFrom, emailCfg.mailServer, emailCfg.mailServer, emailCfg.alertAddress, subject, contentFile_path);
		
		if(saveResult){
			strcat(cmd, " -saveresult /var/alarmResult");
		}
		strcat(cmd, "&");
		system(cmd);
	}
}
#endif

/* len parameter is used only for "is there a timestamp?" check.
 * NB: some callers cheat and supply len==0 when they know
 * that there is no timestamp, short-circuiting the test. */
static void timestamp_and_log(int pri, const char *msg, int len)
{
	char *timestamp;
	time_t now;

#ifdef MSTC_COMMON_PATCH //__MTS__, TengChang, Modify format of timestamp
	struct tm *tmdata;
	char timestamp_buff[32];
	time(&now);
	tmdata = gmtime(&now);
	sprintf(timestamp_buff, "%04d-%02d-%02dT%02d:%02d:%02d", 1900+tmdata->tm_year, tmdata->tm_mon+1, tmdata->tm_mday, tmdata->tm_hour, tmdata->tm_min, tmdata->tm_sec);
	timestamp = timestamp_buff;

	msg += 16;
	timestamp[31] = '\0';
#else
	/* Jan 18 00:11:22 msg... */
	/* 01234567890123456 */
	if (len < 16 || msg[3] != ' ' || msg[6] != ' '
	 || msg[9] != ':' || msg[12] != ':' || msg[15] != ' '
	) {
		time(&now);
		timestamp = ctime(&now) + 4; /* skip day of week */
	} else {
		now = 0;
		timestamp = msg;
		msg += 16;
	}
	timestamp[15] = '\0';
#endif

#ifdef MSTC_COMMON_PATCH //__MTS__, TengChang, Category
	
	//Parse and filter log message by facility
	//Format: syslog/kernel: category:"facility" detail:""
	//For facilities belongs to security log, its facility includes "Sec_" prefix. For example, "Sec_Attack".
	char *str = msg;
	char *end;
	int length;
	char facility_buf[32]={0};
	int i, match;
	char *ptr, *facility;
	int securityLog = 0;
	int brcmLog = 0;
	const char *tempLogFilePath=NULL, *tempFlashLogFilePath=NULL;
	FILE *fp;
	int logType = SYSTEM_TYPE;

	if(!strncmp(str,"syslog: ", strlen("syslog: "))){
		str+=strlen("syslog: ");
	}
	else if(!strncmp(str,"kernel: ", strlen("kernel: "))){
		str+=strlen("kernel: ");
	}
	if(!strncmp(str,"category:\"", strlen("category:\""))){
		str+=strlen("category:\"");
		end = strstr(str, "\" detail:\"");
		length = end - str;
		strncpy(facility_buf,str,length);

		str = end + strlen("\" detail:\"");
		str[strlen(str)-1] = '\0';
	}
	else{
		strcpy(facility_buf,"System");
		brcmLog = 1;
	}

	facility = facility_buf;
	//Strip "Sec_" prefix and set securityLog flag
	if(!strncmp(facility,"Sec_", strlen("Sec_"))){
		facility+=strlen("Sec_");
		securityLog = 1;
	}

	if(isLogCategorySet)
	{
		//alert filter
		i=0;
		match=0;
		while(alertCategory[i][0]!='\0'){
			if(!strcmp(alertCategory[i],facility)){
				match = 1;
				break;
			}
			i++;
		}
		if(match){
			fp = fopen("/var/alarm", "w");
			fprintf(fp,"Alert message from router:\n");
			fprintf(fp, "%s %s: %s\n", timestamp, facility, str );
			fclose(fp);
			sendAlertByMail("Event alarm","/var/alarm",0);
		}

		//log filter
		i=0;
		match=0;
		while(logCategory[i][0]!='\0'){
			if(!strcmp(logCategory[i],facility)){
				match = 1;
				break;
			}
			i++;
		}
		if(!match)
			return;
	}
	
	//Add back the "Sec_" prefix
	if(OPT_circularlog && securityLog)
		facility-=strlen("Sec_");

/*
	while((ptr = strchr(facility,'_'))!=NULL)
		*ptr = ' ';
*/
#endif //__MTS__, TengChang, Category

	if (option_mask32 & OPT_small)
		sprintf(G.printbuf, "%s %s\n", timestamp, msg);
	else {
		char res[20];
		int length; // brcm
		if( parse_fac_prio_20(pri, res) )
		    return;
		length = (strlen(timestamp)+strlen(G.hostname)+strlen(res)+strlen(msg)+9);
		sprintf(G.printbuf, "%s %.64s %s %s %3i\n", timestamp, G.hostname, res, msg, length); // brcm
	}

	/* Log message locally (to file or shared mem) */
	log_locally(now, G.printbuf);
}

#ifdef SYSLOGD_MARK
static void timestamp_and_log_internal(const char *msg)
{
	/* -L, or no -R */
	if (ENABLE_FEATURE_REMOTE_LOG && !(option_mask32 & OPT_locallog))
		return;
	timestamp_and_log(LOG_SYSLOG | LOG_INFO, (char*)msg, 0);
}
#endif

/* tmpbuf[len] is a NUL byte (set by caller), but there can be other,
 * embedded NULs. Split messages on each of these NULs, parse prio,
 * escape control chars and log each locally. */
static void split_escape_and_log(char *tmpbuf, int len)
{
	char *p = tmpbuf;

	tmpbuf += len;
	while (p < tmpbuf) {
		char c;
		char *q = G.parsebuf;
		int pri = (LOG_USER | LOG_NOTICE);

		if (*p == '<') {
			/* Parse the magic priority number */
			pri = bb_strtou(p + 1, &p, 10);
			if (*p == '>')
				p++;
			if (pri & ~(LOG_FACMASK | LOG_PRIMASK))
				pri = (LOG_USER | LOG_NOTICE);
		}

		while ((c = *p++)) {
			if (c == '\n')
				c = ' ';
			if (!(c & ~0x1f) && c != '\t') {
				*q++ = '^';
				c += '@'; /* ^@, ^A, ^B... */
			}
			*q++ = c;
		}
		*q = '\0';

		/* Now log it */
		if (LOG_PRI(pri) < G.logLevel)
			timestamp_and_log(pri, G.parsebuf, q - G.parsebuf);
	}
}

#ifdef SYSLOGD_MARK
static void do_mark(int sig)
{
	if (G.markInterval) {
		timestamp_and_log_internal("-- MARK --");
		alarm(G.markInterval);
	}
}
#endif

/* Don't inline: prevent struct sockaddr_un to take up space on stack
 * permanently */
static NOINLINE int create_socket(void)
{
	struct sockaddr_un sunx;
	int sock_fd;
	char *dev_log_name;

	memset(&sunx, 0, sizeof(sunx));
	sunx.sun_family = AF_UNIX;

	/* Unlink old /dev/log or object it points to. */
	/* (if it exists, bind will fail) */
	strcpy(sunx.sun_path, BRCM_PATH_LOG); // brcm
	dev_log_name = xmalloc_follow_symlinks(BRCM_PATH_LOG); // brcm
	if (dev_log_name) {
		safe_strncpy(sunx.sun_path, dev_log_name, sizeof(sunx.sun_path));
		free(dev_log_name);
	}
	unlink(sunx.sun_path);

	sock_fd = xsocket(AF_UNIX, SOCK_DGRAM, 0);
	xbind(sock_fd, (struct sockaddr *) &sunx, sizeof(sunx));
	chmod(BRCM_PATH_LOG, 0666); // brcm

	return sock_fd;
}

#if ENABLE_FEATURE_REMOTE_LOG
static int try_to_resolve_remote(remoteHost_t *rh)
{
	if (!rh->remoteAddr) {
		unsigned now = monotonic_sec();

		/* Don't resolve name too often - DNS timeouts can be big */
		if ((now - rh->last_dns_resolve) < DNS_WAIT_SEC)
			return -1;
		rh->last_dns_resolve = now;
		rh->remoteAddr = host2sockaddr(rh->remoteHostname, 514);
		if (!rh->remoteAddr)
			return -1;
	}
	return socket(rh->remoteAddr->u.sa.sa_family, SOCK_DGRAM, 0);
}
#endif

static void do_syslogd(void) NORETURN;
static void do_syslogd(void)
{
	int sock_fd;
#if ENABLE_FEATURE_REMOTE_LOG
	llist_t *item;
#endif
#if ENABLE_FEATURE_SYSLOGD_DUP
	int last_sz = -1;
	char *last_buf;
	char *recvbuf = G.recvbuf;
#else
#define recvbuf (G.recvbuf)
#endif

	/* Set up signal handlers (so that they interrupt read()) */
	signal_no_SA_RESTART_empty_mask(SIGTERM, record_signo);
	signal_no_SA_RESTART_empty_mask(SIGINT, record_signo);
	//signal_no_SA_RESTART_empty_mask(SIGQUIT, record_signo);
	signal(SIGHUP, SIG_IGN);
// brcm begin
#ifdef BRCM_CMS_BUILD
	/* In CMS, daemons should ignore SIGINT */
	signal(SIGINT, SIG_IGN);
#endif
// brcm end
#ifdef SYSLOGD_MARK
	signal(SIGALRM, do_mark);
	alarm(G.markInterval);
#endif
	sock_fd = create_socket();

	if (ENABLE_FEATURE_IPC_SYSLOG && (option_mask32 & OPT_circularlog)) {
		ipcsyslog_init();
	}

	// timestamp_and_log_internal("syslogd started: BusyBox v" BB_VER);
	timestamp_and_log(LOG_SYSLOG | LOG_EMERG, "BCM96345 started: BusyBox v" BB_VER, 0);

	while (!bb_got_signal) {
		ssize_t sz;

#if ENABLE_FEATURE_SYSLOGD_DUP
		last_buf = recvbuf;
		if (recvbuf == G.recvbuf)
			recvbuf = G.recvbuf + MAX_READ;
		else
			recvbuf = G.recvbuf;
#endif

#ifdef MSTC_COMMON_PATCH //__MTS__, TengChang, read log from sock_fd to recvbuf
			sz = read(sock_fd, recvbuf, MAX_READ - 1);
			if (sz < 0){
				if (!bb_got_signal)
					bb_perror_msg("read from %s", BRCM_PATH_LOG); // brcm
				break;
			}

			while(1){
				if (sz == 0){
					do{
						sz = read(sock_fd, recvbuf, MAX_READ - 1);
					}while(sz == 0);

					if (sz < 0){
						break;
					}
				}

				// when (sz > 0)
				else{
					// Drop trailing '\n' and NULs (typically there is one NUL) //
					if (recvbuf[sz-1] != '\0' && recvbuf[sz-1] != '\n'){
						break;
					}
					else{
						sz--;
					}
				}
			}

			if (sz < 0){
				if (!bb_got_signal)
					bb_perror_msg("read from %s", BRCM_PATH_LOG); // brcm
				break;
			}
#else
 read_again:
		sz = read(sock_fd, recvbuf, MAX_READ - 1);
		if (sz < 0) {
			if (!bb_got_signal)
				bb_perror_msg("read from %s", BRCM_PATH_LOG); // brcm
			break;
		}

		/* Drop trailing '\n' and NULs (typically there is one NUL) */
		while (1) {
			if (sz == 0)
				goto read_again;
			/* man 3 syslog says: "A trailing newline is added when needed".
			 * However, neither glibc nor uclibc do this:
			 * syslog(prio, "test")   sends "test\0" to /dev/log,
			 * syslog(prio, "test\n") sends "test\n\0".
			 * IOW: newline is passed verbatim!
			 * I take it to mean that it's syslogd's job
			 * to make those look identical in the log files. */
			if (recvbuf[sz-1] != '\0' && recvbuf[sz-1] != '\n')
				break;
			sz--;
		}
#endif //__MTS__, TengChang, read log from sock_fd to recvbuf

#if ENABLE_FEATURE_SYSLOGD_DUP
		if ((option_mask32 & OPT_dup) && (sz == last_sz))
			if (memcmp(last_buf, recvbuf, sz) == 0)
				continue;
		last_sz = sz;
#endif
#if ENABLE_FEATURE_REMOTE_LOG
		/* Stock syslogd sends it '\n'-terminated
		 * over network, mimic that */
		recvbuf[sz] = '\n';

		/* We are not modifying log messages in any way before send */
		/* Remote site cannot trust _us_ anyway and need to do validation again */
		for (item = G.remoteHosts; item != NULL; item = item->link) {
			remoteHost_t *rh = (remoteHost_t *)item->data;

			if (rh->remoteFD == -1) {
				rh->remoteFD = try_to_resolve_remote(rh);
				if (rh->remoteFD == -1)
					continue;
			}
			/* Send message to remote logger, ignore possible error */
			/* TODO: on some errors, close and set G.remoteFD to -1
			 * so that DNS resolution and connect is retried? */
			sendto(rh->remoteFD, recvbuf, sz+1, MSG_DONTWAIT,
				&(rh->remoteAddr->u.sa), rh->remoteAddr->len);
		}
#endif
		if (/*!ENABLE_FEATURE_REMOTE_LOG ||*/ (option_mask32 & OPT_locallog)) {
			recvbuf[sz] = '\0'; /* ensure it *is* NUL terminated */
			split_escape_and_log(recvbuf, sz);
		}
	} /* while (!bb_got_signal) */

	// timestamp_and_log_internal("syslogd exiting"); // brcm
	timestamp_and_log(LOG_SYSLOG | LOG_EMERG, "syslogd exiting", 0);
	puts("syslogd exiting");
	if (ENABLE_FEATURE_IPC_SYSLOG)
		ipcsyslog_cleanup();
	kill_myself_with_sig(bb_got_signal);
#undef recvbuf
}

int syslogd_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int syslogd_main(int argc UNUSED_PARAM, char **argv)
{
	int opts;
	char OPTION_DECL;
#if ENABLE_FEATURE_REMOTE_LOG
	llist_t *remoteAddrList = NULL;
#endif

#ifdef BRCM_CMS_BUILD
    cmsLog_init(EID_SYSLOGD);
    cmsLog_setLevel(DEFAULT_LOG_LEVEL);
#endif

	INIT_G();

	/* No non-option params, -R can occur multiple times */
	opt_complementary = "=0" IF_FEATURE_REMOTE_LOG(":R::");
	opts = getopt32(argv, OPTION_STR, OPTION_PARAM);
#if ENABLE_FEATURE_REMOTE_LOG
	if (opts & OPT_remoteloglevel){ // -r  // brcm
		G.remotelogLevel = xatou_range(opt_r, 0, 7);
		if (G.remotelogLevel < LOG_EMERG)
		     G.remotelogLevel = LOG_ERR;
	}
	while (remoteAddrList) {
		remoteHost_t *rh = xzalloc(sizeof(*rh));
		rh->remoteHostname = llist_pop(&remoteAddrList);
		rh->remoteFD = -1;
		rh->last_dns_resolve = monotonic_sec() - DNS_WAIT_SEC - 1;
		llist_add_to(&G.remoteHosts, rh);
	}
#endif

#ifdef SYSLOGD_MARK
	if (opts & OPT_mark) // -m
		G.markInterval = xatou_range(opt_m, 0, INT_MAX/60) * 60;
#endif
	//if (opts & OPT_nofork) // -n
	//if (opts & OPT_outfile) // -O
	if (opts & OPT_loglevel) { // -l
		G.logLevel = xatou_range(opt_l, 0, 7); // brcm
		if (G.logLevel < LOG_EMERG)
		    G.logLevel = LOG_DEBUG;
	}
	//if (opts & OPT_small) // -S
#if ENABLE_FEATURE_ROTATE_LOGFILE
	if (opts & OPT_filesize) // -s
		G.logFileSize = xatou_range(opt_s, 0, INT_MAX/1024) * 1024;
	if (opts & OPT_rotatecnt) // -b
		G.logFileRotate = xatou_range(opt_b, 0, 99);
#endif
#if ENABLE_FEATURE_IPC_SYSLOG
	if (opt_C) // -Cn
		G.shm_size = xatoul_range(opt_C, 4, INT_MAX/1024) * 1024;
#endif

	/* If they have not specified remote logging, then log locally */
	if (ENABLE_FEATURE_REMOTE_LOG && !(opts & OPT_remotelog)) // -R
		option_mask32 |= OPT_locallog;

#ifdef MSTC_COMMON_PATCH //__MTS__, TengChang, Log Category
	FILE *fp=NULL, *fp2=NULL, *fp3=NULL;
	int i;
	time_t now;
	cmsMsg_init(EID_SYSLOGD, &msgHandle);

	if((fp = fopen("/var/log/log_category", "r"))==NULL)
	{
		isLogCategorySet = 0;
//		printf("open /var/log/log_category fail\n");
//		return EXIT_FAILURE;
	}
	else if((fp2 = fopen("/var/log/alert_category", "r"))==NULL)
	{
		isLogCategorySet = 0;
//		printf("open /var/log/alert_category fail\n");
//		return EXIT_FAILURE;
	}
	else
		isLogCategorySet = 1;

	if(isLogCategorySet)
	{
		i = 0;
		while(fgets(logCategory[i],32,fp)){
			logCategory[i][strlen(logCategory[i])-1] = '\0';
			i++;
		}
		fclose(fp);
		i = 0;
		while(fgets(alertCategory[i],32,fp2)){
			alertCategory[i][strlen(alertCategory[i])-1] = '\0';
			i++;
		}
		fclose(fp2);
	}

	if((fp3 = fopen("/var/log/email_settings", "r"))==NULL)
	{
		printf("open /var/log/email_settings fail\n");
		/* return EXIT_FAILURE; */
	}
	else
	{
		fgets(emailCfg.mailServer,128,fp3);
		emailCfg.mailServer[strlen(emailCfg.mailServer)-1] = '\0';
		fgets(emailCfg.mailSubject,128,fp3);
		emailCfg.mailSubject[strlen(emailCfg.mailSubject)-1] = '\0';
		fgets(emailCfg.mailFrom,128,fp3);
		emailCfg.mailFrom[strlen(emailCfg.mailFrom)-1] = '\0';
		fgets(emailCfg.logAddress,128,fp3);
		emailCfg.alertAddress[strlen(emailCfg.logAddress)-1] = '\0';
		fgets(emailCfg.alertAddress,128,fp3);
		emailCfg.alertAddress[strlen(emailCfg.alertAddress)-1] = '\0';
		fscanf(fp3,"%d\n",&emailCfg.alertInterval);
		now = time(NULL);
		last_alert = now - emailCfg.alertInterval - 1;
		fscanf(fp3,"%d\n",&emailCfg.isAuth);
		if(emailCfg.isAuth){
			fgets(emailCfg.user,128,fp3);
			emailCfg.user[strlen(emailCfg.user)-1] = '\0';
			fgets(emailCfg.password,128,fp3);
			emailCfg.password[strlen(emailCfg.password)-1] = '\0';
		}
		fclose(fp3);
	}

	/*
	printf("**** logCategory ****\n");
	i=0;
	while(logCategory[i][0]!='\0'){
		printf("%s\n", logCategory[i]);
		i++;
	}
	printf("**** alertCategory ****\n");
	i=0;
	while(alertCategory[i][0]!='\0'){
		printf("%s\n", alertCategory[i]);
		i++;
	}*/
#endif //__MTS__, TengChang, Log Category

	/* Store away localhost's name before the fork */
	G.hostname = safe_gethostname();
	*strchrnul(G.hostname, '.') = '\0';

	if (!(opts & OPT_nofork)) {
		bb_daemonize_or_rexec(DAEMON_CHDIR_ROOT, argv);
	}

#ifdef BRCM_CMS_BUILD
    if (setsid() == -1)
    {
       cmsLog_error("Could not detach from terminal");
    }
    else
    {
       cmsLog_debug("detached from terminal");
    }
    /* set signal masks */
    signal(SIGPIPE, SIG_IGN); /* Ignore SIGPIPE signals */
#endif

	//umask(0); - why??
	write_pidfile("/var/run/syslogd.pid");
	do_syslogd();
	/* return EXIT_SUCCESS; */
}

/* Clean up. Needed because we are included from syslogd_and_logger.c */
#undef DEBUG
#undef SYSLOGD_MARK
#undef SYSLOGD_WRLOCK
#undef G
#undef GLOBALS
#undef INIT_G
#undef OPTION_STR
#undef OPTION_DECL
#undef OPTION_PARAM
