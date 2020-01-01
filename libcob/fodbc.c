/*
   Copyright (C) 2002-2012, 2014-2019 Free Software Foundation, Inc.
   Written by Keisuke Nishida, Roger While, Simon Sobisch, Ron Norman

   This file is part of GnuCOBOL.

   The GnuCOBOL runtime library is free software: you can redistribute it
   and/or modify it under the terms of the GNU Lesser General Public License
   as published by the Free Software Foundation, either version 3 of the
   License, or (at your option) any later version.

   GnuCOBOL is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with GnuCOBOL.  If not, see <https://www.gnu.org/licenses/>.
*/


/* Force symbol exports */
#define	COB_LIB_EXPIMP
#include "fileio.h"

#ifdef WITH_ODBC
#ifdef WITH_OCI
#undef WITH_OCI
#endif

#if defined(DB2DBI)
#include <sqlcli1.h>
#include <sqlca.h>
#include <sqludf.h> 
#else
#include <sql.h>
#include <sqlext.h>
#endif

void cob_odbc_init_fileio (cob_file_api *a);

/* Local variables */

static int odbc_sync		(cob_file_api *, cob_file *);
static int odbc_open		(cob_file_api *, cob_file *, char *, const int, const int);
static int odbc_close		(cob_file_api *, cob_file *, const int);
static int odbc_start		(cob_file_api *, cob_file *, const int, cob_field *);
static int odbc_read		(cob_file_api *, cob_file *, cob_field *, const int);
static int odbc_read_next	(cob_file_api *, cob_file *, const int);
static int odbc_write		(cob_file_api *, cob_file *, const int);
static int odbc_delete		(cob_file_api *, cob_file *);
static int odbc_file_delete	(cob_file_api *, cob_file *, char *);
static int odbc_rewrite		(cob_file_api *, cob_file *, const int);
static int odbc_file_unlock (cob_file_api *, cob_file *);
static void odbc_exit_fileio(cob_file_api *);
static int odbc_fork 		(cob_file_api *);

static int odbc_dummy () { return 0; }

static const struct cob_fileio_funcs odbc_indexed_funcs = {
	odbc_open,
	odbc_close,
	odbc_start,
	odbc_read,
	odbc_read_next,
	odbc_write,
	odbc_rewrite,
	odbc_delete,
	odbc_file_delete,
	cob_odbc_init_fileio,
	odbc_exit_fileio,
	odbc_fork,
	odbc_sync,
	(void*)odbc_dummy,
	odbc_file_unlock,
	(void*)odbc_dummy
};

static int		db_join = 1;
static struct db_state db[1];
static int	useDriverCursor  = FALSE;
static int	useIfneededCursor= TRUE;
static int	mssqlver = 2008;
static char	varFetch[80];

struct indexed_file {
	struct file_xfd	*fx;
	int		startcond;
	int		isdesc;				/* Was START for one of <, <=, LAST */
	int		maxkeylen;
	int		primekeylen;
	enum {
		LMANULOCK = 0,
		LAUTOLOCK = 1,
		LEXCLLOCK = 2,
	} lmode;
	unsigned char	*savekey;	/* Work area for saving key value */
	unsigned char	*suppkey;	/* Work area for saving key value */
	unsigned char	*saverec;	/* For saving copy of record */
};

/* Local functions */

/*
	Check if ODBC status is a fatal error of some kind
*/
static int				/* Return TRUE if some fatal error */
chkOdbc(struct db_state *db)
{
	if(memcmp(db->odbcState, "23000", 5) == 0) {
		db->dbStatus = db->dbStsDupKey;
		return TRUE;
	}
	if(memcmp(db->odbcState, "23505", 5) == 0) {
		db->dbStatus = db->dbStsDupKey;
		return TRUE;
	}
	if(memcmp(db->odbcState,"07",2) == 0) {
		db->dbStatus = db->dbFatalStatus = 1007;
		return TRUE;
	}
	if(memcmp(db->odbcState,"08",2) == 0) {
		db->dbStatus = db->dbFatalStatus = 1008;
		return TRUE;
	}
	if(memcmp(db->odbcState,"21",2) == 0) {
		db->dbStatus = db->dbFatalStatus = 1021;
		return TRUE;
	}
	if(memcmp(db->odbcState,"22",2) == 0) {
		if(memcmp(db->odbcState, "22007", 5) == 0
		|| memcmp(db->odbcState, "22008", 5) == 0) {	/* Bad DATE */
			db->dbStatus = db->dbStsInvlNum;
			return TRUE;
		}
		if(memcmp(db->odbcState, "22003", 5) == 0) {	/* Bad Number */
			db->dbStatus = db->dbStsInvlNum;
			return TRUE;
		}
		db->dbStatus = db->dbFatalStatus = 1022;
		return TRUE;
	}
	if(memcmp(db->odbcState,"22",2) == 0) {
		db->dbStatus = db->dbFatalStatus = 1022;
		return TRUE;
	}
	if(memcmp(db->odbcState,"24",2) == 0) {
		db->dbStatus = db->dbFatalStatus = 1024;
		return TRUE;
	}
	if(memcmp(db->odbcState,"25",2) == 0) {
		db->dbStatus = db->dbFatalStatus = 1025;
		return TRUE;
	}
	if(memcmp(db->odbcState,"28",2) == 0) {
		db->dbStatus = db->dbFatalStatus = 1028;
		return TRUE;
	}
	if(memcmp(db->odbcState,"34",2) == 0
	|| memcmp(db->odbcState,"3C",2) == 0
	|| memcmp(db->odbcState,"3D",2) == 0
	|| memcmp(db->odbcState,"3F",2) == 0) {
		db->dbStatus = db->dbFatalStatus = 1034;
		return TRUE;
	}
	if(memcmp(db->odbcState,"42",2) == 0) {
		db->dbStatus = db->dbFatalStatus = 1042;
		return TRUE;
	}
	if(memcmp(db->odbcState,"44",2) == 0) {
		db->dbStatus = db->dbFatalStatus = 1044;
		return TRUE;
	}
	if(memcmp(db->odbcState,"HY",2) == 0) {
		db->dbStatus = db->dbFatalStatus = 1098;
		return TRUE;
	}
	if(memcmp(db->odbcState,"IM",2) == 0) {
		db->dbStatus = db->dbFatalStatus = 1099;
		return TRUE;
	}
	if(memcmp(db->odbcState, "S1T00", 5) == 0) {
		db->dbStatus = db->dbFatalStatus = db->dbStsDeadLock;
		return TRUE;
	}
	if(memcmp(db->odbcState, "40001", 5) == 0) {
		db->dbStatus = db->dbFatalStatus = db->dbStsDeadLock;
		return TRUE;
	}
	if(memcmp(db->odbcState, "S1", 2) == 0) {
		db->dbStatus = db->dbFatalStatus = 1097;
		return TRUE;
	}
	if(memcmp(db->odbcState, "01000", 5) == 0) {
		return FALSE;
	}
	return FALSE;
}

#define szErrMsg 512
#define dbStsRetry          (EAGAIN * 1000)
static int
getOdbcMsg(
	struct db_state	*db,
	SQLHANDLE	hndl,
	int			*errnum,
	char		*szState,
	SQLINTEGER	*odbcStatus,
	char		*errMsg,
	int			errMsgLen,
	SQLSMALLINT *errLen)
{
	int		sts,htype;
	char	lState[5+3];
	char	*cp,msgtxt[szErrMsg + 10];
	if(hndl == db->dbEnvH) {
		htype = SQL_HANDLE_ENV;
	} else if(hndl == db->dbDbcH) {
		htype = SQL_HANDLE_DBC;
	} else {
		htype = SQL_HANDLE_STMT;
	}
	sts = SQLGetDiagRec(htype,hndl,*errnum,(SQLCHAR*)lState,odbcStatus,
						(SQLCHAR *)msgtxt,szErrMsg,errLen);
	if(sts == SQL_SUCCESS) {
		sprintf(szState,"%.5s",lState);
		strcpy(db->dbType,"ODBC");
		cp = msgtxt;
		if(memcmp(cp,"[ma-",3) == 0) {
			cp += 3;
			*errLen = *errLen - 3;
			while(*cp != ']') {
				cp += 1;
				*errLen = *errLen - 1;
			}
			cp += 1;
			*errLen = *errLen - 1;
		}
		if (*cp == '[' && strstr(cp, "MariaDB]") != NULL) {
			while(*cp != ']') {
				cp += 1;
				*errLen = *errLen - 1;
			}
			cp += 1;
			*errLen = *errLen - 1;
			strcpy(db->dbType,"ODBC MariaDB");
			db->mysql = TRUE;
			db->mssql = FALSE;
		}
		if(memcmp(cp,"[unixODBC]",10) == 0) {
			cp += 10;
			*errLen = *errLen - 10;
		}
		if(memcmp(cp,"[Easysoft]",10) == 0) {
			cp += 10;
			*errLen = *errLen - 10;
		}
		if(memcmp(cp,"[SQL Server Driver]",19) == 0) {
			cp += 19;
			*errLen = *errLen - 19;
			strcpy(db->dbType,"MS SQL Server");
			db->mysql = FALSE;
			db->mssql = TRUE;
		}
		if(memcmp(cp,"[IBM]",5) == 0) {
			cp += 5;
			*errLen = *errLen - 5;
		}
		if(cp[*errLen-1] == '\n')
			*errLen = *errLen - 1;
		sprintf(errMsg,"%.*s",*errLen,cp);
	}
	*errnum = *errnum + 1;
	return sts;
}

