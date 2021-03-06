/*
 * ndpiex.c
 * Copyright (C) 2018 by QXIP BV
 * Copyright (C) 2018 by ntop
 *
 * Author: Lorenzo Mangani - Michele Campus
 *         based on code of ndpiReader
 * 
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * ndpiex is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with nDPI.  If not, see <http://www.gnu.org/licenses/>.
 * 
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>
#include <netinet/in.h>

#define __STDC_FORMAT_MACROS

#include <inttypes.h>
#include <linux/if_ether.h>
#include <pcap.h>

#include "ndpi_api.h"

/**
   NOTE: in case we want to use nDPI data struct,
   link ./nDPI/example/ndpi_util.h in Makefile
*/
//#include "nDPI/example/ndpi_util.h"

#define	MAX_OSDPI_IDS            100000
#define	MAX_OSDPI_FLOWS          100000
#define TICK_RESOLUTION          1000

typedef void (*callback)(int, const uint8_t *packet);

// prototypes used function
void init();
void setDatalinkType(pcap_t *handle);
void processPacket(const struct pcap_pkthdr *header, const uint8_t *packet);
void finish();
void addProtocolHandler(callback handler);


// cli options
static char *_pcap_file   = NULL;
static char *results_path = NULL;

// pcap
static char _pcap_error_buffer[PCAP_ERRBUF_SIZE];
static pcap_t *_pcap_handle = NULL;
static int _pcap_datalink_type;

// detection
static struct ndpi_detection_module_struct *ndpi_info_mod = NULL;

#ifdef NDPI_ENABLE_DEBUG_MESSAGES
static NDPI_PROTOCOL_BITMASK debug_messages_bitmask;
#endif

// results
static u_int64_t raw_packet_count;
static u_int64_t ip_packet_count;
static u_int64_t total_bytes;
static u_int64_t protocol_counter[NDPI_MAX_SUPPORTED_PROTOCOLS + 1];
static u_int64_t protocol_counter_bytes[NDPI_MAX_SUPPORTED_PROTOCOLS + 1];


/** id tracking **/
struct osdpi_id {
  u_int8_t ip[4];
  struct ndpi_id_struct *ndpi_id;
};

static u_int32_t size_id_struct;
static struct osdpi_id *osdpi_ids;
static u_int32_t osdpi_id_count;


/** flow tracking **/
struct osdpi_flow {
  
  u_int32_t hashval;
  u_int32_t src_ip;
  u_int32_t dst_ip;
  u_int16_t src_port;
  u_int16_t dst_port;
  u_int8_t detection_completed, protocol, bidirectional, check_extra_packets;
  u_int16_t vlan_id;
  struct ndpi_flow_struct *ndpi_flow;
  char src_name[48], dst_name[48];
  u_int8_t ip_version;
  u_int64_t last_seen;
  u_int64_t src2dst_bytes, dst2src_bytes;
  u_int32_t src2dst_packets, dst2src_packets;
  ndpi_protocol detected_protocol;
  char info[96];
  char host_server_name[192];
  char bittorent_hash[41];
  void *src_id, *dst_id;
};

static u_int32_t size_flow_struct;
static struct osdpi_flow *osdpi_flows;
static u_int32_t osdpi_flow_count;

#ifdef NDPI_ENABLE_DEBUG_MESSAGES
static int string_to_detection_bitmask(char *str, NDPI_PROTOCOL_BITMASK * dbm)
{
  u_int32_t a;
  u_int32_t oldptr = 0;
  u_int32_t ptr = 0;
  NDPI_BITMASK_RESET(*dbm);
    
  printf("Protocol parameter given: %s\n", str);
  
  if (strcmp(str, "all") == 0) {
    printf("Protocol parameter all parsed\n");
    NDPI_BITMASK_SET_ALL(*dbm);
    printf("Bitmask is: " NDPI_BITMASK_DEBUG_OUTPUT_BITMASK_STRING " \n",
	   NDPI_BITMASK_DEBUG_OUTPUT_BITMASK_VALUE(*dbm));
    return 0;
  }
  // parse bitmask
  while (1) {
    if (str[ptr] == 0 || str[ptr] == ' ') {
      printf("Protocol parameter: parsed: %.*s,\n", ptr - oldptr, &str[oldptr]);
      for (a = 1; a <= NDPI_MAX_SUPPORTED_PROTOCOLS; a++) {
                
	if (strlen(prot_short_str[a]) == (ptr - oldptr) &&
	    (memcmp(&str[oldptr], prot_short_str[a], ptr - oldptr) == 0)) {
	  NDPI_ADD_PROTOCOL_TO_BITMASK(*dbm, a);
	  printf("Protocol parameter detected as protocol %s\n", prot_long_str[a]);
	}
      }
      oldptr = ptr + 1;
      if (str[ptr] == 0)
	break;
    }
    ptr++;
  }
  return 0;
}
#endif


