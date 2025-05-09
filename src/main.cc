#include "include.h"
#include "icmp.h"
#include "ipv4.h"
#include "utils.h"
#include "ethernet.h"
#include <map>
/* this normally comes from the pcap.h header file, but we'll just be using
 * a few specific pieces, so we'll add them here
 *
 * The first record in the file contains saved values for some
 * of the flags used in the printout phases of tcpdump.
 */

/* every pcap file starts with this structure */
struct pcap_file_header {
	bpf_u_int32 magic;
	unsigned short version_major;
	unsigned short version_minor;
	bpf_int32 thiszone;	/* gmt to local correction; this is always 0 */
	bpf_u_int32 sigfigs;	/* accuracy of timestamps; this is always 0 */
	bpf_u_int32 snaplen;	/* max length saved portion of each pkt */
	bpf_u_int32 linktype;	/* data link type (LINKTYPE_*) */
};

/*
 * Generic per-packet information, as supplied by libpcap.
 * this is the second record in the file, and every packet starts
 * with this structure (followed by the packet date bytes)
 */
struct pcap_pkthdr {
	bpf_u_int32 ts_secs;		/* time stamp */
	bpf_u_int32 ts_usecs;	/* time stamp */
	bpf_u_int32 caplen;	/* length of portion present */
	bpf_u_int32 len;	/* length of this packet (off wire) */
};

char tcp_flag_string[] = "FSRPAU"; 
uint32_t my_ipv4_addr;
uint32_t mask_length;
int debug = 0;
int resolveDNS = 1;
int reverseEndian = 0;

// since we're reading & writing to the same file, we need 2 different FDs 
// so that the write one can have O_APPEND that will always seek to the EOF to write so we don't overwrite incoming packets
int pcap_fd_read;
int pcap_fd_write;
struct pcap_file_header pfh;

/* iov is the array of iovecs that will be used to write the REPLY packets to the pcap file
 * e.g. iov[0] = ethernet, iov[1] = ipv4, iov[2] = icmp, ..., iov[iov_cnt]. 
 * Note that iov[i] only contains the HEADER at the i-th layer. The data of the i-th packet are the i+1-th, i+2-th, etc. packets due to encapsulation.
 * iov_cnt is the number of iovecs in the array used to write ONE COMPLETE (all protocols) REPLY packet to the pcap file
 * iov_cnt is incremented each time a new protocol is added to the REPLY packet, and reset to 0 each time a new COMPLETE REPLY packet is written.
 */
struct iovec iov[10];
int iov_cnt = 0;

/* MAC to IPv4 and IPv6 */
std::map<uint64_t, uint32_t> arp_cache_v4;
std::map<uint64_t, uint64_t> arp_cache_v6;

/* write pcap header + `len` bytes from `data` to the packet capture file `pcap_fd` */
void write_pcap() {
    struct pcap_pkthdr *pcap_hdr = (struct pcap_pkthdr *)malloc(sizeof(struct pcap_pkthdr));
    // write the pcap header
    struct timeval tv;
    long long ret = gettimeofday(&tv, NULL);
    if (ret == -1) {
        perror("gettimeofday");
        return;
    }

    /* calculate the total length of the PCAP packet */
    long long total_len = 0;
    for (int i = 1 ; i < iov_cnt; i++) total_len += iov[i].iov_len;

    pcap_hdr->ts_secs = tv.tv_sec;
    pcap_hdr->ts_usecs = tv.tv_usec;
    pcap_hdr->caplen = total_len;   // length WITHOUT the pcap header
    pcap_hdr->len = total_len;

    iov[0].iov_base = pcap_hdr;
    iov[0].iov_len = sizeof(struct pcap_pkthdr);
    total_len += sizeof(struct pcap_pkthdr);

    ret = writev(pcap_fd_write, iov, iov_cnt);
    if (ret != total_len) { // MUST write all bytes to consider it a success
        perror("writev");
        return;
    }

    /* reset the iov array */
    for (int i = 0 ; i < iov_cnt; i++) {
        free(iov[i].iov_base);
        iov[i].iov_base = NULL;
        iov[i].iov_len = 0;
    }
    iov_cnt = 1;
}

