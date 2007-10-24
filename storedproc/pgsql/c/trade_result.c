/*
 * This file is released under the terms of the Artistic License.  Please see
 * the file LICENSE, included in this package, for details.
 *
 * Copyright (C) 2007 Mark Wong
 *
 * Based on TPC-E Standard Specification Revision 1.3.
 */

#include <sys/types.h>
#include <unistd.h>
#include <postgres.h>
#include <fmgr.h>
#include <executor/spi.h> /* this should include most necessary APIs */
#include <executor/executor.h>  /* for GetAttributeByName() */
#include <funcapi.h> /* for returning set of rows in order_status */
#include <utils/datetime.h>
#include <utils/numeric.h>

#include "frame.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

#define TRF1_1 \
		"SELECT t_ca_id, t_tt_id, t_s_symb, t_qty, t_chrg, t_lifo,\n" \
		"       t_is_cash\n" \
		"FROM trade\n" \
		"WHERE t_id = %d"

#define TRF1_2 \
		"SELECT tt_name, tt_is_sell, tt_is_mrkt\n" \
		"FROM trade_type\n" \
		"WHERE tt_id = '%s'"

#define TRF1_3 \
		"SELECT hs_qty\n" \
		"FROM holding_summary\n" \
		"WHERE hs_ca_id = %s\n" \
		"  AND hs_s_symb = '%s'"

#define TRF2_1 \
		"SELECT ca_b_id, ca_c_id, ca_tax_st\n" \
		"FROM customer_account\n" \
		"WHERE ca_id = %d"

#define TRF2_2a \
		"INSERT INTO holding_summary(hs_ca_id, hs_s_symb, hs_qty)\n" \
		"VALUES(%d, '%s', %d)"

#define TRF2_2b \
		"UPDATE holding_summary\n" \
		"SET hs_qty = %d\n" \
		"WHERE hs_ca_id = %d\n " \
		"  AND hs_s_symb = '%s'"

#define TRF2_3a \
		"SELECT h_t_id, h_qty, h_price\n" \
		"FROM holding\n" \
		"WHERE h_ca_id = %d\n" \
		"  AND h_s_symb = '%s'\n" \
		"ORDER BY h_dts DESC"

#define TRF2_3b \
		"SELECT h_t_id, h_qty, h_price\n" \
		"FROM holding\n" \
		"WHERE h_ca_id = %d\n" \
		"  AND h_s_symb = '%s'\n" \
		"ORDER BY h_dts ASC"

#define TRF2_4a \
		"INSERT INTO holding_history(hh_h_t_id, hh_t_id, hh_before_qty,\n" \
		"                            hh_after_qty)\n" \
		"VALUES(%d, %d, %d, %d)"

#define TRF2_5a \
		"UPDATE holding\n" \
		"SET h_qty = %d\n" \
		"WHERE h_t_id = %d"

#define TRF2_5b \
		"DELETE FROM holding\n" \
		"WHERE h_t_id = %d"

#define TRF2_7a \
		"INSERT INTO holding(h_t_id, h_ca_id, h_s_symb, h_dts, h_price,\n" \
		"                    h_qty\n" \
		"VALUES (%d, %d, '%s', '%s', %f, %d)\n" \
		"RETURNING t_dts"

#define TRF2_7b \
		"DELETE FROM holding_summary\n" \
		"WHERE hs_ca_id = %d\n" \
		"  AND hs_s_symb = '%s'"

#define TRF2_8a \
		"INSERT INTO holding_summary(hs_ca_id, hs_s_symb, hs_qty)\n" \
		"VALUES (%d, '%s', %d)"

#define TRF2_8b \
		"UPDATE holding_summary\n" \
		"SET hs_qty = %d\n" \
		"WHERE hs_ca_id = %d\n" \
		"  AND hs_s_symb = '%s'"

#define TRF3_1 \
		"SELECT SUM(tx_rate)\n" \
		"FROM taxrate\n" \
		"WHERE tx_id in (SELECT cx_tx_id\n" \
		"                FROM customer_taxrate\n" \
		"                WHERE cx_c_id = %d)\n"

#define TRF3_2 \
		"UPDATE trade\n" \
		"SET t_tax = %s\n" \
		"WHERE t_id = %d"

#define TRF4_1 \
		"SELECT s_ex_id, s_name\n" \
		"FROM security\n" \
		"WHERE s_symb = '%s'"

#define TRF4_2 \
		"SELECT c_tier\n" \
		"FROM customer\n" \
		"WHERE c_id = %d"

#define TRF4_3 \
		"SELECT cr_rate\n" \
		"FROM commission_rate\n" \
		"WHERE cr_c_tier = %s\n" \
		"  AND cr_tt_id = '%s'\n" \
		"  AND cr_ex_id = '%s'\n" \
		"  AND cr_from_qty <= %d\n" \
		"  AND cr_to_qty >= %d\n" \
		"LIMIT 1"

#define TRF5_1 \
		"UPDATE trade\n" \
		"SET t_comm = %f,\n" \
		"    t_dts = '%s',\n" \
		"    t_st_id = '%s',\n" \
		"    t_trade_price = %f\n" \
		"WHERE t_id = %d"

#define TRF5_2 \
		"INSERT INTO trade_history(th_t_id, th_dts, th_st_id)\n" \
		"VALUES (%d, '%s', '%s')"

#define TRF5_3 \
		"UPDATE broker\n" \
		"SET b_comm_total = b_comm_total + %f,\n" \
		"    b_num_trades = b_num_trades + 1\n" \
		"WHERE b_id = %d"

#define TRF6_1 \
		"INSERT INTO settlement(se_t_id, se_cash_type, se_cash_due_date,\n " \
		"                       se_amt)\n" \
		"VALUES (%d, '%s', '%s', %f)"

#define TRF6_2 \
		"UPDATE customer_account\n" \
		"SET ca_bal = ca_bal + %f\n" \
		"WHERE ca_id = %d"

#define TRF6_3 \
		"INSERT INTO cash_transaction(ct_dts, ct_t_id, ct_amt, ct_name)\n" \
		"VALUES ('%s', %d, %f, '%s %d shared of %s')"

#define TRF6_4 \
		"SELECT ca_bal\n" \
		"FROM customer_account\n" \
		"WHERE ca_id = %d"

/* Prototypes. */
void dump_trf1_inputs(int);
void dump_trf2_inputs(int, int, int, char *, int, double, int, int);
void dump_trf3_inputs(double, int, double, int);
void dump_trf4_inputs(int, char *, int, char *);
void dump_trf5_inputs(int, double, char *, char *, int, double);
void dump_trf6_inputs(int, char *, char *, double, char *, int, int, int,
		char *);

