#include "main.h"

/* Constants of the system */
#define MEMPOOL_NAME "cluster_mem_pool"				// Name of the NICs' mem_pool, useless comment....
#define MEMPOOL_ELEM_SZ 2048  					// Power of two greater than 1500
#define MEMPOOL_CACHE_SZ 512  					// Max is 512

#define INTERMEDIATERING_NAME "intermedate_ring"

#define RX_QUEUE_SZ 4096			// The size of rx queue. Max is 4096 and is the one you'll have best performances with. Use lower if you want to use Burst Bulk Alloc.
#define TX_QUEUE_SZ 256			// Unused, you don't tx packets
#define PKT_BURST_SZ 4096		// The max size of batch of packets retreived when invoking the receive function. Use the RX_QUEUE_SZ for high speed

/* Global vars */
char * file_name = NULL;
pcap_dumper_t * pcap_file_p;
uint64_t max_packets = 0 ;
uint64_t buffer_size = 65536;
// uint64_t buffer_size = 1048576;
uint64_t seconds_rotation = 0;
uint64_t last_rotation = 0;
int64_t  nb_rotations=0;
int64_t  max_rotations = -1 ;
uint64_t max_size = 0 ;
uint64_t nb_captured_packets = 0;
uint64_t nb_dumped_packets = 0;
uint64_t sz_dumped_file = 0;
uint64_t start_secs;
int do_shutdown = 0;
pcap_t *pd;
int nb_sys_ports;
static struct rte_mempool * pktmbuf_pool;
static struct rte_ring    * intermediate_ring;

/* Main function */
int main(int argc, char **argv)
{
	int ret;
	int i;

	/* Create handler for SIGINT for CTRL + C closing and SIGALRM to print stats*/
	signal(SIGINT, sig_handler);
	signal(SIGALRM, alarm_routine);

	/* Initialize DPDK enviroment with args, then shift argc and argv to get application parameters */
	ret = rte_eal_init(argc, argv);
	if (ret < 0) FATAL_ERROR("Cannot init EAL\n");
	argc -= ret;
	argv += ret;

	/* Check if this application can use two cores*/
	ret = rte_lcore_count ();
	if (ret != 2) FATAL_ERROR("This application needs exactly two (2) cores.");

	/* Parse arguments */
	parse_args(argc, argv);
	if (ret < 0) FATAL_ERROR("Wrong arguments\n");

	/* Probe PCI bus for ethernet devices, mandatory only in DPDK < 1.8.0 */
	#if RTE_VER_MAJOR == 1 && RTE_VER_MINOR < 8
		ret = rte_eal_pci_probe();
		if (ret < 0) FATAL_ERROR("Cannot probe PCI\n");
	#endif

	/* Get number of ethernet devices */
	nb_sys_ports = rte_eth_dev_count();
	if (nb_sys_ports <= 0) FATAL_ERROR("Cannot find ETH devices\n");
	
	/* Create a mempool with per-core cache, initializing every element for be used as mbuf, and allocating on the current NUMA node */
	pktmbuf_pool = rte_mempool_create(MEMPOOL_NAME, buffer_size-1, MEMPOOL_ELEM_SZ, MEMPOOL_CACHE_SZ, sizeof(struct rte_pktmbuf_pool_private), rte_pktmbuf_pool_init, NULL, rte_pktmbuf_init, NULL,rte_socket_id(), 0);
	
	if(pktmbuf_pool == NULL) FATAL_ERROR("Cannot create cluster");
	//if (pktmbuf_pool == NULL) FATAL_ERROR("Cannot create cluster_mem_pool. Errno: %d [ENOMEM: %d, ENOSPC: %d, E_RTE_NO_TAILQ: %d, E_RTE_NO_CONFIG: %d, E_RTE_SECONDARY: %d, EINVAL: %d, EEXIST: %d]\n", rte_errno, ENOMEM, ENOSPC, E_RTE_NO_TAILQ, E_RTE_NO_CONFIG, E_RTE_SECONDARY, EINVAL, EEXIST  );
	
	/* Init intermediate queue data structures: the ring. */
	intermediate_ring = rte_ring_create (INTERMEDIATERING_NAME, buffer_size, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ );
 	if (intermediate_ring == NULL ) FATAL_ERROR("Cannot create ring");

	/* Operations needed for each ethernet device */			
	for(i=0; i < nb_sys_ports; i++)
		init_port(i);

	/* Start consumer and producer routine on 2 different cores: consumer launched first... */
	ret =  rte_eal_mp_remote_launch (main_loop_consumer, NULL, SKIP_MASTER);
	if (ret != 0) FATAL_ERROR("Cannot start consumer thread\n");	

	/* ... and then loop in consumer */
	main_loop_producer ( NULL );	

	return 0;
}



