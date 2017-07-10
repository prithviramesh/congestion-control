#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/time.h> 
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <assert.h>
#include <sys/select.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include "packet_header.h"

#define  NUM_STATES   3
#define  SLOW_START   0
#define  FAST_RECV    1
#define  CONG_AVOID   2
#define  EVENTS       5
#define	 INVALID_EVT  5

int sequencenumber;
int recv_sockfd;
unsigned char producer_exiting = 0;
 
enum event {
     NEW_ACK       = 0,
     TIMEOUT       = 1,
     DUPACK        = 2,
     DUPACKCNT_3   = 3,
     CNWD_GR_SSTHR = 4
};
 
typedef void (*action_t)(void);
 
typedef struct state_machine {
        int curr_state;
        action_t act[4];
        int next_state;
}state_machine_t;
 
int cWnd;
int ssThresh;
unsigned short dupAckCount;
long totalBytes;
int  state;
int  unExpectedevt;
enum event newEvent;

int   maxcWnd = 0;
int   mincWnd = MSS;  

struct sockaddr_in si_other;
socklen_t  si_other_addrlen;
struct sockaddr_in si_local;
socklen_t  si_other_addrlen;

long readbytes = 0;
long sentbytes = 0;
long lostBytes = 0;

long bytesReceivedByClient; 

typedef struct arg_struct{
	char * filename;
	long numBytes;
} arg_t;

//pthread_t doTimerid;
pthread_t produceid; 
pthread_t recvAckid; 
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;

ack_t  prev_ack;
enum event  timerEvent;  // Can take only TIMEOUT

unsigned char *file_base_addr;
long   file_size;
off_t  offset;
off_t  file_off_to_read;
off_t  offset_just_seen;
 
static inline
void clearDupAck(void)
{
        dupAckCount = 0;
}
 
static inline
void incrDupAckCount(void)
{
        dupAckCount++;
}
 
static inline
void reset_totalBytes(void)
{
	pthread_mutex_lock(&m);
	totalBytes = 0;
	if (cWnd > maxcWnd) {
		maxcWnd = cWnd;
	}
        if (cWnd < mincWnd) {
	        mincWnd = cWnd;
	}
	
	pthread_mutex_unlock(&m);
}
static inline
void incrcWndByMSS(void)
{
    cWnd = cWnd + MSS;
    if (cWnd >= ssThresh) {
	cWnd = ssThresh - 1024;
    }
    reset_totalBytes(); 
}


static inline
void resetcWnd(void)
{
      cWnd = 1 * MSS;
      reset_totalBytes(); 
}
 
static inline
void setcWndToSSThresh(void)
{
        cWnd = ssThresh;
    	reset_totalBytes(); 

}
 
static inline
void setSsThresh(void)
{
        ssThresh = cWnd / 2;
}
 
static inline
void resetSsThresh(void)
{
        ssThresh = 64 * 1024;
}
 
static inline
void setcWndToMssTimesMssOvercWnd(void)
{

	 cWnd = cWnd + (MSS * MSS)/cWnd;
	if (cWnd >= ssThresh)
	{
		cWnd = ssThresh - 1024;
	}
    	
	reset_totalBytes(); 
}
 
 
static inline
void setcWndTossThreshPlus3MSS(void)
{
        cWnd = ssThresh + (3 * MSS);
    	reset_totalBytes(); 
}
 
static inline
void start_machine(void)
{
        resetcWnd();
        resetSsThresh();
        clearDupAck();

     	file_off_to_read  = 0;
	bytesReceivedByClient = 0;

        state = SLOW_START;
}

static inline 
void resetTransmitPtrs(void)
{
	pthread_mutex_lock(&m);
	file_off_to_read = offset_just_seen;
	sequencenumber = prev_ack.sequencenumber + 1;
	lostBytes += (readbytes - bytesReceivedByClient);
	readbytes = bytesReceivedByClient;
	sentbytes = bytesReceivedByClient;
	pthread_mutex_unlock(&m);
}
 
static inline
void InvalidEvt(void)
{
        unExpectedevt = 1;
}