/**************************************************
	Check Status from an ODBC call
	Return 0 if OK to proceed;
	Return 1 if Not OK to proceed;
**************************************************/
static int
chkSts(
	struct db_state		*db, 
	char		*msg, 
	SQLHANDLE	hndl,
	int			odbcSts)
{
	SQLINTEGER	odbcStatus = 0;
	int			htype;
	SQLSMALLINT errLen;
	int			i;
	SQLCHAR		szState[10];
	char		errMsg[szErrMsg+16];

	if(odbcSts == SQL_SUCCESS) {
		db->dbStatus = 0;
		memset(db->odbcState, 0, sizeof(db->odbcState));
		db->scanForNulls = FALSE;
		return 0;
	}
	if(hndl == db->dbEnvH) {
		htype = SQL_HANDLE_ENV;
	} else if(hndl == db->dbDbcH) {
		htype = SQL_HANDLE_DBC;
	} else {
		htype = SQL_HANDLE_STMT;
	}

	if(msg == NULL)
		msg = (void*)"?";

	if(odbcSts == SQL_SUCCESS_WITH_INFO) {
		db->dbStatus = 0;
		db->scanForNulls = TRUE;
		memset(errMsg,0,sizeof(errMsg));
		memset(szState,0,sizeof(szState));
		memset(db->odbcState, 0, sizeof(db->odbcState));
		i = 1;
		getOdbcMsg(db,hndl,&i,(char*)szState,&odbcStatus,errMsg,szErrMsg,&errLen);
		memcpy(db->odbcState, szState, 5);
		DEBUG_LOG("db",("%.40s Status of %d '%.5s'\n", msg, db->dbStatus, szState));
		if(errMsg[0] >= ' ')
			DEBUG_LOG("db",("    : %s\n",errMsg));
		while(TRUE) {
			if(getOdbcMsg(db,hndl,&i,(char*)szState,&odbcStatus,errMsg,szErrMsg,&errLen)
				!= SQL_SUCCESS)
				break;
		}
		return chkOdbc(db);
	}

	if(odbcSts == SQL_NO_DATA) {
		db->dbStatus = db->dbStsNotFound;
		db->scanForNulls = FALSE;
		if (htype == SQL_HANDLE_STMT)
			SQLFreeStmt(hndl,SQL_CLOSE);
		return db->dbStsNotFound;
	}

	if(odbcSts == SQL_INVALID_HANDLE) {
		db->dbStatus = SQL_INVALID_HANDLE;
		db->scanForNulls = FALSE;
		DEBUG_LOG("db",("Invalid Handle: %s\n",msg));
		return SQL_INVALID_HANDLE;
	}

	memset(errMsg,0,sizeof(errMsg));
	if(hndl == NULL) {
		db->dbStatus = odbcSts;
		return 1;
	}

	i = 1;
	getOdbcMsg(db,hndl,&i,(char*)szState,&odbcStatus,errMsg,szErrMsg,&errLen);
	if(odbcStatus < 0)
		db->dbStatus = -odbcStatus;
	else
		db->dbStatus = odbcStatus;
		
	memset(db->odbcState, 0, sizeof(db->odbcState));
	memcpy(db->odbcState, szState, sizeof(db->odbcState)-1);

	if(db->dbStatus == 0
	&& odbcSts == SQL_ERROR) {		/* Catch ODBC ERROR when native status is 0 */
		db->dbStatus = SQL_ERROR;
	}
	if(memcmp(szState,"23000", 5) == 0
	|| memcmp(szState,"23505",5) == 0) {
		DEBUG_LOG("db",("%.40s Status of %d '%.5s'\n", msg, db->dbStatus, szState));
		if(errMsg[0] >= ' ')
			DEBUG_LOG("db",("    : %s\n",errMsg));
		return db->dbStatus = db->dbStsDupKey;
	}

	if(memcmp(szState,"HY000", 5) == 0
	&& strstr(errMsg,"Connection is busy with results for another") != NULL) {
		db->dbStatus = dbStsRetry;
		DEBUG_LOG("db",("Busy connection: %s\n",msg));
		return dbStsRetry;
	}
	i = strlen(errMsg);
	if (i > errLen)
		i = errLen;
	if(errMsg[i-1] == '\n')
		errMsg[--i] = 0;

	if(db->dbStatus == db->dbStsNotFound2)		/* MODE=ANSI 'Not found' */
		db->dbStatus = db->dbStsNotFound;		/* Set internal 'Not found' status */
	else if(db->dbStatus == db->dbStsNullCol)	/* Ignore NULL Column warning */
		db->dbStatus = 0;
	else if(db->dbStatus == 2114)				/* Ignore "Closing a closed cursor" */
		db->dbStatus = 0;

	if(db->dbStatus == 0)
		return 0;

	if(memcmp(db->odbcState, "S1T00", 5) == 0) {
		db->dbStatus = db->dbFatalStatus = db->dbStsDeadLock;
	} else
	if(memcmp(db->odbcState, "42000", 5) == 0
	|| memcmp(db->odbcState, "07002", 5) == 0) {
		DEBUG_LOG("db",("%.40s Status of %d '%.5s'\n", msg, db->dbStatus, szState));
		if(errMsg[0] >= ' ')
			DEBUG_LOG("db",("    : %s\n",errMsg));
		return chkOdbc(db);
	}
	if(db->dbStatus == db->dbStsNoTable) {
		DEBUG_LOG("db",("%.40s Status of %d '%.5s'\n", msg, db->dbStatus, szState));
		if(errMsg[0] >= ' ')
			DEBUG_LOG("db",("    : %s\n",errMsg));
		return 1;
	}

	if(db->dbStatus != 0 
	&& db->dbStatus != db->dbStsNotFound) {
		if(db->dbStatus == db->dbStsRecLock		/* FOR UPDATE NOWAIT and its held! */
		&& db->intRecWait > 1000
		&& db->nMaxRetry > 0) {
			db->nRecWaitTry++;
			sleep(db->intRecWait/1000);				/* Pause a while */
			return 1;								/* Skip logging error message */
		}
		if(db->dbStatus == 3114
		|| db->dbStatus == 3113) {
			db->isopen = FALSE;
			db->dbFatalStatus = db->dbStatus;
		} else
		if(db->dbFatalStatus == 0
		&& db->dbStatus > 1000) {
			db->dbFatalStatus = db->dbStatus;
		} else {
			chkOdbc(db);
		}
		DEBUG_LOG("db",("%.40s Status of %d '%.5s', fatal %d\n", 
							msg, db->dbStatus, szState, db->dbFatalStatus));
		if(errMsg[0] >= ' ')
			DEBUG_LOG("db",("    : %s\n",errMsg));
	}
	if ( (db->dbFatalStatus >= 0)
	&&   (db->dbStatus < 0)
	&&   (db->dbStatus != db->dbStsRecLock)
	&&   (db->dbStatus != db->dbStsDupKey)
	&&   (db->dbStatus != 1722)
	&&   (db->dbStatus != 1410)
	&&   (db->dbStatus != db->dbStsNotFound) ) {
		db->dbFatalStatus = db->dbStatus;
	}

	return 1;
}

/****************************************************
	Bind just column to return data 
****************************************************/
static int
bindColumn(
	struct db_state	*db,
	struct file_xfd *fx,
	SQL_STMT    *s,
	struct map_xfd *col,
	int			pos)
{
	char	msg[64];
	if (col->cmd == XC_DATA
	 && col->colname) {
		col->hostType = SQL_C_CHAR;
		if (col->dtfrm) {
			col->sqlType = SQL_DATE;
		} else if (col->type == COB_XFDT_FLOAT) {
			if (col->size == sizeof(double))
				col->hostType = SQL_C_DOUBLE;
			else
				col->hostType = SQL_C_FLOAT;
			col->sqlType = SQL_FLOAT;
			col->sqlColSize = col->size;
		} else if (col->type == COB_XFDT_BIN) {
			col->sqlColSize = col->size;
			col->hostType = SQL_C_BINARY;
			col->sqlType = SQL_BINARY;
		} else if (col->valnum) {
			col->sqlType = SQL_DECIMAL;
		} else {
			col->sqlType = SQL_CHAR;
		}
	}
	sprintf(msg,"BindColumn %s.%s Pos %d",fx->tablename,col->colname,pos);
	if(chkSts(db,msg,s->handle,
			SQLBindCol(s->handle, pos, col->hostType,
						col->sdata, col->sqlsize, 
						(SQLPOINTER)col->ind))) {
		return 1;
	}
	return 0;
}

