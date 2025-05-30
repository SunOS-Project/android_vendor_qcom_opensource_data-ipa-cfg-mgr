/*
Copyright (c) 2013-2021, The Linux Foundation. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:
		* Redistributions of source code must retain the above copyright
			notice, this list of conditions and the following disclaimer.
		* Redistributions in binary form must reproduce the above
			copyright notice, this list of conditions and the following
			disclaimer in the documentation and/or other materials provided
			with the distribution.
		* Neither the name of The Linux Foundation nor the names of its
			contributors may be used to endorse or promote products derived
			from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
/*
 * ​​​​​Changes from Qualcomm Innovation Center are provided under the following license:
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
#include "IPACM_Conntrack_NATApp.h"
#include "IPACM_ConntrackClient.h"
#include "IPACM_ConntrackListener.h"
#ifdef FEATURE_IPACM_AIDL
#include "IPACM_OffloadManager.h"
#endif
#include "IPACM_Iface.h"

#define INVALID_IP_ADDR 0x0

#define HDR_METADATA_MUX_ID_BMASK 0x00FF0000
#define HDR_METADATA_MUX_ID_SHFT 0x10

#undef strcasesame
#define strcasesame(a, b) (!strcasecmp(a, b))

#undef  SRAM_IN_USE
#define SRAM_IN_USE() \
	( strcasesame(mem_type, "HYBRID" ) || \
	  strcasesame(mem_type, "SRAM" ) )

/* NatApp class Implementation */
NatApp *NatApp::pInstance = NULL;
NatApp::NatApp()
{
	max_entries = 0;
	mem_type = NULL;

	cache = NULL;

	nat_table_hdl = 0;
	pub_ip_addr = 0;
	pub_mux_id = 0;

	curCnt = 0;

	pALGPorts = NULL;
	nALGPort = 0;

	ct = NULL;
	ct_hdl = NULL;

	memset(temp, 0, sizeof(temp));
	m_fd_ipa = open(IPA_DEVICE_NAME, O_RDWR);
	if(m_fd_ipa < 0)
	{
		IPACMERR("Failed to open %s\n",IPA_DEVICE_NAME);
	}
}

NatApp::~NatApp()
{
	if (m_fd_ipa) {
		close(m_fd_ipa);
	}
}

int NatApp::Init(void)
{
	IPACM_Config *pConfig;
	int size = 0;

	pConfig = IPACM_Config::GetInstance();
	if(pConfig == NULL)
	{
		IPACMERR("Unable to get Config instance\n");
		return -1;
	}

	mem_type = pConfig->GetNatMemType();

	max_entries = pConfig->GetNatMaxEntries();

	size = (sizeof(nat_table_entry) * max_entries);
	cache = (nat_table_entry *)malloc(size);
	if(cache == NULL)
	{
		IPACMERR("Unable to allocate memory for cache\n");
		goto fail;
	}
	IPACMDBG("Allocated %d bytes for config manager nat cache\n", size);
	memset(cache, 0, size);

	nALGPort = pConfig->GetAlgPortCnt();
	if(nALGPort > 0)
	{
		pALGPorts = (ipacm_alg *)malloc(sizeof(ipacm_alg) * nALGPort);
		if(pALGPorts == NULL)
		{
			IPACMERR("Unable to allocate memory for alg prots\n");
			goto fail;
		}
		memset(pALGPorts, 0, sizeof(ipacm_alg) * nALGPort);

		pConfig->GetAlgPorts(nALGPort, pALGPorts);

		IPACMDBG("Printing %d alg ports information\n", nALGPort);
		for(int cnt=0; cnt<nALGPort; cnt++)
		{
			IPACMDBG("%d: Proto[%d], port[%d]\n", cnt, pALGPorts[cnt].protocol, pALGPorts[cnt].port);
		}
	}
	else
	{
		IPACMERR("Unable to retrieve ALG prot count\n");
		goto fail;
	}

	return 0;

fail:
	if(cache != NULL)
	{
		free(cache);
	}
	if(pALGPorts != NULL)
	{
		free(pALGPorts);
	}
	return -1;
}

NatApp* NatApp::GetInstance()
{
	if(pInstance == NULL)
	{
		pInstance = new NatApp();

		if(pInstance->Init())
		{
			delete pInstance;
			return NULL;
		}
	}

	return pInstance;
}

uint32_t NatApp::GenerateMetdata(uint8_t mux_id)
{
	return (mux_id << HDR_METADATA_MUX_ID_SHFT) & HDR_METADATA_MUX_ID_BMASK;
}

