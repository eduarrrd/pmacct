/*  
    pmacct (Promiscuous mode IP Accounting package)
    pmacct is Copyright (C) 2003-2016 by Paolo Lucente
*/

/*
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

/* defines */
#define __BGP_LOOKUP_C

/* includes */
#include "pmacct.h"
#include "bgp.h"

void bgp_srcdst_lookup(struct packet_ptrs *pptrs, int type)
{
  struct bgp_misc_structs *bms;
  struct bgp_rt_structs *inter_domain_routing_db;
  struct sockaddr *sa = (struct sockaddr *) pptrs->f_agent, sa_local;
  struct xflow_status_entry *xs_entry = (struct xflow_status_entry *) pptrs->f_status;
  struct bgp_peer *peer;
  struct bgp_node *default_node, *result;
  struct bgp_info *info;
  struct prefix default_prefix;
  int compare_bgp_port;
  int follow_default = config.nfacctd_bgp_follow_default;
  struct in_addr pref4;
#if defined ENABLE_IPV6
  struct in6_addr pref6;
#endif
  u_int32_t modulo, local_modulo, modulo_idx, modulo_max;
  safi_t safi;
  rd_t rd;

  bms = bgp_select_misc_db(type);
  inter_domain_routing_db = bgp_select_routing_db(type);

  if (!bms || !inter_domain_routing_db) return;

  pptrs->bgp_src = NULL;
  pptrs->bgp_dst = NULL;
  pptrs->bgp_src_info = NULL;
  pptrs->bgp_dst_info = NULL;
  pptrs->bgp_peer = NULL;
  pptrs->bgp_nexthop_info = NULL;
  compare_bgp_port = FALSE;
  safi = SAFI_UNICAST;

  memset(&rd, 0, sizeof(rd));

  if (pptrs->bta) {
    sa = &sa_local;
    if (pptrs->bta_af == ETHERTYPE_IP) {
      sa->sa_family = AF_INET;
      ((struct sockaddr_in *)sa)->sin_addr.s_addr = pptrs->bta; 
      if (pptrs->lookup_bgp_port.set) {
	((struct sockaddr_in *)sa)->sin_port = pptrs->lookup_bgp_port.n; 
	compare_bgp_port = TRUE;
      }
    }
#if defined ENABLE_IPV6
    else if (pptrs->bta_af == ETHERTYPE_IPV6) {
      sa->sa_family = AF_INET6;
      ip6_addr_32bit_cpy(&((struct sockaddr_in6 *)sa)->sin6_addr, &pptrs->bta, 0, 0, 1);
      ip6_addr_32bit_cpy(&((struct sockaddr_in6 *)sa)->sin6_addr, &pptrs->bta2, 2, 0, 1);
      if (pptrs->lookup_bgp_port.set) {
        ((struct sockaddr_in6 *)sa)->sin6_port = pptrs->lookup_bgp_port.n; 
	compare_bgp_port = TRUE;
      }
    }
#endif
  }

  start_again:

  peer = bms->bgp_lookup_find_peer(sa, xs_entry, pptrs->l3_proto, compare_bgp_port);
  pptrs->bgp_peer = (char *) peer;

  if (peer) {
    struct host_addr peer_dst_ip;

    modulo = bms->route_info_modulo(peer, NULL);

    // XXX: to be optimized 
    if (bms->table_per_peer_hash == BGP_ASPATH_HASH_PATHID) modulo_max = bms->table_per_peer_buckets; 
    else modulo_max = 1;

    if (peer->cap_add_paths && (config.acct_type == ACCT_NF || config.acct_type == ACCT_SF)) {
      /* administrativia */
      struct pkt_bgp_primitives pbgp, *pbgp_ptr = &pbgp;
      memset(&pbgp, 0, sizeof(struct pkt_bgp_primitives));
      
      /* note: call to [NF|SF]_peer_dst_ip_handler for the purpose of
	 code re-use effectively is defeating the concept of libbgp */
      if (config.acct_type == ACCT_NF) NF_peer_dst_ip_handler(NULL, pptrs, &pbgp_ptr);
      else if (config.acct_type == ACCT_SF) SF_peer_dst_ip_handler(NULL, pptrs, &pbgp_ptr);

      memcpy(&peer_dst_ip, &pbgp.peer_dst_ip, sizeof(struct host_addr));
    }

    if (pptrs->bitr) {
      safi = SAFI_MPLS_VPN;
      memcpy(&rd, &pptrs->bitr, sizeof(rd));
    }

    if (pptrs->l3_proto == ETHERTYPE_IP) {
      if (!pptrs->bgp_src) {
        memcpy(&pref4, &((struct my_iphdr *)pptrs->iph_ptr)->ip_src, sizeof(struct in_addr));
	pptrs->bgp_src = (char *) bgp_node_match_ipv4(inter_domain_routing_db->rib[AFI_IP][safi],
						      &pref4, (struct bgp_peer *) pptrs->bgp_peer,
						      type);
      }
      if (!pptrs->bgp_src_info && pptrs->bgp_src) {
	result = (struct bgp_node *) pptrs->bgp_src;	
        if (result->p.prefixlen >= pptrs->lm_mask_src) {
          pptrs->lm_mask_src = result->p.prefixlen;
          pptrs->lm_method_src = NF_NET_BGP;
        }

	for (info = result->info[modulo]; info; info = info->next) {
	  if (safi != SAFI_MPLS_VPN) {
	    if (info->peer == peer) {
	      pptrs->bgp_src_info = (char *) info;
	      break;
	    }
	  }
	  else {
	    if (info->peer == peer && info->extra && !memcmp(&info->extra->rd, &rd, sizeof(rd_t))) {
	      pptrs->bgp_src_info = (char *) info;
	      break;
	    }
	  }
	}
      }
      if (!pptrs->bgp_dst) {
	memcpy(&pref4, &((struct my_iphdr *)pptrs->iph_ptr)->ip_dst, sizeof(struct in_addr));
	pptrs->bgp_dst = (char *) bgp_node_match_ipv4(inter_domain_routing_db->rib[AFI_IP][safi],
						      &pref4, (struct bgp_peer *) pptrs->bgp_peer,
						      type);
      }
      if (!pptrs->bgp_dst_info && pptrs->bgp_dst) {
	result = (struct bgp_node *) pptrs->bgp_dst;
        if (result->p.prefixlen >= pptrs->lm_mask_dst) {
          pptrs->lm_mask_dst = result->p.prefixlen;
          pptrs->lm_method_dst = NF_NET_BGP;
        }

        for (local_modulo = modulo, modulo_idx = 0; modulo_idx < modulo_max; local_modulo++, modulo_idx++) {
          for (info = result->info[local_modulo]; info; info = info->next) {
	    if (info->peer == peer) {
	      int no_match = FALSE;

	      /* flagging additional checks are required */
	      if (safi == SAFI_MPLS_VPN) no_match++;
	      if (peer->cap_add_paths) no_match++;
 
	      if (safi == SAFI_MPLS_VPN) {
	        if (info->extra && !memcmp(&info->extra->rd, &rd, sizeof(rd_t))) no_match--;
	      }

	      if (peer->cap_add_paths) {
	        if (info->attr) {
		  if (info->attr->mp_nexthop.family == peer_dst_ip.family) {
		    if (!memcmp(&info->attr->mp_nexthop, &peer_dst_ip, HostAddrSz)) no_match--;
		  }
		  else if (info->attr->nexthop.s_addr && peer_dst_ip.family == AF_INET) {
		    if (info->attr->nexthop.s_addr == peer_dst_ip.address.ipv4.s_addr) no_match--;
		  }
	        }
	      }

	      if (!no_match) {
	        pptrs->bgp_dst_info = (char *) info;
	        break;
	      }
	    }
	  }

	  /* if having results, let's not potentially look in other buckets */
	  if (pptrs->bgp_dst_info) break;
        }
      }
    }
#if defined ENABLE_IPV6
    else if (pptrs->l3_proto == ETHERTYPE_IPV6) {
      if (!pptrs->bgp_src) {
        memcpy(&pref6, &((struct ip6_hdr *)pptrs->iph_ptr)->ip6_src, sizeof(struct in6_addr));
	pptrs->bgp_src = (char *) bgp_node_match_ipv6(inter_domain_routing_db->rib[AFI_IP6][safi],
						      &pref6, (struct bgp_peer *) pptrs->bgp_peer,
						      type);
      }
      if (!pptrs->bgp_src_info && pptrs->bgp_src) {
	result = (struct bgp_node *) pptrs->bgp_src;
        if (result->p.prefixlen >= pptrs->lm_mask_src) {
          pptrs->lm_mask_src = result->p.prefixlen;
          pptrs->lm_method_src = NF_NET_BGP;
        }

        for (info = result->info[modulo]; info; info = info->next) {
          if (safi != SAFI_MPLS_VPN) {
            if (info->peer == peer) {
              pptrs->bgp_src_info = (char *) info;
              break;
            }
          }
          else {
            if (info->peer == peer && info->extra && !memcmp(&info->extra->rd, &rd, sizeof(rd_t))) {
              pptrs->bgp_src_info = (char *) info;
              break;
            }
          }
        }
      }
      if (!pptrs->bgp_dst) {
        memcpy(&pref6, &((struct ip6_hdr *)pptrs->iph_ptr)->ip6_dst, sizeof(struct in6_addr));
	pptrs->bgp_dst = (char *) bgp_node_match_ipv6(inter_domain_routing_db->rib[AFI_IP6][safi],
						      &pref6, (struct bgp_peer *) pptrs->bgp_peer,
						      type);
      }
      if (!pptrs->bgp_dst_info && pptrs->bgp_dst) {
	result = (struct bgp_node *) pptrs->bgp_dst; 
        if (result->p.prefixlen >= pptrs->lm_mask_dst) {
          pptrs->lm_mask_dst = result->p.prefixlen;
          pptrs->lm_method_dst = NF_NET_BGP;
        }

        for (local_modulo = modulo, modulo_idx = 0; modulo_idx < modulo_max; local_modulo++, modulo_idx++) {
          for (info = result->info[local_modulo]; info; info = info->next) {
            if (info->peer == peer) {
              int no_match = FALSE;

              /* flagging additional checks are required */
              if (safi == SAFI_MPLS_VPN) no_match++;
              if (peer->cap_add_paths) no_match++;

              if (safi == SAFI_MPLS_VPN) {
                if (info->extra && !memcmp(&info->extra->rd, &rd, sizeof(rd_t))) no_match--;
              } 

              if (peer->cap_add_paths) {
                if (info->attr) {
                  if (info->attr->mp_nexthop.family == peer_dst_ip.family) {
                    if (!memcmp(&info->attr->mp_nexthop, &peer_dst_ip, HostAddrSz)) no_match--;
                  }
                }
              }

              if (!no_match) {
	        pptrs->bgp_dst_info = (char *) info;
	        break;
	      }
	    }
          }

          /* if having results, let's not potentially look in other buckets */
          if (pptrs->bgp_dst_info) break;
        }
      }
    }
#endif

    if (follow_default && safi != SAFI_MPLS_VPN) {
      default_node = NULL;

      if (pptrs->l3_proto == ETHERTYPE_IP) {
        memset(&default_prefix, 0, sizeof(default_prefix));
        default_prefix.family = AF_INET;

        result = (struct bgp_node *) pptrs->bgp_src;
        if (result && prefix_match(&result->p, &default_prefix)) {
	  default_node = result;
	  pptrs->bgp_src = NULL;
	  pptrs->bgp_src_info = NULL;
        }

        result = (struct bgp_node *) pptrs->bgp_dst;
        if (result && prefix_match(&result->p, &default_prefix)) {
	  default_node = result;
	  pptrs->bgp_dst = NULL;
	  pptrs->bgp_dst_info = NULL;
        }
      }
#if defined ENABLE_IPV6
      else if (pptrs->l3_proto == ETHERTYPE_IPV6) {
        memset(&default_prefix, 0, sizeof(default_prefix));
        default_prefix.family = AF_INET6;

        result = (struct bgp_node *) pptrs->bgp_src;
        if (result && prefix_match(&result->p, &default_prefix)) {
          default_node = result;
          info = result->info[modulo];
          pptrs->bgp_src = NULL;
          pptrs->bgp_src_info = NULL;
        }

        result = (struct bgp_node *) pptrs->bgp_dst;
        if (result && prefix_match(&result->p, &default_prefix)) {
          default_node = result;
          info = result->info[local_modulo];
          pptrs->bgp_dst = NULL;
          pptrs->bgp_dst_info = NULL;
        }
      }
#endif
      
      if (!pptrs->bgp_src || !pptrs->bgp_dst) {
	follow_default--;
	compare_bgp_port = FALSE; // XXX: fixme: follow default in NAT traversal scenarios

        if (default_node) {
          if (info && info->attr) {
            if (info->attr->mp_nexthop.family == AF_INET) {
              sa = &sa_local;
              memset(sa, 0, sizeof(struct sockaddr));
              sa->sa_family = AF_INET;
              memcpy(&((struct sockaddr_in *)sa)->sin_addr, &info->attr->mp_nexthop.address.ipv4, 4);
	      goto start_again;
            }
#if defined ENABLE_IPV6
            else if (info->attr->mp_nexthop.family == AF_INET6) {
              sa = &sa_local;
              memset(sa, 0, sizeof(struct sockaddr));
              sa->sa_family = AF_INET6;
              ip6_addr_cpy(&((struct sockaddr_in6 *)sa)->sin6_addr, &info->attr->mp_nexthop.address.ipv6);
              goto start_again;
            }
#endif
            else {
              sa = &sa_local;
              memset(sa, 0, sizeof(struct sockaddr));
              sa->sa_family = AF_INET;
              memcpy(&((struct sockaddr_in *)sa)->sin_addr, &info->attr->nexthop, 4);
              goto start_again;
	    }
	  }
        }
      }
    }

    if (config.nfacctd_bgp_follow_nexthop[0].family && pptrs->bgp_dst && safi != SAFI_MPLS_VPN)
      bgp_follow_nexthop_lookup(pptrs, type);
  }
}