/**
 * @brief Print help instructions
 */
static void help() {
  
  printf("Welcome to nDPIex\n\n");

  printf("ndpiReader -f <file | -e | [-j <file.json>] [-w <file>] \n\n"
	 "Usage:\n"
	 "  -f <file.pcap>            | Specify a pcap file to read packets from or a\n"
	 "  -e                        | Write debug messages to file\n"
	 "  -j <file.json>            | Specify a file to write the content of packets in .json format\n");

}

/**
 * @description: Option parser
 */
static void parseOptions(int argc, char **argv)
{
  int opt;
    
#ifdef NDPI_ENABLE_DEBUG_MESSAGES
  NDPI_BITMASK_SET_ALL(debug_messages_bitmask);
#endif
    
  while ((opt = getopt(argc, argv, "f:e:j:w")) != EOF) {
    switch (opt) {
      
    case 'f':
      _pcap_file = optarg;
      break;
      
    case 'e':
#ifdef NDPI_ENABLE_DEBUG_MESSAGES
      // set debug logging bitmask to all protocols
      if(string_to_detection_bitmask(optarg, &debug_messages_bitmask) != 0) {
	printf("ERROR option -e needs a valid list of protocols");
	exit(-1);
      }
      printf("debug messages Bitmask is: " NDPI_BITMASK_DEBUG_OUTPUT_BITMASK_STRING "\n",
	     NDPI_BITMASK_DEBUG_OUTPUT_BITMASK_VALUE(debug_messages_bitmask));
#else
      printf("ERROR: option -e : DEBUG MESSAGES DEACTIVATED\n");
      exit(-1);
#endif
      break;
      
    case 'j':
#ifndef HAVE_JSON_C
      printf("WARNING: this code has been compiled without JSON-C: json export disabled\n");
#else
      _jsonFilePath = optarg;
      json_flag = 1;
#endif
      break;
      
    default:
      help();
      break;
    }
  }
    
  // check parameters
  if (_pcap_file == NULL || strcmp(_pcap_file, "") == 0) {
    printf("ERROR: no pcap file path provided; use option -f with the path to a valid pcap file\n");
    exit(-1);
  }
}

/* static void debug_printf(u_int32_t protocol, void *id_struct, ndpi_log_level_t log_level, const char *format, ...) */
/* { */
/* #ifdef NDPI_ENABLE_DEBUG_MESSAGES */
/*   if (NDPI_COMPARE_PROTOCOL_TO_BITMASK(debug_messages_bitmask, protocol) != 0) { */
/*     const char *protocol_string; */
/*     const char *file; */
/*     const char *func; */
/*     u_int32_t line; */
/*     va_list ap; */
/*     va_start(ap, format); */
        
/*     protocol_string = prot_short_str[protocol.app_protocol]; */
        
/*     ndpi_debug_get_last_log_function_line(ndpi_info_mod, &file, &func, &line); */
        
/*     printf("\nDEBUG: %s:%s:%u Prot: %s, level: %u packet: %"PRIu64" :", file, func, line, protocol_string, */
/* 	   log_level, raw_packet_count); */
/*     vprintf(format, ap); */
/*     va_end(ap); */
/*   } */
/* #endif */
/* } */

/**
   function to return the ID of protocol
 */
static void *get_id(const u_int8_t * ip)
{
  u_int32_t i;
  for (i = 0; i < osdpi_id_count; i++) {
    if (memcmp(osdpi_ids[i].ip, ip, sizeof(u_int8_t) * 4) == 0) {
      return osdpi_ids[i].ndpi_id;
    }
  }
  if (osdpi_id_count == MAX_OSDPI_IDS) {
    printf("ERROR: maximum unique id count (%u) has been exceeded\n", MAX_OSDPI_IDS);
    exit(-1);
  }
  else {
    struct ndpi_id_struct *ndpi_id;
    memcpy(osdpi_ids[osdpi_id_count].ip, ip, sizeof(u_int8_t) * 4);
    ndpi_id = osdpi_ids[osdpi_id_count].ndpi_id;
        
    osdpi_id_count += 1;
    return ndpi_id;
  }
}