/* NAT APP related object function definitions */
int NatApp::AddTable(uint32_t pub_ip, uint8_t mux_id)
{
	int ret;
	int cnt = 0;
	ipa_nat_ipv4_rule nat_rule;
	IPACMDBG_H("%s() %d\n", __FUNCTION__, __LINE__);

	/* Not reset the cache wait it timeout by destroy event */
#if 0
	if (pub_ip != pub_ip_addr_pre)
	{
		IPACMDBG("Reset the cache because NAT-ipv4 different\n");
		memset(cache, 0, sizeof(nat_table_entry) * max_entries);
		curCnt = 0;
	}
#endif
	ret = ipa_nat_add_ipv4_tbl(pub_ip, mem_type, max_entries, &nat_table_hdl);
	if(ret)
	{
		IPACMERR("unable to create nat table Error:%d\n", ret);
		return ret;
	}
	if(IPACM_Iface::ipacmcfg->GetIPAVer() >= IPA_HW_v4_0) {
		/* modify PDN 0 so it will hold the mux ID in the src metadata field */
		ipa_nat_pdn_entry entry;

		entry.dst_metadata = 0;
		entry.src_metadata = GenerateMetdata(mux_id);
		entry.public_ip = pub_ip;
		ret = ipa_nat_modify_pdn(nat_table_hdl, 0, &entry);
		if(ret)
		{
			IPACMERR("unable to modify PDN 0 entry Error:%d INIT_HDR_METADATA register values will be used!\n", ret);
		}
	}

	/* Add back the cached NAT-entry */
	if (pub_ip == pub_ip_addr_pre)
	{
		IPACMDBG("Restore the cache to ipa NAT-table\n");
		for(cnt = 0; cnt < max_entries; cnt++)
		{
			if(cache[cnt].private_ip !=0)
			{
				memset(&nat_rule, 0 , sizeof(nat_rule));
				nat_rule.private_ip = cache[cnt].private_ip;
				nat_rule.target_ip = cache[cnt].target_ip;
				nat_rule.target_port = cache[cnt].target_port;
				nat_rule.private_port = cache[cnt].private_port;
				nat_rule.public_port = cache[cnt].public_port;
				nat_rule.protocol = cache[cnt].protocol;

				if(ipa_nat_add_ipv4_rule(nat_table_hdl, &nat_rule, &cache[cnt].rule_hdl) < 0)
				{
					IPACMERR("unable to add the rule delete from cache\n");
					memset(&cache[cnt], 0, sizeof(cache[cnt]));
					curCnt--;
					continue;
				}
				cache[cnt].enabled = true;
				/* send connections info to pcie modem only with DL direction */
				if ((CtList->backhaul_mode == Q6_MHI_WAN) && (cache[cnt].dst_nat == true || cache[cnt].protocol == IPPROTO_TCP))
				{
					ret = AddConnection(&cache[cnt]);
					if(ret > 0)
					{
						/* save the rule id for deletion */
						cache[cnt].rule_id = ret;
						IPACMDBG_H("rule-id(%d)\n", cache[cnt].rule_id);
					}
					else
					{
						IPACMERR("unable to add Connection to pcie modem: error:%d\n", ret);
						cache[cnt].rule_id = 0;
					}
				}

				IPACMDBG("On wan-iface reset added below rule successfully\n");
				iptodot("Private IP", nat_rule.private_ip);
				iptodot("Target IP", nat_rule.target_ip);
				IPACMDBG("Private Port:%d \t Target Port: %d\t", nat_rule.private_port, nat_rule.target_port);
				IPACMDBG("Public Port:%d\n", nat_rule.public_port);
				IPACMDBG("protocol: %d\n", nat_rule.protocol);
			}
		}
	}

	pub_ip_addr = pub_ip;
	pub_mux_id = mux_id;
	IPACMDBG(" Set pub_mux_id: %d\t", pub_mux_id);
	return 0;
}

void NatApp::Reset()
{
	nat_table_hdl = 0;
	pub_ip_addr = 0;
	pub_mux_id = 0;
}

int NatApp::DeleteTable(uint32_t pub_ip)
{
	int cnt = 0;
	int ret;
	IPACMDBG_H("%s() %d\n", __FUNCTION__, __LINE__);

	CHK_TBL_HDL();

	if(pub_ip_addr != pub_ip)
	{
		IPACMDBG("Public ip address is not matching\n");
		IPACMERR("unable to delete the nat table\n");
		return -1;
	}

	/* NAT tbl deleted, reset enabled bit */
	for(cnt = 0; cnt < max_entries; cnt++)
	{
		cache[cnt].enabled = false;
		/* send connections del info to pcie modem first */
		if ((CtList->backhaul_mode == Q6_MHI_WAN) && (cache[cnt].dst_nat == true || cache[cnt].protocol == IPPROTO_TCP) && (cache[cnt].rule_id > 0))

		{
			ret = DelConnection(cache[cnt].rule_id);
			if(ret)
			{
				IPACMERR("unable to del Connection to pcie modem: %d\n", ret);
			}
			else
			{
				/* save the rule id for deletion */
				cache[cnt].rule_id = 0;
			}
		}
	}

	ret = ipa_nat_del_ipv4_tbl(nat_table_hdl);
	if(ret)
	{
		IPACMERR("unable to delete nat table Error: %d\n", ret);;
		return ret;
	}

	pub_ip_addr_pre = pub_ip_addr;
	Reset();
	return 0;
}

int NatApp::MoveTable(bool to_ddr)
{
	int ret;

	if (to_ddr) {
		IPACMDBG_H("direction TO_DDR - move and lock table at DDR\n");
		ret = ipa_nat_switch_to(IPA_NAT_MEM_IN_DDR, true);
	} else {
		IPACMDBG_H("direction TO_SRAM - allow table transition to SRAM\n");
		ret = ipa_nat_switch_to(IPA_NAT_MEM_IN_DDR, false);
	}

	return ret;
}

/* Check for duplicate entries */
bool NatApp::ChkForDup(const nat_table_entry *rule)
{
	int cnt = 0;
	IPACMDBG("%s() %d\n", __FUNCTION__, __LINE__);

	for(; cnt < max_entries; cnt++)
	{
		if(cache[cnt].private_ip == rule->private_ip &&
			 cache[cnt].target_ip == rule->target_ip &&
			 cache[cnt].private_port ==  rule->private_port  &&
			 cache[cnt].target_port == rule->target_port &&
			 cache[cnt].protocol == rule->protocol)
		{
			log_nat(rule->protocol,rule->private_ip,rule->target_ip,rule->private_port,\
			rule->target_port,"Duplicate Rule\n");
			return true;
		}
	}

	return false;
}