/* open .dmp pcap file (2 separate fds for read and write) and verify its header */
void setup(char *filename) {
    // get pcap filename in the form X.X.X.0_masklength first, then open pcap it
    int l = strlen(filename);
    char *_filename = (char*)malloc(l + 6);
    if (get_ip_and_filename(filename, _filename, &my_ipv4_addr) != 0) {
        exit(123);
    }
    pcap_fd_read = open(_filename, O_RDONLY);
    pcap_fd_write = open(_filename, O_WRONLY | O_APPEND);
    if (pcap_fd_read < 0 || pcap_fd_write < 0) {
        perror(_filename);
        exit(1);
    }

    /* read the pcap_file_header at the beginning of the file, check it, then print as requested */
    int ret = read(pcap_fd_read, &pfh, sizeof(pfh));
    if (ret <= 0) {
        perror("read");
        exit(1);
    }
    if (ret < (int)sizeof(pfh)) {
        printf("truncated pcap header: only %d bytes\n", ret);
        exit(1);
    }
    if (pfh.magic == PCAP_MAGIC_LITTLE) { // packet is in little-endian
        reverseEndian = 0;
    } else if (pfh.magic == PCAP_MAGIC_BIG) {
        reverseEndian = 1;
    } else {
        fprintf(stderr, "invalid magic number: 0x%08x\n", pfh.magic);
        exit(1);
    }

    // reverse the bytes if the packet was originally in big-endian
    if (reverseEndian) {
        reverse_assign(&pfh.version_major, sizeof(pfh.version_major));
        reverse_assign(&pfh.version_minor, sizeof(pfh.version_minor));
        reverse_assign(&pfh.linktype, sizeof(pfh.linktype));
    }

    if (pfh.version_major != PCAP_VERSION_MAJOR || pfh.version_minor != PCAP_VERSION_MINOR) {
        fprintf(stderr, "invalid pcap version: %d.%d\n", pfh.version_major, pfh.version_minor);
        exit(1);
    }

    printf("header magic: %x\n", PCAP_MAGIC_LITTLE);
    printf("header version: %d %d\n", pfh.version_major, pfh.version_minor);
    printf("header linktype: %d\n\n", pfh.linktype);
}

void loop() {
    unsigned char* in_packet = (unsigned char*)malloc(2 << 20);
	/* now read each packet in the file */
	while (1) {

		/* read the pcap_packet_header, then print as requested */
        struct pcap_pkthdr pph;
        int ret = read(pcap_fd_read, &pph, sizeof(pph));

        /* get cur pos in read */
        off_t cur_pos = lseek(pcap_fd_read, 0, SEEK_CUR);
        if (cur_pos == -1) {
            perror("lseek");
            exit(1);
        }

        if (ret < 0) {
            perror("read");
            exit(1);
        } else if (ret == 0) {       // EOF, wait a little bit before trying again            
            usleep(10000);
            continue;   
        }
        else if (ret < (int)sizeof(pph)) {
            printf("truncated packet header: only %d bytes\n", ret);
            exit(1);
        }

        if (reverseEndian) {
            reverse_assign(&pph.caplen, sizeof(pph.caplen));
            reverse_assign(&pph.ts_secs, sizeof(pph.ts_secs));
            reverse_assign(&pph.ts_usecs, sizeof(pph.ts_usecs));
        }

        // now read the actual packet
        ret = read(pcap_fd_read, in_packet, pph.caplen);        
        if (ret < 0) {
            perror("read");
            exit(1);
        } else if (ret == 0) break;    // EOF
        else if (ret < (int)pph.caplen) {
            printf("truncated packet: only %d bytes\n", ret);
            exit(1);
        }
        in_packet[ret] = '\0';
        
        if (debug) {
            puts("[+] Received a packet.");
        }
        if (reverseEndian) {
            reverse_assign(&pph.ts_secs, sizeof(pph.ts_secs));
            reverse_assign(&pph.ts_usecs, sizeof(pph.ts_usecs));
            reverse_assign(&pph.len, sizeof(pph.len));
        }

         // some format printing stuffs
        char *tmp = (char*)malloc(30);
        if (!tmp) {
            perror("malloc:");
            exit(123);
        }
        long double f = pph.ts_secs;
        f += (pph.ts_usecs / 1000000.0);
        ret = sprintf(tmp, "%.9Lf", f);
        tmp[ret] = '\0';
		printf("%20s\t%d\t%d\t", tmp, pph.caplen, pph.len);
        free(tmp);

        iov_cnt = 1;
        if (pfh.linktype == 1) { 
		    ret = process_ethernet(in_packet, iov_cnt);
            if (ret < 0) {
                fprintf(stderr, "process_ethernet failed.\n");
                break;
            } else if (ret == 0) {
                fprintf(stderr, "process_ethernet returned with 0.\n"); 
                continue;
            }
            write_pcap();
        } 
	}
}

void print_help() {
    printf("Usage: ./twig [-d] [-d] [-d] [-i] IPv4addr_masklength\n");
    printf("\t-i:\t{IPv4addr}_{mask length} e.g. 192.168.1.10_24.\n");
    printf("\t\tTwig should assume that it has IP address 192.168.1.10/24 on that interface and that it should use the following file for reading and writing packets: 192.168.1.0 24.dmp\n");
    printf("\t-d:\tDebugging flag. Can be used up to 3 times to increase verbosity. e.g. ./twig -d -d -d -i 192.168.1.10_24.\n");
    printf("\t-h:\tPrint this help message.\n");
    exit(0);
}


int main(int argc, char *argv[])    
{
	char *filename = NULL;
    if (argc < 2) {
        fprintf(stderr, "No interface provided. Check -i option.\n");
        print_help();
        exit(99);
    }

	for (int i = 1 ; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            debug++;
        } else if (strcmp(argv[i], "-i") == 0) {
            i++;
            filename = argv[i];
        } else if (strcmp(argv[i], "-h") == 0) {
            print_help();
            exit(0);
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            print_help();
            exit(99);
        }
    }
    if (filename == NULL) {
        fprintf(stderr, "No interface provided. Check -i option.\n");
        print_help();
        exit(99);
    }
	setup(filename);
    loop();
}

