/*
** Copyright (C) 2014 Cisco and/or its affiliates. All rights reserved.
** Copyright (C) 2013-2013 Sourcefire, Inc.
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License Version 2 as
** published by the Free Software Foundation.  You may not use, modify or
** distribute this program under any other version of the GNU General
** Public License.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include "parse_rule.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <stdarg.h>
#include <pcap.h>
#include <grp.h>
#include <pwd.h>
#include <fnmatch.h>

#ifdef HAVE_DUMBNET_H
#include <dumbnet.h>
#else
#include <dnet.h>
#endif

#include "snort_bounds.h"
#include "rules.h"
#include "treenodes.h"
#include "parser.h"
#include "cmd_line.h"
#include "parse_conf.h"
#include "parse_otn.h"
#include "snort_debug.h"
#include "util.h"
#include "mstring.h"
#include "detect.h"
#include "decode.h"
#include "fpcreate.h"
#include "generators.h"
#include "tag.h"
#include "signature.h"
#include "filters/sfthreshold.h"
#include "filters/sfthd.h"
#include "snort.h"
#include "asn1.h"
#include "hash/sfghash.h"
#include "ips_options/ips_ip_proto.h"
#include "ips_options/ips_content.h"
#include "sf_vartable.h"
#include "ipv6_port.h"
#include "sfip/sf_ip.h"
#include "sflsq.h"
#include "ppm.h"
#include "filters/rate_filter.h"
#include "filters/detection_filter.h"
#include "detection/sfrim.h"
#include "utils/sfportobject.h"
#include "packet_io/active.h"
#include "file_api/libs/file_config.h"
#include "framework/ips_option.h"
#include "managers/ips_manager.h"
#include "config_file.h"
#include "keywords.h"

#ifdef SIDE_CHANNEL
# include "side_channel/sidechannel.h"
#endif

#include "target_based/sftarget_reader.h"

#define SRC  0
#define DST  1

/* Tracking the port_list_t structure for printing and debugging at
 * this point...temporarily... */
typedef struct
{
    int rule_type;
    int proto;
    int icmp_type;
    int ip_proto;
    char *protocol;
    char *src_port;
    char *dst_port;
    unsigned int gid;
    unsigned int sid;
    int dir;
    char content;
    char uricontent;

} port_entry_t;

typedef struct
{
    int pl_max;
    int pl_cnt;
    port_entry_t pl_array[MAX_RULE_COUNT];

} port_list_t;

/* rule counts for port lists */
typedef struct
{
    int src;
    int dst;
    int aa;  /* any-any */
    int sd;  /* src+dst ports specified */
    int nc;  /* no content */

} rule_count_t;

static int rule_count = 0;
static int detect_rule_count = 0;
static int builtin_rule_count = 0;
static int head_count = 0;          /* number of header blocks (chain heads?) */
static int otn_count = 0;           /* number of chains */

static rule_count_t tcpCnt;
static rule_count_t udpCnt;
static rule_count_t icmpCnt;
static rule_count_t ipCnt;

static port_list_t port_list;

static void port_entry_free(port_entry_t *pentry)
{
    if (pentry->src_port != NULL)
    {
        free(pentry->src_port);
        pentry->src_port = NULL;
    }

    if (pentry->dst_port != NULL)
    {
        free(pentry->dst_port);
        pentry->dst_port = NULL;
    }

    if (pentry->protocol != NULL)
    {
        free(pentry->protocol);
        pentry->protocol = NULL;
    }
}

static int port_list_add_entry( port_list_t * plist, port_entry_t * pentry)
{
    if( !plist )
    {
        port_entry_free(pentry);
        return -1;
    }

    if( plist->pl_cnt >= plist->pl_max )
    {
        port_entry_free(pentry);
        return -1;
    }

    SafeMemcpy( &plist->pl_array[plist->pl_cnt], pentry, sizeof(port_entry_t),
                &plist->pl_array[plist->pl_cnt],
                (char*)(&plist->pl_array[plist->pl_cnt]) + sizeof(port_entry_t));
    plist->pl_cnt++;

    return 0;
}

#if 0
static port_entry_t * port_list_get( port_list_t * plist, int index)
{
    if( index < plist->pl_max )
    {
        return &plist->pl_array[index];
    }
    return NULL;
}

static void port_list_print( port_list_t * plist)
{
    int i;
    for(i=0;i<plist->pl_cnt;i++)
    {
        LogMessage("rule %d { ", i);
        LogMessage(" gid %u sid %u",plist->pl_array[i].gid,plist->pl_array[i].sid );
        LogMessage(" protocol %s", plist->pl_array[i].protocol);
        LogMessage(" dir %d",plist->pl_array[i].dir);
        LogMessage(" src_port %s dst_port %s ",
                plist->pl_array[i].src_port,
                plist->pl_array[i].dst_port );
        LogMessage(" content %d",
                plist->pl_array[i].content);
        LogMessage(" uricontent %d",
                plist->pl_array[i].uricontent);
        LogMessage(" }\n");
    }
}
#endif

static void port_list_free( port_list_t * plist)
{
    int i;
    for(i=0;i<plist->pl_cnt;i++)
    {
        port_entry_free(&plist->pl_array[i]);
    }
    plist->pl_cnt = 0;
}

/*
 * Finish adding the rule to the port tables
 *
 * 1) find the table this rule should belong to (src/dst/any-any tcp,udp,icmp,ip or nocontent)
 * 2) find an index for the sid:gid pair
 * 3) add all no content rules to a single no content port object, the ports are irrelevant so
 *    make it a any-any port object.
 * 4) if it's an any-any rule with content, add to an any-any port object
 * 5) find if we have a port object with these ports defined, if so get it, otherwise create it.
 *    a)do this for src and dst port
 *    b)add the rule index/id to the portobject(s)
 *    c)if the rule is bidir add the rule and port-object to both src and dst tables
 *
 */