/* Delete the entry from Nat table on connection close */
int NatApp::DeleteEntry(const nat_table_entry *rule)
{
	int cnt = 0;
	int ret = 0;
	IPACMDBG("%s() %d\n", __FUNCTION__, __LINE__);

	log_nat(rule->protocol,rule->private_ip,rule->target_ip,rule->private_port,\
	rule->target_port,"for deletion\n");


	for(; cnt < max_entries; cnt++)
	{
		if(cache[cnt].private_ip == rule->private_ip &&
			 cache[cnt].target_ip == rule->target_ip &&
			 cache[cnt].private_port ==  rule->private_port  &&
			 cache[cnt].target_port == rule->target_port &&
			 cache[cnt].protocol == rule->protocol)
		{

			if(cache[cnt].enabled == true)
			{
				/* send connections del info to pcie modem first */
				if ((CtList->backhaul_mode == Q6_MHI_WAN) && (cache[cnt].dst_nat == true || cache[cnt].protocol == IPPROTO_TCP) && (cache[cnt].rule_id > 0))
				{
					ret = DelConnection(cache[cnt].rule_id);
					if(ret)
					{
						IPACMERR("unable to del Connection to pcie modem: %d\n", ret);
					}
					else
					{
						/* save the rule id for deletion */
						cache[cnt].rule_id = 0;
					}
				}

				if(ipa_nat_del_ipv4_rule(nat_table_hdl, cache[cnt].rule_hdl) < 0)
				{
					IPACMERR("%s() %d deletion failed\n", __FUNCTION__, __LINE__);
				}

				IPACMDBG_H("Deleted Nat entry(%d) Successfully\n", cnt);
			}
			else
			{
				IPACMDBG_H("Deleted Nat entry(%d) only from cache\n", cnt);
			}

			memset(&cache[cnt], 0, sizeof(cache[cnt]));
			curCnt--;
			break;
		}
	}

	return 0;
}

/* Add new entry to the nat table on new connection */
int NatApp::AddEntry(const nat_table_entry *rule)
{
	int cnt = 0;
	ipa_nat_ipv4_rule nat_rule;
	int ret = 0;

	IPACMDBG("%s() %d\n", __FUNCTION__, __LINE__);

	CHK_TBL_HDL();
	log_nat(rule->protocol,rule->private_ip,rule->target_ip,rule->private_port,\
	rule->target_port,"for addition\n");
	if(isAlgPort(rule->protocol, rule->private_port) ||
		 isAlgPort(rule->protocol, rule->target_port))
	{
		IPACMERR("connection using ALG Port, ignore\n");
		return -1;
	}

	if(rule->private_ip == 0 ||
		 rule->target_ip == 0 ||
		 rule->private_port == 0  ||
		 rule->target_port == 0 ||
		 rule->protocol == 0)
	{
		IPACMERR("Invalid Connection, ignoring it\n");
		return 0;
	}

	if(!ChkForDup(rule))
	{
		for(; cnt < max_entries; cnt++)
		{
			if(cache[cnt].private_ip == 0 &&
				 cache[cnt].target_ip == 0 &&
				 cache[cnt].private_port == 0  &&
				 cache[cnt].target_port == 0 &&
				 cache[cnt].protocol == 0)
			{
				break;
			}
		}

		if(max_entries == cnt)
		{
			IPACMERR("Error: Unable to add, reached maximum rules\n");
			return -1;
		}
		else
		{
			memset(&nat_rule, 0, sizeof(nat_rule));
			nat_rule.private_ip = rule->private_ip;
			nat_rule.target_ip = rule->target_ip;
			nat_rule.target_port = rule->target_port;
			nat_rule.private_port = rule->private_port;
			nat_rule.public_port = rule->public_port;
			nat_rule.protocol = rule->protocol;

			if(isPwrSaveIf(rule->private_ip) ||
				 isPwrSaveIf(rule->target_ip))
			{
				IPACMDBG("Device is Power Save mode: Dont insert into nat table but cache\n");
				cache[cnt].enabled = false;
				cache[cnt].rule_hdl = 0;
			}
			else
			{

				if(ipa_nat_add_ipv4_rule(nat_table_hdl, &nat_rule, &cache[cnt].rule_hdl) < 0)
				{
					IPACMERR("unable to add the rule\n");
					return -1;
				}

				cache[cnt].enabled = true;
				/* send connections info to pcie modem only with DL direction */
				if ((CtList->backhaul_mode == Q6_MHI_WAN) && (rule->dst_nat == true || rule->protocol == IPPROTO_TCP))
				{
					ret = AddConnection(rule);
					if(ret > 0)
					{
						/* save the rule id for deletion */
						cache[cnt].rule_id = ret;
						IPACMDBG_H("rule-id(%d)\n", cache[cnt].rule_id);
					}
					else
					{
						IPACMERR("unable to add Connection to pcie modem: error:%d\n", ret);
						cache[cnt].rule_id = 0;
					}
				}
			}
			cache[cnt].private_ip = rule->private_ip;
			cache[cnt].target_ip = rule->target_ip;
			cache[cnt].target_port = rule->target_port;
			cache[cnt].private_port = rule->private_port;
			cache[cnt].protocol = rule->protocol;
			cache[cnt].timestamp = 0;
			cache[cnt].public_port = rule->public_port;
			cache[cnt].dst_nat = rule->dst_nat;
			curCnt++;
		}

	}
	else
	{
		IPACMERR("Duplicate rule. Ignore it\n");
		return -1;
	}

	if(cache[cnt].enabled == true)
	{
		IPACMDBG_H("Added rule(%d) successfully\n", cnt);
	}
  else
  {
    IPACMDBG_H("Cached rule(%d) successfully\n", cnt);
  }

	return 0;
}