void bgp_follow_nexthop_lookup(struct packet_ptrs *pptrs, int type)
{
  struct bgp_misc_structs *bms;
  struct bgp_rt_structs *inter_domain_routing_db;
  struct sockaddr *sa = (struct sockaddr *) pptrs->f_agent, sa_local;
  struct bgp_peer *nh_peer;
  struct bgp_node *result_node = NULL;
  struct bgp_info *info;
  char *result = NULL, *saved_info = NULL;
  int peers_idx, ttl = MAX_HOPS_FOLLOW_NH, self = MAX_NH_SELF_REFERENCES;
  int nh_idx, matched = 0;
  struct prefix nh, ch;
  struct in_addr pref4;
#if defined ENABLE_IPV6
  struct in6_addr pref6;
#endif
  char *saved_agent = pptrs->f_agent;
  pm_id_t bta;
  u_int32_t modulo, local_modulo, modulo_idx, modulo_max;

  bms = bgp_select_misc_db(type);
  inter_domain_routing_db = bgp_select_routing_db(type);

  if (!bms || !inter_domain_routing_db) return;

  start_again:

  if (config.nfacctd_bgp_to_agent_map && (*find_id_func)) {
    bta = 0;
    (*find_id_func)((struct id_table *)pptrs->bta_table, pptrs, &bta, NULL);
    if (bta) {
      sa = &sa_local;
      sa->sa_family = AF_INET;
      ((struct sockaddr_in *)sa)->sin_addr.s_addr = bta;
    }
  }

  // XXX: to be generalized
  for (nh_peer = NULL, peers_idx = 0; peers_idx < bms->max_peers; peers_idx++) {
    if (!sa_addr_cmp(sa, &peers[peers_idx].addr) || !sa_addr_cmp(sa, &peers[peers_idx].id)) {
      nh_peer = &peers[peers_idx];
      break;
    }
  }

  if (nh_peer) {
    modulo = bms->route_info_modulo(nh_peer, NULL);

    // XXX: to be optimized 
    if (bms->table_per_peer_hash == BGP_ASPATH_HASH_PATHID) modulo_max = bms->table_per_peer_buckets;
    else modulo_max = 1;

    memset(&ch, 0, sizeof(ch));
    ch.family = AF_INET;
    ch.prefixlen = 32;
    memcpy(&ch.u.prefix4, &nh_peer->addr.address.ipv4, 4);

    if (!result) {
      if (pptrs->l3_proto == ETHERTYPE_IP) {
        memcpy(&pref4, &((struct my_iphdr *)pptrs->iph_ptr)->ip_dst, sizeof(struct in_addr));
        result = (char *) bgp_node_match_ipv4(inter_domain_routing_db->rib[AFI_IP][SAFI_UNICAST], &pref4, nh_peer, type);
      }
#if defined ENABLE_IPV6
      else if (pptrs->l3_proto == ETHERTYPE_IPV6) {
        memcpy(&pref6, &((struct ip6_hdr *)pptrs->iph_ptr)->ip6_dst, sizeof(struct in6_addr));
        result = (char *) bgp_node_match_ipv6(inter_domain_routing_db->rib[AFI_IP6][SAFI_UNICAST], &pref6, nh_peer, type);
      }
#endif
    }

    memset(&nh, 0, sizeof(nh));
    result_node = (struct bgp_node *) result;

    if (result_node) {
      for (local_modulo = modulo, modulo_idx = 0; modulo_idx < modulo_max; local_modulo++, modulo_idx++) {
        for (info = result_node->info[modulo]; info; info = info->next) {
          if (info->peer == nh_peer) break;
	}
      }
    }
    else info = NULL;

    if (info && info->attr) {
      if (info->attr->mp_nexthop.family == AF_INET) {
	nh.family = AF_INET;
	nh.prefixlen = 32;
	memcpy(&nh.u.prefix4, &info->attr->mp_nexthop.address.ipv4, 4);

	for (nh_idx = 0; config.nfacctd_bgp_follow_nexthop[nh_idx].family && nh_idx < FOLLOW_BGP_NH_ENTRIES; nh_idx++) {
	  matched = prefix_match(&config.nfacctd_bgp_follow_nexthop[nh_idx], &nh);
	  if (matched) break;
	}

	if (matched && self > 0 && ttl > 0) { 
	  if (prefix_match(&ch, &nh)) self--;
          sa = &sa_local;
          pptrs->f_agent = (char *) &sa_local;
          memset(sa, 0, sizeof(struct sockaddr));
          sa->sa_family = AF_INET;
          memcpy(&((struct sockaddr_in *)sa)->sin_addr, &info->attr->mp_nexthop.address.ipv4, 4);
	  saved_info = (char *) info;
	  ttl--;
          goto start_again;
        }
	else goto end;
      }
#if defined ENABLE_IPV6
      else if (info->attr->mp_nexthop.family == AF_INET6) {
	nh.family = AF_INET6;
	nh.prefixlen = 128;
	memcpy(&nh.u.prefix6, &info->attr->mp_nexthop.address.ipv6, 16);

        for (nh_idx = 0; config.nfacctd_bgp_follow_nexthop[nh_idx].family && nh_idx < FOLLOW_BGP_NH_ENTRIES; nh_idx++) {
          matched = prefix_match(&config.nfacctd_bgp_follow_nexthop[nh_idx], &nh);
          if (matched) break;
        }

	if (matched && self > 0 && ttl > 0) {
	  if (prefix_match(&ch, &nh)) self--;
          sa = &sa_local;
          pptrs->f_agent = (char *) &sa_local;
          memset(sa, 0, sizeof(struct sockaddr));
          sa->sa_family = AF_INET6;
          ip6_addr_cpy(&((struct sockaddr_in6 *)sa)->sin6_addr, &info->attr->mp_nexthop.address.ipv6);
	  saved_info = (char *) info;
	  ttl--;
          goto start_again;
	}
	else goto end;
      }
#endif
      else {
	nh.family = AF_INET;
	nh.prefixlen = 32;
	memcpy(&nh.u.prefix4, &info->attr->nexthop, 4);

        for (nh_idx = 0; config.nfacctd_bgp_follow_nexthop[nh_idx].family && nh_idx < FOLLOW_BGP_NH_ENTRIES; nh_idx++) {
          matched = prefix_match(&config.nfacctd_bgp_follow_nexthop[nh_idx], &nh);
          if (matched) break;
        }

	if (matched && self > 0 && ttl > 0) {
	  if (prefix_match(&ch, &nh)) self--;
          sa = &sa_local;
          pptrs->f_agent = (char *) &sa_local;
          memset(sa, 0, sizeof(struct sockaddr));
          sa->sa_family = AF_INET;
          memcpy(&((struct sockaddr_in *)sa)->sin_addr, &info->attr->nexthop, 4);
	  saved_info = (char *) info;
	  ttl--;
          goto start_again;
	}
	else goto end;
      }
    }
  }

  end:

  if (saved_info) pptrs->bgp_nexthop_info = saved_info; 
  pptrs->f_agent = saved_agent;
}