static int FinishPortListRule(rule_port_tables_t *port_tables, RuleTreeNode *rtn, OptTreeNode *otn,
                              int proto, port_entry_t *pe, FastPatternConfig *fp)
{
    int large_port_group = 0;
    int src_cnt = 0;
    int dst_cnt = 0;
    int rim_index;
    PortTable *dstTable;
    PortTable *srcTable;
    PortObject *aaObject;
    rule_count_t *prc;

    /* Select the Target PortTable for this rule, based on protocol, src/dst
     * dir, and if there is rule content */
    if (proto == IPPROTO_TCP)
    {
        dstTable = port_tables->tcp_dst;
        srcTable = port_tables->tcp_src;
        aaObject = port_tables->tcp_anyany;
        prc = &tcpCnt;
    }
    else if (proto == IPPROTO_UDP)
    {
        dstTable = port_tables->udp_dst;
        srcTable = port_tables->udp_src;
        aaObject = port_tables->udp_anyany;
        prc = &udpCnt;
    }
    else if (proto == IPPROTO_ICMP)
    {
        dstTable = port_tables->icmp_dst;
        srcTable = port_tables->icmp_src;
        aaObject = port_tables->icmp_anyany;
        prc = &icmpCnt;
    }
    else if (proto == ETHERNET_TYPE_IP)
    {
        dstTable = port_tables->ip_dst;
        srcTable = port_tables->ip_src;
        aaObject = port_tables->ip_anyany;
        prc = &ipCnt;
    }
    else
    {
        return -1;
    }

    /* Count rules with both src and dst specific ports */
    if (!(rtn->flags & ANY_DST_PORT) && !(rtn->flags & ANY_SRC_PORT))
    {
        DEBUG_WRAP(DebugMessage(DEBUG_PORTLISTS,
                   "***\n***Info:  src & dst ports are both specific"
                   " >> gid=%u sid=%u src=%s dst=%s\n***\n",
                   otn->sigInfo.generator, otn->sigInfo.id,
                   pe->src_port, pe->dst_port););

        prc->sd++;
    }

    /* Create/find an index to store this rules sid and gid at,
     * and use as reference in Port Objects */
    rim_index = otn->ruleIndex;

    /* Add up the nocontent rules */
    if (!pe->content && !pe->uricontent)
        prc->nc++;

    /* If not an any-any rule test for port bleedover, if we are using a
     * single rule group, don't bother */
    if (!fpDetectGetSingleRuleGroup(fp) &&
        (rtn->flags & (ANY_DST_PORT|ANY_SRC_PORT)) != (ANY_DST_PORT|ANY_SRC_PORT))
    {
        if (!(rtn->flags & ANY_SRC_PORT))
        {
            src_cnt = PortObjectPortCount(rtn->src_portobject);
            if (src_cnt >= fpDetectGetBleedOverPortLimit(fp))
                large_port_group = 1;
        }

        if (!(rtn->flags & ANY_DST_PORT))
        {
            dst_cnt = PortObjectPortCount(rtn->dst_portobject);
            if (dst_cnt >= fpDetectGetBleedOverPortLimit(fp))
                large_port_group = 1;
        }

        if (large_port_group && fpDetectGetBleedOverWarnings(fp))
        {

            LogMessage("***Bleedover Port Limit(%d) Exceeded for rule %u:%u "
                       "(%d)ports: ", fpDetectGetBleedOverPortLimit(fp),
                       otn->sigInfo.generator, otn->sigInfo.id,
                       (src_cnt > dst_cnt) ? src_cnt : dst_cnt);

            /* If logging to syslog, this will be all multiline */
            fflush(stdout); fflush(stderr);
            PortObjectPrintPortsRaw(rtn->src_portobject);
            LogMessage(" -> ");
            PortObjectPrintPortsRaw(rtn->dst_portobject);
            LogMessage(" adding to any-any group\n");
            fflush(stdout);fflush(stderr);
        }
    }

    /* If an any-any rule add rule index to any-any port object
     * both content and no-content type rules go here if they are
     * any-any port rules...
     * If we have an any-any rule or a large port group or
     * were using a single rule group we make it an any-any rule. */
    if (((rtn->flags & (ANY_DST_PORT|ANY_SRC_PORT)) == (ANY_DST_PORT|ANY_SRC_PORT)) ||
        large_port_group || fpDetectGetSingleRuleGroup(fp))
    {
        if (proto == ETHERNET_TYPE_IP)
        {
            /* Add the IP rules to the higher level app protocol groups, if they apply
             * to those protocols.  All IP rules should have any-any port descriptors
             * and fall into this test.  IP rules that are not tcp/udp/icmp go only into the
             * IP table */
            DEBUG_WRAP(DebugMessage(DEBUG_PORTLISTS,
                                    "Finishing IP any-any rule %u:%u\n",
                                    otn->sigInfo.generator,otn->sigInfo.id););

            switch (GetOtnIpProto(otn))
            {
                case IPPROTO_TCP:
                    PortObjectAddRule(port_tables->tcp_anyany, rim_index);
                    tcpCnt.aa++;
                    break;

                case IPPROTO_UDP:
                    PortObjectAddRule(port_tables->udp_anyany, rim_index);
                    udpCnt.aa++;
                    break;

                case IPPROTO_ICMP:
                    PortObjectAddRule(port_tables->icmp_anyany, rim_index);
                    icmpCnt.aa++;
                    break;

                case -1:  /* Add to all ip proto anyany port tables */
                    PortObjectAddRule(port_tables->tcp_anyany, rim_index);
                    tcpCnt.aa++;

                    PortObjectAddRule(port_tables->udp_anyany, rim_index);
                    udpCnt.aa++;

                    PortObjectAddRule(port_tables->icmp_anyany, rim_index);
                    icmpCnt.aa++;

                    break;

                default:
                    break;
            }

            /* Add to the IP ANY ANY */
            PortObjectAddRule(aaObject, rim_index);
            prc->aa++;
        }
        else
        {
            /* For other protocols-tcp/udp/icmp add to the any any group */
            PortObjectAddRule(aaObject, rim_index);
            prc->aa++;
        }

        return 0; /* done */
    }

    /* add rule index to dst table if we have a specific dst port or port list */
    if (!(rtn->flags & ANY_DST_PORT))
    {
        PortObject *pox;

        prc->dst++;

        DEBUG_WRAP(DebugMessage(DEBUG_PORTLISTS,
                                "Finishing rule: dst port rule\n"););

        /* find the proper port object */
        pox = PortTableFindInputPortObjectPorts(dstTable, rtn->dst_portobject);
        if (pox == NULL)
        {
            /* Create a permanent port object */
            pox = PortObjectDupPorts(rtn->dst_portobject);
            if (pox == NULL)
            {
                ParseError("Could not dup a port object - out of memory.");
            }

            /* Add the port object to the table, and add the rule to the port object */
            PortTableAddObject(dstTable, pox);
        }

        PortObjectAddRule(pox, rim_index);

        /* if bidir, add this rule and port group to the src table */
        if (rtn->flags & BIDIRECTIONAL)
        {
            pox = PortTableFindInputPortObjectPorts(srcTable, rtn->dst_portobject);
            if (pox == NULL)
            {
                pox = PortObjectDupPorts(rtn->dst_portobject);
                if (pox == NULL)
                {
                    ParseError("Could not dup a bidir-port object - out of memory.");
                }

                PortTableAddObject(srcTable, pox);
            }

            PortObjectAddRule(pox, rim_index);
        }
    }

    /* add rule index to src table if we have a specific src port or port list */
    if (!(rtn->flags & ANY_SRC_PORT))
    {
        PortObject *pox;

        prc->src++;

        pox = PortTableFindInputPortObjectPorts(srcTable, rtn->src_portobject);
        if (pox == NULL)
        {
            pox = PortObjectDupPorts(rtn->src_portobject);
            if (pox == NULL)
            {
                ParseError("Could not dup a port object - out of memory.");
            }

            PortTableAddObject(srcTable, pox);
        }

        PortObjectAddRule(pox, rim_index);

        /* if bidir, add this rule and port group to the dst table */
        if (rtn->flags & BIDIRECTIONAL)
        {
            pox = PortTableFindInputPortObjectPorts(dstTable, rtn->src_portobject);
            if (pox == NULL)
            {
                pox = PortObjectDupPorts(rtn->src_portobject);
                if (pox == NULL)
                {
                    ParseError("Could not dup a bidir-port object - out "
                               "of memory.");
                }

                PortTableAddObject(dstTable, pox);
            }

            PortObjectAddRule(pox, rim_index);
        }
    }

    return 0;
}

