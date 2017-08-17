/* mmanon.c
 * anonnymize IP addresses inside the syslog message part
 *
 * Copyright 2013 Adiscon GmbH.
 *
 * This file is part of rsyslog.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *       http://www.apache.org/licenses/LICENSE-2.0
 *       -or-
 *       see COPYING.ASL20 in the source distribution
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "config.h"
#include "rsyslog.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include "conf.h"
#include "syslogd-types.h"
#include "srUtils.h"
#include "template.h"
#include "module-template.h"
#include "errmsg.h"
#include "parserif.h"


MODULE_TYPE_OUTPUT
MODULE_TYPE_NOKEEP
MODULE_CNFNAME("mmanon")


DEFobjCurrIf(errmsg);
DEF_OMOD_STATIC_DATA

/* config variables */

// enumerator for the mode
enum mode {ZERO, RANDOMINT, SIMPLE};

union node {
	struct {
		union node* more;
		union node* less;
	} pointer;
	struct {
		char ip_high[16];
		char ip_low[16];
	} ips;
};


/* define operation modes we have */
#define SIMPLE_MODE 0	 /* just overwrite */
#define REWRITE_MODE 1	 /* rewrite IP address, canoninized */
typedef struct _instanceData {
	struct {
		int8_t bits;
		union node* Root;
		int randConsis;
		enum mode mode;
		uchar replaceChar;
	} ipv4;
} instanceData;

typedef struct wrkrInstanceData {
	instanceData *pData;
} wrkrInstanceData_t;

struct modConfData_s {
	rsconf_t *pConf;	/* our overall config object */
};
static modConfData_t *loadModConf = NULL;/* modConf ptr to use for the current load process */
static modConfData_t *runModConf = NULL;/* modConf ptr to use for the current exec process */


/* tables for interfacing with the v6 config system */
/* action (instance) parameters */
static struct cnfparamdescr actpdescr[] = {
	{ "ipv4.mode", eCmdHdlrGetWord, 0 },
	{ "mode", eCmdHdlrGetWord, 0 },
	{ "ipv4.bits", eCmdHdlrPositiveInt, 0 },
	{ "ipv4.replacechar", eCmdHdlrGetChar, 0},
	{ "replacementchar", eCmdHdlrGetChar, 0}
};
static struct cnfparamblk actpblk =
	{ CNFPARAMBLK_VERSION,
	  sizeof(actpdescr)/sizeof(struct cnfparamdescr),
	  actpdescr
	};

BEGINbeginCnfLoad
CODESTARTbeginCnfLoad
	loadModConf = pModConf;
	pModConf->pConf = pConf;
ENDbeginCnfLoad

BEGINendCnfLoad
CODESTARTendCnfLoad
ENDendCnfLoad

BEGINcheckCnf
CODESTARTcheckCnf
ENDcheckCnf

BEGINactivateCnf
CODESTARTactivateCnf
	runModConf = pModConf;
ENDactivateCnf

BEGINfreeCnf
CODESTARTfreeCnf
ENDfreeCnf


BEGINcreateInstance
CODESTARTcreateInstance
ENDcreateInstance

BEGINcreateWrkrInstance
CODESTARTcreateWrkrInstance
ENDcreateWrkrInstance


BEGINisCompatibleWithFeature
CODESTARTisCompatibleWithFeature
ENDisCompatibleWithFeature


static void
delTree(union node* node, const int layer)
{
	if(node == NULL){
		return;
	}
	if(layer == 31){
		free(node);
	} else {
		delTree(node->pointer.more, layer + 1);
		delTree(node->pointer.less, layer + 1);
		free(node);
	}
}


BEGINfreeInstance
CODESTARTfreeInstance
	delTree(pData->ipv4.Root, 0);
ENDfreeInstance


BEGINfreeWrkrInstance
CODESTARTfreeWrkrInstance
ENDfreeWrkrInstance


static inline void
setInstParamDefaults(instanceData *pData)
{
		pData->ipv4.bits = 16;
		pData->ipv4.Root = NULL;
		pData->ipv4.randConsis = 0;
		pData->ipv4.mode = ZERO;
		pData->ipv4.replaceChar = 'x';
}

BEGINnewActInst
	struct cnfparamvals *pvals;
	int i;