Datum TradeResultFrame1(PG_FUNCTION_ARGS);
Datum TradeResultFrame2(PG_FUNCTION_ARGS);
Datum TradeResultFrame3(PG_FUNCTION_ARGS);
Datum TradeResultFrame4(PG_FUNCTION_ARGS);
Datum TradeResultFrame5(PG_FUNCTION_ARGS);
Datum TradeResultFrame6(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(TradeResultFrame1);
PG_FUNCTION_INFO_V1(TradeResultFrame2);
PG_FUNCTION_INFO_V1(TradeResultFrame3);
PG_FUNCTION_INFO_V1(TradeResultFrame4);
PG_FUNCTION_INFO_V1(TradeResultFrame5);
PG_FUNCTION_INFO_V1(TradeResultFrame6);

void dump_trf1_inputs(int trade_id)
{
	elog(NOTICE, "TRF1: INPUTS START");
	elog(NOTICE, "TRF1: trade_id %d", trade_id);
	elog(NOTICE, "TRF1: INPUTS END");
}

void dump_trf2_inputs(int acct_id, int hs_qty, int is_lifo, char *symbol,
		int trade_id, double trade_price, int trade_qty, int type_is_sell)
{
	elog(NOTICE, "TRF2: INPUTS START");
	elog(NOTICE, "TRF2: acct_id %d", acct_id);
	elog(NOTICE, "TRF2: hs_qty %d", hs_qty);
	elog(NOTICE, "TRF2: is_lifo %d", is_lifo);
	elog(NOTICE, "TRF2: symbol %s", symbol);
	elog(NOTICE, "TRF2: trade_id %d", trade_id);
	elog(NOTICE, "TRF2: trade_price %f", trade_price);
	elog(NOTICE, "TRF2: trade_qty %d", trade_qty);
	elog(NOTICE, "TRF2: type_is_sell %d", type_is_sell);
	elog(NOTICE, "TRF2: INPUTS END");
}

void dump_trf3_inputs(double buy_value, int cust_id, double sell_value,
		int trade_id)
{
	elog(NOTICE, "TRF3: INPUTS START");
	elog(NOTICE, "TRF3: buy_value %f", buy_value);
	elog(NOTICE, "TRF3: cust_id %d", cust_id);
	elog(NOTICE, "TRF3: sell_value %f", sell_value);
	elog(NOTICE, "TRF3: trade_id %d", trade_id);
	elog(NOTICE, "TRF3: INPUTS END");
}

void dump_trf4_inputs(int cust_id, char *symbol, int trade_qty, char *type_id)
{
	elog(NOTICE, "TRF4: INPUTS START");
	elog(NOTICE, "TRF4: cust_id %d", cust_id);
	elog(NOTICE, "TRF4: symbol %s", symbol);
	elog(NOTICE, "TRF4: trade_qty %d", trade_qty);
	elog(NOTICE, "TRF4: type_id %s", type_id);
	elog(NOTICE, "TRF4: INPUTS END");
}

void dump_trf5_inputs(int broker_id, double comm_amount, char *st_completed_id,
		char *trade_dts, int trade_id, double trade_price)
{
	elog(NOTICE, "TRF5: INPUTS START");
	elog(NOTICE, "TRF5: broker_id %d", broker_id);
	elog(NOTICE, "TRF5: comm_amount %f", comm_amount);
	elog(NOTICE, "TRF5: st_completed_id %s", st_completed_id);
	elog(NOTICE, "TRF5: trade_dts %s", trade_dts);
	elog(NOTICE, "TRF5: trade_id %d", trade_id);
	elog(NOTICE, "TRF5: trade_price %f", trade_price);
	elog(NOTICE, "TRF5: INPUTS END");
}

void dump_trf6_inputs(int acct_id, char *due_date, char *s_name,
		double se_amount, char *trade_dts, int trade_id, int trade_is_cash,
		int trade_qty, char *type_name)
{
	elog(NOTICE, "TRF6: INPUTS START");
	elog(NOTICE, "TRF6: acct_id %d", acct_id);
	elog(NOTICE, "TRF6: due_date %s", due_date);
	elog(NOTICE, "TRF6: s_name %s", s_name);
	elog(NOTICE, "TRF6: se_amount %f", se_amount);
	elog(NOTICE, "TRF6: trade_dts %s", trade_dts);
	elog(NOTICE, "TRF6: trade_id %d", trade_id);
	elog(NOTICE, "TRF6: trade_is_cash %d", trade_is_cash);
	elog(NOTICE, "TRF6: trade_qty %d", trade_qty);
	elog(NOTICE, "TRF6: type_name %s", type_name);
	elog(NOTICE, "TRF6: INPUTS END");
}

/* Clause 3.3.8.3 */
Datum TradeResultFrame1(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	AttInMetadata *attinmeta;
	int call_cntr;
	int max_calls;

	int i;

	char **values = NULL;

	/* Stuff done only on the first call of the function. */
	if (SRF_IS_FIRSTCALL()) {
		MemoryContext oldcontext;

		enum trf1 {
				i_acct_id=0, i_charge, i_hs_qty, i_is_lifo, i_status,
				i_symbol, i_trade_is_cash, i_trade_qty, i_type_id,
				i_type_is_market, i_type_is_sell, i_type_name
		};

		int trade_id = PG_GETARG_INT64(0);

		int ret;
		TupleDesc tupdesc;
		SPITupleTable *tuptable = NULL;
		HeapTuple tuple = NULL;

		char sql[2048];

		/*
		 * Prepare a values array for building the returned tuple.
		 * This should be an array of C strings, which will
		 * be processed later by the type input functions.
		 */
		values = (char **) palloc(sizeof(char *) * 7);
		values[i_acct_id] = (char *) palloc((BIGINT_LEN + 1) * sizeof(char));
		values[i_charge] = (char *) palloc((VALUE_T_LEN + 1) * sizeof(char));
		values[i_hs_qty] = (char *) palloc((INTEGER_LEN + 1) * sizeof(char));
		values[i_is_lifo] = (char *) palloc((SMALLINT_LEN + 1) * sizeof(char));
		values[i_status] = (char *) palloc((STATUS_LEN + 1) * sizeof(char));
		values[i_symbol] = (char *) palloc((S_SYMB_LEN + 1) * sizeof(char));
		values[i_trade_is_cash] =
				(char *) palloc((SMALLINT_LEN + 1) * sizeof(char));
		values[i_trade_qty] = (char *) palloc((INTEGER_LEN + 1) * sizeof(char));
		values[i_type_id] = (char *) palloc((TT_ID_LEN + 1) * sizeof(char));
		values[i_type_is_market] =
				(char *) palloc((SMALLINT_LEN + 1) * sizeof(char));
		values[i_type_is_sell] =
				(char *) palloc((SMALLINT_LEN + 1) * sizeof(char));
		values[i_type_name] = (char *) palloc((TT_NAME_LEN + 1) * sizeof(char));

#ifdef DEBUG
		dump_trf1_inputs(trade_id);
#endif

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();
		strcpy(values[i_status], "0");
		funcctx->max_calls = 1;

		/* switch to memory context appropriate for multiple function calls */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		SPI_connect();

		sprintf(sql, TRF1_1, trade_id);
#ifdef DEBUG
		elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
		ret = SPI_exec(sql, 0);
		if (ret == SPI_OK_SELECT) {
			tupdesc = SPI_tuptable->tupdesc;
			tuptable = SPI_tuptable;
			if (SPI_processed > 0) {
				tuple = tuptable->vals[0];
				strcpy(values[i_acct_id], SPI_getvalue(tuple, tupdesc, 1));
				strcpy(values[i_type_id], SPI_getvalue(tuple, tupdesc, 2));
				strcpy(values[i_symbol], SPI_getvalue(tuple, tupdesc, 3));
				strcpy(values[i_trade_qty], SPI_getvalue(tuple, tupdesc, 4));
				strcpy(values[i_charge], SPI_getvalue(tuple, tupdesc, 5));
				strcpy(values[i_is_lifo], SPI_getvalue(tuple, tupdesc, 6));
				strcpy(values[i_trade_is_cash],
						SPI_getvalue(tuple, tupdesc, 7));
			}
		} else {
			FAIL_FRAME(&funcctx->max_calls, values[i_status], sql);
			dump_trf1_inputs(trade_id);
		}

		sprintf(sql, TRF1_2, values[i_type_id]);
#ifdef DEBUG
		elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
		ret = SPI_exec(sql, 0);
		if (ret == SPI_OK_SELECT) {
			tupdesc = SPI_tuptable->tupdesc;
			tuptable = SPI_tuptable;
			if (SPI_processed > 0) {
				tuple = tuptable->vals[0];
				strcpy(values[i_type_name], SPI_getvalue(tuple, tupdesc, 1));
				strcpy(values[i_type_is_sell], SPI_getvalue(tuple, tupdesc, 2));
				strcpy(values[i_type_is_market],
						SPI_getvalue(tuple, tupdesc, 3));
			}
		} else {
			FAIL_FRAME(&funcctx->max_calls, values[i_status], sql);
			dump_trf1_inputs(trade_id);
		}

		sprintf(sql, TRF1_3, values[i_acct_id], values[i_symbol]);
#ifdef DEBUG
		elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
		ret = SPI_exec(sql, 0);
		if (ret == SPI_OK_SELECT) {
			tupdesc = SPI_tuptable->tupdesc;
			tuptable = SPI_tuptable;
			if (SPI_processed > 0) {
				tuple = tuptable->vals[0];
				strcpy(values[i_hs_qty], SPI_getvalue(tuple, tupdesc, 1));
			} else {
				strcpy(values[i_hs_qty], "0");
			}
		} else {
			FAIL_FRAME(&funcctx->max_calls, values[i_status], sql);
			dump_trf1_inputs(trade_id);
		}

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) !=
				TYPEFUNC_COMPOSITE) {
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("function returning record called in context "
							"that cannot accept type record")));
		}

		/*
		 * generate attribute metadata needed later to produce tuples from raw
		 * C strings
		 */
		attinmeta = TupleDescGetAttInMetadata(tupdesc);
		funcctx->attinmeta = attinmeta;

		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();

	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;
	attinmeta = funcctx->attinmeta;

	if (call_cntr < max_calls) {
		/* do when there is more left to send */
		HeapTuple tuple;
		Datum result;

#ifdef DEBUG
		for (i = 0; i < 7; i++) {
			elog(NOTICE, "%d %s", i, values[i]);
		}
#endif /* DEBUG */

		/* Build a tuple. */
		tuple = BuildTupleFromCStrings(attinmeta, values);

		/* Make the tuple into a datum. */
		result = HeapTupleGetDatum(tuple);

		/* Clean up. */
		for (i = 0; i < 7; i++) {
			pfree(values[i]);
		}
		pfree(values);

		SRF_RETURN_NEXT(funcctx, result);
	} else {
		/* Do when there is no more left. */
		SPI_finish();
		SRF_RETURN_DONE(funcctx);
	}
}