static int ValidateIPList(sfip_var_t *addrset, const char *token)
{
    if(!addrset || !(addrset->head||addrset->neg_head))
    {
        ParseError("Empty IP used either as source IP or as "
            "destination IP in a rule. IP list: %s.", token);
    }

    return 0;
}

static int ProcessIP(
    SnortConfig*, char *addr, RuleTreeNode *rtn, int mode, int)
{
    vartable_t *ip_vartable = get_ips_policy()->ip_vartable;

    assert(rtn);
    /* If a rule has a variable in it, we want to copy that variable's
     * contents to the IP variable (IP list) stored with the rtn.
     * This code tries to look up the variable, and if found, will copy it
     * to the rtn->{sip,dip} */
    if(mode == SRC)
    {
        int ret;

        if (rtn->sip == NULL)
        {
            sfip_var_t *tmp = sfvt_lookup_var(ip_vartable, addr);
            if (tmp != NULL)
            {
                rtn->sip = sfvar_create_alias(tmp, tmp->name);
                if (rtn->sip == NULL)
                    ret = SFIP_FAILURE;
                else
                    ret = SFIP_SUCCESS;
            }
            else
            {
                rtn->sip = (sfip_var_t *)SnortAlloc(sizeof(sfip_var_t));
                ret = sfvt_add_to_var(ip_vartable, rtn->sip, addr);
            }
        }
        else
        {
            ret = sfvt_add_to_var(ip_vartable, rtn->sip, addr);
        }

        /* The function sfvt_add_to_var adds 'addr' to the variable 'rtn->sip' */
        if (ret != SFIP_SUCCESS)
        {
            if(ret == SFIP_LOOKUP_FAILURE)
            {
                ParseError("Undefined variable in the string: %s.", addr);
            }
            else if(ret == SFIP_CONFLICT)
            {
                ParseError("Negated IP ranges that are more general than "
                           "non-negated ranges are not allowed. Consider "
                           "inverting the logic: %s.", addr);
            }
            else if(ret == SFIP_NOT_ANY)
            {
                ParseError("!any is not allowed: %s.", addr);
            }
            else
            {
                ParseError("Unable to process the IP address: %s.", addr);
            }
        }

        if(rtn->sip->head && rtn->sip->head->flags & SFIP_ANY)
        {
            rtn->flags |= ANY_SRC_IP;
        }
    }
    /* mode == DST */
    else
    {
        int ret;

        if (rtn->dip == NULL)
        {
            sfip_var_t *tmp = sfvt_lookup_var(ip_vartable, addr);
            if (tmp != NULL)
            {
                rtn->dip = sfvar_create_alias(tmp, tmp->name);
                if (rtn->dip == NULL)
                    ret = SFIP_FAILURE;
                else
                    ret = SFIP_SUCCESS;
            }
            else
            {
                rtn->dip = (sfip_var_t *)SnortAlloc(sizeof(sfip_var_t));
                ret = sfvt_add_to_var(ip_vartable, rtn->dip, addr);
            }
        }
        else
        {
            ret = sfvt_add_to_var(ip_vartable, rtn->dip, addr);
        }

        if (ret != SFIP_SUCCESS)
        {
            if(ret == SFIP_LOOKUP_FAILURE)
            {
                ParseError("Undefined variable in the string: %s.", addr);
            }
            else if(ret == SFIP_CONFLICT)
            {
                ParseError("Negated IP ranges that are more general than "
                           "non-negated ranges are not allowed. Consider "
                           "inverting the logic: %s.", addr);
            }
            else if(ret == SFIP_NOT_ANY)
            {
                ParseError("!any is not allowed: %s.", addr);
            }
            else
            {
                ParseError("Unable to process the IP address: %s.", addr);
            }
        }

        if(rtn->dip->head && rtn->dip->head->flags & SFIP_ANY)
        {
            rtn->flags |= ANY_DST_IP;
        }
    }

    /* Make sure the IP lists provided by the user are valid */
    if (mode == SRC)
        ValidateIPList(rtn->sip, addr);
    else
        ValidateIPList(rtn->dip, addr);

    return 0;
}

/*
*  Parse a port string as a port var, and create or find a port object for it,
*  and add it to the port var table. These are used by the rtn's
*  as src and dst port lists for final rtn/otn processing.
*
*  These should not be confused with the port objects used to merge ports and rules
*  to build PORT_GROUP objects. Those are generated after the otn processing.
*
*/
static PortObject * ParsePortListTcpUdpPort(PortVarTable *pvt,
                                            PortTable *noname, char *port_str)
{
    PortObject * portobject;
    //PortObject * pox;
    POParser     poparser;

    if ((pvt == NULL) || (noname == NULL) || (port_str == NULL))
        return NULL;

    /* 1st - check if we have an any port */
    if( strcasecmp(port_str,"any")== 0 )
    {
        portobject = PortVarTableFind(pvt, "any");
        if (portobject == NULL)
            ParseError("PortVarTable missing an 'any' variable.");

        return portobject;
    }

    /* 2nd - check if we have a PortVar */
    else if( port_str[0]=='$' )
    {
      /*||isalpha(port_str[0])*/ /*TODO: interferes with protocol names for ports*/
      char * name = port_str + 1;

      DEBUG_WRAP(DebugMessage(DEBUG_PORTLISTS,"PortVarTableFind: finding '%s'\n", port_str););

      /* look it up  in the port var table */
      portobject = PortVarTableFind(pvt, name);
      if (portobject == NULL)
          ParseError("***PortVar Lookup failed on '%s'.", port_str);

      DEBUG_WRAP(DebugMessage(DEBUG_PORTLISTS,"PortVarTableFind: '%s' found!\n", port_str););
    }

    /* 3rd -  and finally process a raw port list */
    else
    {
       /* port list = [p,p,p:p,p,...] or p or p:p , no embedded spaces due to tokenizer */
       PortObject * pox;

       DEBUG_WRAP(DebugMessage(DEBUG_PORTLISTS,
                 "parser.c->PortObjectParseString: parsing '%s'\n",port_str););

       portobject = PortObjectParseString(pvt, &poparser, 0, port_str, 0);

       DEBUG_WRAP(DebugMessage(DEBUG_PORTLISTS,
                 "parser.c->PortObjectParseString: '%s' done.\n",port_str););

       if( !portobject )
       {
          const char* errstr = PortObjectParseError( &poparser );
          ParseError("***Rule--PortVar Parse error: (pos=%d,error=%s)\n>>%s\n>>%*s",
                     poparser.pos,errstr,port_str,poparser.pos,"^");
       }

       /* check if we already have this port object in the un-named port var table  ... */
       pox = PortTableFindInputPortObjectPorts(noname, portobject);
       if( pox )
       {
         DEBUG_WRAP(DebugMessage(DEBUG_PORTLISTS,
                    "parser.c: already have '%s' as a PortObject - "
                    "calling PortObjectFree(portbject) line=%d\n",port_str,__LINE__ ););
         PortObjectFree( portobject );
         portobject = pox;
       }
       else
       {
           DEBUG_WRAP(DebugMessage(DEBUG_PORTLISTS,
                "parser.c: adding '%s' as a PortObject line=%d\n",port_str,__LINE__ ););
           /* Add to the un-named port var table */
           if (PortTableAddObject(noname, portobject))
           {
               ParseError("Unable to add raw port object to unnamed "
                          "port var table, out of memory.");
           }
       }
    }

    return portobject;
}