/****************************************************
	Bind just one column as parameter to statment 
****************************************************/
static int
bindParam(
	struct db_state	*db,
	struct file_xfd *fx,
	SQL_STMT    *s,
	struct map_xfd *col,
	int			pos)
{
	char	msg[64];
	if (col->cmd == XC_DATA
	 && col->ind) {
		if (col->setnull) {
			*(SQLLEN*)col->ind = SQL_NULL_DATA;
		} else {
			*(SQLLEN*)col->ind = SQL_NTS;
		}
	}
	sprintf(msg,"BindParam %s.%s Pos %d",fx->tablename,col->colname,pos);
	if(chkSts(db,msg,s->handle,
			SQLBindParameter(s->handle,
						pos, SQL_PARAM_INPUT, col->hostType, col->sqlType,
						col->sqlColSize, col->sqlDecimals,
						col->sdata, col->sqlsize, 
						(SQLPOINTER)col->ind))) {
		return 1;
	}
	return 0;
}

static int
odbc_set_nulls (
	struct db_state	*db,
	struct file_xfd *fx)
{
	int		k;
	for (k=0; k < fx->nmap; k++) {
		if (fx->map[k].cmd == XC_DATA
		 && fx->map[k].ind) {
			if (fx->map[k].setnull) {
				*(SQLLEN*)fx->map[k].ind = SQL_NULL_DATA;
			} else {
				*(SQLLEN*)fx->map[k].ind = SQL_NTS;
			}
		}
	}
	return 0;
}

static int
odbc_any_nulls (
	struct db_state	*db,
	struct file_xfd *fx)
{
	int		k;
	for (k=0; k < fx->nmap; k++) {
		if (fx->map[k].cmd == XC_DATA
		 && fx->map[k].ind) {
			fx->map[k].sqlinlen = fx->map[k].sqlsize;
			if(*(SQLLEN*)fx->map[k].ind == SQL_NULL_DATA) {
				fx->map[k].setnull = TRUE;
			} else if(*(SQLLEN*)fx->map[k].ind == SQL_NTS) {
				fx->map[k].setnull = FALSE;
			} else {
				fx->map[k].setnull = FALSE;
				fx->map[k].sqlinlen = *(SQLLEN*)fx->map[k].ind;
			}
		}
	}
	return 0;
}

static int
odbc_setup_stmt (
	struct db_state	*db,
	struct file_xfd *fx,
	SQL_STMT		*s,
	int				bindtype,
	int				idx)
{
	int		k,pos;
	if (!s->handle) {
		if(chkSts(db,(char*)"Alloc Stmt Handle",db->dbDbcH,
				SQLAllocHandle(SQL_HANDLE_STMT,db->dbDbcH,&s->handle))){
			DEBUG_LOG("db",("SQLAllocHandle %.40s status %d; Failed!\n",s->text,db->dbStatus));
			s->status = db->dbStatus;
			return db->dbStatus;
		}
		s->preped = FALSE;
		s->bound = FALSE;
		s->params = FALSE;
		s->iscursor = FALSE;
	}
	if (!s->preped) {
		if(chkSts(db,(char*)"Prepare Stmt",s->handle,
				SQLPrepare(s->handle,(SQLCHAR*)s->text,strlen(s->text)))){
			DEBUG_LOG("db",("SQLPrepare %.40s status %d; Failed!\n",s->text,db->dbStatus));
			s->status = db->dbStatus;
			return db->dbStatus;
		}
		s->preped = TRUE;
	}
	if (!s->params 
	 && (bindtype & SQL_BIND_PRMS)) {
		pos = 0;
		for (k=0; k < fx->nmap; k++) {
			if (fx->map[k].cmd == XC_DATA
			 && fx->map[k].colname) {
				bindParam (db, fx, s, &fx->map[k], ++pos);
			}
		}
		s->bindpos = pos;
		s->params = TRUE;
	} else
	if (!s->bound 
	 && (bindtype & SQL_BIND_COLS)) {
		pos = 0;
		for (k=0; k < fx->nmap; k++) {
			if (fx->map[k].cmd == XC_DATA
			 && fx->map[k].colname) {
				bindColumn (db, fx, s, &fx->map[k], ++pos);
			}
		}
		s->bindpos = 0;
		s->bound = TRUE;
	}

	if ((bindtype & SQL_BIND_EQ))	{			/* Index columns bind once each */
		pos = s->bindpos;
		for (k=0; k < fx->key[idx]->ncols; k++) {
			bindParam (db, fx, s, &fx->map[fx->key[idx]->col[k]], ++pos);
		}
	} else if ((bindtype & SQL_BIND_WHERE)) {	/* Index Columns for complex WHERE */
		pos = s->bindpos;
		for (k=0; k < fx->key[idx]->ncols-1; k++) {
			bindParam (db, fx, s, &fx->map[fx->key[idx]->col[k]], ++pos);
			bindParam (db, fx, s, &fx->map[fx->key[idx]->col[k]], ++pos);
		}
		bindParam (db, fx, s, &fx->map[fx->key[idx]->col[k]], ++pos);
	}
	s->status = 0;
	return 0;
}

static int
odbc_row_count (
	struct db_state	*db,
	SQL_STMT		*s)
{
	SQLLEN	count;
	count = -1;
	SQLRowCount (s->handle, &count);
	return (int)count;
}

static int
odbc_close_stmt ( SQL_STMT *s)
{
	if (s == NULL
	 || s->handle == NULL)
		return 0;
	SQLFreeStmt(s->handle,SQL_CLOSE);
	s->iscursor = FALSE;
	s->status = 0;
	return 0;
}

static int
odbc_free_stmt ( SQL_STMT *s)
{
	if (s == NULL
	 || s->handle == NULL)
		return 0;
	SQLFreeStmt(s->handle,SQL_CLOSE);
	SQLFreeHandle(SQL_HANDLE_STMT, s->handle);
	s->handle = NULL;
	s->preped = FALSE;
	s->bound = FALSE;
	s->params = FALSE;
	s->iscursor = FALSE;
	s->status = 0;
	return 0;
}

static int
odbc_sync (cob_file_api *a, cob_file *f)
{
	if (!db->isopen)
		return 0;
	if (chkSts(db,(char*) "Commit EndTran ENV",db->dbEnvH,
				SQLEndTran(SQL_HANDLE_ENV,db->dbEnvH,SQL_COMMIT)))
		return COB_STATUS_30_PERMANENT_ERROR;
	if (chkSts(db, (char*)"Commit EndTran DBC",db->dbDbcH,
				SQLEndTran(SQL_HANDLE_DBC,db->dbDbcH,SQL_COMMIT)))
		return COB_STATUS_30_PERMANENT_ERROR;
	return 0;
}

/****************************************************
	Issue one simple SQL statment, no variables
		Return 0 if OK to proceed;
		Return !0 if Not OK to proceed;
*****************************************************/
static int
odbcStmt(
	struct db_state	*db,
	char	*stmt)
{
	SQLHSTMT	stmtHndl;
	int			len, rtn = 0;
	char		msg[80];

	if(chkSts(db,(char*)"Alloc stmtHndl",db->dbDbcH,
				SQLAllocHandle( SQL_HANDLE_STMT, db->dbDbcH, &stmtHndl ))) {
		DEBUG_LOG("db",("SQLAllocHandle %s status %d; Failed!\n",stmt,db->dbStatus));
		return db->dbStatus;
	}

	len = strlen(stmt);
	snprintf(msg,sizeof(msg),"Exec: %.50s",stmt);
	db->dbStatus = 0;
	if(chkSts(db,msg,stmtHndl,
				SQLExecDirect(stmtHndl,(SQLCHAR*)stmt, len))) {
		rtn = db->dbStatus;
		DEBUG_LOG("db",("Stmt: %.50s; Sts %d\n",stmt,db->dbStatus));
	} else if(strncasecmp(stmt,"SELECT ",7) != 0) {
		DEBUG_LOG("db",("Exec: %.50s; OK\n",stmt));
	}
	if (rtn == 0
	 && strncasecmp(stmt,"SELECT ",7) == 0) {
		chkSts(db,(char*)"Bind Var",stmtHndl,
				SQLBindCol(stmtHndl, 1, SQL_C_CHAR, varFetch, sizeof(varFetch)-1, NULL));
		varFetch[0] = 0;
		if(chkSts(db,(char*)"Fetch Stmt",stmtHndl, SQLFetch(stmtHndl))) {
			DEBUG_LOG("db",("Fetch: %.50s; Sts %d\n",stmt,db->dbStatus));
			rtn = db->dbStatus;
		} else {
			DEBUG_LOG("db",("Fetch: %.50s; '%s' OK\n",stmt,varFetch));
		}
	}
	SQLFreeHandle(SQL_HANDLE_STMT, stmtHndl);
	return rtn;
}