state_machine_t st_table[EVENTS][NUM_STATES] = {
    {{SLOW_START, {incrcWndByMSS, clearDupAck, 0, 0}, SLOW_START}, {FAST_RECV, {setcWndToSSThresh, clearDupAck, 0, 0}, CONG_AVOID}, {CONG_AVOID, {setcWndToMssTimesMssOvercWnd, clearDupAck, 0, 0}, CONG_AVOID}}, // New ACK
    {{SLOW_START, {setSsThresh, resetcWnd, clearDupAck, resetTransmitPtrs}, SLOW_START}, {FAST_RECV, {setSsThresh, resetcWnd, clearDupAck,resetTransmitPtrs}, SLOW_START}, {CONG_AVOID, {setSsThresh, resetcWnd, clearDupAck, resetTransmitPtrs}, SLOW_START}}, // Timeout
    {{SLOW_START,  {incrDupAckCount, 0, 0, resetTransmitPtrs}, SLOW_START}, {FAST_RECV, {incrcWndByMSS, 0, 0, 0}, FAST_RECV}, {CONG_AVOID, {incrDupAckCount, 0, 0, resetTransmitPtrs}, CONG_AVOID}}, // Dup Ack
    {{SLOW_START,  {setSsThresh, setcWndTossThreshPlus3MSS, 0, 0}, FAST_RECV}, {FAST_RECV, {InvalidEvt, 0, 0, 0}, FAST_RECV}, {CONG_AVOID, {setSsThresh, setcWndTossThreshPlus3MSS, 0, 0}, FAST_RECV}}, // Dup Ack = 3        
    {{SLOW_START, {resetcWnd, resetSsThresh, clearDupAck, 0}, CONG_AVOID}, {FAST_RECV, {InvalidEvt, 0, 0, 0}, FAST_RECV}, {CONG_AVOID, {InvalidEvt, 0, 0, 0}, CONG_AVOID}} // CWND > SSTHRESH
};


static inline
setSeqNum(int seq_num)
{
	pthread_mutex_lock(&m);
	sequencenumber = seq_num;
	pthread_mutex_unlock(&m);
}

static inline
int getAndIncrseqNumber(void)
{
	int seq_num;

	pthread_mutex_lock(&m);
	seq_num = sequencenumber;
	sequencenumber++;
	pthread_mutex_unlock(&m);

	return seq_num;
}

static inline 
void incrTotalBytes(long numBytes)
{
	pthread_mutex_lock(&m);
	totalBytes += numBytes;
	pthread_mutex_unlock(&m);
}


static inline
int get_totalBytes(void)
{
	int rv;
	pthread_mutex_lock(&m);
	rv = totalBytes;
	pthread_mutex_unlock(&m);
	return rv;
}

static void
signal_handler(int sig, siginfo_t *si, void *unused)
{
	return;
}

static void
signal_int_handler(int sig, siginfo_t *si, void *unused)
{
        printf("Totalsize %ld bytes Read %ld Total xfered %ld Tot Received %ld Bytes Re-transmitted %ld\n", 
				file_size, readbytes, sentbytes, bytesReceivedByClient, lostBytes);
	printf("Bye Bye !!\n");
	exit(0);
}


enum event getEvent(ack_t* ack)
{
	int   seq_num;
	int   buf_len;
	long  tot_bytes;
	off_t offst;

        if (ack == NULL) 
		return INVALID_EVT;

	if( timerEvent == TIMEOUT) {
	      timerEvent = INVALID_EVT;
	      return TIMEOUT;
	} 
	else if(cWnd >= ssThresh)
	{
		return CNWD_GR_SSTHR;
	}
	else if(dupAckCount == 3)
	{
      		pthread_mutex_lock(&m);
		file_off_to_read = offset_just_seen;
      		pthread_mutex_unlock(&m);
		setSeqNum(prev_ack.sequencenumber+1);

		return DUPACKCNT_3;
	}
	else 
	{
		seq_num   = ntohl(ack->sequencenumber);
		buf_len   = ntohl(ack->buf_len);
		offst     = ntohl(ack->f_offset);
		tot_bytes = ntohl(ack->tot_bytes);

		if((prev_ack.sequencenumber == seq_num) && 
		   (prev_ack.tot_bytes == tot_bytes) && 
		   (prev_ack.f_offset == offst) && 
		   (prev_ack.buf_len == buf_len))
		{
			return DUPACK;			
		}
		
	        prev_ack.sequencenumber = seq_num;
		prev_ack.buf_len = buf_len;
		prev_ack.tot_bytes = tot_bytes;
		prev_ack.f_offset = offst;

		offset_just_seen = offst;
		bytesReceivedByClient = tot_bytes; 
		reset_totalBytes();
		return NEW_ACK;
	}

