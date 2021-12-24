#ifndef _VS2019PI_H_
#define _VS2019PI_H_

#include <vector>
#include <list>

class CFileItem		//: public CSlrListElement
{
public:
	CFileItem();
	CFileItem(const char* name, const char* fullPath, const char *filterName, bool isDir);
	~CFileItem();

	const char* name;
	const char* fullPath;
	const char* filterName;
	bool isDir;
};

class compareFiles
{
	// simple comparison function
public:
	bool operator()(const CFileItem* f1, const CFileItem* f2)
	{
		if (f1->isDir && !f2->isDir)
		{
			return -1;
		}
		if (!f1->isDir && f2->isDir)
		{
			return 1;
		}

		return 0;
	}
};

std::vector<CFileItem*>* SYS_GetFoldersInFolder(const char* directoryPath);
std::vector<CFileItem*>* SYS_GetFilesInFolder(const char* directoryPath, std::list<const char*>* extensions);

#endif
