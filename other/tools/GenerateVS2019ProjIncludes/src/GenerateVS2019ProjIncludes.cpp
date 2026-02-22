// GenerateVS2019ProjIncludes.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include "SYS_FileSystem.h"
#include <windows.h>
#include <stdio.h>
#include <tchar.h>
#include <strsafe.h>
#include <algorithm>
#include <functional>
#include <Shlobj.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <io.h>

#include "json.h"

#define LOGD printf

#define CONFIG_JSON "config.json"

char projectName[MAX_PATH];
char filtersOutFile[MAX_PATH];
char compileOutFile[MAX_PATH];

struct FolderData
{
	char *folder;
	char* folderPrefix;
	char* folderFilterPrefix;
};

std::vector<FolderData*> rootFolders;
std::vector<char*> libFolders;

std::vector<CFileItem *> recursiveFolders;
std::list<const char*> extensionsCpp;
std::list<const char*> extensionsH;

void addRoot(const char* rootFolder, const char *prefix, const char *filterNamePrefix)
{
	LOGD("addRoot: rootFolder=%s prefix=%s filter=%s\n", rootFolder, prefix, filterNamePrefix);
	std::vector<CFileItem*>* folders = SYS_GetFoldersInFolder(rootFolder);

	CFileItem* fileItem = new CFileItem(rootFolder, prefix, filterNamePrefix, true);
	recursiveFolders.push_back(fileItem);

	for (std::vector<CFileItem*>::iterator it = folders->begin(); it != folders->end(); it++)
	{
		CFileItem* fileItem = *it;

		char *bufPrefix = new char[MAX_PATH];
		sprintf(bufPrefix, "%s\\%s", prefix, fileItem->name);

		char* bufFilterNamePrefix = new char[MAX_PATH];
		sprintf(bufFilterNamePrefix, "%s\\%s", filterNamePrefix, fileItem->name);

		//LOGD("adding %s fullpath=%s\n", buf, fileItem->fullPath);

		addRoot(fileItem->fullPath, bufPrefix, bufFilterNamePrefix);
	}

	// fuck memory leaks
}

FILE* fp;
FILE* fpCompile;