/* Loop function, batch timing implemented */
static int main_loop_producer(__attribute__((unused)) void * arg){
	struct rte_mbuf * pkts_burst[PKT_BURST_SZ];
	struct timeval t_pack;
	struct rte_mbuf * m;
	int read_from_port = 0;
	int i, nb_rx, ret;


	/* Start stats */
   	alarm(1);

	for (i=0;i<nb_sys_ports; i++)
		rte_eth_stats_reset ( i );

	/* Infinite loop */
	for (;;) {

		/* Read a burst for current port at queue 'nb_istance'*/
		nb_rx = rte_eth_rx_burst(read_from_port, 0, pkts_burst, PKT_BURST_SZ);

		/* For each received packet. */
		for (i = 0; likely( i < nb_rx ) ; i++) {

			/* Retreive packet from burst, increase the counter */
			m = pkts_burst[i];
			nb_captured_packets++;

			/* Timestamp the packet */
			ret = gettimeofday(&t_pack, NULL);
			if (ret != 0) FATAL_ERROR("Error: gettimeofday failed. Quitting...\n");

			/* Writing packet timestamping in unused mbuf fields. (wild approach ! ) */
			m->tx_offload = t_pack.tv_sec;
			m->udata64 =  t_pack.tv_usec;

			/*Enqueieing buffer */
			ret = rte_ring_enqueue (intermediate_ring, m);

		}

		/* Increasing reading port number in Round-Robin logic */
		read_from_port = (read_from_port + 1) % nb_sys_ports;

	}
	return 0;
}

static int main_loop_consumer(__attribute__((unused)) void * arg){

	struct timeval t_pack;
	struct rte_mbuf * m;
	u_char * packet;
	char file_name_rotated [1000];
	int ret;
	struct pcap_pkthdr pcap_hdr;

	/* Open pcap file for writing */
	pd = pcap_open_dead(DLT_EN10MB, 65535 );
	pcap_file_p = pcap_dump_open(pd, file_name);
	if(pcap_file_p==NULL)
		FATAL_ERROR("Error in opening pcap file\n");
	printf("Opened file %s\n", file_name);

	/* Init first rotation */
	ret = gettimeofday(&t_pack, NULL);
	if (ret != 0) FATAL_ERROR("Error: gettimeofday failed. Quitting...\n");
	last_rotation = t_pack.tv_sec;
	start_secs = t_pack.tv_sec;

	/* Infinite loop for consumer thread */
	for(;;){

		/* Dequeue packet */
		ret = rte_ring_dequeue(intermediate_ring, (void**)&m);
		
		/* Continue polling if no packet available */
		if( unlikely (ret != 0)) continue;

		/* Read timestamp of the packet */
		t_pack.tv_usec = m->udata64;
		t_pack.tv_sec = m->tx_offload;

		/* Rotate if needed */
		if ( seconds_rotation > 0 && t_pack.tv_sec - last_rotation > seconds_rotation ){

			last_rotation = t_pack.tv_sec;
			nb_rotations ++;

			/* Quit if the number of rotations is the max */
			if (max_rotations != -1 && nb_rotations > max_rotations)
				sig_handler(SIGINT);

			/* Close the pcap file */
			pcap_close(pd);
			pcap_dump_close(pcap_file_p);

			/* Open pcap file for writing */
			sprintf(file_name_rotated, "%s%ld", file_name, nb_rotations);
			pd = pcap_open_dead(DLT_EN10MB, 65535 );
			pcap_file_p = pcap_dump_open(pd, file_name_rotated);
			if(pcap_file_p==NULL)
				FATAL_ERROR("Error in opening pcap file\n");
			printf("\nOpened file %s\n", file_name_rotated);
		}

		/* Compile pcap header */
		pcap_hdr.ts = t_pack;
		pcap_hdr.caplen = rte_pktmbuf_data_len(m); 
		pcap_hdr.len = rte_pktmbuf_data_len(m); 
		packet = rte_pktmbuf_mtod(m, u_char * ); 	

		/* Write on pcap */
		pcap_dump ((u_char *)pcap_file_p, & pcap_hdr,  packet);
		nb_dumped_packets++;
		sz_dumped_file += rte_pktmbuf_data_len(m) + sizeof (pcap_hdr) ;

		/* Quit if reached the size threshold */
		if (max_size != 0 && sz_dumped_file >= max_size*1024)
			sig_handler(SIGINT);

		/* Quit if reached the packet threshold */
		if (max_packets != 0 && nb_dumped_packets >= max_packets)
			sig_handler(SIGINT);

		/* Free the buffer */
		rte_pktmbuf_free((struct rte_mbuf *)m);
	}
}