/* Add new entry to the nat table on new connection, return rule-id */
int NatApp::AddConnection(const nat_table_entry *rule)
{
	int len, res = IPACM_SUCCESS;
	ipa_ioc_add_flt_rule *pFilteringTable = NULL;

	/* contruct filter rules to pcie modem */
	struct ipa_flt_rule_add flt_rule_entry;
	ipa_ioc_generate_flt_eq flt_eq;

	IPACMDBG("\n");
	len = sizeof(struct ipa_ioc_add_flt_rule) + sizeof(struct ipa_flt_rule_add);
	pFilteringTable = (struct ipa_ioc_add_flt_rule*)malloc(len);
	if (pFilteringTable == NULL)
	{
		IPACMERR("Error Locate ipa_flt_rule_add memory...\n");
		return IPACM_FAILURE;
	}
	memset(pFilteringTable, 0, len);


	pFilteringTable->commit = 1;
	pFilteringTable->global = false;
	pFilteringTable->ip = IPA_IP_v4;
	pFilteringTable->num_rules = (uint8_t)1;

	/* Configuring Software-Routing Filtering Rule */
	memset(&flt_rule_entry, 0, sizeof(struct ipa_flt_rule_add));
	flt_rule_entry.at_rear = true;
	flt_rule_entry.flt_rule_hdl = -1;
	flt_rule_entry.status = -1;

	flt_rule_entry.rule.retain_hdr = 1;
	flt_rule_entry.rule.to_uc = 0;
	flt_rule_entry.rule.eq_attrib_type = 1;
	flt_rule_entry.rule.action = IPA_PASS_TO_ROUTING;
	if (IPACM_Iface::ipacmcfg->isIPAv3Supported())
		flt_rule_entry.rule.hashable = true;
	flt_rule_entry.rule.attrib.attrib_mask |= IPA_FLT_SRC_PORT;
	flt_rule_entry.rule.attrib.src_port = rule->target_port;
	flt_rule_entry.rule.attrib.attrib_mask |= IPA_FLT_DST_PORT;
	flt_rule_entry.rule.attrib.dst_port = rule->public_port;
	flt_rule_entry.rule.attrib.attrib_mask |= IPA_FLT_PROTOCOL;
	flt_rule_entry.rule.attrib.u.v4.protocol = rule->protocol;
	flt_rule_entry.rule.attrib.attrib_mask |= IPA_FLT_DST_ADDR;
	flt_rule_entry.rule.attrib.u.v4.dst_addr_mask = 0xFFFFFFFF;
	flt_rule_entry.rule.attrib.u.v4.dst_addr = rule->public_ip;
	flt_rule_entry.rule.attrib.attrib_mask |= IPA_FLT_SRC_ADDR;
	flt_rule_entry.rule.attrib.u.v4.src_addr_mask = 0xFFFFFFFF;
	flt_rule_entry.rule.attrib.u.v4.src_addr = rule->target_ip;
	IPACMDBG_H("src(0x%x) port(%d)->dst(0x%x) port(%d), protocol(%d) pub_mux_id (%d)\n",
				rule->target_ip, rule->target_port, rule->public_ip, rule->public_port,
				rule->protocol, pub_mux_id);

	memset(&flt_eq, 0, sizeof(flt_eq));
	memcpy(&flt_eq.attrib, &flt_rule_entry.rule.attrib, sizeof(flt_eq.attrib));
	flt_eq.ip = IPA_IP_v4;
	if(0 != ioctl(m_fd_ipa, IPA_IOC_GENERATE_FLT_EQ, &flt_eq))
	{
		IPACMERR("Failed to get eq_attrib\n");
		res = IPACM_FAILURE;
		goto fail;
	}
	memcpy(&flt_rule_entry.rule.eq_attrib,
		&flt_eq.eq_attrib,
		sizeof(flt_rule_entry.rule.eq_attrib));
	memcpy(&(pFilteringTable->rules[0]), &flt_rule_entry, sizeof(struct ipa_flt_rule_add));

	if(false == IPACM_Iface::m_filtering.AddOffloadFilteringRule(pFilteringTable, pub_mux_id, 0))
	{
		IPACMERR("Failed to install WAN DL filtering table.\n");
		res = IPACM_FAILURE;
		goto fail;
	}

	/* get rule-id */
	res = pFilteringTable->rules[0].flt_rule_hdl;

fail:
	if(pFilteringTable != NULL)
	{
		free(pFilteringTable);
	}
	return res;
}

int NatApp::DelConnection(const uint32_t rule_id)
{
	int len, res = IPACM_SUCCESS;
	ipa_ioc_del_flt_rule *pFilteringTable = NULL;


	struct ipa_flt_rule_del flt_rule_entry;

	IPACMDBG("\n");
	len = sizeof(struct ipa_ioc_del_flt_rule) + sizeof(struct ipa_flt_rule_del);
	pFilteringTable = (struct ipa_ioc_del_flt_rule*)malloc(len);
	if (pFilteringTable == NULL)
	{
		IPACMERR("Error Locate ipa_ioc_del_flt_rule memory...\n");
		return IPACM_FAILURE;
	}
	memset(pFilteringTable, 0, len);


	pFilteringTable->commit = 1;
	pFilteringTable->ip = IPA_IP_v4;
	pFilteringTable->num_hdls = (uint8_t)1;

	/* Configuring Software-Routing Filtering Rule */
	memset(&flt_rule_entry, 0, sizeof(struct ipa_flt_rule_del));
	flt_rule_entry.hdl = rule_id;

	memcpy(&(pFilteringTable->hdl[0]), &flt_rule_entry, sizeof(struct ipa_flt_rule_del));

	if(false == IPACM_Iface::m_filtering.DelOffloadFilteringRule(pFilteringTable))
	{
		IPACMERR("Failed to install WAN DL filtering table.\n");
		res = IPACM_FAILURE;
		goto fail;
	}

fail:
	if(pFilteringTable != NULL)
	{
		free(pFilteringTable);
	}
	return res;
}