/* Clause 3.3.8.4 */
Datum TradeResultFrame2(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	AttInMetadata *attinmeta;
	int call_cntr;
	int max_calls;

	int i;

	char **values = NULL;

	/* Stuff done only on the first call of the function. */
	if (SRF_IS_FIRSTCALL()) {
		MemoryContext oldcontext;

		enum trf2 {
				i_broker_id=0, i_buy_value, i_cust_id, i_sell_value, i_status,
				i_tax_status, i_trade_dts
		};

		int acct_id = PG_GETARG_INT64(0);
		int hs_qty = PG_GETARG_INT32(1);
		int is_lifo = PG_GETARG_INT16(2);
		char *symbol_p = (char *) PG_GETARG_TEXT_P(3);
		int trade_id = PG_GETARG_INT64(4);
		Numeric trade_price_num = PG_GETARG_NUMERIC(5);
		int trade_qty = PG_GETARG_INT32(6);
		int type_is_sell = PG_GETARG_INT16(7);

		int ret;
		TupleDesc tupdesc;
		SPITupleTable *tuptable = NULL;
		HeapTuple tuple = NULL;

		char sql[2048];

		char symbol[S_SYMB_LEN + 1];
		double trade_price;
		int needed_qty = trade_qty;

		double buy_value = 0;
		double sell_value = 0;

		strcpy(symbol, DatumGetCString(DirectFunctionCall1(textout,
				PointerGetDatum(symbol_p))));
		trade_price = DatumGetFloat8(DirectFunctionCall1(
				numeric_float8_no_overflow, PointerGetDatum(trade_price_num)));

		/*
		 * Prepare a values array for building the returned tuple.
		 * This should be an array of C strings, which will
		 * be processed later by the type input functions.
		 */
		values = (char **) palloc(sizeof(char *) * 7);
		values[i_broker_id] = (char *) palloc((BIGINT_LEN + 1) * sizeof(char));
		values[i_buy_value] =
				(char *) palloc((S_PRICE_T_LEN + 1) * sizeof(char));
		values[i_cust_id] = (char *) palloc((BIGINT_LEN + 1) * sizeof(char));
		values[i_sell_value] =
				(char *) palloc((S_PRICE_T_LEN + 1) * sizeof(char));
		values[i_status] = (char *) palloc((STATUS_LEN + 1) * sizeof(char));
		values[i_tax_status] =
				(char *) palloc((SMALLINT_LEN + 1) * sizeof(char));
		values[i_trade_dts] = (char *) palloc((MAXDATELEN + 1) * sizeof(char));

#ifdef DEBUG
		dump_trf2_inputs(acct_id, hs_qty, is_lifo, symbol, trade_id,
				trade_price, trade_qty, type_is_sell);
#endif

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();
		strcpy(values[i_status], "0");
		funcctx->max_calls = 1;

		/* switch to memory context appropriate for multiple function calls */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		SPI_connect();

		sprintf(sql, TRF2_1, acct_id);
#ifdef DEBUG
		elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
		ret = SPI_exec(sql, 0);
		if (ret == SPI_OK_SELECT) {
			tupdesc = SPI_tuptable->tupdesc;
			tuptable = SPI_tuptable;
			if (SPI_processed > 0) {
				tuple = tuptable->vals[0];
				strcpy(values[i_broker_id], SPI_getvalue(tuple, tupdesc, 1));
				strcpy(values[i_cust_id], SPI_getvalue(tuple, tupdesc, 2));
				strcpy(values[i_tax_status], SPI_getvalue(tuple, tupdesc, 3));
			}
		} else {
			FAIL_FRAME(&funcctx->max_calls, values[i_status], sql);
			dump_trf2_inputs(acct_id, hs_qty, is_lifo, symbol, trade_id,
				trade_price, trade_qty, type_is_sell);
		}

		if (type_is_sell == 1) {
			if (hs_qty == 0) {
				sprintf(sql, TRF2_2a, acct_id, symbol, trade_qty);
#ifdef DEBUG
				elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
				ret = SPI_exec(sql, 0);
				if (ret != SPI_OK_INSERT) {
					FAIL_FRAME(&funcctx->max_calls, values[i_status], sql);
					dump_trf2_inputs(acct_id, hs_qty, is_lifo, symbol,
							trade_id, trade_price, trade_qty, type_is_sell);
				}
			} else if (hs_qty != trade_qty) {
				sprintf(sql, TRF2_2b, hs_qty - trade_qty, acct_id, symbol);
#ifdef DEBUG
				elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
				ret = SPI_exec(sql, 0);
				if (ret != SPI_OK_UPDATE) {
					FAIL_FRAME(&funcctx->max_calls, values[i_status], sql);
					dump_trf2_inputs(acct_id, hs_qty, is_lifo, symbol,
							trade_id, trade_price, trade_qty, type_is_sell);
				}
			}

			if (hs_qty > 0) {
				if (is_lifo == 1) {
					sprintf(sql, TRF2_3a, acct_id, symbol);
				} else {
					sprintf(sql, TRF2_3b, acct_id, symbol);
				}
#ifdef DEBUG
				elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
				ret = SPI_exec(sql, 0);
				if (ret != SPI_OK_SELECT) {
					FAIL_FRAME(&funcctx->max_calls, values[i_status], sql);
					dump_trf2_inputs(acct_id, hs_qty, is_lifo, symbol,
							trade_id, trade_price, trade_qty, type_is_sell);
				}
				i = 0;
				tupdesc = SPI_tuptable->tupdesc;
				tuptable = SPI_tuptable;
				while (needed_qty > 0 && i < SPI_processed) {
					int hold_id;
					int hold_qty;
					double hold_price;

					tuple = tuptable->vals[i++];
					hold_id = atoi(SPI_getvalue(tuple, tupdesc, 1));
					hold_qty = atoi(SPI_getvalue(tuple, tupdesc, 2));
					hold_price = atof(SPI_getvalue(tuple, tupdesc, 3));

					if (hold_qty > needed_qty ) {
						sprintf(sql, TRF2_4a, trade_id, hold_id, hold_qty,
								hold_qty - needed_qty);
#ifdef DEBUG
						elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
						ret = SPI_exec(sql, 0);
						if (ret != SPI_OK_INSERT) {
							FAIL_FRAME(&funcctx->max_calls, values[i_status],
									sql);
							dump_trf2_inputs(acct_id, hs_qty, is_lifo, symbol,
									trade_id, trade_price, trade_qty,
									type_is_sell);
						}

						sprintf(sql, TRF2_5a, hold_qty - needed_qty, hold_id);
#ifdef DEBUG
						elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
						ret = SPI_exec(sql, 0);
						if (ret != SPI_OK_UPDATE) {
							FAIL_FRAME(&funcctx->max_calls, values[i_status],
									sql);
							dump_trf2_inputs(acct_id, hs_qty, is_lifo, symbol,
									trade_id, trade_price, trade_qty,
									type_is_sell);
						}

						buy_value += (double) needed_qty * hold_price;
						sell_value += (double) needed_qty * trade_price;
						needed_qty = 0;
					} else {
						sprintf(sql, TRF2_4a, trade_id, hold_id, hold_qty, 0);
#ifdef DEBUG
						elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
						ret = SPI_exec(sql, 0);
						if (ret != SPI_OK_INSERT) {
							FAIL_FRAME(&funcctx->max_calls, values[i_status],
									sql);
							dump_trf2_inputs(acct_id, hs_qty, is_lifo, symbol,
									trade_id, trade_price, trade_qty,
									type_is_sell);
						}

						sprintf(sql, TRF2_5b, hold_id);
#ifdef DEBUG
						elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
						ret = SPI_exec(sql, 0);
						if (ret != SPI_OK_DELETE) {
							FAIL_FRAME(&funcctx->max_calls, values[i_status],
									sql);
							dump_trf2_inputs(acct_id, hs_qty, is_lifo, symbol,
									trade_id, trade_price, trade_qty,
									type_is_sell);
						}
					}
				}
			}

			if (needed_qty > 0) {
				sprintf(sql, TRF2_4a, trade_id, trade_id, 0, -1 * needed_qty);
#ifdef DEBUG
				elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
				ret = SPI_exec(sql, 0);
				if (ret != SPI_OK_INSERT) {
					FAIL_FRAME(&funcctx->max_calls, values[i_status], sql);
					dump_trf2_inputs(acct_id, hs_qty, is_lifo, symbol,
							trade_id, trade_price, trade_qty, type_is_sell);
				}

				sprintf(sql, TRF2_7a, trade_id, acct_id, symbol, "now()",
						trade_price, -1 * needed_qty);
#ifdef DEBUG
				elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
				ret = SPI_exec(sql, 0);
				if (ret == SPI_OK_INSERT_RETURNING && SPI_processed > 0) {
					tupdesc = SPI_tuptable->tupdesc;
					tuptable = SPI_tuptable;
					tuple = tuptable->vals[0];
					strcpy(values[i_trade_dts],
							SPI_getvalue(tuple, tupdesc, 1));
				} else {
					FAIL_FRAME(&funcctx->max_calls, values[i_status], sql);
					dump_trf2_inputs(acct_id, hs_qty, is_lifo, symbol,
							trade_id, trade_price, trade_qty, type_is_sell);
				}
			} else if (hs_qty == trade_qty) {
				sprintf(sql, TRF2_7b, acct_id, symbol);
#ifdef DEBUG
				elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
				ret = SPI_exec(sql, 0);
				if (ret != SPI_OK_DELETE) {
					FAIL_FRAME(&funcctx->max_calls, values[i_status], sql);
					dump_trf2_inputs(acct_id, hs_qty, is_lifo, symbol,
							trade_id, trade_price, trade_qty, type_is_sell);
				}
			}
		} else {
			if (hs_qty == 0) {
				sprintf(sql, TRF2_8a, acct_id, symbol, trade_qty);
#ifdef DEBUG
				elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
				ret = SPI_exec(sql, 0);
				if (ret != SPI_OK_INSERT) {
					FAIL_FRAME(&funcctx->max_calls, values[i_status], sql);
					dump_trf2_inputs(acct_id, hs_qty, is_lifo, symbol,
							trade_id, trade_price, trade_qty, type_is_sell);
				}
			} else if (hs_qty != trade_qty) {
				sprintf(sql, TRF2_8b, hs_qty + trade_qty, acct_id, symbol);
#ifdef DEBUG
				elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
				ret = SPI_exec(sql, 0);
				if (ret != SPI_OK_UPDATE) {
					FAIL_FRAME(&funcctx->max_calls, values[i_status], sql);
					dump_trf2_inputs(acct_id, hs_qty, is_lifo, symbol,
							trade_id, trade_price, trade_qty, type_is_sell);
				}
			}

			if (hs_qty < 0) {
				if (is_lifo == 1) {
					sprintf(sql, TRF2_3a, acct_id, symbol);
				} else {
					sprintf(sql, TRF2_3b, acct_id, symbol);
				}
#ifdef DEBUG
				elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
				ret = SPI_exec(sql, 0);
				if (ret != SPI_OK_SELECT) {
					FAIL_FRAME(&funcctx->max_calls, values[i_status], sql);
					dump_trf2_inputs(acct_id, hs_qty, is_lifo, symbol,
						trade_id, trade_price, trade_qty, type_is_sell);
				}
				tupdesc = SPI_tuptable->tupdesc;
				tuptable = SPI_tuptable;
				i = 0;
				while (needed_qty > 0 && i < SPI_processed) {
					int hold_id;
					int hold_qty;
					double hold_price;

					tuple = tuptable->vals[i++];
					hold_id = atoi(SPI_getvalue(tuple, tupdesc, 1));
					hold_qty = atoi(SPI_getvalue(tuple, tupdesc, 2));
					hold_price = atof(SPI_getvalue(tuple, tupdesc, 3));

					if (hold_qty + needed_qty < 0) {
						sprintf(sql, TRF2_4a, hold_id, trade_id, hold_qty,
								hold_qty + needed_qty);
#ifdef DEBUG
						elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
						ret = SPI_exec(sql, 0);
						if (ret != SPI_OK_INSERT) {
							FAIL_FRAME(&funcctx->max_calls, values[i_status],
									sql);
							dump_trf2_inputs(acct_id, hs_qty, is_lifo, symbol,
									trade_id, trade_price, trade_qty,
									type_is_sell);
						}

						sprintf(sql, TRF2_5a, hold_qty + needed_qty, hold_id);
#ifdef DEBUG
						elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
						ret = SPI_exec(sql, 0);
						if (ret != SPI_OK_UPDATE) {
							FAIL_FRAME(&funcctx->max_calls, values[i_status],
									sql);
							dump_trf2_inputs(acct_id, hs_qty, is_lifo, symbol,
									trade_id, trade_price, trade_qty,
									type_is_sell);
						}

						buy_value += (double) needed_qty * hold_price;
						sell_value += (double) needed_qty * trade_price;
						needed_qty = 0;
					} else {
						sprintf(sql, TRF2_4a, trade_id, hold_id, hold_qty, 0);
#ifdef DEBUG
						elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
						ret = SPI_exec(sql, 0);
						if (ret != SPI_OK_INSERT) {
							FAIL_FRAME(&funcctx->max_calls, values[i_status],
									sql);
							dump_trf2_inputs(acct_id, hs_qty, is_lifo, symbol,
									trade_id, trade_price, trade_qty,
									type_is_sell);
						}

						sprintf(sql, TRF2_5b, hold_id);
#ifdef DEBUG
						elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
						ret = SPI_exec(sql, 0);
						if (ret != SPI_OK_DELETE) {
							FAIL_FRAME(&funcctx->max_calls, values[i_status],
									sql);
							dump_trf2_inputs(acct_id, hs_qty, is_lifo, symbol,
									trade_id, trade_price, trade_qty,
									type_is_sell);
						}
					}
				}

				if (needed_qty > 0) {
					sprintf(sql, TRF2_4a, trade_id, trade_id, 0, needed_qty);
#ifdef DEBUG
					elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
					ret = SPI_exec(sql, 0);
					if (ret != SPI_OK_INSERT) {
						FAIL_FRAME(&funcctx->max_calls, values[i_status], sql);
						dump_trf2_inputs(acct_id, hs_qty, is_lifo, symbol,
								trade_id, trade_price, trade_qty, type_is_sell);
					}

					sprintf(sql, TRF2_7a, trade_id, acct_id, symbol, "now()",
							trade_price, needed_qty);
#ifdef DEBUG
					elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
					ret = SPI_exec(sql, 0);
					if (ret == SPI_OK_INSERT_RETURNING && SPI_processed > 0) {
						tupdesc = SPI_tuptable->tupdesc;
						tuptable = SPI_tuptable;
						tuple = tuptable->vals[0];
						strcpy(values[i_trade_dts],
								SPI_getvalue(tuple, tupdesc, 1));
					} else {
						FAIL_FRAME(&funcctx->max_calls, values[i_status], sql);
						dump_trf2_inputs(acct_id, hs_qty, is_lifo, symbol,
								trade_id, trade_price, trade_qty, type_is_sell);
					}
				} else {
					sprintf(sql, TRF2_7b, acct_id, symbol);
#ifdef DEBUG
					elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
					ret = SPI_exec(sql, 0);
					if (ret != SPI_OK_DELETE) {
						FAIL_FRAME(&funcctx->max_calls, values[i_status], sql);
						dump_trf2_inputs(acct_id, hs_qty, is_lifo, symbol,
								trade_id, trade_price, trade_qty, type_is_sell);
					}
				}
			}
		}

		sprintf(values[i_buy_value], "%.2f", buy_value);
		sprintf(values[i_sell_value], "%.2f", sell_value);

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) !=
				TYPEFUNC_COMPOSITE) {
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("function returning record called in context "
							"that cannot accept type record")));
		}

		/*
		 * generate attribute metadata needed later to produce tuples from raw
		 * C strings
		 */
		attinmeta = TupleDescGetAttInMetadata(tupdesc);
		funcctx->attinmeta = attinmeta;

		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();

	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;
	attinmeta = funcctx->attinmeta;

	if (call_cntr < max_calls) {
		/* do when there is more left to send */
		HeapTuple tuple;
		Datum result;

#ifdef DEBUG
		for (i = 0; i < 7; i++) {
			elog(NOTICE, "%d %s", i, values[i]);
		}
#endif /* DEBUG */

		/* Build a tuple. */
		tuple = BuildTupleFromCStrings(attinmeta, values);

		/* Make the tuple into a datum. */
		result = HeapTupleGetDatum(tuple);

		/* Clean up. */
		for (i = 0; i < 7; i++) {
			pfree(values[i]);
		}
		pfree(values);

		SRF_RETURN_NEXT(funcctx, result);
	} else {
		/* Do when there is no more left. */
		SPI_finish();
		SRF_RETURN_DONE(funcctx);
	}
}

