#pragma once

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

inline bool SYS_WindowsPathStartsWith(std::string_view path, std::string_view prefix)
{
	return path.size() >= prefix.size() && path.substr(0, prefix.size()) == prefix;
}

inline bool SYS_WindowsPathHasExtendedPrefix(std::string_view path)
{
	return SYS_WindowsPathStartsWith(path, "\\\\?\\") ||
		   SYS_WindowsPathStartsWith(path, "\\\\.\\");
}

inline bool SYS_WindowsPathIsDriveAbsolute(std::string_view path)
{
	if (path.size() < 3)
		return false;
	unsigned char c = (unsigned char)path[0];
	return std::isalpha(c) && path[1] == ':' && (path[2] == '\\' || path[2] == '/');
}

inline bool SYS_WindowsPathIsUnc(std::string_view path)
{
	return SYS_WindowsPathStartsWith(path, "\\\\") && !SYS_WindowsPathHasExtendedPrefix(path);
}

inline std::string SYS_WindowsPathBackslashes(std::string path)
{
	for (char &c : path)
	{
		if (c == '/')
			c = '\\';
	}
	return path;
}

// Normalize a UTF-8 Windows path before passing it to Win32 wide APIs.
// Relative paths stay relative; absolute drive and UNC paths get the Win32
// extended-length prefix so MAX_PATH is not applied by _wfopen/FindFirstFileW.
inline std::string SYS_WindowsNormalizeLongPathUtf8(std::string path)
{
	path = SYS_WindowsPathBackslashes(std::move(path));
	if (path.empty() || SYS_WindowsPathHasExtendedPrefix(path))
		return path;

	if (SYS_WindowsPathIsUnc(path))
		return std::string("\\\\?\\UNC") + path.substr(1);

	if (SYS_WindowsPathIsDriveAbsolute(path))
		return std::string("\\\\?\\") + path;

	return path;
}

inline std::vector<std::string> SYS_WindowsExpandDialogSelectionUtf8(const std::vector<std::string> &dialogParts)
{
	std::vector<std::string> selectedPaths;
	if (dialogParts.empty())
		return selectedPaths;

	if (dialogParts.size() == 1)
	{
		selectedPaths.push_back(SYS_WindowsPathBackslashes(dialogParts[0]));
		return selectedPaths;
	}

	std::string folder = SYS_WindowsPathBackslashes(dialogParts[0]);
	for (size_t i = 1; i < dialogParts.size(); i++)
	{
		std::string fileName = SYS_WindowsPathBackslashes(dialogParts[i]);
		if (fileName.empty())
			continue;

		std::string fullPath = folder;
		if (!fullPath.empty() && fullPath.back() != '\\')
			fullPath += "\\";
		fullPath += fileName;
		selectedPaths.push_back(fullPath);
	}

	return selectedPaths;
}