void NatApp::UpdateCTUdpTs(nat_table_entry *rule, uint32_t new_ts)
{
#ifdef FEATURE_IPACM_AIDL
	IOffloadManager::ConntrackTimeoutUpdater::natTimeoutUpdate_t entry;
	IPACM_OffloadManager* OffloadMng;
#endif
	iptodot("Private IP:", rule->private_ip);
	iptodot("Target IP:",  rule->target_ip);
	IPACMDBG("Private Port: %d, Target Port: %d\n", rule->private_port, rule->target_port);

#ifndef FEATURE_IPACM_AIDL
	int ret;
	if(!ct_hdl)
	{
		ct_hdl = nfct_open(CONNTRACK, 0);
		if(!ct_hdl)
		{
			PERROR("nfct_open");
			return;
		}
	}

	if(!ct)
	{
		ct = nfct_new();
		if(!ct)
		{
			PERROR("nfct_new");
			return;
		}
	}

	nfct_set_attr_u8(ct, ATTR_L3PROTO, AF_INET);
	if(rule->protocol == IPPROTO_UDP)
	{
		nfct_set_attr_u8(ct, ATTR_L4PROTO, rule->protocol);
		nfct_set_attr_u32(ct, ATTR_TIMEOUT, udp_timeout);
	}
	else
	{
		nfct_set_attr_u8(ct, ATTR_L4PROTO, rule->protocol);
		nfct_set_attr_u32(ct, ATTR_TIMEOUT, tcp_timeout);
	}

	if(rule->dst_nat == false)
	{
		nfct_set_attr_u32(ct, ATTR_IPV4_SRC, htonl(rule->private_ip));
		nfct_set_attr_u16(ct, ATTR_PORT_SRC, htons(rule->private_port));

		nfct_set_attr_u32(ct, ATTR_IPV4_DST, htonl(rule->target_ip));
		nfct_set_attr_u16(ct, ATTR_PORT_DST, htons(rule->target_port));

		IPACMDBG("dst nat is not set\n");
	}
	else
	{
		nfct_set_attr_u32(ct, ATTR_IPV4_SRC, htonl(rule->target_ip));
		nfct_set_attr_u16(ct, ATTR_PORT_SRC, htons(rule->target_port));

		nfct_set_attr_u32(ct, ATTR_IPV4_DST, htonl(pub_ip_addr));
		nfct_set_attr_u16(ct, ATTR_PORT_DST, htons(rule->public_port));

		IPACMDBG("dst nat is set\n");
	}

	iptodot("Source IP:", nfct_get_attr_u32(ct, ATTR_IPV4_SRC));
	iptodot("Destination IP:",  nfct_get_attr_u32(ct, ATTR_IPV4_DST));
	IPACMDBG("Source Port: %d, Destination Port: %d\n",
					 nfct_get_attr_u16(ct, ATTR_PORT_SRC), nfct_get_attr_u16(ct, ATTR_PORT_DST));

	IPACMDBG("updating %d connection with time: %d\n",
					 rule->protocol, nfct_get_attr_u32(ct, ATTR_TIMEOUT));

	ret = nfct_query(ct_hdl, NFCT_Q_UPDATE, ct);
	if(ret == -1)
	{
		IPACMERR("unable to update time stamp");
		DeleteEntry(rule);
	}
	else
	{
		rule->timestamp = new_ts;
		IPACMDBG("Updated time stamp successfully\n");
	}
#else
	if(rule->protocol == IPPROTO_UDP)
	{
		entry.proto = IOffloadManager::ConntrackTimeoutUpdater::UDP;;
	}
	else
	{
		entry.proto = IOffloadManager::ConntrackTimeoutUpdater::TCP;
	}

	if(rule->dst_nat == false)
	{
		entry.src.ipAddr = htonl(rule->private_ip);
		entry.src.port = rule->private_port;
		entry.dst.ipAddr = htonl(rule->target_ip);
		entry.dst.port = rule->target_port;
		IPACMDBG("dst nat is not set\n");
	}
	else
	{
		entry.src.ipAddr = htonl(rule->target_ip);
		entry.src.port = rule->target_port;
		entry.dst.ipAddr = htonl(pub_ip_addr);
		entry.dst.port = rule->public_port;
		IPACMDBG("dst nat is set\n");
	}

	iptodot("Source IP:", entry.src.ipAddr);
	iptodot("Destination IP:",  entry.dst.ipAddr);
	IPACMDBG("Source Port: %d, Destination Port: %d\n",
					entry.src.port, entry.dst.port);

	OffloadMng = IPACM_OffloadManager::GetInstance();
	if (OffloadMng->touInstance == NULL) {
		IPACMERR("OffloadMng->touInstance is NULL, can't forward to framework!\n");
	} else {
		OffloadMng->touInstance->updateTimeout(entry);
		IPACMDBG("Updated time stamp successfully\n");
		rule->timestamp = new_ts;
	}
#endif
	return;
}

void NatApp::UpdateUDPTimeStamp()
{
	int cnt;
	uint32_t ts;
	bool read_to = false;
	bool keep_awake;

	keep_awake = ( max_entries && SRAM_IN_USE() && ipa_nat_is_sram_supported() );

	if ( keep_awake )
	{
		IPACMDBG("Voting clock on\n");

		if ( ipa_nat_vote_clock(IPA_APP_CLK_VOTE) != 0 )
		{
			IPACMERR("Voting clock on failed\n");
			return;
		}
	}

	for(cnt = 0; cnt < max_entries; cnt++)
	{
		ts = 0;
		if(cache[cnt].enabled == true &&
		   (cache[cnt].private_ip != cache[cnt].public_ip))
		{
			IPACMDBG("\n");
			if(ipa_nat_query_timestamp(nat_table_hdl, cache[cnt].rule_hdl, &ts) < 0)
			{
				IPACMERR("unable to retrieve timeout for rule hanle: %d\n", cache[cnt].rule_hdl);
				continue;
			}

			if(cache[cnt].timestamp == ts)
			{
				IPACMDBG("No Change in Time Stamp: cahce:%d, ipahw:%d\n",
								                  cache[cnt].timestamp, ts);
				continue;
			}

			if (read_to == false) {
				read_to = true;
				Read_TcpUdp_Timeout();
			}

			UpdateCTUdpTs(&cache[cnt], ts);
		} /* end of outer if */

	} /* end of for loop */

	if ( keep_awake )
	{
		IPACMDBG("Voting clock off\n");

		if ( ipa_nat_vote_clock(IPA_APP_CLK_DEVOTE) != 0 )
		{
			IPACMERR("Voting clock off failed\n");
		}
	}
}