void GenerateFilters()
{
	fprintf(fp, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
	fprintf(fp, "<Project ToolsVersion=\"4.0\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n");
	fprintf(fp, "  <ItemGroup>\n");
	fprintf(fp, "    <Filter Include=\"Source Files\">\n");
	fprintf(fp, "      <UniqueIdentifier>{4FC737F1-C7A5-4376-A066-2A32D752A2FF}</UniqueIdentifier>\n");
	fprintf(fp, "      <Extensions>cpp;c;cc;cxx;def;odl;idl;hpj;bat;asm;asmx</Extensions>\n");
	fprintf(fp, "    </Filter>\n");
	fprintf(fp, "    <Filter Include=\"Header Files\">\n");
	fprintf(fp, "      <UniqueIdentifier>{93995380-89BD-4b04-88EB-625FBE52EBFB}</UniqueIdentifier>\n");
	fprintf(fp, "      <Extensions>h;hh;hpp;hxx;hm;inl;inc;ipp;xsd</Extensions>\n");
	fprintf(fp, "    </Filter>\n");
	fprintf(fp, "    <Filter Include=\"Resource Files\">\n");
	fprintf(fp, "      <UniqueIdentifier>{67DA6AB6-F800-4c08-8B7A-83BB121AAD01}</UniqueIdentifier>\n");
	fprintf(fp, "      <Extensions>rc;ico;cur;bmp;dlg;rc2;rct;bin;rgs;gif;jpg;jpeg;jpe;resx;tiff;tif;png;wav;mfcribbon-ms</Extensions>\n");
	fprintf(fp, "    </Filter>\n");

	for (std::vector<CFileItem*>::iterator it = recursiveFolders.begin(); it != recursiveFolders.end(); it++)
	{
		CFileItem* itemFolder = *it;
		fprintf(fp, "    <Filter Include=\"%s\">\n", itemFolder->filterName);
		//fprintf(fp, "      <UniqueIdentifier>{67DA6AB6-F800-4c08-8B7A-83BB121AAD01}</UniqueIdentifier>\n");
		fprintf(fp, "    </Filter>\n");
	}
	fprintf(fp, "  </ItemGroup>\n");

	LOGD("-----------------------\n");
	fprintf(fp, "  <ItemGroup>\n");
	fprintf(fpCompile, "  <ItemGroup>\n");

	for (std::vector<CFileItem*>::iterator it = recursiveFolders.begin(); it != recursiveFolders.end(); it++)
	{
		CFileItem* itemFolder = *it;

		LOGD("get %s\n", itemFolder->name);
		std::vector<CFileItem*>* files = SYS_GetFilesInFolder(itemFolder->name, &extensionsCpp);
		for (std::vector<CFileItem*>::iterator itFiles = files->begin(); itFiles != files->end(); itFiles++)
		{
			CFileItem* file = *itFiles;
			if (file->isDir)
				continue;
			fprintf(fp, "    <ClCompile Include=\"%s\\%s\">\n", itemFolder->fullPath, file->name);
			fprintf(fp, "      <Filter>%s</Filter>\n", itemFolder->filterName);
			fprintf(fp, "    </ClCompile>\n");

			fprintf(fpCompile, "    <ClCompile Include=\"%s\\%s\" />\n", itemFolder->fullPath, file->name);
		}
	}

	fprintf(fp, "  </ItemGroup>\n");
	fprintf(fpCompile, "  </ItemGroup>\n");

	LOGD("-----------------------\n");
	fprintf(fp, "  <ItemGroup>\n");
	fprintf(fpCompile, "  <ItemGroup>\n");

	for (std::vector<CFileItem*>::iterator it = recursiveFolders.begin(); it != recursiveFolders.end(); it++)
	{
		CFileItem* itemFolder = *it;

		LOGD("get %s\n", itemFolder->name);
		std::vector<CFileItem*>* files = SYS_GetFilesInFolder(itemFolder->name, &extensionsH);
		for (std::vector<CFileItem*>::iterator itFiles = files->begin(); itFiles != files->end(); itFiles++)
		{
			CFileItem* file = *itFiles;
			if (file->isDir)
				continue;
			fprintf(fp, "    <ClInclude Include=\"%s\\%s\">\n", itemFolder->fullPath, file->name);
			fprintf(fp, "      <Filter>%s</Filter>\n", itemFolder->filterName);
			fprintf(fp, "    </ClInclude>\n");

			fprintf(fpCompile, "    <ClInclude Include=\"%s\\%s\" />\n", itemFolder->fullPath, file->name);
		}
	}

	fprintf(fp, "  </ItemGroup>\n");
	fprintf(fp, "</Project>\n");

	fprintf(fpCompile, "  </ItemGroup>\n");
}

void GenerateIncludeDirectories()
{
	fprintf(fpCompile, "<AdditionalIncludeDirectories>");
	for (std::vector<CFileItem*>::iterator it = recursiveFolders.begin(); it != recursiveFolders.end(); it++)
	{
		CFileItem* itemFolder = *it;
		fprintf(fpCompile, "%s;", itemFolder->name);
	}
	fprintf(fpCompile, "%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>");
}

#define BUFSIZE 1*1024*1024

int main()
{
	// This will generate VS2019 compliant sections with files and folders to be included

	LOGD("** PARSING JSON **\n");

	char *buf = new char[BUFSIZE];
	fp = fopen(CONFIG_JSON, "rb");
	if (!fp)
	{
		printf(CONFIG_JSON " file is missing.\n");
		exit(-1);
	}

	fread(buf, 1, BUFSIZE, fp);
	fclose(fp);

	LOGD("buf='%s'\n", buf);
	std::string jsonInput(buf);
	json::jobject result = json::jobject::parse(jsonInput);

	std::string valueProjectName = result["projectName"];
	strcpy(projectName, valueProjectName.c_str());

	LOGD("projectName=%s\n", projectName);

	std::vector<std::string> codeArray = result["code"];
	for (std::vector<std::string>::iterator it = codeArray.begin(); it != codeArray.end(); it++)
	{
		std::string str = *it;
		strcpy(buf, str.c_str());

		std::string codeInput(buf);
		json::jobject codeResult = json::jobject::parse(codeInput);

		FolderData* folderData = new FolderData();

		std::string value = (std::string)codeResult["folder"];
		folderData->folder = new char[value.size()+1];
		strcpy(folderData->folder, value.c_str());

		value = (std::string)codeResult["prefix"];
		folderData->folderPrefix = new char[value.size()+1];
		strcpy(folderData->folderPrefix, value.c_str());

		value = (std::string)codeResult["filterPrefix"];
		folderData->folderFilterPrefix = new char[value.size()+1];
		strcpy(folderData->folderFilterPrefix, value.c_str());

		rootFolders.push_back(folderData);
		LOGD("code folder=%s prefix=%s filterPrefix=%s\n", folderData->folder, folderData->folderPrefix, folderData->folderFilterPrefix);
	}

	std::vector<std::string> libArray = result["lib"];
	for (std::vector<std::string>::iterator it = libArray.begin(); it != libArray.end(); it++)
	{
		std::string str = *it;
		strcpy(buf, str.c_str());

		std::string codeInput(buf);
		json::jobject codeResult = json::jobject::parse(codeInput);

		std::string value = codeResult["folder"];
		char *folder = new char[value.size() + 1];
		strcpy(folder, value.c_str());

		libFolders.push_back(folder);

		LOGD("lib folder=%s\n", folder);
	}

	sprintf(filtersOutFile, "%s.vcxproj.filters", projectName);
	sprintf(compileOutFile, "%s.vcxproj.txt", projectName);

	extensionsCpp.push_back("cpp");
	extensionsCpp.push_back("c");
	extensionsH.push_back("h");

	LOGD("\n** CODE FOLDERS **\n");
	// recursive generate code folders
	for (std::vector<FolderData*>::iterator it = rootFolders.begin(); it != rootFolders.end(); it++)
	{
		FolderData* rootFolder = *it;

		addRoot(rootFolder->folder, rootFolder->folderPrefix, rootFolder->folderFilterPrefix);
	}

	fp = fopen(filtersOutFile, "wb");
	fpCompile = fopen(compileOutFile, "wb");

	if (fp == NULL || fpCompile == NULL)
	{
		printf("Can't create out file\n");
		exit(-1);
	}

	LOGD("Generating filters & compile\n");
	GenerateFilters();

	LOGD("\n** LIB INCLUDE FOLDERS **\n");
	// recursive generate lib folders
	for (std::vector<char*>::iterator it = libFolders.begin(); it != libFolders.end(); it++)
	{
		char* libFolder = *it;

		addRoot(libFolder, "", "");
	}

	fprintf(fpCompile, "\n#\n\n");

	GenerateIncludeDirectories();

	fclose(fp);
	fclose(fpCompile);

	LOGD("\n** DONE **\n");

	//<AdditionalIncludeDirectories>C:\develop\MTEngineSDL\src\Engine\Libs\zlib; C:\develop\MTEngineSDL\src\Engine\Libs\tremor\Ogg; C:\develop\MTEngineSDL\src\Engine\Libs\tremor; C:\develop\MTEngineSDL\src\Engine\Libs\mtrand; C:\develop\MTEngineSDL\src\Engine\Libs\lodepng; C:\develop\MTEngineSDL\src\Engine\Libs\libpng; C:\develop\MTEngineSDL\src\Engine\Libs\jpeg\jpeg - 9a; C:\develop\MTEngineSDL\src\Engine\Libs\jpeg; C:\develop\MTEngineSDL\src\Engine\Libs\imgui; C:\develop\MTEngineSDL\src\Engine\Libs; C:\develop\MTEngineSDL\src\Engine\Funct\ImageProcessing\stb_image; C:\develop\MTEngineSDL\src\Engine\Funct\ImageProcessing\resampler; C:\develop\MTEngineSDL\src\Engine\Funct\ImageProcessing; C:\develop\MTEngineSDL\src\Engine\Funct; C:\develop\MTEngineSDL\src\Engine\Core\Pool; C:\develop\MTEngineSDL\src\Engine\Core; C:\develop\MTEngineSDL\src\Engine\Audio\WAV; C:\develop\MTEngineSDL\src\Engine\Audio; C:\develop\MTEngineSDL\src\Engine; C:\develop\MTEngineSDL\src\Embedded; C:\develop\MTEngineSDL\src; C:\develop\MTEngineSDL\platform\Windows\src.Windows; % (AdditionalIncludeDirectories) < / AdditionalIncludeDirectories >

	//  <ItemGroup>
	//    <ClCompile Include = "..\..\..\src\DummyInit.cpp" / >
	//    <ClCompile Include = "..\..\..\src\Recognition\CRecognitionAlgorithm.cpp" / >
	//    <ClCompile Include = "..\..\..\src\Recognition\CRecognitionAlgorithmDummy.cpp" / >
	//    <ClCompile Include = "..\..\..\src\Utils\IMG_Utils.cpp" / >
	//  </ItemGroup>
	//  <ItemGroup>
	//    <ClInclude Include = "..\..\..\src\DummyInit.h" / >
	//	  <ClInclude Include = "..\..\..\src\Recognition\CRecognitionAlgorithm.h" / >
	//    <ClInclude Include = "..\..\..\src\Utils\IMG_Utils.h" / >
	//  </ItemGroup>

	//GenerateFilters();
}

