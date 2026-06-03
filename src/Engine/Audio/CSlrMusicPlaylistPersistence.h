#pragma once

#include "CSlrMusicPlaylistController.h"

class CSlrMusicPlaylistPersistence
{
public:
	static bool SaveToFile(const CSlrMusicPlaylistController &controller,
					   const char *filePath,
					   const CSlrMusicPlaylistSaveOptions &options);

	static bool LoadFromFile(CSlrMusicPlaylistController &controller,
					   const char *filePath,
					   const CSlrMusicPlaylistLoadOptions &options,
					   CSlrMusicPlaylistDocumentInfo *infoOut = nullptr);
};