/*
 *   Process the rule, add it to the appropriate PortObject
 *   and add the PortObject to the rtn.
 *
 *   TCP/UDP rules use ports/portlists, icmp uses the icmp type field and ip uses the protocol
 *   field as a dst port for the purposes of looking up a rule group as packets are being
 *   processed.
 *
 *   TCP/UDP- use src/dst ports
 *   ICMP   - use icmp type as dst port,src=-1
 *   IP     - use protocol as dst port,src=-1
 *
 *   rtn - proto_node
 *   port_str - port list string or port var name
 *   proto - protocol
 *   dst_flag - dst or src port flag, true = dst, false = src
 *
 */
static int ParsePortList(RuleTreeNode *rtn, PortVarTable *pvt, PortTable *noname,
                         char *port_str, int proto, int dst_flag)
{
    PortObject *portobject = NULL;  /* src or dst */

    /* Get the protocol specific port object */
    if( proto == IPPROTO_TCP || proto == IPPROTO_UDP )
    {
        portobject = ParsePortListTcpUdpPort(pvt, noname, port_str);
    }
    else /* ICMP, IP  - no real ports just Type and Protocol */
    {
        portobject = PortVarTableFind(pvt, "any");
        if (portobject == NULL)
        {
            ParseError("PortVarTable missing an 'any' variable.");
        }
    }

    DEBUG_WRAP(DebugMessage(DEBUG_PORTLISTS,"Rule-PortVar Parsed: %s \n",port_str););

    /* !ports - port lists can be mixed 80:90,!82,
    * so the old NOT flag is depracated for port lists
    */

    /* set up any any flags */
    if( PortObjectHasAny(portobject) )
    {
         if( dst_flag )
             rtn->flags |= ANY_DST_PORT;
         else
             rtn->flags |= ANY_SRC_PORT;
    }

    /* check for a pure not rule - fatal if we find one */
    if( PortObjectIsPureNot( portobject ) )
    {
        ParseError("Pure NOT ports are not allowed.");
        /*
           if( dst_flag )
           rtn->flags |= EXCEPT_DST_PORT;
           else
           rtn->flags |= EXCEPT_SRC_PORT;
           */
    }

    /*
    * set to the port object for this rules src/dst port,
    * these are used during rtn/otn port verification of the rule.
    */

    if (dst_flag)
         rtn->dst_portobject = portobject;
    else
         rtn->src_portobject = portobject;

    return 0;
}

/****************************************************************************
 *
 * Function: TestHeader(RuleTreeNode *, RuleTreeNode *)
 *
 * Purpose: Check to see if the two header blocks are identical
 *
 * Arguments: rule => uh
 *            rtn  => uuuuhhhhh....
 *
 * Returns: 1 if they match, 0 if they don't
 *
 ***************************************************************************/
static int TestHeader(RuleTreeNode * rule, RuleTreeNode * rtn)
{
    if ((rule == NULL) || (rtn == NULL))
        return 0;

    if (rule->type != rtn->type)
        return 0;

    if (rule->proto != rtn->proto)
        return 0;

    /* For custom rule type declarations */
    if (rule->listhead != rtn->listhead)
        return 0;

    if (rule->flags != rtn->flags)
        return 0;

    if ((rule->sip != NULL) && (rtn->sip != NULL) &&
            (sfvar_compare(rule->sip, rtn->sip) != SFIP_EQUAL))
    {
        return 0;
    }

    if ((rule->dip != NULL) && (rtn->dip != NULL) &&
            (sfvar_compare(rule->dip, rtn->dip) != SFIP_EQUAL))
    {
        return 0;
    }

    /* compare the port group pointers - this prevents confusing src/dst port objects
     * with the same port set, and it's quicker. It does assume that we only have
     * one port object and pointer for each unique port set...this is handled by the
     * parsing and initial port object storage and lookup.  This must be consistent during
     * the rule parsing phase. - man */
    if ((rule->src_portobject != rtn->src_portobject)
            || (rule->dst_portobject != rtn->dst_portobject))
    {
        return 0;
    }

    return 1;
}

/**returns matched header node.
*/
static RuleTreeNode * findHeadNode(
    SnortConfig *sc, RuleTreeNode *testNode,
    PolicyId policyId)
{
    RuleTreeNode *rtn;
    OptTreeNode *otn;
    SFGHASH_NODE *hashNode;

    for (hashNode = sfghash_findfirst(sc->otn_map);
         hashNode;
         hashNode = sfghash_findnext(sc->otn_map))
    {
        otn = (OptTreeNode *)hashNode->data;
        rtn = getRtnFromOtn(otn, policyId);

        if (TestHeader(rtn, testNode))
            return rtn;
    }

    return NULL;
}

/****************************************************************************
 *
 * Function: XferHeader(RuleTreeNode *, RuleTreeNode *)
 *
 * Purpose: Transfer the rule block header data from point A to point B
 *
 * Arguments: rule => the place to xfer from
 *            rtn => the place to xfer to
 *
 * Returns: void function
 *
 ***************************************************************************/
static void XferHeader(RuleTreeNode *test_node, RuleTreeNode *rtn)
{
    rtn->flags = test_node->flags;
    rtn->type = test_node->type;
    rtn->sip = test_node->sip;
    rtn->dip = test_node->dip;

    rtn->proto = test_node->proto;

    rtn->src_portobject = test_node->src_portobject;
    rtn->dst_portobject = test_node->dst_portobject;
}

/****************************************************************************
 *
 * Function: AddRuleFuncToList(int (*func)(), RuleTreeNode *)
 *
 * Purpose:  Adds RuleTreeNode associated detection functions to the
 *          current rule's function list
 *
 * Arguments: *func => function pointer to the detection function
 *            rtn   => pointer to the current rule
 *
 * Returns: void function
 *
 ***************************************************************************/