void print_stats (void){
	struct rte_eth_stats stat;
	int i;
	uint64_t good_pkt = 0, miss_pkt = 0;

	/* Print per port stats */
	for (i = 0; i < nb_sys_ports; i++){	
		rte_eth_stats_get(i, &stat);
		good_pkt += stat.ipackets;
		miss_pkt += stat.imissed;
		printf("\nPORT: %2d Rx: %ld Drp: %ld Tot: %ld Perc: %.3f%%", i, stat.ipackets, stat.imissed, stat.ipackets+stat.imissed, (float)stat.imissed/(stat.ipackets+stat.imissed)*100 );
	}
	printf("\n-------------------------------------------------");
	printf("\nTOT:     Rx: %ld Drp: %ld Tot: %ld Perc: %.3f%%", good_pkt, miss_pkt, good_pkt+miss_pkt, (float)miss_pkt/(good_pkt+miss_pkt)*100 );
	printf("\n");

}

void alarm_routine (__attribute__((unused)) int unused){

	/* If the program is quitting don't print anymore */
	if(do_shutdown) return;

	/* Print per port stats */
	print_stats();

	/* Schedule an other print */
	alarm(1);
	signal(SIGALRM, alarm_routine);

}

/* Signal handling function */
static void sig_handler(int signo)
{
	uint64_t diff;
	int ret;
	struct timeval t_end;

	/* Catch just SIGINT */
	if (signo == SIGINT){

		/* Signal the shutdown */
		do_shutdown=1;

		/* Print the per port stats  */
		printf("\n\nQUITTING...\n");

		ret = gettimeofday(&t_end, NULL);
		if (ret != 0) FATAL_ERROR("Error: gettimeofday failed. Quitting...\n");		
		diff = t_end.tv_sec - start_secs;
		printf("The capture lasted %ld seconds.\n", diff);
		print_stats();

		/* Close the pcap file */
		pcap_close(pd);
		pcap_dump_close(pcap_file_p);
		exit(0);	
	}
}

