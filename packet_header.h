#include <sys/types.h>

enum packet_type {
        NORMAL_PKT = 1,
        EOF_PKT    = 2,
};

//#define   MSS      1430

typedef struct packet_t_struct{
        int            packet_type;
        int            buf_bytes;
        int            sequencenumber;
	off_t          f_offset;
        unsigned char  data[0];
} packet_t;

#define    MSS        (1458 - sizeof(packet_t))

typedef struct eof_packet_t_struct {
        unsigned short packet_type;
        int  eof;
        long file_sz;
} eof_packet_t;

typedef struct ack_struct{
        int            sequencenumber;
        int            buf_len;
        long           tot_bytes;
	off_t          f_offset;
} ack_t;