void AddRuleFuncToList(int (*rfunc) (Packet *, RuleTreeNode *, struct _RuleFpList *, int), RuleTreeNode * rtn)
{
    RuleFpList *idx;

    DEBUG_WRAP(DebugMessage(DEBUG_CONFIGRULES,"Adding new rule to list\n"););

    idx = rtn->rule_func;
    if(idx == NULL)
    {
        rtn->rule_func = (RuleFpList *)SnortAlloc(sizeof(RuleFpList));

        rtn->rule_func->RuleHeadFunc = rfunc;
    }
    else
    {
        while(idx->next != NULL)
            idx = idx->next;

        idx->next = (RuleFpList *)SnortAlloc(sizeof(RuleFpList));
        idx = idx->next;
        idx->RuleHeadFunc = rfunc;
    }
}

/****************************************************************************
 *
 * Function: AddrToFunc(RuleTreeNode *, u_long, u_long, int, int)
 *
 * Purpose: Links the proper IP address testing function to the current RTN
 *          based on the address, netmask, and addr flags
 *
 * Arguments: rtn => the pointer to the current rules list entry to attach to
 *            ip =>  IP address of the current rule
 *            mask => netmask of the current rule
 *            exception_flag => indicates that a "!" has been set for this
 *                              address
 *            mode => indicates whether this is a rule for the source
 *                    or destination IP for the rule
 *
 * Returns: void function
 *
 ***************************************************************************/
static void AddrToFunc(RuleTreeNode * rtn, int mode)
{
    /*
     * if IP and mask are both 0, this is a "any" IP and we don't need to
     * check it
     */
    switch(mode)
    {
        case SRC:
            if((rtn->flags & ANY_SRC_IP) == 0)
            {
                DEBUG_WRAP(DebugMessage(DEBUG_CONFIGRULES,"CheckSrcIP -> "););
                AddRuleFuncToList(CheckSrcIP, rtn);
            }

            break;

        case DST:
            if((rtn->flags & ANY_DST_IP) == 0)
            {
                DEBUG_WRAP(DebugMessage(DEBUG_CONFIGRULES,"CheckDstIP -> "););
                AddRuleFuncToList(CheckDstIP, rtn);
            }

            break;
    }
}

/****************************************************************************
 *
 * Function: PortToFunc(RuleTreeNode *, int, int, int)
 *
 * Purpose: Links in the port analysis function for the current rule
 *
 * Arguments: rtn => the pointer to the current rules list entry to attach to
 *            any_flag =>  accept any port if set
 *            except_flag => indicates negation (logical NOT) of the test
 *            mode => indicates whether this is a rule for the source
 *                    or destination port for the rule
 *
 * Returns: void function
 *
 ***************************************************************************/
static void PortToFunc(RuleTreeNode * rtn, int any_flag, int except_flag, int mode)
{
    /*
     * if the any flag is set we don't need to perform any test to match on
     * this port
     */
    if(any_flag)
        return;

    /* if the except_flag is up, test with the "NotEq" funcs */
    if(except_flag)
    {
        switch(mode)
        {
            case SRC:
                DEBUG_WRAP(DebugMessage(DEBUG_CONFIGRULES,"CheckSrcPortNotEq -> "););
                AddRuleFuncToList(CheckSrcPortNotEq, rtn);
                break;


            case DST:
                DEBUG_WRAP(DebugMessage(DEBUG_CONFIGRULES,"CheckDstPortNotEq -> "););
                AddRuleFuncToList(CheckDstPortNotEq, rtn);
                break;
        }

        return;
    }
    /* default to setting the straight test function */
    switch(mode)
    {
        case SRC:
            DEBUG_WRAP(DebugMessage(DEBUG_CONFIGRULES,"CheckSrcPortEqual -> "););
            AddRuleFuncToList(CheckSrcPortEqual, rtn);
            break;

        case DST:
            DEBUG_WRAP(DebugMessage(DEBUG_CONFIGRULES,"CheckDstPortEqual -> "););
            AddRuleFuncToList(CheckDstPortEqual, rtn);
            break;
    }

    return;
}

/****************************************************************************
 *
 * Function: SetupRTNFuncList(RuleTreeNode *)
 *
 * Purpose: Configures the function list for the rule header detection
 *          functions (addrs and ports)
 *
 * Arguments: rtn => the pointer to the current rules list entry to attach to
 *
 * Returns: void function
 *
 ***************************************************************************/
static void SetupRTNFuncList(RuleTreeNode * rtn)
{
    DEBUG_WRAP(DebugMessage(DEBUG_CONFIGRULES,"Initializing RTN function list!\n"););
    DEBUG_WRAP(DebugMessage(DEBUG_CONFIGRULES,"Functions: "););

    if(rtn->flags & BIDIRECTIONAL)
    {
        DEBUG_WRAP(DebugMessage(DEBUG_CONFIGRULES,"CheckBidirectional->\n"););
        AddRuleFuncToList(CheckBidirectional, rtn);
    }
    else
    {
        /* Attach the proper port checking function to the function list */
        /*
         * the in-line "if's" check to see if the "any" or "not" flags have
         * been set so the PortToFunc call can determine which port testing
         * function to attach to the list
         */
        PortToFunc(rtn, (rtn->flags & ANY_DST_PORT ? 1 : 0),
                   (rtn->flags & EXCEPT_DST_PORT ? 1 : 0), DST);

        /* as above */
        PortToFunc(rtn, (rtn->flags & ANY_SRC_PORT ? 1 : 0),
                   (rtn->flags & EXCEPT_SRC_PORT ? 1 : 0), SRC);

        /* link in the proper IP address detection function */
        AddrToFunc(rtn, SRC);

        /* last verse, same as the first (but for dest IP) ;) */
        AddrToFunc(rtn, DST);
    }

    DEBUG_WRAP(DebugMessage(DEBUG_CONFIGRULES,"RuleListEnd\n"););

    /* tack the end (success) function to the list */
    AddRuleFuncToList(RuleListEnd, rtn);
}

/****************************************************************************
 *
 * Function: ProcessHeadNode(RuleTreeNode *, ListHead *, int)
 *
 * Purpose:  Process the header block info and add to the block list if
 *           necessary
 *
 * Arguments: test_node => data generated by the rules parsers
 *            list => List Block Header refernece
 *            protocol => ip protocol
 *
 * Returns: void function
 *
 ***************************************************************************/
static RuleTreeNode * ProcessHeadNode(
    SnortConfig *sc, RuleTreeNode *test_node,
    ListHead *list)
{
    RuleTreeNode *rtn = findHeadNode(
        sc, test_node, get_ips_policy()->policy_id);

    /* if it doesn't match any of the existing nodes, make a new node and
     * stick it at the end of the list */
    if (rtn == NULL)
    {
        DEBUG_WRAP(DebugMessage(DEBUG_CONFIGRULES,"Building New Chain head node\n"););

        rtn = (RuleTreeNode *)SnortAlloc(sizeof(RuleTreeNode));

        rtn->otnRefCount++;

        /* copy the prototype header info into the new header block */
        XferHeader(test_node, rtn);

        head_count++;
        rtn->head_node_number = head_count;

        /* initialize the function list for the new RTN */
        SetupRTNFuncList(rtn);

        /* add link to parent listhead */
        rtn->listhead = list;

        DEBUG_WRAP(DebugMessage(DEBUG_CONFIGRULES,
                "New Chain head flags = 0x%X\n", rtn->flags););
    }
    else
    {
        rtn->otnRefCount++;
        FreeRuleTreeNode(test_node);
    }

    return rtn;
}

