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
	bool operator()(const CFileItem *f1, const CFileItem *f2) const
	{
		// Directories sort before files.
		//
		// This MUST be a strict weak ordering. The previous body returned
		// -1 / 1 / 0 out of a bool function: -1 and 1 BOTH convert to true,
		// so dir-vs-file and file-vs-dir each reported "less than" and every
		// such pair compared less than the other. std::sort is undefined
		// behaviour on a comparator like that -- it can misorder the listing
		// or run past the end of the sequence on a large enough folder.
		if (f1->isDir != f2->isDir)
			return f1->isDir;

		return false;
	}
};

std::vector<CFileItem*>* SYS_GetFoldersInFolder(const char* directoryPath);
std::vector<CFileItem*>* SYS_GetFilesInFolder(const char* directoryPath, std::list<const char*>* extensions);

#endif