static void
odbc_create_table (
	struct db_state	*db,
	struct file_xfd *fx)
{
	int	k;
	cob_load_ddl (fx);
	if (odbcStmt (db, fx->create_table))
		return;
	for (k=0; k < fx->nkeys; k++) {
		if (odbcStmt (db, fx->key[k]->create_index))
			return;
	}
}

/* INDEXED */

static void
join_environment (cob_file_api *a)
{
	char	*env, tmp[256];
	SQLSMALLINT		len;

	db_join = -1;
	memset(db,0,sizeof(struct db_state));
	db->dbStsOk			= 0;
	db->dbStsDupKey		= 2601;
	db->dbStsNotFound	= SQL_NO_DATA;
	db->dbStsNotFound2	= SQL_NO_DATA;
#ifdef WITH_OCI
	db->isoci			= TRUE;
	db->oracle			= TRUE;
	db->dbStsRecLock	= 54;		/*  Oracle row locked by other */
	strcpy(db->dbType,"Oracle OCI");
#endif
#ifdef WITH_ODBC
	db->dbStsRecLock	= -54999;	/* No such status for SQL Server */
	db->dbStsNoTable	= 1146;
	db->isodbc			= TRUE;
	strcpy(db->dbType,"ODBC");
#endif
#ifdef WITH_DB2
	db->dbStsRecLock	= -54999;	/* No such status for SQL Server */
	db->isodbc			= TRUE;
	db->db2				= TRUE;
	strcpy(db->dbType,"DB2");
#endif
	db->dbStsDeadLock	= 1205;
	db->dbStsNoSpace	= 1653;
	db->dbStsNullCol	= 1405;
	db->dbStsInvlNum	= 1722;
	db->dbStsBadRowid	= 1410;
	if(chkSts(db,(char*)"Alloc Env",NULL,
		SQLAllocHandle( SQL_HANDLE_ENV, SQL_NULL_HANDLE, &db->dbEnvH ))) {
		DEBUG_LOG("db",("SQLAllocHandle Env status %d; Failed!\n",db->dbStatus));
		return;
	}
	if(chkSts(db,(char*)"ODBC VER",db->dbEnvH,
		SQLSetEnvAttr(db->dbEnvH,SQL_ATTR_ODBC_VERSION,(SQLPOINTER)SQL_OV_ODBC3,SQL_IS_UINTEGER))) {
		DEBUG_LOG("db",("SQLSetEnvAttr Ver status %d; Failed!\n",db->dbStatus));
		return;
	}
	if(chkSts(db,(char*)"Alloc DBC",db->dbEnvH,
		SQLAllocHandle( SQL_HANDLE_DBC, db->dbEnvH, &db->dbDbcH ))) {
		DEBUG_LOG("db",("SQLAllocHandle DBC status %d; Failed!\n",db->dbStatus));
		return;
	}
	if((env=getSchemaEnvName(db,tmp,"_DSN",db->dbDsn)) != NULL) {
		DEBUG_LOG("db",("Env: %s -> %s\n",tmp,env));
	}
	if((env=getSchemaEnvName(db,tmp,"_UID",db->dbUser)) != NULL) {
		DEBUG_LOG("db",("Env: %s -> %s\n",tmp,env));
	}
	if((env=getSchemaEnvName(db,tmp,"_PWD",db->dbPwd)) != NULL) {
		DEBUG_LOG("db",("Env: %s -> %s\n",tmp,env));
	}
	if((env=getSchemaEnvName(db,tmp,"_CON",db->dbCon)) != NULL) {
		DEBUG_LOG("db",("Env: %s -> %s\n",tmp,env));
	}
#if !defined(WITH_DB2)
	if(useDriverCursor) {
		if(chkSts(db,(char*)"ODBC CURSOR",db->dbEnvH,
			SQLSetConnectAttr(db->dbDbcH,SQL_ATTR_ODBC_CURSORS ,
										(SQLPOINTER)SQL_CUR_USE_DRIVER,SQL_IS_UINTEGER))) {
			DEBUG_LOG("db",("ODBC Cursor status %d; Failed!\n",db->dbStatus));
			return;
		}
	} else if(useIfneededCursor) {
		if(chkSts(db,(char*)"ODBC CURSOR",db->dbEnvH,
			SQLSetConnectAttr(db->dbDbcH,SQL_ATTR_ODBC_CURSORS ,
										(SQLPOINTER)SQL_CUR_USE_IF_NEEDED,SQL_IS_UINTEGER))) {
			DEBUG_LOG("db",("ODBC Cursor status %d; Failed!\n",db->dbStatus));
			return;
		}
	} else {
		if(chkSts(db,(char*)"ODBC CURSOR",db->dbEnvH,
			SQLSetConnectAttr(db->dbDbcH,SQL_ATTR_ODBC_CURSORS ,
										(SQLPOINTER)SQL_CUR_USE_ODBC,SQL_IS_UINTEGER))) {
			DEBUG_LOG("db",("ODBC Cursor status %d; Failed!\n",db->dbStatus));
			return;
		}
	}
	if(db->dbCon[0] > ' ') {
		len = (SQLSMALLINT)sprintf(tmp,"%s",db->dbCon);
		if(chkSts(db, (char*)"Driver Connect", db->dbDbcH,
				SQLDriverConnect(db->dbDbcH, NULL,
							(SQLCHAR*)tmp, SQL_NTS,
							(SQLCHAR*)tmp, sizeof(tmp),
							&len, (SQLSMALLINT)SQL_DRIVER_NOPROMPT) ) ) {
			DEBUG_LOG("db",("SQLDriverConnect status %d; Failed!\n",db->dbStatus));
			DEBUG_LOG("db",(" DriverConnect[%s]\n",db->dbCon));
			db->isopen = FALSE;			/* Data Base is NOT connected */
			if(db->dbDbcH) {
				SQLDisconnect( db->dbDbcH );
				SQLFreeHandle( SQL_HANDLE_DBC, db->dbDbcH);
				db->dbDbcH = NULL;
			}
			if(db->dbEnvH) {
				SQLFreeHandle( SQL_HANDLE_ENV, db->dbEnvH);
				db->dbEnvH = NULL;
			}
			return;
		}
	} else 
#endif
	if(db->dbDsn[0] > ' '
	&& (db->dbUser[0] <= ' ' || db->dbPwd[0] <= ' ')) {	/* Connect with DSN name only */
		if(chkSts(db, (char*)"Connect DSN", db->dbDbcH,
				SQLConnect(db->dbDbcH,
							(SQLCHAR*)db->dbDsn,strlen(db->dbDsn),
							NULL, 0,
							NULL, 0))) {
			DEBUG_LOG("db",("SQLConnect DSN '%s' status %d; Failed!\n",
								db->dbDsn,db->dbStatus));
			db->isopen = FALSE;			/* Data Base is NOT connected */
			if(db->dbDbcH) {
				SQLDisconnect( db->dbDbcH );
				SQLFreeHandle( SQL_HANDLE_DBC, db->dbDbcH);
				db->dbDbcH = NULL;
			}
			if(db->dbEnvH) {
				SQLFreeHandle( SQL_HANDLE_ENV, db->dbEnvH);
				db->dbEnvH = NULL;
			}
			return;
		}
	} else {
		if(db->dbDsn[0] <= ' '
		|| db->dbUser[0] <= ' '
		|| db->dbPwd[0] <= ' ') {
			DEBUG_LOG("db",("~ERROR ODBC Connection is not defined\n"));
			logSchemaEnvName (db, "_NAME");
			logSchemaEnvName (db, "_DSN");
			logSchemaEnvName (db, "_UID");
			db->dbStatus = -99;
			db->isopen = FALSE;			/* Data Base is NOT connected */
			if(db->dbDbcH) {
				SQLDisconnect( db->dbDbcH );
				SQLFreeHandle( SQL_HANDLE_DBC, db->dbDbcH);
				db->dbDbcH = NULL;
			}
			if(db->dbEnvH) {
				SQLFreeHandle( SQL_HANDLE_ENV, db->dbEnvH);
				db->dbEnvH = NULL;
			}
			return;
		}

		if(chkSts(db, (char*)"Session Connect", db->dbDbcH,
				SQLConnect(db->dbDbcH,
							(SQLCHAR*)db->dbDsn,strlen(db->dbDsn),
							(SQLCHAR*)db->dbUser,strlen(db->dbUser),
							(SQLCHAR*)db->dbPwd, strlen(db->dbPwd)))) {
			DEBUG_LOG("db",("SQLConnect status %d; Failed!\n",db->dbStatus));
			DEBUG_LOG("db",("DSN: %s, UID: %s, PWD: %s\n",db->dbDsn,db->dbUser,db->dbPwd));
			db->isopen = FALSE;			/* Data Base is NOT connected */
			if(db->dbDbcH) {
				SQLDisconnect( db->dbDbcH );
				SQLFreeHandle( SQL_HANDLE_DBC, db->dbDbcH);
				db->dbDbcH = NULL;
			}
			if(db->dbEnvH) {
				SQLFreeHandle( SQL_HANDLE_ENV, db->dbEnvH);
				db->dbEnvH = NULL;
			}
			return;
		}
	}
	if(db->dbStatus == -1017
	|| db->dbStatus ==  1017) { /* Invalid User/pass */
		DEBUG_LOG("db",(" %s: User %s, Pwd %s\n",db->dbType,db->dbUser,db->dbPwd));
		return;
	}
	if(db->dbStatus) {
		DEBUG_LOG("db",("SessionBegin status %d; Failed!\n",db->dbStatus));
		DEBUG_LOG("db",("%s: User %s, Pwd %s\n",db->dbType,db->dbUser,db->dbPwd));
		return;
	}
	if(db->db2) {
		DEBUG_LOG("db",("DB2 Connect: DSN: %s, User %s, Pwd %s\n",
								db->dbDsn,db->dbUser,db->dbPwd));
	}
	if(db->arrayFetch > 1
	|| db->stmtCache > 0) {
		char	amsg[40];
		if(db->arrayFetch > 1)
			sprintf(amsg,"; Array fetch %d",db->arrayFetch);
		else
			strcpy(amsg,"");
		DEBUG_LOG("db",("%s: Version %s  Cache %d Selects%s\n",
							db->dbType,"Experimental",db->stmtCache,amsg));
	}

	if((env=getSchemaEnvName(db,tmp,"_TRC",NULL)) != NULL) {
		if(odbcStmt(db,(char*)"ALTER SESSION SET SQL_TRACE = TRUE"))
			return;
	}
	if(db->oracle) {
		/* The DMS Emulation code uses DECIMAL POINT internally */
		if(odbcStmt(db,(char*)"ALTER SESSION SET NLS_NUMERIC_CHARACTERS = '.,'")) {
			return;
		}

		/* Set The default format for handling DATE fields */
		if(db->dateFormat != NULL
		&& strlen(db->dateFormat) > 0) {
			DEBUG_LOG("db",("NOTE: Default DATE field format is '%s'\n",db->dateFormat));
			sprintf(tmp,"ALTER SESSION SET NLS_DATE_FORMAT = '%s'",db->dateFormat);
			if(odbcStmt(db,tmp)) {
				return;
			}
		}

	}

	if(db->oracle) {
		if(odbcStmt(db,(char*)"ALTER SESSION SET OPTIMIZER_MODE = FIRST_ROWS")) {
			return;
		}
	}

	if(chkSts(db,(char*)"AUTO COMMIT OFF",db->dbEnvH,
		SQLSetConnectAttr(db->dbDbcH,SQL_ATTR_AUTOCOMMIT,
									(SQLPOINTER)SQL_AUTOCOMMIT_OFF,SQL_IS_UINTEGER))) {
		DEBUG_LOG("db",("AutoCommit status %d; Failed!\n",db->dbStatus));
		return;
	}
	db_join = 0;			/* All connect steps completed */
	DEBUG_LOG("db",("%s successful connection\n",db->dbType));
	if(odbcStmt(db,(char*)"SELECT @@version")) {
		return;
	} else {
		if (strcasestr(varFetch,"MariaDB")) {
			db->mssql = FALSE;
			db->db2 = FALSE;
			db->mysql = TRUE;
		} else if (strcasestr(varFetch,"MySQL")) {
			db->mssql = FALSE;
			db->db2 = FALSE;
			db->mysql = TRUE;
		} else if (strcasestr(varFetch,"Microsoft SQL")) {
			db->mssql = TRUE;
			db->db2 = FALSE;
			db->mysql = FALSE;
			if ((env = strcasestr(varFetch,"Server")) != NULL) {
				env += 7;
				if (isdigit(*env))
					mssqlver = atoi(env);
				DEBUG_LOG("db",("MS SQL Server %d\n",mssqlver));
			}
		} else if (strcasestr(varFetch,"DB2")) {
			db->mssql = FALSE;
			db->db2 = TRUE;
			db->mysql = FALSE;
		}
	}
}