struct bgp_peer *bgp_lookup_find_bgp_peer(struct sockaddr *sa, struct xflow_status_entry *xs_entry, u_int16_t l3_proto, int compare_bgp_port)
{
  struct bgp_peer *peer;
  u_int32_t peer_idx, *peer_idx_ptr;
  int peers_idx;

  peer_idx = 0; peer_idx_ptr = NULL;
  if (xs_entry) {
    if (l3_proto == ETHERTYPE_IP) {
      peer_idx = xs_entry->peer_v4_idx; 
      peer_idx_ptr = &xs_entry->peer_v4_idx;
    }
#if defined ENABLE_IPV6
    else if (l3_proto == ETHERTYPE_IPV6) {
      peer_idx = xs_entry->peer_v6_idx; 
      peer_idx_ptr = &xs_entry->peer_v6_idx;
    }
#endif
  }

  if (xs_entry && peer_idx) {
    if ((!sa_addr_cmp(sa, &peers[peer_idx].addr) || !sa_addr_cmp(sa, &peers[peer_idx].id)) &&
        (!compare_bgp_port || !sa_port_cmp(sa, peers[peer_idx].tcp_port))) {
      peer = &peers[peer_idx];
    }
    /* If no match then let's invalidate the entry */
    else {
      *peer_idx_ptr = 0;
      peer = NULL;
    }
  }
  else {
    for (peer = NULL, peers_idx = 0; peers_idx < config.nfacctd_bgp_max_peers; peers_idx++) {
      if ((!sa_addr_cmp(sa, &peers[peers_idx].addr) || !sa_addr_cmp(sa, &peers[peers_idx].id)) && 
	  (!compare_bgp_port || !sa_port_cmp(sa, peers[peer_idx].tcp_port))) {
        peer = &peers[peers_idx];
        if (xs_entry && peer_idx_ptr) *peer_idx_ptr = peers_idx;
        break;
      }
    }
  }