/**
   function to return the flow of protocol
 */
static struct osdpi_flow *get_osdpi_flow(const struct iphdr *iph, u_int16_t ipsize)
{
  u_int32_t i;
  u_int16_t l4_packet_len;
  struct tcphdr *tcph = NULL;
  struct udphdr *udph = NULL;

  u_int32_t src_ip;
  u_int32_t dst_ip;
  u_int16_t src_port;
  u_int16_t dst_port;

  if(ipsize < 20)
    return NULL;

  if((iph->ihl * 4) > ipsize || ipsize < ntohs(iph->tot_len)
     || (iph->frag_off & htons(0x1FFF)) != 0)
    return NULL;

  l4_packet_len = ntohs(iph->tot_len) - (iph->ihl * 4);

  if(iph->saddr < iph->daddr) {
    src_ip = iph->saddr;
    dst_ip = iph->daddr;
  }
  else {
    src_ip = iph->daddr;
    dst_ip = iph->saddr;
  }

  // TCP
  if(iph->protocol == 6 && l4_packet_len >= 20) {
    tcph = (struct tcphdr *) ((u_int8_t *) iph + iph->ihl * 4);
    if(iph->saddr < iph->daddr) {
      src_port = tcph->source;
      dst_port = tcph->dest;
    }
    else {
      src_port = tcph->dest;
      dst_port = tcph->source;
    }
  }
  // UDP
  else if(iph->protocol == 17 && l4_packet_len >= 8) {
    udph = (struct udphdr *) ((u_int8_t *) iph + iph->ihl * 4);
    if(iph->saddr < iph->daddr) {
      src_port = udph->source;
      dst_port = udph->dest;
    }
    else {
      src_port = udph->dest;
      dst_port = udph->source;
    }
  }
  else {
    // non tcp/udp protocols
    src_port = 0;
    dst_port = 0;
  }

  /*** CHECK this for (maybe must be changed) ***/
  for(i = 0; i < osdpi_flow_count; i++) {
    if(osdpi_flows[i].protocol == iph->protocol &&
       osdpi_flows[i].src_ip == src_ip &&
       osdpi_flows[i].dst_ip == dst_ip &&
       osdpi_flows[i].src_port == src_port && osdpi_flows[i].dst_port == dst_port) {
      return &osdpi_flows[i];
    }
  }
  if(osdpi_flow_count == MAX_OSDPI_FLOWS) {
    printf("ERROR: maximum flow count (%u) has been exceeded\n", MAX_OSDPI_FLOWS);
    exit(-1);
  }
  else {

    // new flow allocated to be returned from the function
    struct osdpi_flow *new_flow;
    
    osdpi_flows[osdpi_flow_count].protocol = iph->protocol;
    osdpi_flows[osdpi_flow_count].src_ip = src_ip;
    osdpi_flows[osdpi_flow_count].dst_ip = dst_ip;
    osdpi_flows[osdpi_flow_count].src_port = src_port;
    osdpi_flows[osdpi_flow_count].dst_port = dst_port;

    // new flow
    new_flow = &osdpi_flows[osdpi_flow_count];

    osdpi_flow_count += 1;
    return new_flow;
  }
}


/**
   Initialize all structures necessary for detection
*/
static void setupDetection(void)
{
  u_int32_t i;
  NDPI_PROTOCOL_BITMASK all;

  // init global detection structure
  ndpi_info_mod = ndpi_init_detection_module();
  if(ndpi_info_mod == NULL) {
    printf("ERROR: global structure initialization failed\n");
    exit(-1);
  }
  
  // enable all protocols
  NDPI_BITMASK_SET_ALL(all);
  ndpi_set_protocol_detection_bitmask2(ndpi_info_mod, &all);

  // allocate memory for id and flow tracking
  size_id_struct = ndpi_detection_get_sizeof_ndpi_id_struct();
  size_flow_struct = ndpi_detection_get_sizeof_ndpi_flow_struct();

  // allocate memory for ids struct
  osdpi_ids = calloc(MAX_OSDPI_IDS, sizeof(struct osdpi_id));
  if(osdpi_ids == NULL) {
    printf("ERROR: malloc for osdpi_ids failed\n");
    exit(-1);
  }
  for(i = 0; i < MAX_OSDPI_IDS; i++) {
    /* memset(&osdpi_ids[i], 0, sizeof(struct osdpi_id)); */
    osdpi_ids[i].ndpi_id = calloc(1, size_id_struct);
    if(osdpi_ids[i].ndpi_id == NULL) {
      printf("ERROR: malloc for ndpi_id struct inside osdpi_ids failed\n");
      exit(-1);
    }
  }

  // allocate memory for flow struct
  osdpi_flows = calloc(MAX_OSDPI_FLOWS, sizeof(struct osdpi_flow));
  if(osdpi_flows == NULL) {
    printf("ERROR: malloc for osdpi_flows failed\n");
    exit(-1);
  }
  for (i = 0; i < MAX_OSDPI_FLOWS; i++) {
    /* memset(&osdpi_flows[i], 0, sizeof(struct osdpi_flow)); */
    osdpi_flows[i].ndpi_flow = calloc(1, size_flow_struct);
    if(osdpi_flows[i].ndpi_flow == NULL) {
      printf("ERROR: malloc for ndpi_flow_struct failed\n");
      exit(-1);
    }
  }

  // clear memory for results
  /* memset(protocol_counter, 0, (NDPI_MAX_SUPPORTED_PROTOCOLS + 1) * sizeof(u_int64_t)); */
  /* memset(protocol_counter_bytes, 0, (NDPI_MAX_SUPPORTED_PROTOCOLS + 1) * sizeof(u_int64_t)); */
}