/* Delete file */
static int
odbc_file_delete (cob_file_api *a, cob_file *f, char *filename)
{
	struct indexed_file	*p;
	char		buff[COB_FILE_MAX+1];
	struct file_xfd	*fx;

	DEBUG_LOG("db",("DELETE FILE %s\n",f->select_name));
	if (db_join) {			/* Join DataBase, on first OPEN of INDEXED file */
		join_environment (a);
		if (db_join < 0) {
			return COB_STATUS_30_PERMANENT_ERROR;
		}
	}
	if (f->file == NULL) {
		fx = cob_load_xfd (f, NULL, sizeof(SQLLEN));
		if (fx == NULL) {
			return COB_STATUS_30_PERMANENT_ERROR;
		}
		p = cob_malloc (sizeof (struct indexed_file));
		f->file = p;
		f->flag_file_lock = 0;	
		f->curkey = -1;
		p->fx = fx;
	}
	p = f->file;
	fx = p->fx;
	snprintf(buff,sizeof(buff),"DROP TABLE %s",fx->tablename);
	if (f->open_mode == COB_OPEN_CLOSED) {
		odbc_close (a, f, 0);
	}
	DEBUG_LOG("db",("%s\n",buff));
	if (odbcStmt(db,buff)
	 && db->dbStatus == db->dbStsNoTable) {
		return 0;
	} 
	if (db->dbStatus != db->dbStsOk) {
		return COB_STATUS_30_PERMANENT_ERROR;
	}

	return 0;
}