CODESTARTnewActInst
	DBGPRINTF("newActInst (mmanon)\n");
	if((pvals = nvlstGetParams(lst, &actpblk, NULL)) == NULL) {
		ABORT_FINALIZE(RS_RET_MISSING_CNFPARAMS);
	}

	CODE_STD_STRING_REQUESTnewActInst(1)
	CHKiRet(OMSRsetEntry(*ppOMSR, 0, NULL, OMSR_TPL_AS_MSG));
	CHKiRet(createInstance(&pData));
	setInstParamDefaults(pData);

	for(i = 0 ; i < actpblk.nParams ; ++i) {
		if(!pvals[i].bUsed)
			continue;
		if(!strcmp(actpblk.descr[i].name, "ipv4.mode") || !strcmp(actpblk.descr[i].name, "mode")) {
			if(!es_strbufcmp(pvals[i].val.d.estr, (uchar*)"zero",
					 sizeof("zero")-1)) {
				pData->ipv4.mode = ZERO;
			} else if(!es_strbufcmp(pvals[i].val.d.estr, (uchar*)"random",
					 sizeof("random")-1)) {
				pData->ipv4.mode = RANDOMINT;
			} else if(!es_strbufcmp(pvals[i].val.d.estr, (uchar*)"simple",
					 sizeof("simple")-1) ||
					!es_strbufcmp(pvals[i].val.d.estr, (uchar*)"rewrite",
					 sizeof("rewrite")-1)) {
				pData->ipv4.mode = SIMPLE;
			} else if(!es_strbufcmp(pvals[i].val.d.estr, (uchar*)"random-consistent",
					 sizeof("random-consistent")-1)) {
				pData->ipv4.mode = RANDOMINT;
				pData->ipv4.randConsis = 1;
			}
		} else if(!strcmp(actpblk.descr[i].name, "ipv4.bits")) {
			if((int8_t) pvals[i].val.d.n <= 32) {
				pData->ipv4.bits = (int8_t) pvals[i].val.d.n;
			} else {
				pData->ipv4.bits = 32;
				parser_errmsg("warning: invalid number of ipv4.bits (%d), corrected to 32", (int8_t) pvals[i].val.d.n);
			}
		} else if(!strcmp(actpblk.descr[i].name, "ipv4.replacechar") || !strcmp(actpblk.descr[i].name, "replacementchar")) {
			uchar* tmp = (uchar*) es_str2cstr(pvals[i].val.d.estr, NULL);
			pData->ipv4.replaceChar = tmp[0];
			free(tmp);
		} else {
			dbgprintf("program error, non-handled "
			  "param '%s'\n", actpblk.descr[i].name);
		}
	}

	int bHadBitsErr = 0;
	if(pData->ipv4.mode == SIMPLE) {
		if(pData->ipv4.bits < 8 && pData->ipv4.bits > -1) {
			pData->ipv4.bits = 8;
			bHadBitsErr = 1;
		} else if(pData->ipv4.bits < 16 && pData->ipv4.bits > 8) {
			pData->ipv4.bits = 16;
			bHadBitsErr = 1;
		} else if(pData->ipv4.bits < 24 && pData->ipv4.bits > 16) {
			pData->ipv4.bits = 24;
			bHadBitsErr = 1;
		} else if((pData->ipv4.bits != 32 && pData->ipv4.bits > 24) || pData->ipv4.bits < 0) {
			pData->ipv4.bits = 32;
			bHadBitsErr = 1;
		}
		if(bHadBitsErr) {
			LogError(0, RS_RET_INVLD_ANON_BITS,
				"mmanon: invalid number of ipv4 bits "
				"in simple mode, corrected to %d",
				pData->ipv4.bits);
		}
	}

CODE_STD_FINALIZERnewActInst
	cnfparamvalsDestruct(pvals, &actpblk);
ENDnewActInst


BEGINdbgPrintInstInfo
CODESTARTdbgPrintInstInfo
ENDdbgPrintInstInfo


BEGINtryResume
CODESTARTtryResume
ENDtryResume

/* returns -1 if no integer found, else integer */
static int64_t
getPosInt(const uchar *const __restrict__ buf,
	const size_t buflen,
	size_t *const __restrict__ nprocessed)
{
	int64_t val = 0;
	size_t i;
	for(i = 0 ; i < buflen ; i++) {
		if('0' <= buf[i] && buf[i] <= '9')
			val = val*10 + buf[i]-'0';
		else
			break;
	}
	*nprocessed = i;
	if(i == 0)
		val = -1;
	return val;
}

/* 1 - is IPv4, 0 not */

