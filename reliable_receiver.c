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
#include <assert.h>
#include "packet_header.h"

int  sequencenumber;
int  buf_len;
off_t offst = 0;

unsigned int lossCtr = 0;

enum state {
	GOOD_STATE = 1,
	LOSSY_STATE = 2
};

struct sockaddr_in si_me, si_other;
int recv_sockfd;
struct sockaddr_in peer_addr;
socklen_t peer_addr_len = sizeof(struct sockaddr_in);
unsigned short int port_num;
packet_t *recv_packet;

enum state getState(int sequencenum, off_t off)
{
	if ((sequencenum == (sequencenumber + 1)) && (off == offst)) { 
	    return GOOD_STATE;
	} else {
	    return LOSSY_STATE;
	}
}

int setup_network(unsigned short int UDPport)
{

      if ((recv_sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))==-1)
      {
              perror("Socket call failed\n");
              exit(1);
      }

      memset((char *) &si_me, 0, sizeof(si_me));
      si_me.sin_family = AF_INET;
      si_me.sin_addr.s_addr = htonl(INADDR_ANY);
      si_me.sin_port = htons(UDPport);
      if (bind(recv_sockfd, (struct sockaddr*)&si_me, sizeof(si_me)) == -1) {
                perror("Bind Error");
		exit(1);
       }
       port_num = UDPport;

       return 0;
}

void* 
producer(void* arg)
{
	char* destfile = (char *)arg;
        ack_t  ack_now;
	int num;
	FILE*  dest;
	int recvbytes;
	long writebytes = 0;
	long totalRecv = 0;
	int state = 0;
	int prev_state = 0;
	
	recv_packet = malloc(sizeof(packet_t)+MSS);
	
	dest = fopen(destfile, "wb");
	if (dest == NULL) {
		perror("Fopen failed\n");
		exit(1);
	}

	offst = 0;
	while(1)
	{
		if((recvbytes = recvfrom(recv_sockfd, recv_packet, sizeof(packet_t)+MSS, MSG_WAITALL,  
			(struct sockaddr *)&peer_addr, &peer_addr_len)) == -1)
		{
			perror("Packet recieve error");
			exit(1);
		}

		totalRecv += recvbytes;
		if (recv_packet->packet_type == EOF_PKT) {
			eof_packet_t *ep = malloc(sizeof(eof_packet_t));;
			memcpy(ep, &recv_packet, sizeof(eof_packet_t));
			printf("Total bytes written %ld File size %d EOF %d Final Local State %d Loss Ctr %d\n", 
						writebytes, ntohl(ep->file_sz), ntohl(ep->eof), state, lossCtr);
                	fflush(dest);
			fclose(dest);
			free(ep);
			break;
		}

	        switch(getState(ntohl(recv_packet->sequencenumber), ntohl(recv_packet->f_offset))) {
		case GOOD_STATE:
			{
			     sequencenumber++;
		             buf_len = ntohl(recv_packet->buf_bytes);
			     if((num = fwrite(recv_packet->data, 1, buf_len, dest)) == -1) {
                        			perror("fwrite");
                        			exit(1);

                	     }
			     offst = ftell(dest);

			     writebytes += num;
			     ack_now.buf_len = recv_packet->buf_bytes; 
			     ack_now.sequencenumber = htonl(sequencenumber);
			     ack_now.tot_bytes = htonl(writebytes);
			     ack_now.f_offset = htonl(ftell(dest));
			     if (peer_addr.sin_port != htons(port_num)) {
				    peer_addr.sin_port = htons(port_num);
			     }
			     if((num = sendto(recv_sockfd, &ack_now, sizeof(ack_t), MSG_WAITALL, 
						(struct sockaddr *)&peer_addr, peer_addr_len)) == -1)
			     {
					printf("Peer Addr %s Peer port %d Peer AF %d\n", 
						inet_ntoa(peer_addr.sin_addr), ntohs(peer_addr.sin_port),
						ntohs(peer_addr.sin_family));
	     					perror("Send ack error 1");
	     					exit(1);
	 		     }
			}
			state = GOOD_STATE;
			prev_state = GOOD_STATE;
			break;
		 case LOSSY_STATE:
		        {
			        if (prev_state == GOOD_STATE) {
				     lossCtr++;
				}

				ack_now.buf_len = htonl(buf_len);
				ack_now.tot_bytes = htonl(writebytes);
				ack_now.sequencenumber = htonl(sequencenumber);
			        ack_now.f_offset = htonl(ftell(dest));

				if((num = sendto(recv_sockfd, &ack_now, sizeof(ack_t), 0, 
						(struct sockaddr *)&peer_addr, peer_addr_len)) == -1)
				{
	     					perror("Send ack error 1");
	     					exit(1);
	 			}
				state = LOSSY_STATE;
				prev_state = LOSSY_STATE;
			}
			break;
		}
	}
	printf("Producer done and exiting\n");
	return NULL;
}

void 
reliablyReceive(unsigned short int myUDPport, char* destinationFile)
{
	pthread_t produceid;

	char* file = malloc(strlen(destinationFile) + 1);

	memset(file, 0, strlen(destinationFile)+1);
	memcpy(file, destinationFile, strlen(destinationFile));

	if (pthread_create(&produceid, NULL, &producer, file) < 0) {
			printf("PThread create error\n");
			perror("");
	}

	pthread_join(produceid, NULL);

	return;
}

void 
init(unsigned short int udpPort)
{
        int i;

	setup_network(udpPort);
	sequencenumber = 0;
}



int main(int argc, char** argv)
{
	unsigned short int udpPort;
	
	
	if(argc != 3)
	{
		fprintf(stderr, "usage: %s UDP_port filename_to_write\n\n", argv[0]);
		exit(1);
	}
	


	udpPort = (unsigned short int)atoi(argv[1]);
 	init(udpPort);
	reliablyReceive(udpPort, argv[2]);

	return 0;
}