/****************************************************************************
 *
 * Function: mergeDuplicateOtn()
 *
 * Purpose:  Conditionally removes duplicate SID/GIDs. Keeps duplicate with
 *           higher revision.  If revision is the same, keeps newest rule.
 *
 * Arguments: otn_cur => The current version
 *            rtn => the RTN chain to check
 *            char => String describing the rule
 *
 * Returns: 0 if original rule stays, 1 if new rule stays
 *
 ***************************************************************************/
static int mergeDuplicateOtn(
    SnortConfig *sc, OptTreeNode *otn_cur,
    OptTreeNode *otn_new, RuleTreeNode *rtn_new)
{
    RuleTreeNode *rtn_cur = NULL;
    RuleTreeNode *rtnTmp2 = NULL;
    unsigned i;

    if (otn_cur->proto != otn_new->proto)
    {
        ParseError("GID %d SID %d in rule duplicates previous rule, with "
                   "different protocol.",
                   otn_new->sigInfo.generator, otn_new->sigInfo.id);
    }

    rtn_cur = getRtnFromOtn(otn_cur);

    if((rtn_cur != NULL) && (rtn_cur->type != rtn_new->type))
    {
        ParseError("GID %d SID %d in rule duplicates previous rule, with "
                   "different type.",
                   otn_new->sigInfo.generator, otn_new->sigInfo.id);
    }

    if ( otn_new->sigInfo.rev < otn_cur->sigInfo.rev )
    {
        //current OTN is newer version. Keep current and discard the new one.
        //OTN is for new policy group, salvage RTN
        deleteRtnFromOtn(otn_new);

        ParseWarning(
            "%d:%d duplicates previous rule. Using revision %d.",
            otn_cur->sigInfo.generator, otn_cur->sigInfo.id, otn_cur->sigInfo.rev);

        /* Now free the OTN itself -- this function is also used
         * by the hash-table calls out of OtnRemove, so it cannot
         * be modified to delete data for rule options */
        OtnFree(otn_new);

        //Add rtn to current otn for the first rule instance in a policy,
        //otherwise ignore it
        if (rtn_cur == NULL)
        {
            addRtnToOtn(otn_cur, rtn_new);
        }
        else
        {
            DestroyRuleTreeNode(rtn_new);
        }

        return 0;
    }

    //delete current rule instance and keep the new one

    for (i = 0; i < otn_cur->proto_node_num; i++)
    {
        rtnTmp2 = deleteRtnFromOtn(otn_cur, i);

        if (rtnTmp2 && (i != get_ips_policy()->policy_id))
        {
            addRtnToOtn(otn_new, rtnTmp2, i);
        }
    }

    if (rtn_cur)
    {
        if (ScConfErrorOut())
        {
            ParseError(
                "%d:%d:%d duplicates previous rule.",
                otn_new->sigInfo.generator, otn_new->sigInfo.id, otn_new->sigInfo.rev);
        }
        else
        {
            ParseWarning(
                "%d:%d duplicates previous rule. Using revision %d.",
                otn_new->sigInfo.generator, otn_new->sigInfo.id, otn_new->sigInfo.rev);
        }

        if ( otn_new->sigInfo.text_rule )
            detect_rule_count--;
        else
            builtin_rule_count--;
    }

    otn_count--;

    OtnRemove(sc->otn_map, otn_cur);
    DestroyRuleTreeNode(rtn_cur);

    return 1;
}

static void ValidateFastPattern(OptTreeNode *otn)
{
    OptFpList *fpl = NULL;
    int fp_only = 0;

    for(fpl = otn->opt_func; fpl != NULL; fpl = fpl->next)
    {
        /* a relative option is following a fast_pattern:only and
         * there was no resets.
         */
        if (fp_only == 1)
        {
            if (fpl->isRelative)
                ParseWarning("relative rule option used after "
                    "fast_pattern:only");
        }

        /* reset the check if one of these are present.
         */
        if ((fpl->type == RULE_OPTION_TYPE_FILE_DATA) ||
            (fpl->type == RULE_OPTION_TYPE_PKT_DATA) ||
            (fpl->type == RULE_OPTION_TYPE_BASE64_DATA) ||
            (fpl->type == RULE_OPTION_TYPE_PCRE) ||
            (fpl->type == RULE_OPTION_TYPE_BYTE_JUMP) ||
            (fpl->type == RULE_OPTION_TYPE_BYTE_EXTRACT))
        {
            fp_only = 0;
        }

        /* set/unset the check on content options.
         */
        if ((fpl->type == RULE_OPTION_TYPE_CONTENT) ||
            (fpl->type == RULE_OPTION_TYPE_CONTENT_URI))
        {
            if ( is_fast_pattern_only(fpl) )
                fp_only = 1;
            else
                fp_only = 0;
        }
    }
}