bool NatApp::isAlgPort(uint8_t proto, uint16_t port)
{
	int cnt;
	for(cnt = 0; cnt < nALGPort; cnt++)
	{
		if(proto == pALGPorts[cnt].protocol &&
			 port == pALGPorts[cnt].port)
		{
			return true;
		}
	}

	return false;
}

bool NatApp::isPwrSaveIf(uint32_t ip_addr)
{
	int cnt;

	for(cnt = 0; cnt < IPA_MAX_NUM_WIFI_CLIENTS; cnt++)
	{
		if(0 != PwrSaveIfs[cnt] &&
			 ip_addr == PwrSaveIfs[cnt])
		{
			return true;
		}
	}

	return false;
}

int NatApp::UpdatePwrSaveIf(uint32_t client_lan_ip)
{
	int cnt, ret;
	IPACMDBG_H("Received IP address: 0x%x\n", client_lan_ip);

	if(client_lan_ip == INVALID_IP_ADDR)
	{
		IPACMERR("Invalid ip address received\n");
		return -1;
	}

	/* check for duplicate events */
	for(cnt = 0; cnt < IPA_MAX_NUM_WIFI_CLIENTS; cnt++)
	{
		if(PwrSaveIfs[cnt] == client_lan_ip)
		{
			IPACMDBG("The client 0x%x is already in power save\n", client_lan_ip);
			return 0;
		}
	}

	for(cnt = 0; cnt < IPA_MAX_NUM_WIFI_CLIENTS; cnt++)
	{
		if(PwrSaveIfs[cnt] == 0)
		{
			PwrSaveIfs[cnt] = client_lan_ip;
			break;
		}
	}

	for(cnt = 0; cnt < max_entries; cnt++)
	{
		if(cache[cnt].private_ip == client_lan_ip &&
			 cache[cnt].enabled == true)
		{
			/* send connections del info to pcie modem first */
			if ((CtList->backhaul_mode == Q6_MHI_WAN) && (cache[cnt].dst_nat == true || cache[cnt].protocol == IPPROTO_TCP) && (cache[cnt].rule_id > 0))
			{
				ret = DelConnection(cache[cnt].rule_id);
				if(ret)
				{
					IPACMERR("unable to del Connection to pcie modem: %d\n", ret);
				}
				else
				{
					/* save the rule id for deletion */
					cache[cnt].rule_id = 0;
				}
			}

			if(ipa_nat_del_ipv4_rule(nat_table_hdl, cache[cnt].rule_hdl) < 0)
			{
				IPACMERR("unable to delete the rule\n");
				continue;
			}

			cache[cnt].enabled = false;
			cache[cnt].rule_hdl = 0;
		}
	}

	return 0;
}

int NatApp::ResetPwrSaveIf(uint32_t client_lan_ip)
{
	int cnt, ret;
	ipa_nat_ipv4_rule nat_rule;

	IPACMDBG_H("Received ip address: 0x%x\n", client_lan_ip);

	if(client_lan_ip == INVALID_IP_ADDR)
	{
		IPACMERR("Invalid ip address received\n");
		return -1;
	}

	for(cnt = 0; cnt < IPA_MAX_NUM_WIFI_CLIENTS; cnt++)
	{
		if(PwrSaveIfs[cnt] == client_lan_ip)
		{
			PwrSaveIfs[cnt] = 0;
			break;
		}
	}

	for(cnt = 0; cnt < max_entries; cnt++)
	{
		IPACMDBG("cache (%d): enable %d, ip 0x%x\n", cnt, cache[cnt].enabled, cache[cnt].private_ip);

		if(cache[cnt].private_ip == client_lan_ip &&
			 cache[cnt].enabled == false)
		{
			memset(&nat_rule, 0 , sizeof(nat_rule));
			nat_rule.private_ip = cache[cnt].private_ip;
			nat_rule.target_ip = cache[cnt].target_ip;
			nat_rule.target_port = cache[cnt].target_port;
			nat_rule.private_port = cache[cnt].private_port;
			nat_rule.public_port = cache[cnt].public_port;
			nat_rule.protocol = cache[cnt].protocol;

			if(ipa_nat_add_ipv4_rule(nat_table_hdl, &nat_rule, &cache[cnt].rule_hdl) < 0)
			{
				IPACMERR("unable to add the rule delete from cache\n");
				memset(&cache[cnt], 0, sizeof(cache[cnt]));
				curCnt--;
				continue;
			}
			cache[cnt].enabled = true;
			/* send connections info to pcie modem only with DL direction */
			if ((CtList->backhaul_mode == Q6_MHI_WAN) && (cache[cnt].dst_nat == true || cache[cnt].protocol == IPPROTO_TCP))
			{
				ret = AddConnection(&cache[cnt]);
				if(ret > 0)
				{
					/* save the rule id for deletion */
					cache[cnt].rule_id = ret;
					IPACMDBG_H("rule-id(%d)\n", cache[cnt].rule_id);
				}
				else
				{
					IPACMERR("unable to add Connection to pcie modem: error:%d\n", ret);
					cache[cnt].rule_id = 0;
				}
			}

			IPACMDBG("On power reset added below rule successfully\n");
			iptodot("Private IP", nat_rule.private_ip);
			iptodot("Target IP", nat_rule.target_ip);
			IPACMDBG("Private Port:%d \t Target Port: %d\t", nat_rule.private_port, nat_rule.target_port);
			IPACMDBG("Public Port:%d\n", nat_rule.public_port);
			IPACMDBG("protocol: %d\n", nat_rule.protocol);

		}
	}

	return -1;
}