/* OPEN INDEXED file */
static int
odbc_open (cob_file_api *a, cob_file *f, char *filename, const int mode, const int sharing)
{
	struct indexed_file	*p;
	int				i, k, ln;
	char		buff[COB_FILE_MAX+1];
#ifdef COB_DEBUG_LOG
	const char	*optyp = "?";
#endif
	struct file_xfd	*fx;

	fx = cob_load_xfd (f, NULL, sizeof(SQLLEN));
	if (fx == NULL) {
		return COB_STATUS_30_PERMANENT_ERROR;
	}
	if (db_join) {			/* Join DataBase, on first OPEN of INDEXED file */
		join_environment (a);
		if (db_join < 0) {
			return COB_STATUS_30_PERMANENT_ERROR;
		}
	}

	p = cob_malloc (sizeof (struct indexed_file));
	f->file = p;
	f->flag_file_lock = 0;	
	f->curkey = -1;
	p->startcond = -1;
	p->fx = fx;
	p->primekeylen = db_keylen (f, 0);
	p->maxkeylen = p->primekeylen;
	for (i=1; i < MAXNUMKEYS && i < f->nkeys; i++) {
		ln = db_keylen (f, i);
		if (ln < 0)
			break;
		if (ln > p->maxkeylen)
			p->maxkeylen = ln;
	}

	switch (mode) {
	case COB_OPEN_OUTPUT:
#ifdef COB_DEBUG_LOG
		optyp = "OUTPUT";
#endif
		snprintf(buff,sizeof(buff),"TRUNCATE TABLE %s",fx->tablename);
		if (odbcStmt(db,buff)
		 && db->dbStatus == db->dbStsNoTable) {
			odbc_create_table (db, fx);
		} 
		if (db->dbStatus != db->dbStsOk) {
			return COB_STATUS_30_PERMANENT_ERROR;
		}
		if(chkSts(db,(char*)"AUTO COMMIT ON",db->dbDbcH,
			SQLSetConnectAttr(db->dbDbcH,SQL_ATTR_AUTOCOMMIT,
										(SQLPOINTER)SQL_AUTOCOMMIT_ON,SQL_IS_UINTEGER))) {
			break;
		}
		DEBUG_LOG("db",("AutoCommit is ON!\n"));
		break;
	case COB_OPEN_INPUT:
	case COB_OPEN_I_O:
	case COB_OPEN_EXTEND:
#ifdef COB_DEBUG_LOG
		if (mode == COB_OPEN_INPUT)
			optyp = "INPUT";
		else if (mode == COB_OPEN_I_O)
			optyp = "IO";
		else
			optyp = "EXTEND";
#endif
		snprintf(buff,sizeof(buff),"SELECT 1 FROM %s WHERE 1 = 0",fx->tablename);
		if (odbcStmt(db,buff)
		 && db->dbStatus == db->dbStsNoTable) {
			odbc_create_table (db, fx);
			if (db->dbStatus != db->dbStsOk)
				return COB_STATUS_30_PERMANENT_ERROR;
		} else if (db->dbStatus != db->dbStsNotFound) {
				return COB_STATUS_30_PERMANENT_ERROR;
		}
		break;
	}

	if ((f->share_mode & COB_SHARE_NO_OTHER)
	 || (f->lock_mode & COB_FILE_EXCLUSIVE) ) {
		p->lmode = LEXCLLOCK;
	} else if (!f->lock_mode) {
		if (mode != COB_OPEN_INPUT) {
			p->lmode = LEXCLLOCK;
		} else {
			p->lmode = LMANULOCK;
		}
	} else if ((f->lock_mode & COB_LOCK_AUTOMATIC) && mode != COB_OPEN_INPUT) {
		p->lmode = LAUTOLOCK;
	} else {
		p->lmode = LMANULOCK;
	}

	if (p->lmode == LEXCLLOCK) {
		if(db->mysql) {
			snprintf(buff,sizeof(buff),"LOCK TABLES %s %s",fx->tablename,
						mode == COB_OPEN_INPUT?"READ":"WRITE");
			if(odbcStmt(db,buff))
				return COB_STATUS_30_PERMANENT_ERROR;
		}
	}

	f->open_mode = mode;
	f->last_open_mode = mode;
	f->flag_nonexistent = 0;
	f->flag_end_of_file = 0;
	f->flag_begin_of_file = 0;
	p->savekey = cob_malloc ((size_t)(p->maxkeylen + 1));
	p->saverec = cob_malloc ((size_t)(f->record_max + 1));
	for (k=0; k < fx->nmap; k++) {
		if (fx->map[k].cmd == XC_DATA
		 && fx->map[k].colname) {
			fx->map[k].hostType = SQL_C_CHAR;
			if (fx->map[k].dtfrm) {
				fx->map[k].sqlType = SQL_DATE;
			} else if (fx->map[k].type == COB_XFDT_FLOAT) {
				if (fx->map[k].size == sizeof(double))
					fx->map[k].hostType = SQL_C_DOUBLE;
				else
					fx->map[k].hostType = SQL_C_FLOAT;
				fx->map[k].sqlType = SQL_FLOAT;
				fx->map[k].sqlColSize = fx->map[k].size;
			} else if (fx->map[k].type == COB_XFDT_BIN) {
				fx->map[k].sqlColSize = fx->map[k].size;
				fx->map[k].hostType = SQL_C_BINARY;
				fx->map[k].sqlType = SQL_BINARY;
			} else if (fx->map[k].valnum) {
				fx->map[k].sqlType = SQL_DECIMAL;
			} else {
				fx->map[k].sqlType = SQL_CHAR;
			}
		}
	}
	DEBUG_LOG("db",("OPEN %s %s\n",optyp,f->select_name));

	return COB_STATUS_00_SUCCESS;
}

/* Close the INDEXED file */

static int
odbc_close (cob_file_api *a, cob_file *f, const int opt)
{
	struct indexed_file	*p;
	struct file_xfd	*fx;
	int		k;
	COB_UNUSED (a);
	COB_UNUSED (opt);

	p = f->file;

	if (p) {
		if (p->fx) {
			fx = p->fx;
			odbc_free_stmt  (&fx->insert);
			odbc_free_stmt  (&fx->delete);
			odbc_free_stmt  (&fx->update);
			odbc_free_stmt  (fx->start);
			fx->start = NULL;
			for (k=0; k < fx->nkeys; k++) {
				odbc_free_stmt  (&fx->key[k]->where_eq);
				odbc_free_stmt  (&fx->key[k]->where_ge);
				odbc_free_stmt  (&fx->key[k]->where_gt);
				odbc_free_stmt  (&fx->key[k]->where_le);
				odbc_free_stmt  (&fx->key[k]->where_lt);
				odbc_free_stmt  (&fx->key[k]->where_ne);
				odbc_free_stmt  (&fx->key[k]->where_fi);
				odbc_free_stmt  (&fx->key[k]->where_la);
			}
			cob_drop_xfd (fx);
		}
		cob_free (p);
	}
	f->file = NULL;
	f->open_mode = COB_OPEN_CLOSED;
	DEBUG_LOG("db",("CLOSE %s\n",f->select_name));

	return COB_STATUS_00_SUCCESS;
}


/* START INDEXED file with positioning */

static int
odbc_start (cob_file_api *a, cob_file *f, const int cond, cob_field *key)
{
	int		ky, klen, partlen, paramtype;
	const char *type = "?";
	struct indexed_file	*p;
	struct file_xfd	*fx;
	COB_UNUSED (a);

	ky = cob_findkey (f, key, &klen, &partlen);
	if (ky < 0) {
		DEBUG_LOG("db",("Start key not found!\n"));
		return COB_STATUS_30_PERMANENT_ERROR;
	}
	p = f->file;
	fx = p->fx;
	p->startcond = cond;
	f->curkey = ky;
	paramtype = SQL_BIND_NO;

	odbc_close_stmt (fx->start);
	fx->start = NULL;
	switch (cond) {
	case COB_EQ:
		if (fx->key[ky]->where_eq.text == NULL) {
			fx->key[ky]->where_eq.text = cob_sql_stmt (db, fx, (char*)"SELECT", ky, type="=");
		}
		fx->start = &fx->key[ky]->where_eq;
		p->isdesc = FALSE;
		paramtype = SQL_BIND_EQ;
		break;
	case COB_GE:
		if (fx->key[ky]->where_ge.text == NULL) {
			fx->key[ky]->where_ge.text = cob_sql_stmt (db, fx, (char*)"SELECT", ky, type=">=");
		}
		fx->start = &fx->key[ky]->where_ge;
		p->isdesc = FALSE;
		paramtype = SQL_BIND_WHERE;
		break;
	case COB_GT:
		if (fx->key[ky]->where_gt.text == NULL) {
			fx->key[ky]->where_gt.text = cob_sql_stmt (db, fx, (char*)"SELECT", ky, type=">");
		}
		fx->start = &fx->key[ky]->where_gt;
		p->isdesc = FALSE;
		paramtype = SQL_BIND_WHERE;
		break;
	case COB_LE:
		if (fx->key[ky]->where_le.text == NULL) {
			fx->key[ky]->where_le.text = cob_sql_stmt (db, fx, (char*)"SELECT", ky, type="<=");
		}
		fx->start = &fx->key[ky]->where_le;
		p->isdesc = TRUE;
		paramtype = SQL_BIND_WHERE;
		break;
	case COB_LT:
		if (fx->key[ky]->where_lt.text == NULL) {
			fx->key[ky]->where_lt.text = cob_sql_stmt (db, fx, (char*)"SELECT", ky, type="<");
		}
		fx->start = &fx->key[ky]->where_lt;
		p->isdesc = TRUE;
		paramtype = SQL_BIND_WHERE;
		break;
	case COB_NE:
		if (fx->key[ky]->where_ne.text == NULL) {
			fx->key[ky]->where_ne.text = cob_sql_stmt (db, fx, (char*)"SELECT", ky, type="!");
		}
		fx->start = &fx->key[ky]->where_ne;
		p->isdesc = TRUE;
		paramtype = SQL_BIND_EQ;
		break;
	case COB_FI:
		if (fx->key[ky]->where_fi.text == NULL) {
			fx->key[ky]->where_fi.text = cob_sql_stmt (db, fx, (char*)"SELECT", ky, type="*");
		}
		fx->start = &fx->key[ky]->where_fi;
		p->isdesc = FALSE;
		paramtype = SQL_BIND_NO;
		break;
	case COB_LA:
		if (fx->key[ky]->where_la.text == NULL) {
			fx->key[ky]->where_la.text = cob_sql_stmt (db, fx, (char*)"SELECT", ky, type="$");
		}
		fx->start = &fx->key[ky]->where_la;
		p->isdesc = TRUE;
		paramtype = SQL_BIND_NO;
		break;
	}
	DEBUG_LOG("db",("Start %s %s index %d  Bind %02X\n",f->select_name,type,ky,paramtype));
	cob_index_to_xfd (db, fx, f, ky);
	odbc_setup_stmt (db, fx, fx->start, SQL_BIND_COLS|paramtype, ky);
	if (fx->start->status) {
		fx->start = NULL;
		cob_sql_dump_data (db, fx);
		return COB_STATUS_30_PERMANENT_ERROR;
	}
	cob_sql_dump_stmt (db, fx->start->text, FALSE);
	if(chkSts(db,(char*)"Start",fx->start->handle,
			SQLExecute(fx->start->handle))){
		return COB_STATUS_30_PERMANENT_ERROR;
	}

	return COB_STATUS_00_SUCCESS;
}