/**
   Function to terminate detection
 */
static void terminateDetection(void)
{
  u_int32_t i;

  // free ids
  for(i = 0; i < MAX_OSDPI_IDS; i++) {
    free(osdpi_ids[i].ndpi_id);
  }
  
  free(osdpi_ids);

  // free flows
  for (i = 0; i < MAX_OSDPI_FLOWS; i++) {
    free(osdpi_flows[i].ndpi_flow);
  }
  
  free(osdpi_flows);
}


/**
   Function to process the packet:
   determine the flow of a packet and try to decode it
   @return: 0 if success; != 0 fail

   @Note: ipsize = header->len - ip_offset ; rawsize = header->len
*/
static unsigned int packet_processing(const u_int64_t time,
				      const struct iphdr *iph,
				      uint16_t ipsize,
				      uint16_t rawsize)
{
  struct ndpi_id_struct *src = NULL;
  struct ndpi_id_struct *dst = NULL;
  struct osdpi_flow *flow = NULL;
  struct ndpi_flow_struct *ipq_flow = NULL;
  ndpi_protocol protocol;

  src = get_id((u_int8_t *) &iph->saddr);
  dst = get_id((u_int8_t *) &iph->daddr);

  flow = get_osdpi_flow(iph, ipsize);
  if(flow != NULL) {
    ipq_flow = flow->ndpi_flow;
  }

  // update stats for ip pkts and bytes
  ip_packet_count++;
  total_bytes += rawsize;
  
  // only handle unfragmented packets
  if((iph->frag_off & htons(0x1FFF)) == 0) {
    
    // here the actual detection is performed
    protocol = ndpi_detection_process_packet(ndpi_info_mod, ipq_flow, (uint8_t *) iph, ipsize, time, src, dst);
  }
  else {
    printf("\n\nWARNING: fragmented ip packets are not supported and will be skipped \n\n");
    sleep(2);
    return -1;
  }

  /* This is necessary to avoid different detection from ndpiex and ndpiReader;
     Inside nDPI there is a change from master and app protocol */
  if(protocol.master_protocol != 0)
    protocol.app_protocol = protocol.master_protocol;
  
  protocol_counter[protocol.app_protocol]++;
  protocol_counter_bytes[protocol.app_protocol] += rawsize;
    
  if(flow != NULL) {
    flow->detected_protocol = protocol;
    ///	printf("\nproto: %u %s",protocol.app_protocol, ndpi_get_proto_name(ndpi_info_mod, flow->detected_protocol.app_protocol) );
  }
  return 0;
}


/**
   Function to print the results obtained
 */