/* Clause 3.3.8.5 */
Datum TradeResultFrame3(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	AttInMetadata *attinmeta;
	int call_cntr;
	int max_calls;

	int i;

	char **values = NULL;

	/* Stuff done only on the first call of the function. */
	if (SRF_IS_FIRSTCALL()) {
		MemoryContext oldcontext;

		enum trf3 { i_status=0, i_tax_amount };

		Numeric buy_value_num = PG_GETARG_NUMERIC(0);
		int cust_id = PG_GETARG_INT64(1);
		Numeric sell_value_num = PG_GETARG_NUMERIC(2);
		int trade_id = PG_GETARG_INT64(3);

		int ret;
		TupleDesc tupdesc;
		SPITupleTable *tuptable = NULL;
		HeapTuple tuple = NULL;

		char sql[2048];

		double buy_value;
		double sell_value;

		buy_value = DatumGetFloat8(DirectFunctionCall1(
				numeric_float8_no_overflow, PointerGetDatum(buy_value_num)));
		sell_value = DatumGetFloat8(DirectFunctionCall1(
				numeric_float8_no_overflow, PointerGetDatum(sell_value_num)));

		/*
		 * Prepare a values array for building the returned tuple.
		 * This should be an array of C strings, which will
		 * be processed later by the type input functions.
		 */
		values = (char **) palloc(sizeof(char *) * 2);
		values[i_status] = (char *) palloc((STATUS_LEN + 1) * sizeof(char));
		values[i_tax_amount] =
				(char *) palloc((VALUE_T_LEN + 1) * sizeof(char));

#ifdef DEBUG
		dump_trf3_inputs(buy_value, cust_id, sell_value, trade_id);
#endif

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();
		strcpy(values[i_status], "0");
		funcctx->max_calls = 1;

		/* switch to memory context appropriate for multiple function calls */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		SPI_connect();

		sprintf(sql, TRF3_1, cust_id);
#ifdef DEBUG
		elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
		ret = SPI_exec(sql, 0);
		if (ret == SPI_OK_SELECT) {
			tupdesc = SPI_tuptable->tupdesc;
			tuptable = SPI_tuptable;
			if (SPI_processed > 0) {
				double tax_rates;
				tuple = tuptable->vals[0];
				tax_rates = atof(SPI_getvalue(tuple, tupdesc, 1));
				sprintf(values[i_tax_amount], "%8.2f",
						(sell_value - buy_value) * tax_rates);
			}
		} else {
			FAIL_FRAME(&funcctx->max_calls, values[i_status], sql);
			dump_trf3_inputs(buy_value, cust_id, sell_value, trade_id);
		}

		sprintf(sql, TRF3_2, values[i_tax_amount], trade_id);
#ifdef DEBUG
		elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
		ret = SPI_exec(sql, 0);
		if (ret != SPI_OK_UPDATE) {
			FAIL_FRAME(&funcctx->max_calls, values[i_status], sql);
			dump_trf3_inputs(buy_value, cust_id, sell_value, trade_id);
		}

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) !=
				TYPEFUNC_COMPOSITE) {
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("function returning record called in context "
							"that cannot accept type record")));
		}

		/*
		 * generate attribute metadata needed later to produce tuples from raw
		 * C strings
		 */
		attinmeta = TupleDescGetAttInMetadata(tupdesc);
		funcctx->attinmeta = attinmeta;

		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();

	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;
	attinmeta = funcctx->attinmeta;

	if (call_cntr < max_calls) {
		/* do when there is more left to send */
		HeapTuple tuple;
		Datum result;

#ifdef DEBUG
		for (i = 0; i < 2; i++) {
			elog(NOTICE, "%d %s", i, values[i]);
		}
#endif /* DEBUG */

		/* Build a tuple. */
		tuple = BuildTupleFromCStrings(attinmeta, values);

		/* Make the tuple into a datum. */
		result = HeapTupleGetDatum(tuple);

		/* Clean up. */
		for (i = 0; i < 2; i++) {
			pfree(values[i]);
		}
		pfree(values);

		SRF_RETURN_NEXT(funcctx, result);
	} else {
		/* Do when there is no more left. */
		SPI_finish();
		SRF_RETURN_DONE(funcctx);
	}
}