/* Random READ of the INDEXED file  */

static int
odbc_read (cob_file_api *a, cob_file *f, cob_field *key, const int read_opts)
{
	struct indexed_file	*p;
	struct file_xfd	*fx;
	struct map_xfd *col;
	int			k, ky, pos, klen, partlen;
	int			ret = COB_STATUS_00_SUCCESS;
	COB_UNUSED (a);

	ky = cob_findkey (f, key, &klen, &partlen);
	if (ky < 0) {
		return COB_STATUS_30_PERMANENT_ERROR;
	}
	p = f->file;
	fx = p->fx;
	f->curkey = ky;
	p->startcond = -1;
	if (read_opts & COB_READ_LOCK) {
		p->lmode = LEXCLLOCK;
	} else if (read_opts & COB_READ_WAIT_LOCK) {
		if (f->retry_mode == 0
		|| (f->retry_mode & COB_RETRY_FOREVER)) {
			p->lmode = LEXCLLOCK;
		} else {
			p->lmode = LEXCLLOCK;
		}
	} else if ((f->lock_mode & COB_LOCK_AUTOMATIC)
		&& (f->open_mode != COB_OPEN_INPUT) ) {
		p->lmode = LAUTOLOCK;
	}
	if (fx->start)
		odbc_close_stmt (fx->start);
	if (fx->key[ky]->where_eq.text == NULL) {
		fx->key[ky]->where_eq.text = cob_sql_stmt (db, fx, (char*)"SELECT", ky, "=");
	}
	fx->start = &fx->key[ky]->where_eq;
	odbc_close_stmt (fx->start);
	p->isdesc = FALSE;
	cob_index_to_xfd (db, fx, f, ky);
	odbc_setup_stmt (db, fx, fx->start, SQL_BIND_COLS, 0);
	if (fx->start->status) {
		fx->start = NULL;
		cob_sql_dump_data (db, fx);
		return COB_STATUS_30_PERMANENT_ERROR;
	}
	odbc_set_nulls (db, fx);
	pos = 0;
	for (k=0; k < fx->key[ky]->ncols; k++) {
		col = &fx->map[fx->key[ky]->col[k]];
		bindParam (db, fx, fx->start, col, ++pos);
	}
	if(chkSts(db,(char*)"Read Exec",fx->start->handle,
			SQLExecute(fx->start->handle))){
		return COB_STATUS_30_PERMANENT_ERROR;
	}
	if(chkSts(db,(char*)"Read",fx->start->handle, SQLFetch(fx->start->handle))) {
		DEBUG_LOG("db",("Read: %.40s...; Sts %d '%.5s'\n",fx->start->text,
									db->dbStatus,db->odbcState));
		cob_sql_dump_stmt (db, fx->start->text, FALSE);
		cob_sql_dump_index (db, fx, ky);
		if (db->dbStatus == db->dbStsNotFound)
			ret = COB_STATUS_23_KEY_NOT_EXISTS;
		else
			ret = COB_STATUS_30_PERMANENT_ERROR;
	} else {
		DEBUG_LOG("db",("Read: %s; OK\n",f->select_name));
		odbc_any_nulls (db, fx);
		cob_sql_dump_data (db, fx);
		cob_xfd_to_file (db, fx, f);
	}

	return ret;
}

/* Sequential READ of the INDEXED file */

static int
odbc_read_next (cob_file_api *a, cob_file *f, const int read_opts)
{
	struct indexed_file	*p;
	struct file_xfd	*fx;
	int			ky;
	int			ret = COB_STATUS_00_SUCCESS;
	COB_UNUSED (a);

	if (f->open_mode == COB_OPEN_CLOSED)
		return COB_STATUS_49_I_O_DENIED;
	p = f->file;
	fx = p->fx;
	if (f->curkey < 0)
		f->curkey = 0;
	ky = f->curkey;
	switch (read_opts & COB_READ_MASK) {
	default:
    case COB_READ_NEXT:                 
		if (p->startcond != COB_GT) {
			if (fx->key[ky]->where_gt.text == NULL) {
				fx->key[ky]->where_gt.text = cob_sql_stmt (db, fx, (char*)"SELECT", ky, ">");
			}
			fx->start = &fx->key[ky]->where_gt;
			odbc_close_stmt (fx->start);
			p->isdesc = FALSE;
			odbc_setup_stmt (db, fx, fx->start, SQL_BIND_COLS|SQL_BIND_WHERE, f->curkey);
			if(chkSts(db,(char*)"Read Next Exec",fx->start->handle,
					SQLExecute(fx->start->handle))){
				return COB_STATUS_30_PERMANENT_ERROR;
			}
			p->startcond = COB_GT;
		}
		if (fx->start
		 && !p->isdesc) {
			if(chkSts(db,(char*)"Read Next",fx->start->handle, SQLFetch(fx->start->handle))) {
				DEBUG_LOG("db",("Read Next: %.50s; Sts %d\n",fx->start->text,db->dbStatus));
				if (db->dbStatus == db->dbStsNotFound)
					ret = COB_STATUS_10_END_OF_FILE;
				else
					ret = COB_STATUS_30_PERMANENT_ERROR;
			} else {
				DEBUG_LOG("db",("Read Next: %s; OK\n",f->select_name));
				odbc_any_nulls (db, fx);
				cob_xfd_to_file (db, fx, f);
			}
		} else {
			ret = COB_STATUS_10_END_OF_FILE;
		}
		break;
	case COB_READ_PREVIOUS:
		if (p->startcond != COB_LT) {
			if (fx->key[ky]->where_lt.text == NULL) {
				fx->key[ky]->where_lt.text = cob_sql_stmt (db, fx, (char*)"SELECT", ky, "<");
			}
			fx->start = &fx->key[ky]->where_lt;
			odbc_close_stmt (fx->start);
			p->isdesc = TRUE;
			odbc_setup_stmt (db, fx, fx->start, SQL_BIND_COLS|SQL_BIND_WHERE, f->curkey);
			if(chkSts(db,(char*)"Read Prev Exec",fx->start->handle,
					SQLExecute(fx->start->handle))){
				return COB_STATUS_30_PERMANENT_ERROR;
			}
			p->startcond = COB_LT;
		}
		if (fx->start
		 && p->isdesc) {
			if(chkSts(db,(char*)"Read Prev",fx->start->handle, SQLFetch(fx->start->handle))) {
				DEBUG_LOG("db",("Read Prev: %.50s; Sts %d\n",fx->start->text,db->dbStatus));
				if (db->dbStatus == db->dbStsNotFound)
					ret = COB_STATUS_10_END_OF_FILE;
				else
					ret = COB_STATUS_30_PERMANENT_ERROR;
			} else {
				DEBUG_LOG("db",("Read Prev: %s; OK\n",f->select_name));
				odbc_any_nulls (db, fx);
				cob_xfd_to_file (db, fx, f);
			}
		} else {
			ret = COB_STATUS_10_END_OF_FILE;
		}
		break;
	case COB_READ_FIRST:
		if (fx->key[ky]->where_fi.text == NULL) {
			fx->key[ky]->where_fi.text = cob_sql_stmt (db, fx, (char*)"SELECT", ky, "*");
		}
		fx->start = &fx->key[ky]->where_fi;
		odbc_close_stmt (fx->start);
		odbc_setup_stmt (db, fx, fx->start, SQL_BIND_NO, 0);
		if (fx->start->status) {
			fx->start = NULL;
			cob_sql_dump_data (db, fx);
			return COB_STATUS_30_PERMANENT_ERROR;
		}
		p->isdesc = FALSE;
		p->startcond = COB_GT;
		if(chkSts(db,(char*)"Exec First",fx->start->handle,
				SQLExecute(fx->start->handle))){
			return COB_STATUS_30_PERMANENT_ERROR;
		}
		if(chkSts(db,(char*)"Read First",fx->start->handle, SQLFetch(fx->start->handle))) {
			DEBUG_LOG("db",("Read First: %.50s; Sts %d\n",fx->start->text,db->dbStatus));
			if (db->dbStatus == db->dbStsNotFound)
				ret = COB_STATUS_10_END_OF_FILE;
			else
				ret = COB_STATUS_30_PERMANENT_ERROR;
		} else {
			DEBUG_LOG("db",("Read First: %s; OK\n",f->select_name));
			odbc_any_nulls (db, fx);
			cob_xfd_to_file (db, fx, f);
		}
		break;
	case COB_READ_LAST:
		if (fx->key[ky]->where_fi.text == NULL) {
			fx->key[ky]->where_fi.text = cob_sql_stmt (db, fx, (char*)"SELECT", ky, "$");
		}
		fx->start = &fx->key[ky]->where_fi;
		odbc_close_stmt (fx->start);
		odbc_setup_stmt (db, fx, fx->start, SQL_BIND_NO, 0);
		if (fx->start->status) {
			fx->start = NULL;
			cob_sql_dump_data (db, fx);
			return COB_STATUS_30_PERMANENT_ERROR;
		}
		if(chkSts(db,(char*)"Read Last",fx->start->handle,
				SQLExecute(fx->start->handle))){
			return COB_STATUS_30_PERMANENT_ERROR;
		}
		p->isdesc = TRUE;
		p->startcond = COB_LT;
		if(chkSts(db,(char*)"Read Last",fx->start->handle, SQLFetch(fx->start->handle))) {
			DEBUG_LOG("db",("Read Last: %.50s; Sts %d\n",fx->start->text,db->dbStatus));
			if (db->dbStatus == db->dbStsNotFound)
				ret = COB_STATUS_10_END_OF_FILE;
			else
				ret = COB_STATUS_30_PERMANENT_ERROR;
		} else {
			DEBUG_LOG("db",("Read Last: %s; OK\n",f->select_name));
			odbc_any_nulls (db, fx);
			cob_xfd_to_file (db, fx, f);
		}
		break;
	}

	return ret;
}