static void printResults(void)
{
  u_int32_t i, j, protocol_flows = 0;
  char *proto_name;
  
  printf("\x1b[2K\n");
  printf("pcap file contains\n");
  printf("\tip packets:   \x1b[33m%-13"PRIu64"\x1b[0m of %"PRIu64" packets total\n", ip_packet_count, raw_packet_count);
  printf("\tip bytes:     \x1b[34m%-13"PRIu64"\x1b[0m\n", total_bytes);
  printf("\tunique ids:   \x1b[35m%-13u\x1b[0m\n", osdpi_id_count);
  printf("\tunique flows: \x1b[36m%-13u\x1b[0m\n", osdpi_flow_count);

  printf("\n\ndetected protocols:\n");

  for(i = 0; i <= NDPI_MAX_SUPPORTED_PROTOCOLS; i++) {

    // count flows for that protocol
    for (j = 0; j < osdpi_flow_count; j++) {
      if (osdpi_flows[j].detected_protocol.app_protocol == i) {
	protocol_flows++;
      }
    }

    // call the nDPI function to get the protocol name
    proto_name = ndpi_get_proto_name(ndpi_info_mod, i);
    
    // if a protocol is detected, print it
    if(protocol_counter[i] > 0) {
      printf("\t\x1b[31m%-20s\x1b[0m packets: \x1b[33m%-13"PRIu64"\x1b[0m bytes: \x1b[34m%-13"PRIu64"\x1b[0m "
      	     "flows: \x1b[36m%-13u\x1b[0m\n",
      	     proto_name, protocol_counter[i], protocol_counter_bytes[i], protocol_flows);
    }
  }
  printf("\n");
}

/**
   function to open a pcap file
*/
static void openPcapFile(void)
{
  /* trying to open a pcap file */
  _pcap_handle = pcap_open_offline(_pcap_file, _pcap_error_buffer);

  if (_pcap_handle == NULL) {
    printf("ERROR: could not open pcap file: %s\n", _pcap_error_buffer);
    exit(-1);
  }
}


/**
   function to close a pcap file
*/
static void closePcapFile(void)
{
  if (_pcap_handle != NULL) {
    pcap_close(_pcap_handle);
  }
}


/**
   Callback function for each packet in the pcap file 
*/
static void pcap_packet_callback(u_char * args, const struct pcap_pkthdr *header, const u_char * packet)
{

  u_int64_t time;
  static u_int64_t lasttime = 0;
  u_int16_t type;

  // check datalink type
  _pcap_datalink_type = pcap_datalink(_pcap_handle);

  
  /* --- Ethernet header --- */
  const struct ethhdr *ethernet = (struct ethhdr *) packet;
  /* --- IP header --- */
  struct iphdr *iph = (struct iphdr *) &packet[sizeof(struct ethhdr)];

  // increment packet count stat
  raw_packet_count++;

  time = ((u_int64_t) header->ts.tv_sec) * TICK_RESOLUTION +
    header->ts.tv_usec / (1000000 / TICK_RESOLUTION);
  
  if(lasttime > time) {
    // printf("\nWARNING: timestamp bug in the pcap file (ts delta: %"PRIu64", repairing)\n", lasttime - time);
    time = lasttime;
  }
  lasttime = time;

  // IP type
  type = ethernet->h_proto;

  // just work on Ethernet packets that contain IP
  /*** TODO: extend datalink type support ***/
  if (_pcap_datalink_type == DLT_EN10MB &&
      type == htons(ETH_P_IP) &&
      header->caplen >= sizeof(struct ethhdr)) {

    if(header->caplen < header->len) {
      fprintf(stderr, "\n\nWARNING: packet capture size is smaller than packet size, DETECTION MIGHT NOT WORK CORRECTLY OR EVEN CRASH\n\n");
      sleep(2);
    }

    /*** TODO: extend for IPv6 ***/
    if(iph->version != 4) {
	printf("\n\nWARNING: only IPv4 packets are supported, all other packets will be discarded\n\n");
	sleep(2);
	return;
    } 
    // process the packet
    packet_processing(time, iph, header->len - sizeof(struct ethhdr), header->len);
  }
}


/**
   Call pcap_loop() to process packets from a pcap_handle
  */
static void runPcapLoop(void)
{
  if (_pcap_handle) {
    pcap_loop(_pcap_handle, -1, &pcap_packet_callback, NULL);
  }
}


/**
   @description: MAIN FUNCTION
**/
int main(int argc, char **argv)
{
  /* TODO: better to change some variables from global to local 
     for more visibility */
  
  parseOptions(argc, argv);
  
  setupDetection();

  openPcapFile();
  
  runPcapLoop();

  closePcapFile();
  
  printResults();

  terminateDetection();

  if(results_path) free(results_path);
  if(ndpi_info_mod) ndpi_exit_detection_module(ndpi_info_mod);
  
  return 0;
}

/* ***************************************************** */

callback protocolHandler;

void addProtocolHandler(callback handler) {
  protocolHandler = handler;
}

void onProtocol(uint16_t id, const uint8_t *packet) {
  if (protocolHandler) {
    protocolHandler(id, packet);
  }
}

/*************************************************/
