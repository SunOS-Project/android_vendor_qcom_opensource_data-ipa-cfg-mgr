/*
Copyright (c) 2013-2019, The Linux Foundation. All rights reserved.

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

#ifndef IPACM_CONNTRACK_LISTENER
#define IPACM_CONNTRACK_LISTENER

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#ifndef in_addr_t
typedef uint32_t in_addr_t;
#endif
#include <arpa/inet.h>
#include <netinet/in.h>
#include <errno.h>

#include "IPACM_CmdQueue.h"
#include "IPACM_Conntrack_NATApp.h"
#include "IPACM_Listener.h"
#ifdef CT_OPT
#include "IPACM_LanToLan.h"
#endif

#define MAX_IFACE_ADDRESS 50
#define MAX_STA_CLNT_IFACES 10
#define STA_CLNT_SUBNET_MASK 0xFFFFFF00

typedef struct _nat_entry_bundle
{
	struct nf_conntrack *ct;
	enum nf_conntrack_msg_type type;
	nat_table_entry *rule;
	bool isTempEntry;

}nat_entry_bundle;

typedef struct _ct_entry
{
	struct nf_conntrack *ct;
	u_int8_t  protocol;
	enum nf_conntrack_msg_type type;
}ct_entry;

class IPACM_ConntrackListener : public IPACM_Listener
{

private:
	bool isCTReg;
	bool isNatThreadStart;
	bool WanUp;
	bool isProcessCTDone;
	NatApp *nat_inst;

	int NatIfaceCnt;
	int StaClntCnt;
	NatIfaces *pNatIfaces;
	uint32_t nat_iface_ipv4_addr[MAX_IFACE_ADDRESS];
	uint32_t nonnat_iface_ipv4_addr[MAX_IFACE_ADDRESS];
	uint32_t sta_clnt_ipv4_addr[MAX_STA_CLNT_IFACES];
	IPACM_Config *pConfig;
	ct_entry *ct_entries;
	ct_entry ct_cache[MAX_CONNTRACK_ENTRIES];
#ifdef CT_OPT
	IPACM_LanToLan *p_lan2lan;
#endif

	void ProcessCTMessage(void *);
	bool ProcessTCPorUDPMsg(struct nf_conntrack *,
	enum nf_conntrack_msg_type, u_int8_t);
	void TriggerWANUp(void *);
	void TriggerWANDown(uint32_t);
	int  CreateNatThreads(void);
	bool AddIface(nat_table_entry *, bool *);
	void AddORDeleteNatEntry(const nat_entry_bundle *);
	void PopulateTCPorUDPEntry(struct nf_conntrack *, uint32_t, nat_table_entry *);
	void CheckSTAClient(const nat_table_entry *, bool *);
	int CheckNatIface(ipacm_event_data_all *, bool *);
	void HandleNonNatIPAddr(void *, bool);
	void HandleNatTableMove(void *in_param);

#ifdef CT_OPT
	void ProcessCTV6Message(void *);
	void HandleLan2Lan(struct nf_conntrack *,
		enum nf_conntrack_msg_type, nat_table_entry* );
#endif

public:
	char wan_ifname[IPA_IFACE_NAME_LEN];
	uint32_t wan_ipaddr;
	ipacm_wan_iface_type backhaul_mode;
	bool isReadCTDone;
	IPACM_ConntrackListener();
	void event_callback(ipa_cm_event_id, void *data);
	inline bool isWanUp()
	{
		return WanUp;
	}

	void HandleNeighIpAddrAddEvt(ipacm_event_data_all *);
	void HandleNeighIpAddrDelEvt(uint32_t);
	void HandleSTAClientAddEvt(uint32_t);
	void HandleSTAClientDelEvt(uint32_t);
	int  CreateConnTrackThreads(void);
	void readConntrack(int fd);
	void processConntrack(void);
	void CacheORDeleteConntrack(struct nf_conntrack *ct,
		enum nf_conntrack_msg_type type, u_int8_t protocol);
	void processCacheConntrack(void);
};

extern IPACM_ConntrackListener *CtList;

#endif /* IPACM_CONNTRACK_LISTENER */