  return peer;
}

void pkt_to_cache_bgp_primitives(struct cache_bgp_primitives *c, struct pkt_bgp_primitives *p, pm_cfgreg_t what_to_count)
{
  if (c && p) {
    c->peer_src_as = p->peer_src_as;
    c->peer_dst_as = p->peer_dst_as;
    memcpy(&c->peer_src_ip, &p->peer_src_ip, HostAddrSz);
    memcpy(&c->peer_dst_ip, &p->peer_dst_ip, HostAddrSz);
    if (what_to_count & COUNT_STD_COMM) {
      if (!c->std_comms) {
	c->std_comms = malloc(MAX_BGP_STD_COMMS);
	if (!c->std_comms) goto malloc_failed;
      }
      memcpy(c->std_comms, p->std_comms, MAX_BGP_STD_COMMS);
    }
    else {
      if (c->std_comms) {
	free(c->std_comms);
	c->std_comms = NULL;
      }
    }
    if (what_to_count & COUNT_EXT_COMM) {
      if (!c->ext_comms) {
	c->ext_comms = malloc(MAX_BGP_EXT_COMMS);
	if (!c->ext_comms) goto malloc_failed;
      }
      memcpy(c->ext_comms, p->ext_comms, MAX_BGP_EXT_COMMS);
    }
    else {
      if (c->ext_comms) {
	free(c->ext_comms);
	c->ext_comms = NULL;
      }
    }
    if (what_to_count & COUNT_AS_PATH) {
      if (!c->as_path) {
	c->as_path = malloc(MAX_BGP_ASPATH);
	if (!c->as_path) goto malloc_failed;
      }
      memcpy(c->as_path, p->as_path, MAX_BGP_ASPATH);
    }
    else {
      if (c->as_path) {
	free(c->as_path);
	c->as_path = NULL;
      }
    }
    c->local_pref = p->local_pref;
    c->med = p->med;
    if (what_to_count & COUNT_SRC_STD_COMM) {
      if (!c->src_std_comms) {
	c->src_std_comms = malloc(MAX_BGP_STD_COMMS);
	if (!c->src_std_comms) goto malloc_failed;
      }
      memcpy(c->src_std_comms, p->src_std_comms, MAX_BGP_STD_COMMS);
    }
    else {
      if (c->src_std_comms) {
	free(c->src_std_comms);
	c->src_std_comms = NULL;
      }
    }
    if (what_to_count & COUNT_SRC_EXT_COMM) {
      if (!c->src_ext_comms) {
	c->src_ext_comms = malloc(MAX_BGP_EXT_COMMS);
	if (!c->src_ext_comms) goto malloc_failed;
      }
      memcpy(c->src_ext_comms, p->src_ext_comms, MAX_BGP_EXT_COMMS);
    }
    else {
      if (c->src_ext_comms) {
	free(c->src_ext_comms);
	c->src_ext_comms = NULL;
      }
    }
    if (what_to_count & COUNT_SRC_AS_PATH) {
      if (!c->src_as_path) {
	c->src_as_path = malloc(MAX_BGP_ASPATH);
	if (!c->src_as_path) goto malloc_failed;
      }
      memcpy(c->src_as_path, p->src_as_path, MAX_BGP_ASPATH);
    }
    else {
      if (c->src_as_path) {
	free(c->src_as_path);
	c->src_as_path = NULL;
      }
    }
    c->src_local_pref = p->src_local_pref;
    c->src_med = p->src_med;
    memcpy(&c->mpls_vpn_rd, &p->mpls_vpn_rd, sizeof(rd_t));

    return;

    malloc_failed:
    Log(LOG_WARNING, "WARN ( %s/%s ): malloc() failed (pkt_to_cache_bgp_primitives).\n", config.name, config.type);
  }
}

