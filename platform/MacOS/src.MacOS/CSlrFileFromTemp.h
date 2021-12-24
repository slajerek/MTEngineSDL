#ifndef _CFILEFROMTEMP_H_
#define _CFILEFROMTEMP_H_

#include "SYS_Defs.h"
#include "CSlrFileFromDocuments.h"

class CSlrFileFromTemp : public CSlrFileFromDocuments
{
public:
	CSlrFileFromTemp(const char *fileName);
	CSlrFileFromTemp(const char *fileName, u8 fileMode);
	virtual void Open(const char *fileName);
	virtual void OpenForWrite(const char *fileName);
};

#endif
//_CFILEFROMDOCUMENTS_H_