static int
syntax_ipv4(const uchar *const __restrict__ buf,
	const size_t buflen,
	size_t *const __restrict__ nprocessed)
{
	int64_t val;
	size_t nproc;
	size_t i;
	int r = 0;

	val = getPosInt(buf, buflen, &i);
	if(val < 0 || val > 255)
		goto done;

	if(buf[i] != '.') goto done;
	i++;
	val = getPosInt(buf+i, buflen-i, &nproc);
	if(val < 0 || val > 255)
		goto done;
	i += nproc;

	if(buf[i] != '.') goto done;
	i++;
	val = getPosInt(buf+i, buflen-i, &nproc);
	if(val < 0 || val > 255)
		goto done;
	i += nproc;

	if(buf[i] != '.') goto done;
	i++;
	val = getPosInt(buf+i, buflen-i, &nproc);
	if(val < 0 || val > 255)
		goto done;
	i += nproc;

	//printf("IP Addr[%zd]: '%s'\n", i, buf);
	*nprocessed = i;
	r = 1;

done:
	return r;
}

static unsigned
ipv42num(const char *str)
{
	unsigned num[4] = {0, 0, 0, 0};
	unsigned value = -1;
	size_t len = strlen(str);
	int cyc = 0;
	for(unsigned i = 0 ; i < len ; i++) {
		switch(str[i]){
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			num[cyc] = num[cyc]*10+(str[i]-'0');
			break;
		case '.':
			cyc++;
			break;
		}
	}

	value = num[0]*256*256*256+num[1]*256*256+num[2]*256+num[3];
	return(value);
}


static unsigned
code_int(unsigned ip, instanceData *pData){
	unsigned random;
	unsigned long long shiftIP_subst = ip;
	// variable needed because shift operation of 32nd bit in unsigned does not work
	switch(pData->ipv4.mode){
	case ZERO:
		shiftIP_subst = ((shiftIP_subst>>(pData->ipv4.bits))<<(pData->ipv4.bits));
		return (unsigned)shiftIP_subst;
	case RANDOMINT:
		shiftIP_subst = ((shiftIP_subst>>(pData->ipv4.bits))<<(pData->ipv4.bits));
		// multiply the random number between 0 and 1 with a mask of (2^n)-1:
		random = (unsigned)((rand()/(double)RAND_MAX)*((1ull<<(pData->ipv4.bits))-1));
		return (unsigned)shiftIP_subst + random;
	case SIMPLE:  //can't happen, since this case is caught at the start of anonipv4()
	default:
		LogError(0, RS_RET_INTERNAL_ERROR, "mmanon: unexpected code path reached in code_int function");
		return 0;
	}
}


static int
num2ipv4(unsigned num, char *str) {
	int numip[4];
	size_t len;
	for(int i = 0 ; i < 4 ; i++){
		numip[i] = num % 256;
		num = num / 256;
	}
	len = snprintf(str, 16, "%d.%d.%d.%d", numip[3], numip[2], numip[1], numip[0]);
	return len;
}


static void
getipv4(uchar *start, size_t end, char *address)
{
	size_t i;
	for(i = 0; i < end; i++){
		address[i] = *(start+i);
	}
	address[i] = '\0';
}


static char*
findip(char* address, instanceData *pData)
{
	int i;
	unsigned num;
	union node* current;
	union node* Last;
	int MoreLess;
	char* CurrentCharPtr;

	current = pData->ipv4.Root;
	num = ipv42num(address);
	for(i = 0; i < 31; i++){
		if(pData->ipv4.Root == NULL) {
			current = (union node*)calloc(1, sizeof(union node));
			pData->ipv4.Root = current;
		}
		Last = current;
		if((num >> (31 - i)) & 1){
			current = current->pointer.more;
			MoreLess = 1;
		} else {
			current = current->pointer.less;
			MoreLess = 0;
		}
		if(current == NULL){
			current = (union node*)calloc(1, sizeof(union node));
			if(MoreLess == 1){
				Last->pointer.more = current;
			} else {
				Last->pointer.less = current;
			}
		}
	}
	if(num & 1){
		CurrentCharPtr = current->ips.ip_high;
	} else {
		CurrentCharPtr = current->ips.ip_low;
	}
	if(CurrentCharPtr[0] != '\0'){
		return CurrentCharPtr;
	} else {
		num = code_int(num, pData);
		num2ipv4(num, CurrentCharPtr);
		return CurrentCharPtr;
	}
}