uint32_t NatApp::GetTableHdl(uint32_t in_ip_addr)
{
	if(in_ip_addr == pub_ip_addr)
	{
		return nat_table_hdl;
	}

	return -1;
}

void NatApp::AddTempEntry(const nat_table_entry *new_entry)
{
	int cnt;

	IPACMDBG("Received below Temp Nat entry\n");
	iptodot("Private IP", new_entry->private_ip);
	iptodot("Target IP", new_entry->target_ip);
	IPACMDBG("Private Port: %d\t Target Port: %d\t", new_entry->private_port, new_entry->target_port);
	IPACMDBG("protocolcol: %d\n", new_entry->protocol);

	if(isAlgPort(new_entry->protocol, new_entry->private_port) ||
		 isAlgPort(new_entry->protocol, new_entry->target_port))
	{
		IPACMDBG("connection using ALG Port. Dont insert into nat cache\n");
		return;
	}

	if(ChkForDup(new_entry))
	{
		return;
	}

	for(cnt=0; cnt<MAX_TEMP_ENTRIES; cnt++)
	{
		if(temp[cnt].private_ip == new_entry->private_ip &&
			 temp[cnt].target_ip == new_entry->target_ip &&
			 temp[cnt].private_port ==  new_entry->private_port  &&
			 temp[cnt].target_port == new_entry->target_port &&
			 temp[cnt].protocol == new_entry->protocol)
		{
			IPACMDBG("Received duplicate Temp entry\n");
			return;
		}
	}

	for(cnt=0; cnt<MAX_TEMP_ENTRIES; cnt++)
	{
		if(temp[cnt].private_ip == 0 &&
			 temp[cnt].target_ip == 0)
		{
			memcpy(&temp[cnt], new_entry, sizeof(nat_table_entry));
			IPACMDBG("Added Temp Entry\n");
			return;
		}
	}

	IPACMDBG("Unable to add temp entry, cache full\n");
	return;
}

void NatApp::DeleteTempEntry(const nat_table_entry *entry)
{
	int cnt;

	IPACMDBG("Received below nat entry\n");
	iptodot("Private IP", entry->private_ip);
	iptodot("Target IP", entry->target_ip);
	IPACMDBG("Private Port: %d\t Target Port: %d\n", entry->private_port, entry->target_port);
	IPACMDBG("protocol: %d\n", entry->protocol);

	for(cnt=0; cnt<MAX_TEMP_ENTRIES; cnt++)
	{
		if(temp[cnt].private_ip == entry->private_ip &&
			 temp[cnt].target_ip == entry->target_ip &&
			 temp[cnt].private_port ==  entry->private_port  &&
			 temp[cnt].target_port == entry->target_port &&
			 temp[cnt].protocol == entry->protocol)
		{
			memset(&temp[cnt], 0, sizeof(nat_table_entry));
			IPACMDBG("Delete Temp Entry\n");
			return;
		}
	}

	IPACMDBG("No Such Temp Entry exists\n");
	return;
}

void NatApp::FlushTempEntries(uint32_t ip_addr, bool isAdd,
		bool isDummy)
{
	int cnt;
	int ret;

	IPACMDBG_H("Received below with isAdd:%d ", isAdd);
	iptodot("IP Address: ", ip_addr);

	for(cnt=0; cnt<MAX_TEMP_ENTRIES; cnt++)
	{
		if(temp[cnt].private_ip == ip_addr ||
			 temp[cnt].target_ip == ip_addr)
		{
			if(isAdd)
			{
				if(temp[cnt].public_ip == pub_ip_addr)
				{
					if (isDummy) {
						/* To avoild DL expections for non IPA path */
						temp[cnt].private_ip = temp[cnt].public_ip;
						temp[cnt].private_port = temp[cnt].public_port;
						IPACMDBG("Flushing dummy temp rule");
						iptodot("Private IP", temp[cnt].private_ip);
					}

					ret = AddEntry(&temp[cnt]);
					if(ret)
					{
						IPACMERR("unable to add temp entry: %d\n", ret);
						continue;
					}
				}
			}
			memset(&temp[cnt], 0, sizeof(nat_table_entry));
		}
	}

	return;
}