/* Clause 3.3.8.6 */
Datum TradeResultFrame4(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	AttInMetadata *attinmeta;
	int call_cntr;
	int max_calls;

	int i;

	char **values = NULL;

	/* Stuff done only on the first call of the function. */
	if (SRF_IS_FIRSTCALL()) {
		MemoryContext oldcontext;

		enum trf4 { i_comm_rate=0, i_s_name, i_status };

		int cust_id = PG_GETARG_INT64(0);
		char *symbol_p = (char *) PG_GETARG_TEXT_P(1);
		int trade_qty = PG_GETARG_INT32(2);
		char *type_id_p = (char *) PG_GETARG_TEXT_P(3);

		int ret;
		TupleDesc tupdesc;
		SPITupleTable *tuptable = NULL;
		HeapTuple tuple = NULL;

		char sql[2048];

		char symbol[S_SYMB_LEN + 1];
		char type_id[TT_ID_LEN + 1];

		char s_ex_id[S_EX_ID_LEN];
		char c_tier[SMALLINT_LEN + 1];

		strcpy(symbol, DatumGetCString(DirectFunctionCall1(textout,
				PointerGetDatum(symbol_p))));
		strcpy(type_id, DatumGetCString(DirectFunctionCall1(textout,
				PointerGetDatum(type_id_p))));

		/*
		 * Prepare a values array for building the returned tuple.
		 * This should be an array of C strings, which will
		 * be processed later by the type input functions.
		 */
		values = (char **) palloc(sizeof(char *) * 3);
		values[i_comm_rate] = (char *) palloc((CR_RATE_LEN + 1) * sizeof(char));
		values[i_s_name] = (char *) palloc((S_NAME_LEN + 1) * sizeof(char));
		values[i_status] = (char *) palloc((STATUS_LEN + 1) * sizeof(char));


#ifdef DEBUG
		dump_trf4_inputs(cust_id, symbol, trade_qty, type_id);
#endif

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();
		strcpy(values[i_status], "0");
		funcctx->max_calls = 1;

		/* switch to memory context appropriate for multiple function calls */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		SPI_connect();

		sprintf(sql, TRF4_1, symbol);
#ifdef DEBUG
		elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
		ret = SPI_exec(sql, 0);
		if (ret == SPI_OK_SELECT && SPI_processed > 0) {
			tupdesc = SPI_tuptable->tupdesc;
			tuptable = SPI_tuptable;
			tuple = tuptable->vals[0];
			strcpy(s_ex_id, SPI_getvalue(tuple, tupdesc, 1));
			strcpy(values[i_s_name], SPI_getvalue(tuple, tupdesc, 2));
		} else {
			FAIL_FRAME(&funcctx->max_calls, values[i_status], sql);
			dump_trf4_inputs(cust_id, symbol, trade_qty, type_id);
		}

		sprintf(sql, TRF4_2, cust_id);
#ifdef DEBUG
		elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
		ret = SPI_exec(sql, 0);
		if (ret == SPI_OK_SELECT && SPI_processed > 0) {
			tupdesc = SPI_tuptable->tupdesc;
			tuptable = SPI_tuptable;
			tuple = tuptable->vals[0];
			strcpy(c_tier, SPI_getvalue(tuple, tupdesc, 1));
		} else {
			FAIL_FRAME(&funcctx->max_calls, values[i_status], sql);
			dump_trf4_inputs(cust_id, symbol, trade_qty, type_id);
		}

		sprintf(sql, TRF4_3, c_tier, type_id, s_ex_id, trade_qty, trade_qty);
#ifdef DEBUG
		elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
		ret = SPI_exec(sql, 0);
		if (ret == SPI_OK_SELECT && SPI_processed > 0) {
			tupdesc = SPI_tuptable->tupdesc;
			tuptable = SPI_tuptable;
			tuple = tuptable->vals[0];
			strcpy(values[i_comm_rate], SPI_getvalue(tuple, tupdesc, 1));
		} else {
			FAIL_FRAME(&funcctx->max_calls, values[i_status], sql);
			dump_trf4_inputs(cust_id, symbol, trade_qty, type_id);
		}

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) !=
				TYPEFUNC_COMPOSITE) {
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("function returning record called in context "
							"that cannot accept type record")));
		}

		/*
		 * generate attribute metadata needed later to produce tuples from raw
		 * C strings
		 */
		attinmeta = TupleDescGetAttInMetadata(tupdesc);
		funcctx->attinmeta = attinmeta;

		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();

	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;
	attinmeta = funcctx->attinmeta;

	if (call_cntr < max_calls) {
		/* do when there is more left to send */
		HeapTuple tuple;
		Datum result;

#ifdef DEBUG
		for (i = 0; i < 3; i++) {
			elog(NOTICE, "%d %s", i, values[i]);
		}
#endif /* DEBUG */

		/* Build a tuple. */
		tuple = BuildTupleFromCStrings(attinmeta, values);

		/* Make the tuple into a datum. */
		result = HeapTupleGetDatum(tuple);

		/* Clean up. */
		for (i = 0; i < 3; i++) {
			pfree(values[i]);
		}
		pfree(values);

		SRF_RETURN_NEXT(funcctx, result);
	} else {
		/* Do when there is no more left. */
		SPI_finish();
		SRF_RETURN_DONE(funcctx);
	}
}