/* WRITE to the INDEXED file  */

static int
odbc_write (cob_file_api *a, cob_file *f, const int opt)
{
	struct indexed_file	*p;
	struct file_xfd	*fx;
	int			ret = COB_STATUS_00_SUCCESS;
	COB_UNUSED (a);

	if (f->open_mode == COB_OPEN_INPUT
	 || f->open_mode == COB_OPEN_CLOSED)
		return COB_STATUS_49_I_O_DENIED;

	p = f->file;
	fx = p->fx;
	if (fx->insert.text == NULL) {
		fx->insert.text = cob_sql_stmt (db, fx, (char*)"INSERT", 0, NULL);
	}

	cob_file_to_xfd (db, fx, f);

	odbc_set_nulls (db, fx);
	if (!fx->insert.preped) {
		odbc_setup_stmt (db, fx, &fx->insert, SQL_BIND_PRMS, 0);
	}
	if(chkSts(db,(char*)"Exec INSERT",fx->insert.handle,
			SQLExecute(fx->insert.handle))){
		if (db->dbStatus == db->dbStsDupKey) {
			DEBUG_LOG("db",("%.60s Duplicate; Failed!\n",fx->insert.text));
			ret = COB_STATUS_22_KEY_EXISTS;
		} else {
			DEBUG_LOG("db",("SQLExecute %.40s status %d; Failed!\n",fx->insert.text,db->dbStatus));
			ret = COB_STATUS_30_PERMANENT_ERROR;
			cob_sql_dump_data (db, fx);
		}
		return ret;
	}
	DEBUG_LOG("db",("WRITE: %.40s... status %d; Good!\n",fx->insert.text,db->dbStatus));

	return ret;
}


/* DELETE record from the INDEXED file  */

static int
odbc_delete (cob_file_api *a, cob_file *f)
{
	struct indexed_file	*p;
	struct file_xfd	*fx;
	int			k, pos;
	int			ret = COB_STATUS_00_SUCCESS;
	COB_UNUSED (a);

	if (f->open_mode == COB_OPEN_INPUT
	 || f->open_mode == COB_OPEN_CLOSED)
		return COB_STATUS_49_I_O_DENIED;
	p = f->file;
	fx = p->fx;
	if (fx->delete.text == NULL) {
		fx->delete.text = cob_sql_stmt (db, fx, (char*)"DELETE", 0, NULL);
	}

	cob_index_to_xfd (db, fx, f, 0);

	if (!fx->delete.preped) {
		odbc_setup_stmt (db, fx, &fx->delete, SQL_BIND_NO, 0);
		pos = 0;
		for (k=0; k < fx->key[0]->ncols; k++) {
			bindParam (db, fx, &fx->delete, &fx->map[fx->key[0]->col[k]], ++pos);
		}
	}
	if(chkSts(db,(char*)"Exec DELETE",fx->delete.handle,
			SQLExecute(fx->delete.handle))){
		DEBUG_LOG("db",("SQLExecute %.40s status %d; Failed!\n",fx->delete.text,db->dbStatus));
		if (db->dbStatus == db->dbStsNotFound)
			ret = COB_STATUS_23_KEY_NOT_EXISTS;
		else
			ret = COB_STATUS_21_KEY_INVALID;
		cob_sql_dump_data (db, fx);
		return ret;
	}
	k = odbc_row_count (db, &fx->delete);
	if (k == 0)
		ret = COB_STATUS_23_KEY_NOT_EXISTS;
	else if (k > 1)
		ret = COB_STATUS_30_PERMANENT_ERROR;
	DEBUG_LOG("db",("DELETE: %s status %d; %d deleted, return %02d\n",f->select_name,
							db->dbStatus,k,ret));

	return ret;
}

/* REWRITE record to the INDEXED file  */

static int
odbc_rewrite (cob_file_api *a, cob_file *f, const int opt)
{
	struct indexed_file	*p;
	struct file_xfd	*fx;
	int			k, pos;
	int			ret = COB_STATUS_00_SUCCESS;
	COB_UNUSED (a);

	if (f->open_mode == COB_OPEN_INPUT
	 || f->open_mode == COB_OPEN_CLOSED)
		return COB_STATUS_49_I_O_DENIED;
	p = f->file;
	fx = p->fx;
	if (fx->update.text == NULL) {
		fx->update.text = cob_sql_stmt (db, fx, (char*)"UPDATE", 0, NULL);
	}

	cob_file_to_xfd (db, fx, f);

	odbc_set_nulls (db, fx);
	if (!fx->update.preped) {
		odbc_setup_stmt (db, fx, &fx->update, SQL_BIND_PRMS, 0);
		pos = fx->update.bindpos;
		for (k=0; k < fx->key[0]->ncols; k++) {
			bindParam (db, fx, &fx->update, &fx->map[fx->key[0]->col[k]], ++pos);
		}
	}
	if(chkSts(db,(char*)"Exec UPDATE",fx->update.handle,
			SQLExecute(fx->update.handle))){
		if (db->dbStatus == db->dbStsDupKey) {
			DEBUG_LOG("db",("%.60s Duplicate; Failed!\n",fx->update.text));
			ret = COB_STATUS_22_KEY_EXISTS;
		} else {
			DEBUG_LOG("db",("SQLExecute %.40s status %d; Failed!\n",fx->update.text,db->dbStatus));
			ret = COB_STATUS_30_PERMANENT_ERROR;
			cob_sql_dump_data (db, fx);
		}
		return ret;
	}
	k = odbc_row_count (db, &fx->update);
	if (k == 0)
		ret = COB_STATUS_21_KEY_INVALID;
	else if (k > 1)
		ret = COB_STATUS_30_PERMANENT_ERROR;
	DEBUG_LOG("db",("REWRITE: %s, status %d; %d updated, return %02d!\n",f->select_name,
						db->dbStatus,k,ret));

	return ret;
}


static int
odbc_file_unlock (cob_file_api *a, cob_file *f)
{
	COB_UNUSED (a);
	if (COB_FILE_SPECIAL(f)) {
		return 0;
	}
	if (f->organization == COB_ORG_SORT) {
		return 0;
	}

	if (f->open_mode != COB_OPEN_CLOSED 
	 && f->open_mode != COB_OPEN_LOCKED) {
		return 0;
	}
	return 0;
}

/* Call this routine when a new process has been forked */
static int
odbc_fork (cob_file_api *a)
{
	COB_UNUSED (a);
	return 0;
}

static void
odbc_exit_fileio (cob_file_api *a)
{
	COB_UNUSED (a);
}

void
cob_odbc_init_fileio (cob_file_api *a)
{
	a->io_funcs[COB_IO_ODBC] = (void*)&odbc_indexed_funcs;
}

#endif