/* Init each port with the configuration contained in the structs. Every interface has nb_sys_cores queues */
static void init_port(int i) {

		int ret;
		uint8_t rss_key [40];
		struct rte_eth_link link;
		struct rte_eth_dev_info dev_info;
		struct rte_eth_rss_conf rss_conf;
		// struct rte_eth_fdir_stats fdir_conf;

		/* Retreiving and printing device infos */
		rte_eth_dev_info_get(i, &dev_info);
		printf("Name:%s\n\tDriver name: %s\n\tMax rx queues: %d\n\tMax tx queues: %d\n", dev_info.pci_dev->driver->name,dev_info.driver_name, dev_info.max_rx_queues, dev_info.max_tx_queues);
		printf("\tPCI Adress: %04d:%02d:%02x:%01d\n", dev_info.pci_dev->addr.domain, dev_info.pci_dev->addr.bus, dev_info.pci_dev->addr.devid, dev_info.pci_dev->addr.function);

		/* Configure device with '1' rx queues and 1 tx queue */
		ret = rte_eth_dev_configure(i, 1, 1, &port_conf);
		if (ret < 0) rte_panic("Error configuring the port\n");

		/* For each RX queue in each NIC */
		/* Configure rx queue j of current device on current NUMA socket. It takes elements from the mempool */
		ret = rte_eth_rx_queue_setup(i, 0, RX_QUEUE_SZ, rte_socket_id(), &rx_conf, pktmbuf_pool);
		if (ret < 0) FATAL_ERROR("Error configuring receiving queue\n");
		/* Configure mapping [queue] -> [element in stats array] */
		ret = rte_eth_dev_set_rx_queue_stats_mapping 	(i, 0, 0);
		//if (ret < 0) FATAL_ERROR("Error configuring receiving queue stats\n");


		/* Configure tx queue of current device on current NUMA socket. Mandatory configuration even if you want only rx packet */
		ret = rte_eth_tx_queue_setup(i, 0, TX_QUEUE_SZ, rte_socket_id(), &tx_conf);
		if (ret < 0) FATAL_ERROR("Error configuring transmitting queue. Errno: %d (%d bad arg, %d no mem)\n", -ret, EINVAL ,ENOMEM);

		/* Start device */		
		ret = rte_eth_dev_start(i);
		if (ret < 0) FATAL_ERROR("Cannot start port\n");

		/* Enable receipt in promiscuous mode for an Ethernet device */
		rte_eth_promiscuous_enable(i);

		/* Print link status */
		rte_eth_link_get_nowait(i, &link);
		if (link.link_status) 	printf("\tPort %d Link Up - speed %u Mbps - %s\n", (uint8_t)i, (unsigned)link.link_speed,(link.link_duplex == ETH_LINK_FULL_DUPLEX) ?("full-duplex") : ("half-duplex\n"));
		else			printf("\tPort %d Link Down\n",(uint8_t)i);

		/* Print RSS support, not reliable because a NIC could support rss configuration just in rte_eth_dev_configure whithout supporting rte_eth_dev_rss_hash_conf_get*/
		rss_conf.rss_key = rss_key;
		ret = rte_eth_dev_rss_hash_conf_get (i,&rss_conf);
		if (ret == 0) printf("\tDevice supports RSS\n"); else printf("\tDevice DOES NOT support RSS\n");
		
		/* Print Flow director support */
		// ret = rte_eth_dev_fdir_get_infos (i, &fdir_conf);
		// if (ret == 0) printf("\tDevice supports Flow Director\n"); else printf("\tDevice DOES NOT support Flow Director\n"); 

	
}

static int parse_args(int argc, char **argv)
{
	int option;
	

	/* Retrive arguments */
	while ((option = getopt(argc, argv,"w:c:B:G:W:C:")) != -1) {
        	switch (option) {
             		case 'w' : file_name = strdup(optarg); /* File name, mandatory */
                 		break;
			case 'c': max_packets = atol (optarg); /* Max number of packets to save; default is infinite */
				break;
			case 'B': buffer_size = atoi (optarg); /* Buffer size in packets. Must be a power of two . Default is 1048576 */
				break;
			case 'G': seconds_rotation = atoi (optarg); /* Rotation of output in seconds. A progressive number will be added to file name */
				break;
			case 'W': max_rotations = atoi (optarg); /* Max rotations done. In case of 0, the program quits after first rotation time */
				break;
			case 'C': max_size = atoi (optarg); /* Max file size in KB. When reached, the program quits */
				break;
             		default: return -1; 
		}
   	}

	/* Returning bad value in case of wrong arguments */
	if(file_name == NULL || isPowerOfTwo (buffer_size)!=1 )
		return -1;

	return 0;

}

int isPowerOfTwo (unsigned int x)
{
  return ((x != 0) && !(x & (x - 1)));
}