void cache_to_pkt_bgp_primitives(struct pkt_bgp_primitives *p, struct cache_bgp_primitives *c)
{
  if (c && p) {
    memset(p, 0, PbgpSz);

    p->peer_src_as = c->peer_src_as;
    p->peer_dst_as = c->peer_dst_as;
    memcpy(&p->peer_src_ip, &c->peer_src_ip, HostAddrSz);
    memcpy(&p->peer_dst_ip, &c->peer_dst_ip, HostAddrSz);
    if (c->std_comms) memcpy(p->std_comms, c->std_comms, MAX_BGP_STD_COMMS);
    if (c->ext_comms) memcpy(p->ext_comms, c->ext_comms, MAX_BGP_EXT_COMMS);
    if (c->as_path) memcpy(p->as_path, c->as_path, MAX_BGP_ASPATH);
    p->local_pref = c->local_pref;
    p->med = c->med;
    if (c->src_std_comms) memcpy(p->src_std_comms, c->src_std_comms, MAX_BGP_STD_COMMS);
    if (c->src_ext_comms) memcpy(p->src_ext_comms, c->src_ext_comms, MAX_BGP_EXT_COMMS);
    if (c->src_as_path) memcpy(p->src_as_path, c->src_as_path, MAX_BGP_ASPATH);
    p->src_local_pref = c->src_local_pref;
    p->src_med = c->src_med;
    memcpy(&p->mpls_vpn_rd, &c->mpls_vpn_rd, sizeof(rd_t));
  }
}

void free_cache_bgp_primitives(struct cache_bgp_primitives **c)
{
  struct cache_bgp_primitives *cbgp = *c;

  if (c && *c) {
    if (cbgp->std_comms) free(cbgp->std_comms);
    if (cbgp->ext_comms) free(cbgp->ext_comms);
    if (cbgp->as_path) free(cbgp->as_path);
    if (cbgp->src_std_comms) free(cbgp->src_std_comms);
    if (cbgp->src_ext_comms) free(cbgp->src_ext_comms);
    if (cbgp->src_as_path) free(cbgp->src_as_path);

    memset(cbgp, 0, sizeof(struct cache_bgp_primitives));
    free(*c);
    *c = NULL;
  }
}

u_int32_t bgp_route_info_modulo_pathid(struct bgp_peer *peer, path_id_t *path_id)
{
  struct bgp_misc_structs *bms = bgp_select_misc_db(peer->type);
  path_id_t local_path_id = 1;

  if (path_id && *path_id) local_path_id = *path_id;

  return (((peer->fd * bms->table_per_peer_buckets) +
          ((local_path_id - 1) % bms->table_per_peer_buckets)) %
          (bms->table_peer_buckets * bms->table_per_peer_buckets));
}
