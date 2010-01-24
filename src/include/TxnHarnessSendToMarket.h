/*
 * TxnHarnessSendToMarket.h
 * SendToMarket class
 *
 * 2006 Rilson Nascimento
 *
 * 06 July 2006
 */

#ifndef TXN_HARNESS_SENDTOMARKET_H
#define TXN_HARNESS_SENDTOMARKET_H

#include "locking.h"

using namespace TPCE;

class CSendToMarket : public CSendToMarketInterface
{
	ofstream*	m_pfLog;
	int		m_MEport;
	CSocket		*m_Socket;
	CMutex	m_LogLock;

public:
	void LogErrorMessage(const string sErr);

	CSendToMarket(ofstream* pfile, int MEport = DriverMarketPort);
	~CSendToMarket();

	virtual bool SendToMarket(TTradeRequest &trade_mes);
};

#endif	// TXN_HARNESS_SENDTOMARKET_H