	return INVALID_EVT;
	
}

void
setup_file_for_read(char* filename)
{
        int fd;
        struct stat sb;

        fd = open(filename, 0, O_RDONLY);
        if(fd < 0)
        {
                perror("Error w/ Opening File");
                exit(1);
        }

        if (fstat(fd, &sb) == -1) {
            /* To obtain file size */
            perror("Fstat error");
            exit(1);
        }

        file_size = sb.st_size;

        file_base_addr = mmap(NULL, file_size, PROT_READ, MAP_SHARED, fd, 0);
        if (file_base_addr == MAP_FAILED) {
                perror("MMAP Failed");
                exit(1);
        }

        return;
}

void*
producer(void* arg)
{
	arg_t* args = (arg_t *) arg;
        packet_t *p = malloc(sizeof(packet_t)+MSS);
	int len = 0;
	int numbytes = 0;
	long bytestoread = args->numBytes;
	int i;

	setup_file_for_read(args->filename);

	if (bytestoread < file_size) {
	    // Clip the file size to what ever need to be rad
	    file_size = bytestoread;
	}


	sentbytes = 0;
	assert(p != NULL);

	while(1) 
	{
		while(1) {
		        int  rv;
			rv = get_totalBytes();
			if (rv >= cWnd) {	
				pthread_yield();
		        } else {
			    break;
			}
		}

	        pthread_mutex_lock(&m);
	        offset = file_off_to_read;
		pthread_mutex_unlock(&m);

                if ((offset+MSS) < file_size) {
            		len = MSS;
        	} else {
            	    if (offset < file_size) {
                        len = (file_size - offset);
                    } else {
                        len = 0;
                    }
               }

	       if (len > 0) {
		   memset(p->data, 0, MSS);
	           p->packet_type = NORMAL_PKT;
	           readbytes += len;
		   p->buf_bytes = htonl(len);
		   p->sequencenumber = htonl(getAndIncrseqNumber());
		   p->f_offset = htonl(offset);
	           memcpy(p->data, file_base_addr+offset, len);

		   if ((numbytes = sendto(recv_sockfd, p, sizeof(packet_t)+MSS, MSG_WAITALL, 
			(struct sockaddr *)&si_other, si_other_addrlen)) == -1) {
			    perror("producer 1: sendto");
			    exit(1);
		   }

		   incrTotalBytes(numbytes);
	           sentbytes += ntohl(p->buf_bytes);

	           pthread_mutex_lock(&m);
		   file_off_to_read += len;
		   pthread_mutex_unlock(&m);
	        }

	        if (len == 0) {
		     eof_packet_t *p1;

		     free(p);
		     p1 = malloc(sizeof(eof_packet_t));
		     p1->packet_type = EOF_PKT;
		     p1->eof = htonl(1);
		     p1->file_sz = htonl(file_size);
		     if ((numbytes = sendto(recv_sockfd, p1, sizeof(packet_t), MSG_WAITALL, 
					(struct sockaddr *)&si_other, si_other_addrlen)) == -1) {
			    perror("producer 2: sendto");
			    exit(1);
		     }
		     free(p1);
                     printf("Totalsize %ld bytes Read %ld Total xfered %ld Tot Received %ld Bytes Re-transmitted %ld\n", 
				file_size, readbytes, sentbytes, bytesReceivedByClient, lostBytes);
		     break;
		}
	}

	producer_exiting = 1;
	pthread_kill(recvAckid, SIGUSR1);
	//pthread_kill(doTimerid, SIGUSR1);

	printf("Producer done and hence exiting \n");
	printf("Congestion window Max %d Min: %d Last Value: %d\n", maxcWnd, mincWnd, cWnd);

	munmap(file_base_addr, file_size);

	return NULL;
}