static OptTreeNode* ParseRuleOptions(
    SnortConfig *sc, RuleTreeNode *rtn, char *rule_opts,
    int protocol, bool text)
{
    OptTreeNode *otn;
    int num_detection_opts = 0;
    OptFpList *fpl = NULL;

    otn = (OptTreeNode *)SnortAlloc(sizeof(OptTreeNode));
    otn->state = (OtnState*)SnortAlloc(sizeof(OtnState)*get_instance_max());

    otn->chain_node_number = otn_count;
    otn->proto = protocol;
    otn->sigInfo.generator = GENERATOR_SNORT_ENGINE;
    otn->sigInfo.text_rule = text;

    /* Set the default rule state */
    otn->enabled = ScDefaultRuleState();

    if (rule_opts == NULL)
        ParseError("Each rule must contain a sid.");

    else
    {
        const char* so_opts = nullptr;
        char **toks;
        int num_toks;
        int i;

        OptTreeNode *otn_dup;
    
        if ((rule_opts[0] != '(') || (rule_opts[strlen(rule_opts) - 1] != ')'))
            ParseError("Rule options must be enclosed in '(' and ')'.");
    
        parse_otn_clear(); 

        /* Move past '(' and zero out ')' */
        rule_opts++;
        rule_opts[strlen(rule_opts) - 1] = '\0';
    
        toks = mSplit(rule_opts, ";", 0, &num_toks, '\\');
    
        for (i = 0; i < num_toks; i++)
        {
            char **opts;
            int num_opts;
    
            /* break out the option name from its data */
            opts = mSplit(toks[i], ":", 2, &num_opts, '\\');
    
            if ( !parse_otn(
                sc, rtn, otn, opts[0], opts[1], &so_opts) )
            {
                int type;
    
                if ( !IpsManager::get_option(
                    sc, otn, protocol, opts[0], opts[1], type) )
                {
                    ParseError("Unknown rule option: %s.", opts[0]);
                }
                num_detection_opts++;
            }
    
            mSplitFree(&opts, num_opts);
        }
    
        if ( so_opts )
        {
            mSplitFree(&toks, num_toks);
            toks = mSplit(so_opts, ";", 0, &num_toks, '\\');

            for (i = 0; i < num_toks-1; i++)
            {
                char **opts;
                int num_opts;
    
                /* break out the option name from its data */
                opts = mSplit(toks[i], ":", 2, &num_opts, '\\');
    
                if ( !parse_otn(
                    sc, rtn, otn, opts[0], opts[1], &so_opts) )
                {
                    int type;
    
                    if ( !IpsManager::get_option(
                        sc, otn, protocol, opts[0], opts[1], type) )
                    {
                        ParseError("Unknown rule option: %s.", opts[0]);
                    }
                    num_detection_opts++;
                }
                mSplitFree(&opts, num_opts);
            }
        }
        mSplitFree(&toks, num_toks);

        if ( num_detection_opts > 0 && !otn->sigInfo.text_rule )
            ParseError("Builtin rules do not support detection options.");
    
        if ( !otn->sigInfo.id )
            ParseError("Each rule must contain a rule sid.");
    
        addRtnToOtn(otn, rtn);
    
        /* Check for duplicate SID */
        otn_dup = OtnLookup(sc->otn_map, otn->sigInfo.generator, otn->sigInfo.id);
        if (otn_dup != NULL)
        {
            otn->ruleIndex = otn_dup->ruleIndex;
    
            if (mergeDuplicateOtn(sc, otn_dup, otn, rtn) == 0)
            {
                /* We are keeping the old/dup OTN and trashing the new one
                 * we just created - it's free'd in the remove dup function */
                return NULL;
            }
        }
        else
        {
            otn->ruleIndex = RuleIndexMapAdd(
                ruleIndexMap, otn->sigInfo.generator, otn->sigInfo.id);
        }
    }

    otn->num_detection_opts += num_detection_opts;
    otn_count++;

    if ( otn->sigInfo.text_rule )
        detect_rule_count++;
    else
        builtin_rule_count++;

    fpl = AddOptFuncToList(OptListEnd, otn);
    fpl->type = RULE_OPTION_TYPE_LEAF_NODE;

    ValidateFastPattern(otn);

    /* setup gid,sid->otn mapping */
    OtnLookupAdd(sc->otn_map, otn);

    return otn;
}

/****************************************************************************
 *
 * Function: parse_rule()
 *
 * Purpose:  Process an individual rule and add it to the rule list
 *
 * Arguments: rule => rule string
 *
 * Returns: void function
 *
 ***************************************************************************/
void parse_rule(
    SnortConfig *sc, const char *args,
    RuleType rule_type, ListHead *list)
{
    char **toks = NULL;
    int num_toks = 0;
    int protocol = 0;
    RuleTreeNode test_rtn;
    RuleTreeNode *rtn;
    OptTreeNode *otn;
    char *roptions = NULL;
    char* tmp_args = NULL;
    port_entry_t pe;
    bool text;

    IpsPolicy* p = get_ips_policy();
    PortVarTable *portVarTable = p->portVarTable;
    PortTable *nonamePortVarTable = p->nonamePortVarTable;

    if ((sc == NULL) || (args == NULL))
      return;

    memset(&test_rtn, 0, sizeof(RuleTreeNode));

    memset(&pe, 0, sizeof(pe));

    DEBUG_WRAP(DebugMessage(DEBUG_CONFIGRULES,"[*] Rule start\n"););

    /* for builtin rules, assume a header of 'tcp any any -> any any' */
    if (*args == '(')
    {
        text = false;

        test_rtn.flags |= ANY_DST_PORT;
        test_rtn.flags |= ANY_SRC_PORT;
        test_rtn.flags |= ANY_DST_IP;
        test_rtn.flags |= ANY_SRC_IP;
        test_rtn.flags |= BIDIRECTIONAL;
        test_rtn.type = rule_type;
        protocol = IPPROTO_TCP;

        tmp_args = SnortStrdup(args);
        roptions = tmp_args;
    }
    else
    {
        text = true;

        /* proto ip port dir ip port r*/
        toks = mSplit(args, " \t", 7, &num_toks, '\\');

        /* A rule might not have rule options */
        if (num_toks < 6)
        {
            ParseError("Bad rule in rules file: %s", args);
        }

        if (num_toks == 7)
            roptions = toks[6];

        test_rtn.type = rule_type;

        /* Set the rule protocol - fatal errors if protocol not found */
        protocol = GetRuleProtocol(toks[0]);
        test_rtn.proto = protocol;

        switch (protocol)
        {
            case IPPROTO_TCP:
                sc->ip_proto_array[IPPROTO_TCP] = 1;
                break;
            case IPPROTO_UDP:
                sc->ip_proto_array[IPPROTO_UDP] = 1;
                break;
            case IPPROTO_ICMP:
                sc->ip_proto_array[IPPROTO_ICMP] = 1;
                sc->ip_proto_array[IPPROTO_ICMPV6] = 1;
                break;
            case ETHERNET_TYPE_IP:
                /* This will be set via ip_protos */
                // FIXIT need to add these for a single ip any any rule?
                sc->ip_proto_array[IPPROTO_TCP] = 1;
                sc->ip_proto_array[IPPROTO_UDP] = 1;
                sc->ip_proto_array[IPPROTO_ICMP] = 1;
                sc->ip_proto_array[IPPROTO_ICMPV6] = 1;
                break;
            default:
                ParseError("Bad protocol: %s", toks[0]);
                break;
        }

        /* Process the IP address and CIDR netmask - changed version 1.2.1
         * "any" IP's are now set to addr 0, netmask 0, and the normal rules are
         * applied instead of checking the flag if we see a "!<ip number>" we
         * need to set a flag so that we can properly deal with it when we are
         * processing packets. */
        ProcessIP(sc, toks[1], &test_rtn, SRC, 0);

        /* Check to make sure that the user entered port numbers.
         * Sometimes they forget/don't know that ICMP rules need them */
        if ((strcasecmp(toks[2], RULE_DIR_OPT__DIRECTIONAL) == 0) ||
            (strcasecmp(toks[2], RULE_DIR_OPT__BIDIRECTIONAL) == 0))
        {
            ParseError("Port value missing in rule!");
        }

        DEBUG_WRAP(DebugMessage(DEBUG_PORTLISTS,"Src-Port: %s\n",toks[2]););

        if (ParsePortList(&test_rtn, portVarTable, nonamePortVarTable,
                          toks[2], protocol, 0 /* =src port */ ))
        {
            ParseError("Bad source port: '%s'", toks[2]);
        }

        /* changed version 1.8.4
         * Die when someone has tried to define a rule character other
         * than -> or <> */
        if ((strcmp(toks[3], RULE_DIR_OPT__DIRECTIONAL) != 0) &&
            (strcmp(toks[3], RULE_DIR_OPT__BIDIRECTIONAL) != 0))
        {
            ParseError("Illegal direction specifier: %s", toks[3]);
        }

        /* New in version 1.3: support for bidirectional rules
         * This checks the rule "direction" token and sets the bidirectional
         * flag if the token = '<>' */
        if (strcmp(toks[3], RULE_DIR_OPT__BIDIRECTIONAL) == 0)
        {
            DEBUG_WRAP(DebugMessage(DEBUG_CONFIGRULES,"Bidirectional rule!\n"););
            test_rtn.flags |= BIDIRECTIONAL;
        }

        /* changed version 1.2.1
         * "any" IP's are now set to addr 0, netmask 0, and the normal rules are
         * applied instead of checking the flag
         * If we see a "!<ip number>" we need to set a flag so that we can
         * properly deal with it when we are processing packets */
        ProcessIP(sc, toks[4], &test_rtn, DST, 0);

        DEBUG_WRAP(DebugMessage(DEBUG_PORTLISTS,"Dst-Port: %s\n", toks[5]););

        if (ParsePortList(&test_rtn, portVarTable, nonamePortVarTable,
                          toks[5], protocol, 1 /* =dst port */ ))
        {
            ParseError("Bad destination port: '%s'", toks[5]);
        }
    }

    DEBUG_WRAP(DebugMessage(DEBUG_CONFIGRULES,"test_rtn.flags = 0x%X\n", test_rtn.flags););
    DEBUG_WRAP(DebugMessage(DEBUG_CONFIGRULES,"Processing Head Node....\n"););

    test_rtn.listhead = list;

    rtn = ProcessHeadNode(sc, &test_rtn, list);
    /* The IPs in the test node get free'd in ProcessHeadNode if there is
     * already a matching RTN.  The portobjects will get free'd when the
     * port var table is free'd */

    DEBUG_WRAP(DebugMessage(DEBUG_CONFIGRULES,"Parsing Rule Options...\n"););

    otn = ParseRuleOptions(sc, rtn, roptions, protocol, text);
    if (otn == NULL)
    {
        /* This otn is a dup and we're choosing to keep the old one */
        if ( tmp_args )
            free(tmp_args);
        mSplitFree(&toks, num_toks);
        return;
    }

    rule_count++;

    /* Get rule option info */
    pe.gid = otn->sigInfo.generator;
    pe.sid = otn->sigInfo.id;

    /* Have to have at least 6 toks */
    if (num_toks != 0)
    {
        pe.protocol = SnortStrdup(toks[0]);
        pe.src_port = SnortStrdup(toks[2]);
        pe.dst_port = SnortStrdup(toks[5]);
    }

    /* See what kind of content is going in the fast pattern matcher */
    {
        /* Since http_cookie content is not used in fast pattern matcher,
         * need to iterate the entire list */
        if ( otn_has_plugin(otn, RULE_OPTION_TYPE_CONTENT_URI) )
        {
            OptFpList* fpl = otn->opt_func;

            while ( fpl )
            {
                if ( fpl->type == RULE_OPTION_TYPE_CONTENT_URI )
                {
                    PatternMatchData* pmd = get_pmd(fpl);

                    if ( IsHttpBufFpEligible(pmd->http_buffer) )
                    {
                        pe.uricontent = 1;
                        break;
                    }
                }
                fpl = fpl->next;
            }
        }

        if (!pe.uricontent && otn_has_plugin(otn, RULE_OPTION_TYPE_CONTENT) )
        {
            pe.content = 1;
        }
    }

    if (rtn->flags & BIDIRECTIONAL)
         pe.dir = 1;

    pe.proto = protocol;
    pe.rule_type = rule_type;

    port_list_add_entry(&port_list, &pe);

    /*
     * The src/dst port parsing must be done before the Head Nodes are processed, since they must
     * compare the ports/port_objects to find the right rtn list to add the otn rule to.
     *
     * After otn processing we can finalize port object processing for this rule
     */
    if (FinishPortListRule(sc->port_tables, rtn, otn, protocol, &pe, sc->fast_pattern_config))
        ParseError("Failed to finish a port list rule.");

    if ( tmp_args )
        free(tmp_args);
    mSplitFree(&toks, num_toks);
}