static void
process_IPv4 (char* address, instanceData *pData)
{
	char* current;
	unsigned num;

	if(pData->ipv4.randConsis){
		current = findip(address, pData);
		strcpy(address, current);
	}else {
		num = ipv42num(address);
		num = code_int(num, pData);
		num2ipv4(num, address);
	}
}


static void
simpleAnon(instanceData *pData, uchar *msg, int *hasChanged, int iplen)
{
	int maxidx = iplen - 1;

	int j = -1;
	for(int i = (pData->ipv4.bits / 8); i > 0; i--) {
		j++;
		while('0' <= msg[maxidx - j] && msg[maxidx - j] <= '9') {
			if(msg[maxidx - j] != pData->ipv4.replaceChar) {
				msg[maxidx - j] = pData->ipv4.replaceChar;
				*hasChanged = 1;
			}
			j++;
		}
	}
}


static void
anonipv4(instanceData *pData, uchar **msg, int *pLenMsg, int *idx, int *hasChanged)
{
	char address[16];
	char caddress[16];
	int offset = *idx;
	uchar* msgcpy = *msg;
	size_t iplen;
	size_t caddresslen;
	int oldLen = *pLenMsg;

	if(syntax_ipv4((*msg) + offset, *pLenMsg - offset, &iplen)) {
		if(pData->ipv4.mode == SIMPLE) {
			simpleAnon(pData, *msg + *idx, hasChanged, iplen);
			*idx += iplen;
			return;
		}

		getipv4(*msg + offset, iplen, address);
		offset = offset + iplen; //iplen includes the character on offset
		strcpy(caddress, address);
		process_IPv4(caddress, pData);
		caddresslen = strlen(caddress);
		*hasChanged = 1;

		if(caddresslen != strlen(address)) {
			*pLenMsg = *pLenMsg + (caddresslen - strlen(address));
			*msg = (uchar*) malloc(*pLenMsg);
			memcpy(*msg, msgcpy, *idx);
		}
		memcpy(*msg + *idx, caddress, caddresslen);
		*idx = *idx + caddresslen;
		if(*idx < *pLenMsg) {
			memcpy(*msg + *idx, msgcpy + offset, oldLen - offset);
		}
		if(msgcpy != *msg) {
			free(msgcpy);
		}
	}
}


BEGINdoAction_NoStrings
	smsg_t **ppMsg = (smsg_t **) pMsgData;
	smsg_t *pMsg = ppMsg[0];
	uchar *msg;
	int lenMsg;
	int i;
	int hasChanged = 0;
CODESTARTdoAction
	lenMsg = getMSGLen(pMsg);
	msg = (uchar*)strdup((char*)getMSG(pMsg));

	for(i = 0 ; i <= lenMsg - 7 ; ++i) {
		anonipv4(pWrkrData->pData, &msg, &lenMsg, &i, &hasChanged);
	}
	if(hasChanged) {
		MsgReplaceMSG(pMsg, msg, lenMsg);
	}
	free(msg);
ENDdoAction


BEGINparseSelectorAct
CODESTARTparseSelectorAct
CODE_STD_STRING_REQUESTparseSelectorAct(1)
	if(strncmp((char*) p, ":mmanon:", sizeof(":mmanon:") - 1)) {
		errmsg.LogError(0, RS_RET_LEGA_ACT_NOT_SUPPORTED,
			"mmanon supports only v6+ config format, use: "
			"action(type=\"mmanon\" ...)");
	}
	ABORT_FINALIZE(RS_RET_CONFLINE_UNPROCESSED);
CODE_STD_FINALIZERparseSelectorAct
ENDparseSelectorAct


BEGINmodExit
CODESTARTmodExit
	objRelease(errmsg, CORE_COMPONENT);
ENDmodExit


BEGINqueryEtryPt
CODESTARTqueryEtryPt
CODEqueryEtryPt_STD_OMOD_QUERIES
CODEqueryEtryPt_STD_OMOD8_QUERIES
CODEqueryEtryPt_STD_CONF2_OMOD_QUERIES
CODEqueryEtryPt_STD_CONF2_QUERIES
ENDqueryEtryPt



BEGINmodInit()
CODESTARTmodInit
	*ipIFVersProvided = CURR_MOD_IF_VERSION; /* we only support the current interface specification */
CODEmodInit_QueryRegCFSLineHdlr
	DBGPRINTF("mmanon: module compiled with rsyslog version %s.\n", VERSION);
	CHKiRet(objUse(errmsg, CORE_COMPONENT));
ENDmodInit