void*
recvAck(void* arg)
{
	ack_t recv_ack;
	state_machine_t  s;
       	long numBytes;
        struct sigaction sa;
        struct timeval tv;
	fd_set rd_fd;
	int retval;
 		

        sa.sa_flags = SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sa.sa_sigaction = signal_handler; 
        if (sigaction(SIGUSR1, &sa, NULL) == -1) {
            perror("sigaction");
	    exit(EXIT_FAILURE);
        }
        printf("recvAck started \n"); 

		
	FD_ZERO(&rd_fd);
	while(!producer_exiting) {
	    tv.tv_sec = 0;
    	    tv.tv_usec = 5000;

	    FD_SET(recv_sockfd, &rd_fd);
	    retval = select(recv_sockfd+1, &rd_fd, NULL, NULL, &tv);

	    if(producer_exiting)
	       continue;
	   
 	    if(FD_ISSET(recv_sockfd, &rd_fd)) {
		FD_CLR(recv_sockfd, &rd_fd);
	        if((numBytes = recvfrom(recv_sockfd, &recv_ack, sizeof(ack_t), MSG_WAITALL, NULL, 0)) != 
				sizeof(ack_t))
	        {
		        if ((errno != EAGAIN) && (errno != EWOULDBLOCK) && (errno != EINTR)) { 
				perror("Recieve error");
				exit(1);
			}
	        }
	     } else {
		timerEvent = TIMEOUT;
	     }

	     if(producer_exiting)
	         continue;
	     
	     newEvent = getEvent(&recv_ack);
	     s.curr_state = state;
                 
	     s = st_table[newEvent][state];
 	
	     // Take actions
             if(s.act[0] != 0) {
                 (s.act[0])();
             }
             if(s.act[1] != 0) {
                 (s.act[1])();
             }
             if(s.act[2] != 0) {
                  (s.act[2])();
             }
 	     if(s.act[3] != 0) {
                  (s.act[3])();
             }

             // Set new state
	     state = s.next_state;
        }
        printf("Receive ack exiting\n");
	
	return NULL;
}

int 
setup_network(unsigned short int hostUDPport, char* hostname)
{
	int flags;


        if ((recv_sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
	     perror("Recieve error");
	     exit(1);
	}

        memset((char *) &si_local, 0, sizeof(si_local));
	si_local.sin_family = AF_INET;
	si_local.sin_port = htons(hostUDPport);  
	si_local.sin_addr.s_addr = INADDR_ANY;
	if((bind(recv_sockfd, (struct sockaddr *)&si_local, sizeof(si_local))) < 0) {
             printf("bind error with port %d %s\n", errno, strerror(errno));
        }
   
        memset((char *) &si_other, 0, sizeof(si_other));
        si_other.sin_family = AF_INET;
        si_other.sin_port = htons(hostUDPport);
        if (inet_aton(hostname, &si_other.sin_addr) == 0) {
            fprintf(stderr, "inet_aton() failed\n");
            exit(1);
        }
        si_other_addrlen = sizeof(struct sockaddr_in);

       return 0;
}

void 
reliablyTransfer (char* hostname, unsigned short int hostUDPport, char* filename, long numBytes)
{ 
	arg_t* args = malloc(sizeof(arg_t));
	
	
	assert(args != NULL);
	args->filename = filename;
	args->numBytes = numBytes;
			
        //pthread_create(&doTimerid, NULL, doTimer, NULL);
        pthread_create(&produceid, NULL, producer, (void*)args);
        pthread_create(&recvAckid, NULL, recvAck, NULL);

        pthread_join(produceid, NULL);
        pthread_join(recvAckid, NULL);
        //pthread_join(doTimerid, NULL);

	for (;;) {
	    if(producer_exiting)
		break;
	}
}

void 
init(unsigned short int hostUDPport, char* hostname)
{

	setup_network(hostUDPport, hostname);
 	start_machine(); // set Start state
	sequencenumber = 1;
}

int main(int argc, char** argv)
{
	unsigned short int udpPort;
	long numBytes;
        struct sigaction sa;
	
	if(argc != 5)
	{
		fprintf(stderr, "usage: %s receiver_hostname receiver_port filename_to_xfer bytes_to_xfer\n\n", argv[0]);
		exit(1);
	}

        sa.sa_flags = SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sa.sa_sigaction = signal_int_handler; 
        if (sigaction(SIGINT, &sa, NULL) == -1) {
            perror("sigaction");
	    exit(EXIT_FAILURE);
        }

	
	udpPort = (unsigned short int)atoi(argv[2]);
	numBytes = atol(argv[4]);
	
	init(udpPort, argv[1]);
	reliablyTransfer(argv[1], udpPort, argv[3], numBytes);

	return 0;
}
