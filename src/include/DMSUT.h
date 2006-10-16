/*
 * DMSUT.h
 * DM - SUT Interface class
 *
 * 2006 Rilson Nascimento
 *
 * 12 August 2006
 */

#ifndef DM_SUT_H
#define DM_SUT_H

#include "DMSUTInterface.h"

namespace TPCE
{

class CDMSUT : public CDMSUTInterface, public CBaseInterface
{

public:

	CDMSUT(char* addr, const int iListenPort, ofstream* pflog, ofstream* pfmix,
			CSyncLock* pLogLock, CSyncLock* pMixLock);
	~CDMSUT(void);

	virtual bool DataMaintenance( PDataMaintenanceTxnInput pTxnInput );	// return whether it was successful
	virtual bool TradeCleanup( PTradeCleanupTxnInput pTxnInput );	// return whether it was successful
};

}	// namespace TPCE

#endif	// DM_SUT_H