/* Clause 3.3.8.7 */
Datum TradeResultFrame5(PG_FUNCTION_ARGS)
{
	int status = 0;

	int broker_id = PG_GETARG_INT64(0);
	Numeric comm_amount_num = PG_GETARG_NUMERIC(1);
	char *st_completed_id_p = (char *) PG_GETARG_TEXT_P(2);
	Timestamp trade_dts_ts = PG_GETARG_TIMESTAMP(3);
	int trade_id = PG_GETARG_INT64(4);
	Numeric trade_price_num = PG_GETARG_NUMERIC(5);

	int ret;
	TupleDesc tupdesc;

	struct pg_tm tt, *tm = &tt;
	fsec_t fsec;
	char *tzn = NULL;

	char sql[2048];

	double comm_amount;
	double trade_price;
	char trade_dts[MAXDATELEN + 1];
	char st_completed_id[T_ST_ID_LEN + 1];

	strcpy(st_completed_id, DatumGetCString(DirectFunctionCall1(textout,
			PointerGetDatum(st_completed_id_p))));

	comm_amount = DatumGetFloat8(DirectFunctionCall1(
			numeric_float8_no_overflow, PointerGetDatum(comm_amount_num)));
	trade_price = DatumGetFloat8(DirectFunctionCall1(
			numeric_float8_no_overflow, PointerGetDatum(trade_price_num)));

	if (timestamp2tm(trade_dts_ts, NULL, tm, &fsec, NULL, NULL) == 0) {
		EncodeDateTime(tm, fsec, NULL, &tzn, USE_ISO_DATES, trade_dts);
	}


#ifdef DEBUG
	dump_trf5_inputs(broker_id, comm_amount, st_completed_id, trade_dts,
			trade_id, trade_price);
#endif

	SPI_connect();

	sprintf(sql, TRF5_1, comm_amount, trade_dts, st_completed_id, trade_price,
			trade_id);
#ifdef DEBUG
	elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
	ret = SPI_exec(sql, 0);
	if (ret != SPI_OK_UPDATE) {
		FAIL_FRAME2(status, sql);
		dump_trf5_inputs(broker_id, comm_amount, st_completed_id, trade_dts,
				trade_id, trade_price);
	}
	tupdesc = SPI_tuptable->tupdesc;

	sprintf(sql, TRF5_2, trade_id, trade_dts, st_completed_id);
#ifdef DEBUG
	elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
	ret = SPI_exec(sql, 0);
	if (ret != SPI_OK_INSERT) {
		FAIL_FRAME2(status, sql);
		dump_trf5_inputs(broker_id, comm_amount, st_completed_id, trade_dts,
				trade_id, trade_price);
	}

	sprintf(sql, TRF5_3, comm_amount, broker_id);
#ifdef DEBUG
	elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
	ret = SPI_exec(sql, 0);
	if (ret != SPI_OK_UPDATE) {
		FAIL_FRAME2(status, sql);
		dump_trf5_inputs(broker_id, comm_amount, st_completed_id, trade_dts,
				trade_id, trade_price);
	}

#ifdef DEBUG
	elog(NOTICE, "0 %d", status);
#endif /* DEBUG */

	SPI_finish();
	PG_RETURN_INT32(status);
}