int get_rule_count()
{ return rule_count; }

void parse_rule_init()
{
    rule_count = 0;
    detect_rule_count = 0;
    builtin_rule_count = 0;
    head_count = 0;
    otn_count = 0;

    port_list_free(&port_list);
    memset(&port_list, 0, sizeof(port_list));
    port_list.pl_max = MAX_RULE_COUNT;

    memset(&tcpCnt, 0, sizeof(tcpCnt));
    memset(&udpCnt, 0, sizeof(udpCnt));
    memset(&ipCnt, 0, sizeof(ipCnt));
    memset(&icmpCnt, 0, sizeof(icmpCnt));
}

void parse_rule_term()
{
    port_list_free(&port_list);
}

void parse_rule_print()
{
    LogMessage("%s\n", LOG_DIV);
    LogMessage("rule counts\n");

    LogMessage("%25.25s: %-12u\n", "total rules loaded", rule_count);

    if ( !rule_count )
        return;

    LogMessage("%25.25s: %-12u\n", "text rules", detect_rule_count);
    LogMessage("%25.25s: %-12u\n", "builtin rules", builtin_rule_count);
    LogMessage("%25.25s: %-12u\n", "option chains", otn_count);
    LogMessage("%25.25s: %-12u\n", "chain headers", head_count);

    LogMessage("%s\n", LOG_DIV);
    LogMessage("rule port counts\n");
    LogMessage("%8s%8s%8s%8s%8s\n", " ", "tcp", "udp", "icmp", "ip");

    if ( tcpCnt.src || udpCnt.src || icmpCnt.src || ipCnt.src )
        LogMessage("%8s%8u%8u%8u%8u\n", "src",
            tcpCnt.src, udpCnt.src, icmpCnt.src, ipCnt.src);

    if ( tcpCnt.dst || udpCnt.dst || icmpCnt.dst || ipCnt.dst )
        LogMessage("%8s%8u%8u%8u%8u\n", "dst",
            tcpCnt.dst, udpCnt.dst, icmpCnt.dst, ipCnt.dst);

    if ( tcpCnt.aa || udpCnt.aa || icmpCnt.aa || ipCnt.aa )
        LogMessage("%8s%8u%8u%8u%8u\n", "any",
            tcpCnt.aa, udpCnt.aa, icmpCnt.aa, ipCnt.aa);

    if ( tcpCnt.nc || udpCnt.nc || icmpCnt.nc || ipCnt.nc )
        LogMessage("%8s%8u%8u%8u%8u\n", "nc",
            tcpCnt.nc, udpCnt.nc, icmpCnt.nc, ipCnt.nc);

    if ( tcpCnt.sd || udpCnt.sd || icmpCnt.sd || ipCnt.sd )
        LogMessage("%8s%8u%8u%8u%8u\n", "s+d",
            tcpCnt.sd, udpCnt.sd, icmpCnt.sd, ipCnt.sd);

    //print_rule_index_map( ruleIndexMap );
    //port_list_print( &port_list );
}