int NatApp::DelEntriesOnClntDiscon(uint32_t ip_addr)
{
	int cnt, tmp = 0, ret;
	IPACMDBG_H("Received IP address: 0x%x\n", ip_addr);

	if(ip_addr == INVALID_IP_ADDR)
	{
		IPACMERR("Invalid ip address received\n");
		return -1;
	}

	for(cnt = 0; cnt < IPA_MAX_NUM_WIFI_CLIENTS; cnt++)
	{
		if(PwrSaveIfs[cnt] == ip_addr)
		{
			PwrSaveIfs[cnt] = 0;
			IPACMDBG("Remove %d power save entry\n", cnt);
			break;
		}
	}

	for(cnt = 0; cnt < max_entries; cnt++)
	{
		if(cache[cnt].private_ip == ip_addr)
		{
			if(cache[cnt].enabled == true)
			{
				/* send connections del info to pcie modem first */
				if ((CtList->backhaul_mode == Q6_MHI_WAN) && (cache[cnt].dst_nat == true || cache[cnt].protocol == IPPROTO_TCP) && (cache[cnt].rule_id > 0))
				{
					ret = DelConnection(cache[cnt].rule_id);
					if(ret)
					{
						IPACMERR("unable to del Connection to pcie modem: %d\n", ret);
					}
					else
					{
						/* save the rule id for deletion */
						cache[cnt].rule_id = 0;
					}
				}

				if(ipa_nat_del_ipv4_rule(nat_table_hdl, cache[cnt].rule_hdl) < 0)
				{
					IPACMERR("unable to delete the rule\n");
					continue;
				}
				else
				{
					IPACMDBG("won't delete the rule\n");
					cache[cnt].enabled = false;
					tmp++;
				}
			}
			IPACMDBG("won't delete the rule for entry %d, enabled %d\n",cnt, cache[cnt].enabled);
		}
	}

	IPACMDBG("Deleted (but cached) %d entries\n", tmp);
	return 0;
}

int NatApp::DelEntriesOnSTAClntDiscon(uint32_t ip_addr)
{
	int cnt, tmp = curCnt, ret;
	IPACMDBG_H("Received IP address: 0x%x\n", ip_addr);

	if(ip_addr == INVALID_IP_ADDR)
	{
		IPACMERR("Invalid ip address received\n");
		return -1;
	}


	for(cnt = 0; cnt < max_entries; cnt++)
	{
		if(cache[cnt].target_ip == ip_addr)
		{
			if(cache[cnt].enabled == true)
			{
				/* send connections del info to pcie modem first */
				if ((CtList->backhaul_mode == Q6_MHI_WAN) && (cache[cnt].dst_nat == true || cache[cnt].protocol == IPPROTO_TCP) && (cache[cnt].rule_id > 0))
				{
					ret = DelConnection(cache[cnt].rule_id);
					if(ret)
					{
						IPACMERR("unable to del Connection to pcie modem: %d\n", ret);
					}
					else
					{
						/* save the rule id for deletion */
						cache[cnt].rule_id = 0;
					}
				}

				if(ipa_nat_del_ipv4_rule(nat_table_hdl, cache[cnt].rule_hdl) < 0)
				{
					IPACMERR("unable to delete the rule\n");
					continue;
				}
			}

			memset(&cache[cnt], 0, sizeof(cache[cnt]));
			curCnt--;
		}
	}

	IPACMDBG("Deleted %d entries\n", (tmp - curCnt));
	return 0;
}

void NatApp::CacheEntry(const nat_table_entry *rule)
{
	int cnt;

	if(rule->private_ip == 0 ||
		 rule->target_ip == 0 ||
		 rule->private_port == 0  ||
		 rule->target_port == 0 ||
		 rule->protocol == 0)
	{
		IPACMERR("Invalid Connection, ignoring it\n");
		return;
	}

	if(!ChkForDup(rule))
	{
		for(cnt=0; cnt < max_entries; cnt++)
		{
			if(cache[cnt].private_ip == 0 &&
				 cache[cnt].target_ip == 0 &&
				 cache[cnt].private_port == 0  &&
				 cache[cnt].target_port == 0 &&
				 cache[cnt].protocol == 0)
			{
				break;
			}
		}

		if(max_entries == cnt)
		{
			IPACMERR("Error: Unable to add, reached maximum rules\n");
			return;
		}
		else
		{
			cache[cnt].enabled = false;
			cache[cnt].rule_hdl = 0;
			cache[cnt].private_ip = rule->private_ip;
			cache[cnt].target_ip = rule->target_ip;
			cache[cnt].target_port = rule->target_port;
			cache[cnt].private_port = rule->private_port;
			cache[cnt].protocol = rule->protocol;
			cache[cnt].timestamp = 0;
			cache[cnt].public_port = rule->public_port;
			cache[cnt].public_ip = rule->public_ip;
			cache[cnt].dst_nat = rule->dst_nat;
			curCnt++;
		}

	}
	else
	{
		IPACMERR("Duplicate rule. Ignore it\n");
		return;
	}

	IPACMDBG("Cached rule(%d) successfully\n", cnt);
	return;
}

void NatApp::Read_TcpUdp_Timeout(void) {
#ifdef FEATURE_IPACM_AIDL
	tcp_timeout = 432000;
	udp_timeout = 120;
	IPACMDBG_H("udp timeout value: %d\n", udp_timeout);
	IPACMDBG_H("tcp timeout value: %d\n", tcp_timeout);
#else
	FILE *udp_fd = NULL, *tcp_fd = NULL;
	/* Read UDP timeout value */
	udp_fd = fopen(IPACM_UDP_FULL_FILE_NAME, "r");
	if (udp_fd == NULL) {
		IPACMERR("unable to open %s\n", IPACM_UDP_FULL_FILE_NAME);
		goto fail;
	}

	if (fscanf(udp_fd, "%d", &udp_timeout) != 1) {
		IPACMERR("Error reading udp timeout\n");
	}
	IPACMDBG_H("udp timeout value: %d\n", udp_timeout);


	/* Read TCP timeout value */
	tcp_fd = fopen(IPACM_TCP_FULL_FILE_NAME, "r");
	if (tcp_fd == NULL) {
		IPACMERR("unable to open %s\n", IPACM_TCP_FULL_FILE_NAME);
		goto fail;
	}


	if (fscanf(tcp_fd, "%d", &tcp_timeout) != 1) {
		IPACMERR("Error reading tcp timeout\n");
	}
	IPACMDBG_H("tcp timeout value: %d\n", tcp_timeout);

fail:
	if (udp_fd) {
		fclose(udp_fd);
	}
	if (tcp_fd) {
		fclose(tcp_fd);
	}
#endif
	return;
}