/* Clause 3.3.8.8 */
Datum TradeResultFrame6(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	AttInMetadata *attinmeta;
	int call_cntr;
	int max_calls;

	int i;

	char **values = NULL;

	/* Stuff done only on the first call of the function. */
	if (SRF_IS_FIRSTCALL()) {
		MemoryContext oldcontext;

		enum trf6 { i_acct_bal=0, i_status };

		int acct_id = PG_GETARG_INT64(0);
		Timestamp due_date_ts = PG_GETARG_TIMESTAMP(1);
		char *s_name_p = (char *) PG_GETARG_TEXT_P(2);
		Numeric se_amount_num = PG_GETARG_NUMERIC(3);
		Timestamp trade_dts_ts = PG_GETARG_TIMESTAMP(4);
		int trade_id = PG_GETARG_INT64(5);
		int trade_is_cash = PG_GETARG_INT16(6);
		int trade_qty = PG_GETARG_INT32(7);
		char *type_name_p = (char *) PG_GETARG_TEXT_P(8);

		struct pg_tm tt, *tm = &tt;
		fsec_t fsec;
		char *tzn = NULL;

		int ret;
		TupleDesc tupdesc;
		SPITupleTable *tuptable = NULL;
		HeapTuple tuple = NULL;

		char sql[2048];

		char s_name[S_NAME_LEN + 1];
		char type_name[TT_NAME_LEN + 1];
		double se_amount;

		char due_date[MAXDATELEN + 1];
		char trade_dts[MAXDATELEN + 1];

		char cash_type[41];

		se_amount = DatumGetFloat8(DirectFunctionCall1(
				numeric_float8_no_overflow, PointerGetDatum(se_amount_num)));

		strcpy(s_name, DatumGetCString(DirectFunctionCall1(textout,
				PointerGetDatum(s_name_p))));
		strcpy(type_name, DatumGetCString(DirectFunctionCall1(textout,
				PointerGetDatum(type_name_p))));

		if (timestamp2tm(due_date_ts, NULL, tm, &fsec, NULL, NULL) == 0) {
			EncodeDateTime(tm, fsec, NULL, &tzn, USE_ISO_DATES, due_date);
		}
		if (timestamp2tm(trade_dts_ts, NULL, tm, &fsec, NULL, NULL) == 0) {
			EncodeDateTime(tm, fsec, NULL, &tzn, USE_ISO_DATES, trade_dts);
		}

		/*
		 * Prepare a values array for building the returned tuple.
		 * This should be an array of C strings, which will
		 * be processed later by the type input functions.
		 */
		values = (char **) palloc(sizeof(char *) * 2);
		values[i_acct_bal] =
				(char *) palloc((BALANCE_T_LEN + 1) * sizeof(char));
		values[i_status] = (char *) palloc((STATUS_LEN + 1) * sizeof(char));

#ifdef DEBUG
		dump_trf6_inputs(acct_id, due_date, s_name, se_amount, trade_dts,
				trade_id, trade_is_cash, trade_qty, type_name);
#endif

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();
		strcpy(values[i_status], "0");
		funcctx->max_calls = 1;

		/* switch to memory context appropriate for multiple function calls */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		SPI_connect();

		if (trade_is_cash == 1) {
			strcpy(cash_type, "Cash Account");
		} else {
			strcpy(cash_type, "Margin");
		}

		sprintf(sql, TRF6_1, trade_id, cash_type, due_date, se_amount);
#ifdef DEBUG
		elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
		ret = SPI_exec(sql, 0);
		if (ret != SPI_OK_INSERT) {
			FAIL_FRAME(&funcctx->max_calls, values[i_status], sql);
			dump_trf6_inputs(acct_id, due_date, s_name, se_amount, trade_dts,
					trade_id, trade_is_cash, trade_qty, type_name);
		}

		if (trade_is_cash == 1) {
			sprintf(sql, TRF6_2, se_amount, acct_id);
#ifdef DEBUG
			elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
			ret = SPI_exec(sql, 0);
			if (ret != SPI_OK_UPDATE) {
				FAIL_FRAME(&funcctx->max_calls, values[i_status], sql);
				dump_trf6_inputs(acct_id, due_date, s_name, se_amount,
						trade_dts, trade_id, trade_is_cash, trade_qty,
						type_name);
			}

			sprintf(sql, TRF6_3, trade_dts, trade_id, se_amount, type_name,
					trade_qty, s_name);
#ifdef DEBUG
			elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
			ret = SPI_exec(sql, 0);
			if (ret != SPI_OK_INSERT) {
				FAIL_FRAME(&funcctx->max_calls, values[i_status], sql);
				dump_trf6_inputs(acct_id, due_date, s_name, se_amount,
						trade_dts, trade_id, trade_is_cash, trade_qty,
						type_name);
			}
		}

		sprintf(sql, TRF6_4, acct_id);
#ifdef DEBUG
		elog(NOTICE, "SQL\n%s", sql);
#endif /* DEBUG */
		ret = SPI_exec(sql, 0);
		if (ret == SPI_OK_SELECT && SPI_processed > 0) {
			tupdesc = SPI_tuptable->tupdesc;
			tuptable = SPI_tuptable;
			tuple = tuptable->vals[0];
			strcpy(values[i_acct_bal], SPI_getvalue(tuple, tupdesc, 1));
		} else {
			FAIL_FRAME(&funcctx->max_calls, values[i_status], sql);
			dump_trf6_inputs(acct_id, due_date, s_name, se_amount, trade_dts,
					trade_id, trade_is_cash, trade_qty, type_name);
		}

		/* Build a tuple descriptor for our result type */
		if (get_call_result_type(fcinfo, NULL, &tupdesc) !=
				TYPEFUNC_COMPOSITE) {
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("function returning record called in context "
							"that cannot accept type record")));
		}

		/*
		 * generate attribute metadata needed later to produce tuples from raw
		 * C strings
		 */
		attinmeta = TupleDescGetAttInMetadata(tupdesc);
		funcctx->attinmeta = attinmeta;

		MemoryContextSwitchTo(oldcontext);
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();

	call_cntr = funcctx->call_cntr;
	max_calls = funcctx->max_calls;
	attinmeta = funcctx->attinmeta;

	if (call_cntr < max_calls) {
		/* do when there is more left to send */
		HeapTuple tuple;
		Datum result;

#ifdef DEBUG
		for (i = 0; i < 2; i++) {
			elog(NOTICE, "%d %s", i, values[i]);
		}
#endif /* DEBUG */

		/* Build a tuple. */
		tuple = BuildTupleFromCStrings(attinmeta, values);

		/* Make the tuple into a datum. */
		result = HeapTupleGetDatum(tuple);

		/* Clean up. */
		for (i = 0; i < 2; i++) {
			pfree(values[i]);
		}
		pfree(values);

		SRF_RETURN_NEXT(funcctx, result);
	} else {
		/* Do when there is no more left. */
		SPI_finish();
		SRF_RETURN_DONE(funcctx);
	}
}